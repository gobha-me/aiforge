#include <aiforge/runtime/tool_profiles.hpp>

#include <algorithm>
#include <array>
#include <ranges>
#include <set>
#include <string_view>
#include <utility>

#include <aiforge/detail/utf8_text.hpp>

namespace aiforge::runtime {
namespace {

[[nodiscard]] auto error(const ToolProfileErrorCode code, std::string message)
    -> std::unexpected<ToolProfileError> {
  return std::unexpected(ToolProfileError{code, std::move(message)});
}

[[nodiscard]] auto valid_limits(const ToolProfileLimits& limits) -> bool {
  return limits.maximum_profiles != 0 && limits.maximum_profile_id_bytes != 0 &&
         limits.maximum_profile_name_bytes != 0 &&
         limits.maximum_tools_per_profile != 0 &&
         limits.maximum_tool_name_bytes != 0 && limits.maximum_total_bytes != 0;
}

[[nodiscard]] auto valid_profile_id(const std::string_view value,
                                    const std::size_t maximum_bytes) -> bool {
  if (value.empty() || value.size() > maximum_bytes) return false;
  const auto first = static_cast<unsigned char>(value.front());
  if (first < 'a' || first > 'z') return false;
  return std::ranges::all_of(value.substr(1), [](const char raw) {
    const auto character = static_cast<unsigned char>(raw);
    return (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9') || character == '-' ||
           character == '_';
  });
}

[[nodiscard]] auto valid_single_line_text(const std::string_view value,
                                          const std::size_t maximum_bytes)
    -> bool {
  return !value.empty() && value.size() <= maximum_bytes &&
         detail::is_safe_utf8_text(value) &&
         value.find_first_of("\r\n\t") == std::string_view::npos;
}

[[nodiscard]] auto valid_tool_name(const std::string_view value,
                                   const std::size_t maximum_bytes) -> bool {
  return valid_single_line_text(value, maximum_bytes) &&
         value.find_first_of("*?[]{}") == std::string_view::npos;
}

[[nodiscard]] auto add_size(std::size_t& total, const std::size_t amount,
                            const std::size_t maximum) -> bool {
  if (amount > maximum - std::min(total, maximum)) return false;
  total += amount;
  return true;
}

[[nodiscard]] auto make_id(const std::string_view value)
    -> domain::ToolProfileId {
  return *domain::ToolProfileId::from(std::string{value});
}

[[nodiscard]] auto find_profile(
    const std::span<const ToolProfile> profiles,
    const domain::ToolProfileId& profile_id) noexcept -> const ToolProfile* {
  const auto found =
      std::ranges::find(profiles, profile_id, &ToolProfile::profile_id);
  return found == profiles.end() ? nullptr : &*found;
}

[[nodiscard]] auto contains_tool(const ToolProfile& profile,
                                 const std::string_view tool_name) noexcept
    -> bool {
  return std::ranges::contains(profile.tool_names, tool_name);
}

[[nodiscard]] auto valid_category(const ToolCategory category) noexcept
    -> bool {
  switch (category) {
    case ToolCategory::interaction:
    case ToolCategory::memory:
    case ToolCategory::repository:
    case ToolCategory::process:
    case ToolCategory::media:
    case ToolCategory::other: return true;
  }
  return false;
}

[[nodiscard]] auto validate_desired_tools(
    const ToolProfile& selected,
    const std::optional<std::vector<std::string>>& desired,
    const ToolProfileLimits& limits) -> std::expected<void, ToolProfileError> {
  if (!desired) return {};
  if (desired->size() > limits.maximum_tools_per_profile) {
    return error(ToolProfileErrorCode::resource_exhausted,
                 "desired tool selection contains too many tools");
  }
  std::set<std::string_view> unique_names;
  std::size_t total_bytes{};
  for (const auto& tool_name : *desired) {
    if (!valid_tool_name(tool_name, limits.maximum_tool_name_bytes) ||
        !contains_tool(selected, tool_name)) {
      return error(ToolProfileErrorCode::invalid_profile,
                   "desired tools must be exact selected-profile members");
    }
    if (!unique_names.insert(tool_name).second) {
      return error(ToolProfileErrorCode::duplicate_tool,
                   "desired tool selection contains a duplicate name");
    }
    if (!add_size(total_bytes, tool_name.size(), limits.maximum_total_bytes)) {
      return error(ToolProfileErrorCode::resource_exhausted,
                   "desired tool selection exceeds its total byte limit");
    }
  }
  return {};
}

[[nodiscard]] auto canonical_desired_tools(
    const ToolProfile& selected,
    const std::optional<std::vector<std::string>>& desired)
    -> std::optional<std::vector<std::string>> {
  if (!desired) return std::nullopt;
  std::vector<std::string> result;
  result.reserve(desired->size());
  for (const auto& tool_name : selected.tool_names) {
    if (std::ranges::contains(*desired, tool_name)) {
      result.push_back(tool_name);
    }
  }
  return result;
}

[[nodiscard]] auto find_maximum_profile(
    const std::span<const ToolProfile> profiles,
    const std::optional<domain::ToolProfileId>& profile_id,
    const std::string_view label)
    -> std::expected<const ToolProfile*, ToolProfileError> {
  if (!profile_id) return nullptr;
  const auto* profile = find_profile(profiles, *profile_id);
  if (profile == nullptr) {
    return error(ToolProfileErrorCode::unknown_profile,
                 std::string{label} + " maximum tool profile is unknown");
  }
  return profile;
}

[[nodiscard]] auto policy_allows_declaration(
    const backend::ToolDeclaration& declaration,
    const domain::ToolPolicyProvenance* policy) noexcept -> bool {
  if (declaration.effects.empty()) return declaration.capability_scopes.empty();
  if (policy == nullptr) return false;
  if (std::ranges::any_of(declaration.effects, [&](const auto effect) {
        return !std::ranges::contains(policy->effect_ceiling, effect);
      })) {
    return false;
  }
  return std::ranges::all_of(
      declaration.capability_scopes, [&](const auto& requested) {
        return std::ranges::any_of(
            policy->capability_ceiling, [&](const auto& ceiling) {
              return capability_scope_covers(ceiling, requested);
            });
      });
}

struct CheckedSelection {
  ToolProfileSelection selection;
  const ToolProfile* selected{};
  const ToolProfile* model_maximum{};
  const ToolProfile* persona_maximum{};
  const domain::ToolPolicyProvenance* policy{};
};

[[nodiscard]] auto check_selection(ToolProfileSelection selection,
                                   const ToolPolicy& launch_policy,
                                   const ToolProfileLimits& limits)
    -> std::expected<CheckedSelection, ToolProfileError> {
  const auto profiles = builtin_tool_profiles();
  if (auto valid = validate_tool_profiles(profiles, limits); !valid) {
    return std::unexpected(std::move(valid.error()));
  }
  const auto* selected = find_profile(profiles, selection.selected_profile_id);
  if (selected == nullptr) {
    return error(ToolProfileErrorCode::unknown_profile,
                 "selected tool profile is unknown");
  }
  if (auto valid = validate_desired_tools(*selected,
                                          selection.desired_tool_names, limits);
      !valid) {
    return std::unexpected(std::move(valid.error()));
  }
  selection.desired_tool_names =
      canonical_desired_tools(*selected, selection.desired_tool_names);

  auto model_maximum = find_maximum_profile(
      profiles, selection.model_maximum_profile_id, "selected model");
  if (!model_maximum) return std::unexpected(std::move(model_maximum.error()));
  auto persona_maximum = find_maximum_profile(
      profiles, selection.persona_maximum_profile_id, "active persona");
  if (!persona_maximum) {
    return std::unexpected(std::move(persona_maximum.error()));
  }
  const auto* policy = launch_policy.provenance();
  if (policy != nullptr && !domain::validate_tool_policy_provenance(*policy)) {
    return error(ToolProfileErrorCode::invalid_profile,
                 "active launch policy provenance is invalid");
  }
  return CheckedSelection{std::move(selection), selected, *model_maximum,
                          *persona_maximum, policy};
}

[[nodiscard]] auto availability_reason(const RegisteredTool* registered,
                                       const std::string& tool_name,
                                       const CheckedSelection& checked)
    -> ToolProfileAvailabilityReason {
  if (registered == nullptr) {
    return ToolProfileAvailabilityReason::tool_not_registered;
  }
  if (checked.selected->profile_id == make_id("essentials") &&
      (!registered->declaration.effects.empty() ||
       !registered->declaration.capability_scopes.empty())) {
    return ToolProfileAvailabilityReason::profile_contract_mismatch;
  }
  if (checked.selection.desired_tool_names &&
      !std::ranges::contains(*checked.selection.desired_tool_names,
                             tool_name)) {
    return ToolProfileAvailabilityReason::session_tool_disabled;
  }
  if (checked.model_maximum != nullptr &&
      !contains_tool(*checked.model_maximum, tool_name)) {
    return ToolProfileAvailabilityReason::model_profile_limit;
  }
  if (checked.persona_maximum != nullptr &&
      !contains_tool(*checked.persona_maximum, tool_name)) {
    return ToolProfileAvailabilityReason::persona_profile_limit;
  }
  if (!checked.selection.model_tool_calling_support.has_value()) {
    return ToolProfileAvailabilityReason::model_tool_calling_unknown;
  }
  if (!*checked.selection.model_tool_calling_support) {
    return ToolProfileAvailabilityReason::model_tool_calling_unsupported;
  }
  if (registered->category == ToolCategory::process &&
      checked.policy != nullptr &&
      checked.policy->identity == "aiforge.tool-launch-policy.v2" &&
      !checked.policy->achieved_restriction_level) {
    return ToolProfileAvailabilityReason::launch_policy_denied;
  }
  if (!policy_allows_declaration(registered->declaration, checked.policy)) {
    return ToolProfileAvailabilityReason::launch_policy_denied;
  }
  return ToolProfileAvailabilityReason::available;
}

// clang-format off
// NOLINTNEXTLINE(cert-err58-cpp) -- Built-in allocation failure is process-fatal.
const std::array kBuiltinProfiles{
    ToolProfile{
        make_id("essentials"), "Essentials", {"ask_user", "propose_memory"}},
    ToolProfile{make_id("repository-read"),
                "Repository read",
                {"ask_user", "propose_memory", "read_repository_file"}},
    ToolProfile{make_id("media"),
                "Media",
                {"ask_user", "propose_memory", "generate_image"}},
    ToolProfile{make_id("off"), "Off", {}},
};
// clang-format on

} // namespace

// clang-format off
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Explicitly validates every bounded catalog invariant.
auto validate_tool_profiles(const std::span<const ToolProfile> profiles,
                            const ToolProfileLimits limits)
    -> std::expected<void, ToolProfileError> {
  // clang-format on
  try {
    if (!valid_limits(limits)) {
      return error(ToolProfileErrorCode::invalid_limits,
                   "tool profile limits must be positive");
    }
    if (profiles.empty()) {
      return error(ToolProfileErrorCode::invalid_profile,
                   "tool profile catalog must not be empty");
    }
    if (profiles.size() > limits.maximum_profiles) {
      return error(ToolProfileErrorCode::resource_exhausted,
                   "tool profile catalog contains too many profiles");
    }

    std::set<std::string_view> profile_ids;
    std::set<std::string_view> profile_names;
    std::size_t total_bytes{};
    for (const auto& profile : profiles) {
      if (!valid_profile_id(profile.profile_id.value(),
                            limits.maximum_profile_id_bytes) ||
          !valid_single_line_text(profile.name,
                                  limits.maximum_profile_name_bytes)) {
        return error(ToolProfileErrorCode::invalid_profile,
                     "tool profile identity or name is invalid");
      }
      if (!profile_ids.insert(profile.profile_id.value()).second ||
          !profile_names.insert(profile.name).second) {
        return error(ToolProfileErrorCode::duplicate_profile,
                     "tool profile identities and names must be unique");
      }
      if (profile.tool_names.size() > limits.maximum_tools_per_profile) {
        return error(ToolProfileErrorCode::resource_exhausted,
                     "tool profile contains too many tools");
      }
      if (!add_size(total_bytes, profile.profile_id.value().size(),
                    limits.maximum_total_bytes) ||
          !add_size(total_bytes, profile.name.size(),
                    limits.maximum_total_bytes)) {
        return error(ToolProfileErrorCode::resource_exhausted,
                     "tool profile catalog exceeds its total byte limit");
      }

      std::set<std::string_view> tool_names;
      for (const auto& tool_name : profile.tool_names) {
        if (!valid_tool_name(tool_name, limits.maximum_tool_name_bytes)) {
          return error(ToolProfileErrorCode::invalid_profile,
                       "tool profile contains an invalid or wildcard name");
        }
        if (!tool_names.insert(tool_name).second) {
          return error(ToolProfileErrorCode::duplicate_tool,
                       "tool profile contains a duplicate tool name");
        }
        if (!add_size(total_bytes, tool_name.size(),
                      limits.maximum_total_bytes)) {
          return error(ToolProfileErrorCode::resource_exhausted,
                       "tool profile catalog exceeds its total byte limit");
        }
      }
    }
    return {};
  } catch (...) {
    return error(ToolProfileErrorCode::internal_failure,
                 "tool profile validation failed internally");
  }
}

auto builtin_tool_profiles() noexcept -> std::span<const ToolProfile> {
  return kBuiltinProfiles;
}

auto tool_profile_availability_reason_text(
    const ToolProfileAvailabilityReason reason) noexcept -> std::string_view {
  switch (reason) {
    case ToolProfileAvailabilityReason::available: return "available";
    case ToolProfileAvailabilityReason::tool_not_registered:
      return "tool is not registered in this runtime";
    case ToolProfileAvailabilityReason::profile_contract_mismatch:
      return "registered tool exceeds the profile's no-authority contract";
    case ToolProfileAvailabilityReason::session_tool_disabled:
      return "tool is disabled by the session selection";
    case ToolProfileAvailabilityReason::model_profile_limit:
      return "tool is outside the selected model's maximum profile";
    case ToolProfileAvailabilityReason::persona_profile_limit:
      return "tool is outside the active persona's maximum profile";
    case ToolProfileAvailabilityReason::launch_policy_denied:
      return "tool is unavailable under the active launch policy";
    case ToolProfileAvailabilityReason::model_tool_calling_unsupported:
      return "selected model does not support tool calling";
    case ToolProfileAvailabilityReason::model_tool_calling_unknown:
      return "selected model tool-calling support is unknown";
  }
  return "tool availability is unknown";
}

auto tool_profile_category_members(
    const ToolRegistrySnapshot& full_registry,
    const domain::ToolProfileId& selected_profile_id,
    const ToolCategory category, const ToolProfileLimits limits)
    -> std::expected<std::vector<std::string>, ToolProfileError> {
  try {
    const auto profiles = builtin_tool_profiles();
    if (auto valid = validate_tool_profiles(profiles, limits); !valid) {
      return std::unexpected(std::move(valid.error()));
    }
    if (!valid_category(category)) {
      return error(ToolProfileErrorCode::invalid_profile,
                   "tool category is invalid");
    }
    const auto* selected = find_profile(profiles, selected_profile_id);
    if (selected == nullptr) {
      return error(ToolProfileErrorCode::unknown_profile,
                   "selected tool profile is unknown");
    }
    std::vector<std::string> result;
    result.reserve(selected->tool_names.size());
    for (const auto& tool_name : selected->tool_names) {
      const auto* registered = full_registry.find(tool_name);
      if (registered != nullptr && registered->category == category) {
        result.push_back(tool_name);
      }
    }
    return result;
  } catch (...) {
    return error(ToolProfileErrorCode::internal_failure,
                 "tool profile category expansion failed internally");
  }
}

auto resolve_tool_profile(const ToolRegistrySnapshot& full_registry,
                          ToolProfileSelection selection,
                          const ToolPolicy& launch_policy,
                          const ToolProfileLimits limits)
    -> std::expected<ToolProfileResolution, ToolProfileError> {
  try {
    auto checked = check_selection(std::move(selection), launch_policy, limits);
    if (!checked) return std::unexpected(std::move(checked.error()));
    ToolProfileResolution result{
        *checked->selected, {}, {}, checked->selection};
    result.tool_availability.reserve(checked->selected->tool_names.size());
    std::vector<std::string> effective_names;
    effective_names.reserve(checked->selected->tool_names.size());
    for (const auto& tool_name : checked->selected->tool_names) {
      const auto* registered = full_registry.find(tool_name);
      const auto reason = availability_reason(registered, tool_name, *checked);
      if (reason == ToolProfileAvailabilityReason::available)
        effective_names.push_back(tool_name);
      result.tool_availability.push_back({tool_name, reason});
    }

    auto effective = full_registry.subset(effective_names);
    if (!effective) {
      return error(ToolProfileErrorCode::internal_failure,
                   "effective tool profile could not be constructed");
    }
    result.effective_tools = std::move(*effective);
    return result;
  } catch (...) {
    return error(ToolProfileErrorCode::internal_failure,
                 "tool profile resolution failed internally");
  }
}

auto resolve_tool_profile(const ToolRegistrySnapshot& full_registry,
                          const domain::ToolProfileId& selected_profile_id,
                          const std::optional<bool> model_tool_calling_support,
                          const std::span<const ToolProfile> profiles,
                          const ToolProfileLimits limits)
    -> std::expected<ToolProfileResolution, ToolProfileError> {
  try {
    if (auto valid = validate_tool_profiles(profiles, limits); !valid) {
      return std::unexpected(std::move(valid.error()));
    }
    const auto selected = std::ranges::find(profiles, selected_profile_id,
                                            &ToolProfile::profile_id);
    if (selected == profiles.end()) {
      return error(ToolProfileErrorCode::unknown_profile,
                   "selected tool profile is unknown");
    }

    ToolProfileResolution result{
        *selected,
        {},
        {},
        ToolProfileSelection{selected_profile_id, std::nullopt, std::nullopt,
                             std::nullopt, model_tool_calling_support}};
    result.tool_availability.reserve(selected->tool_names.size());
    std::vector<std::string> effective_names;
    effective_names.reserve(selected->tool_names.size());
    for (const auto& tool_name : selected->tool_names) {
      auto reason = ToolProfileAvailabilityReason::available;
      const auto* registered = full_registry.find(tool_name);
      if (registered == nullptr) {
        reason = ToolProfileAvailabilityReason::tool_not_registered;
      } else if (selected->profile_id == make_id("essentials") &&
                 (!registered->declaration.effects.empty() ||
                  !registered->declaration.capability_scopes.empty())) {
        reason = ToolProfileAvailabilityReason::profile_contract_mismatch;
      } else if (!model_tool_calling_support.has_value()) {
        reason = ToolProfileAvailabilityReason::model_tool_calling_unknown;
      } else if (!*model_tool_calling_support) {
        reason = ToolProfileAvailabilityReason::model_tool_calling_unsupported;
      } else {
        effective_names.push_back(tool_name);
      }
      result.tool_availability.push_back({tool_name, reason});
    }

    auto effective = full_registry.subset(effective_names);
    if (!effective) {
      return error(ToolProfileErrorCode::internal_failure,
                   "effective tool profile could not be constructed");
    }
    result.effective_tools = std::move(*effective);
    return result;
  } catch (...) {
    return error(ToolProfileErrorCode::internal_failure,
                 "tool profile resolution failed internally");
  }
}

} // namespace aiforge::runtime
