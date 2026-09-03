#include <aiforge/domain/provenance.hpp>

#include <algorithm>
#include <cstdint>
#include <set>
#include <string_view>
#include <utility>

namespace aiforge::domain {
namespace {

[[nodiscard]] auto failure(const RunProvenanceErrorCode code,
                           std::string message)
    -> std::unexpected<RunProvenanceError> {
  return std::unexpected(RunProvenanceError{code, std::move(message)});
}

[[nodiscard]] auto valid_utf8(const std::string_view value) -> bool {
  std::size_t index{};
  while (index < value.size()) {
    const auto lead = static_cast<unsigned char>(value[index]);
    std::size_t continuation_count{};
    std::uint32_t code_point{};
    if (lead <= 0x7fU) {
      ++index;
      continue;
    }
    if (lead >= 0xc2U && lead <= 0xdfU) {
      continuation_count = 1;
      code_point = lead & 0x1fU;
    } else if (lead >= 0xe0U && lead <= 0xefU) {
      continuation_count = 2;
      code_point = lead & 0x0fU;
    } else if (lead >= 0xf0U && lead <= 0xf4U) {
      continuation_count = 3;
      code_point = lead & 0x07U;
    } else {
      return false;
    }
    if (index + continuation_count >= value.size()) return false;
    for (std::size_t offset = 1; offset <= continuation_count; ++offset) {
      const auto byte = static_cast<unsigned char>(value[index + offset]);
      if ((byte & 0xc0U) != 0x80U) return false;
      code_point = (code_point << 6U) | (byte & 0x3fU);
    }
    if ((continuation_count == 2 && code_point < 0x800U) ||
        (continuation_count == 3 && code_point < 0x10000U) ||
        (code_point >= 0xd800U && code_point <= 0xdfffU) ||
        code_point > 0x10ffffU) {
      return false;
    }
    index += continuation_count + 1;
  }
  return true;
}

[[nodiscard]] auto has_control_character(const std::string_view value) -> bool {
  return std::ranges::any_of(value, [](const char character) {
    const auto byte = static_cast<unsigned char>(character);
    return byte < 0x20U || byte == 0x7fU;
  });
}

// Bounded single-line text: no control characters, valid UTF-8, within budget.
[[nodiscard]] auto bounded_text(const std::string_view value,
                                const std::size_t maximum,
                                const bool allow_empty = false) -> bool {
  if (value.empty()) return allow_empty;
  return value.size() <= maximum && !has_control_character(value) &&
         valid_utf8(value);
}

// Identities name software, keys, and locators. They stay in a conservative
// ASCII set so a persisted document cannot smuggle structure or whitespace.
[[nodiscard]] auto valid_identity(const std::string_view value,
                                  const std::size_t maximum) -> bool {
  if (value.empty() || value.size() > maximum) return false;
  return std::ranges::all_of(value, [](const char character) {
    const auto byte = static_cast<unsigned char>(character);
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' ||
           byte == '-' || byte == '+' || byte == ':';
  });
}

// A credential locator may name a path segment, so '/' is additionally allowed.
// Whitespace and '=' remain excluded: an accidental secret is far likelier to
// contain them than a variable name or path is.
[[nodiscard]] auto valid_credential_identity(const std::string_view value,
                                             const std::size_t maximum)
    -> bool {
  if (value.empty() || value.size() > maximum) return false;
  return std::ranges::all_of(value, [](const char character) {
    const auto byte = static_cast<unsigned char>(character);
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' ||
           byte == '-' || byte == ':' || byte == '/';
  });
}

[[nodiscard]] auto valid_configuration_key(const std::string_view value,
                                           const std::size_t maximum) -> bool {
  if (value.empty() || value.size() > maximum) return false;
  return std::ranges::all_of(value, [](const char character) {
    const auto byte = static_cast<unsigned char>(character);
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' ||
           byte == '-';
  });
}

[[nodiscard]] auto validate_configuration(
    const std::vector<ConfigurationProvenanceEntry>& configuration,
    const RunProvenanceLimits& limits)
    -> std::expected<void, RunProvenanceError> {
  if (configuration.size() > limits.maximum_configuration_entries) {
    return failure(RunProvenanceErrorCode::too_many_entries,
                   "the configuration entry count exceeds its limit");
  }
  std::set<std::string_view> keys;
  for (const auto& entry : configuration) {
    if (!valid_configuration_key(entry.key, limits.maximum_key_bytes)) {
      return failure(RunProvenanceErrorCode::invalid_key,
                     "a configuration key is empty, oversized, or malformed");
    }
    if (!keys.insert(entry.key).second) {
      return failure(RunProvenanceErrorCode::duplicate_key,
                     "configuration key '" + entry.key + "' is duplicated");
    }
    // The load-bearing rule: a sensitive key contributes presence and source,
    // never its resolved value.
    if (entry.sensitive && entry.value) {
      return failure(RunProvenanceErrorCode::sensitive_value_recorded,
                     "configuration key '" + entry.key +
                         "' is sensitive and cannot record a value");
    }
    if (entry.value && !entry.value_present) {
      return failure(RunProvenanceErrorCode::invalid_key,
                     "configuration key '" + entry.key +
                         "' records a value while reporting none resolved");
    }
    if (entry.value) {
      if (entry.value->size() > limits.maximum_value_bytes) {
        return failure(RunProvenanceErrorCode::value_too_large,
                       "the value of configuration key '" + entry.key +
                           "' exceeds its byte limit");
      }
      if (!valid_utf8(*entry.value)) {
        return failure(RunProvenanceErrorCode::invalid_key,
                       "the value of configuration key '" + entry.key +
                           "' is not valid UTF-8");
      }
    }
    if (entry.decisions.size() > limits.maximum_decisions_per_entry) {
      return failure(RunProvenanceErrorCode::too_many_entries,
                     "configuration key '" + entry.key +
                         "' exceeds its decision limit");
    }
  }
  return {};
}

// clang-format off
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Explicitly validates every durable tool-provenance invariant.
[[nodiscard]] auto validate_tools(const std::vector<ToolProvenanceEntry>& tools,
                                  const RunProvenanceLimits& limits)
    -> std::expected<void, RunProvenanceError> {
  // clang-format on
  if (tools.size() > limits.maximum_tools) {
    return failure(RunProvenanceErrorCode::too_many_entries,
                   "the tool entry count exceeds its limit");
  }
  std::set<std::string_view> names;
  for (const auto& tool : tools) {
    if (!bounded_text(tool.tool_name, limits.maximum_identity_bytes)) {
      return failure(RunProvenanceErrorCode::invalid_tool,
                     "a tool name is empty, oversized, or malformed");
    }
    if (!names.insert(tool.tool_name).second) {
      return failure(RunProvenanceErrorCode::duplicate_tool,
                     "tool '" + tool.tool_name + "' is duplicated");
    }
    if (tool.registration_digest &&
        (tool.registration_digest->size() != 71 ||
         !tool.registration_digest->starts_with("sha256:") ||
         !std::ranges::all_of(tool.registration_digest->substr(7),
                              [](const unsigned char value) {
                                return (value >= '0' && value <= '9') ||
                                       (value >= 'a' && value <= 'f');
                              }))) {
      return failure(RunProvenanceErrorCode::invalid_tool,
                     "tool '" + tool.tool_name +
                         "' has an invalid registration digest");
    }
    if (tool.declared_effects.size() > limits.maximum_effects_per_tool) {
      return failure(RunProvenanceErrorCode::invalid_tool,
                     "tool '" + tool.tool_name +
                         "' exceeds its declared effect limit");
    }
    if (tool.capability_scopes.size() > limits.maximum_scopes_per_tool) {
      return failure(RunProvenanceErrorCode::invalid_tool,
                     "tool '" + tool.tool_name +
                         "' exceeds its capability scope limit");
    }
    for (const auto& scope : tool.capability_scopes) {
      if (!bounded_text(scope.kind, limits.maximum_identity_bytes) ||
          !bounded_text(scope.value, limits.maximum_value_bytes)) {
        return failure(RunProvenanceErrorCode::invalid_tool,
                       "tool '" + tool.tool_name +
                           "' declares a malformed scope");
      }
    }
  }
  return {};
}

[[nodiscard]] auto validate_tool_profile(
    const std::optional<ToolProfileProvenance>& profile,
    const RunProvenanceLimits& limits)
    -> std::expected<void, RunProvenanceError> {
  if (!profile) return {};
  const auto valid_profile_id = [&](const ToolProfileId& id) {
    const auto value = id.value();
    if (value.empty() ||
        value.size() >
            std::min(limits.maximum_identity_bytes, ToolProfileId::max_size) ||
        value.front() < 'a' || value.front() > 'z') {
      return false;
    }
    return std::ranges::all_of(value.substr(1), [](const char raw) {
      const auto character = static_cast<unsigned char>(raw);
      return (character >= 'a' && character <= 'z') ||
             (character >= '0' && character <= '9') || character == '-' ||
             character == '_';
    });
  };
  if (!valid_profile_id(profile->selected_profile_id) ||
      (profile->model_maximum_profile_id &&
       !valid_profile_id(*profile->model_maximum_profile_id)) ||
      (profile->persona_maximum_profile_id &&
       !valid_profile_id(*profile->persona_maximum_profile_id))) {
    return failure(
        RunProvenanceErrorCode::invalid_tool_profile,
        "a tool profile reference is empty, oversized, or malformed");
  }
  return {};
}

[[nodiscard]] auto valid_effect(const Effect effect) noexcept -> bool {
  switch (effect) {
    case Effect::read:
    case Effect::write:
    case Effect::remove:
    case Effect::execute:
    case Effect::network:
    case Effect::communicate:
    case Effect::spend:
    case Effect::change_infrastructure:
    case Effect::change_privileges: return true;
  }
  return false;
}

[[nodiscard]] auto effect_within_restriction(
    const Effect effect, const ToolRestrictionLevel restriction) noexcept
    -> bool {
  switch (restriction) {
    case ToolRestrictionLevel::high: return false;
    case ToolRestrictionLevel::medium: return effect == Effect::read;
    case ToolRestrictionLevel::low:
      return effect != Effect::change_infrastructure &&
             effect != Effect::change_privileges;
    case ToolRestrictionLevel::none: return true;
  }
  return false;
}

[[nodiscard]] auto valid_restriction(
    const ToolRestrictionLevel restriction) noexcept -> bool {
  switch (restriction) {
    case ToolRestrictionLevel::high:
    case ToolRestrictionLevel::medium:
    case ToolRestrictionLevel::low:
    case ToolRestrictionLevel::none: return true;
  }
  return false;
}

[[nodiscard]] auto valid_approval_mode(const ToolApprovalMode mode) noexcept
    -> bool {
  switch (mode) {
    case ToolApprovalMode::prompt:
    case ToolApprovalMode::automatic:
    case ToolApprovalMode::allow_all: return true;
  }
  return false;
}

[[nodiscard]] auto add_policy_bytes(const std::size_t amount,
                                    const std::size_t maximum,
                                    std::size_t& total) -> bool {
  if (amount > maximum - std::min(total, maximum)) return false;
  total += amount;
  return true;
}

[[nodiscard]] auto validate_policy_effects(const ToolPolicyProvenance& policy)
    -> std::expected<std::set<Effect>, RunProvenanceError> {
  std::set<Effect> effects;
  for (const auto effect : policy.effect_ceiling) {
    if (!valid_effect(effect) ||
        !effect_within_restriction(effect, policy.restriction_level) ||
        !effects.insert(effect).second) {
      return failure(RunProvenanceErrorCode::invalid_tool_policy,
                     "the tool policy effect ceiling is invalid");
    }
  }
  return effects;
}

[[nodiscard]] auto validate_policy_scopes(const ToolPolicyProvenance& policy,
                                          const RunProvenanceLimits& limits,
                                          const std::set<Effect>& effects,
                                          std::size_t& total)
    -> std::expected<void, RunProvenanceError> {
  std::vector<CapabilityScope> scopes;
  for (const auto& scope : policy.capability_ceiling) {
    if (!effects.contains(scope.effect) ||
        !bounded_text(scope.kind, limits.maximum_identity_bytes) ||
        !bounded_text(scope.value, limits.maximum_value_bytes) ||
        std::ranges::find(scopes, scope) != scopes.end()) {
      return failure(RunProvenanceErrorCode::invalid_tool_policy,
                     "the tool policy capability ceiling is invalid");
    }
    if (!add_policy_bytes(scope.kind.size(), limits.maximum_total_bytes,
                          total) ||
        !add_policy_bytes(scope.value.size(), limits.maximum_total_bytes,
                          total)) {
      return failure(
          RunProvenanceErrorCode::resource_exhausted,
          "the tool policy provenance exceeds its total byte budget");
    }
    scopes.push_back(scope);
  }
  if (std::ranges::any_of(effects, [&](const auto effect) {
        return std::ranges::none_of(scopes, [effect](const auto& scope) {
          return scope.effect == effect;
        });
      })) {
    return failure(
        RunProvenanceErrorCode::invalid_tool_policy,
        "every tool policy effect requires an explicit capability scope");
  }
  return {};
}

[[nodiscard]] auto validate_automatic_tools(const ToolPolicyProvenance& policy,
                                            const RunProvenanceLimits& limits,
                                            std::size_t& total)
    -> std::expected<void, RunProvenanceError> {
  std::set<std::string_view> automatic_tools;
  for (const auto& name : policy.automatically_eligible_tools) {
    if (!bounded_text(name, limits.maximum_identity_bytes) ||
        !automatic_tools.insert(name).second) {
      return failure(RunProvenanceErrorCode::invalid_tool_policy,
                     "the automatic tool policy list is invalid");
    }
    if (!add_policy_bytes(name.size(), limits.maximum_total_bytes, total)) {
      return failure(
          RunProvenanceErrorCode::resource_exhausted,
          "the tool policy provenance exceeds its total byte budget");
    }
  }
  return {};
}

[[nodiscard]] auto validate_tool_policy_impl(
    const std::optional<ToolPolicyProvenance>& policy,
    const RunProvenanceLimits& limits)
    -> std::expected<void, RunProvenanceError> {
  if (!policy) return {};
  if (policy->identity != "aiforge.tool-launch-policy.v1" ||
      !valid_identity(policy->permission_profile_id.value(),
                      limits.maximum_identity_bytes) ||
      !valid_restriction(policy->restriction_level) ||
      !valid_approval_mode(policy->approval_mode) ||
      policy->effect_ceiling.size() > limits.maximum_policy_effects ||
      policy->capability_ceiling.size() > limits.maximum_policy_scopes ||
      policy->automatically_eligible_tools.size() >
          limits.maximum_automatic_tools) {
    return failure(RunProvenanceErrorCode::invalid_tool_policy,
                   "the tool policy provenance is malformed or unbounded");
  }
  if (policy->approval_mode != ToolApprovalMode::automatic &&
      !policy->automatically_eligible_tools.empty()) {
    return failure(RunProvenanceErrorCode::invalid_tool_policy,
                   "automatic tool eligibility conflicts with approval mode");
  }
  std::size_t total{};
  if (!add_policy_bytes(policy->identity.size(), limits.maximum_total_bytes,
                        total) ||
      !add_policy_bytes(policy->permission_profile_id.value().size(),
                        limits.maximum_total_bytes, total)) {
    return failure(RunProvenanceErrorCode::resource_exhausted,
                   "the tool policy provenance exceeds its total byte budget");
  }
  auto effects = validate_policy_effects(*policy);
  if (!effects) return std::unexpected(effects.error());
  if (auto scopes = validate_policy_scopes(*policy, limits, *effects, total);
      !scopes) {
    return scopes;
  }
  return validate_automatic_tools(*policy, limits, total);
}

[[nodiscard]] auto valid_request_option_source(
    const RequestOptionSource source) noexcept -> bool {
  switch (source) {
    case RequestOptionSource::provider_default:
    case RequestOptionSource::configuration:
    case RequestOptionSource::session_override: return true;
  }
  return false;
}

[[nodiscard]] auto validate_request_options(
    const std::vector<EffectiveRequestOption>& request_options,
    const RunProvenanceLimits& limits)
    -> std::expected<void, RunProvenanceError> {
  if (request_options.size() > limits.maximum_request_options) {
    return failure(RunProvenanceErrorCode::too_many_entries,
                   "the request option entry count exceeds its limit");
  }
  std::set<std::string_view> keys;
  for (const auto& entry : request_options) {
    if (!valid_configuration_key(entry.key,
                                 limits.maximum_request_option_key_bytes)) {
      return failure(RunProvenanceErrorCode::invalid_request_option,
                     "a request option key is empty, oversized, or malformed");
    }
    if (!keys.insert(entry.key).second) {
      return failure(RunProvenanceErrorCode::duplicate_key,
                     "request option key '" + entry.key + "' is duplicated");
    }
    if (!valid_request_option_source(entry.source)) {
      return failure(RunProvenanceErrorCode::invalid_request_option,
                     "request option '" + entry.key +
                         "' has an unknown source");
    }
    const bool provider_default =
        entry.source == RequestOptionSource::provider_default;
    if (provider_default == entry.value.has_value()) {
      return failure(RunProvenanceErrorCode::invalid_request_option,
                     "request option '" + entry.key +
                         "' has a value inconsistent with its source");
    }
    if (entry.value &&
        entry.value->size() > limits.maximum_request_option_value_bytes) {
      return failure(RunProvenanceErrorCode::value_too_large,
                     "request option '" + entry.key +
                         "' has an oversized value");
    }
    if (entry.value &&
        !bounded_text(*entry.value, limits.maximum_request_option_value_bytes,
                      true)) {
      return failure(RunProvenanceErrorCode::invalid_request_option,
                     "request option '" + entry.key +
                         "' has a malformed value");
    }
  }
  return {};
}

[[nodiscard]] auto configuration_bytes(
    const std::vector<ConfigurationProvenanceEntry>& configuration)
    -> std::size_t {
  std::size_t total{};
  for (const auto& entry : configuration) {
    total += entry.key.size();
    if (entry.value) total += entry.value->size();
  }
  return total;
}

[[nodiscard]] auto component_bytes(
    const std::vector<RuntimeComponentVersion>& components) -> std::size_t {
  std::size_t total{};
  for (const auto& component : components) {
    total += component.component.size() + component.version.size();
  }
  return total;
}

[[nodiscard]] auto tool_bytes(const std::vector<ToolProvenanceEntry>& tools)
    -> std::size_t {
  std::size_t total{};
  for (const auto& tool : tools) {
    total += tool.tool_name.size();
    if (tool.registration_digest) total += tool.registration_digest->size();
    for (const auto& scope : tool.capability_scopes) {
      total += scope.kind.size() + scope.value.size();
    }
  }
  return total;
}

[[nodiscard]] auto request_option_bytes(
    const std::vector<EffectiveRequestOption>& options) -> std::size_t {
  std::size_t total{};
  for (const auto& option : options) {
    total += option.key.size();
    if (option.value) total += option.value->size();
  }
  return total;
}

[[nodiscard]] auto tool_profile_bytes(
    const std::optional<ToolProfileProvenance>& profile) -> std::size_t {
  if (!profile) return 0;
  std::size_t total = profile->selected_profile_id.value().size();
  if (profile->model_maximum_profile_id) {
    total += profile->model_maximum_profile_id->value().size();
  }
  if (profile->persona_maximum_profile_id) {
    total += profile->persona_maximum_profile_id->value().size();
  }
  return total;
}

[[nodiscard]] auto tool_policy_bytes(
    const std::optional<ToolPolicyProvenance>& policy) -> std::size_t {
  if (!policy) return 0;
  std::size_t total =
      policy->identity.size() + policy->permission_profile_id.value().size();
  for (const auto& scope : policy->capability_ceiling) {
    total += scope.kind.size() + scope.value.size();
  }
  for (const auto& name : policy->automatically_eligible_tools) {
    total += name.size();
  }
  return total;
}

[[nodiscard]] auto total_bytes(const RunProvenance& provenance) -> std::size_t {
  std::size_t total = provenance.aiforge_version.size() +
                      provenance.backend_id.size() +
                      provenance.model_id.value().size();
  if (provenance.backend_version) total += provenance.backend_version->size();
  if (provenance.credential_source) {
    total += provenance.credential_source->identity.size();
  }
  total += configuration_bytes(provenance.configuration);
  total += component_bytes(provenance.components);
  total += tool_bytes(provenance.tools);
  total += request_option_bytes(provenance.effective_request_options);
  total += tool_profile_bytes(provenance.tool_profile);
  total += tool_policy_bytes(provenance.tool_policy);
  return total;
}

} // namespace

auto validate_tool_policy_provenance(const ToolPolicyProvenance& provenance,
                                     const RunProvenanceLimits limits)
    -> std::expected<void, RunProvenanceError> {
  if (limits.maximum_identity_bytes == 0 || limits.maximum_value_bytes == 0 ||
      limits.maximum_total_bytes == 0 || limits.maximum_policy_effects == 0 ||
      limits.maximum_policy_scopes == 0 ||
      limits.maximum_automatic_tools == 0) {
    return failure(RunProvenanceErrorCode::invalid_limits,
                   "a tool policy provenance limit is zero");
  }
  return validate_tool_policy_impl(
      std::optional<ToolPolicyProvenance>{provenance}, limits);
}

auto validate_run_provenance(const RunProvenance& provenance,
                             const RunProvenanceLimits limits)
    -> std::expected<void, RunProvenanceError> {
  if (limits.maximum_configuration_entries == 0 ||
      limits.maximum_decisions_per_entry == 0 ||
      limits.maximum_key_bytes == 0 || limits.maximum_value_bytes == 0 ||
      limits.maximum_identity_bytes == 0 || limits.maximum_components == 0 ||
      limits.maximum_tools == 0 || limits.maximum_effects_per_tool == 0 ||
      limits.maximum_scopes_per_tool == 0 ||
      limits.maximum_request_options == 0 ||
      limits.maximum_request_option_key_bytes == 0 ||
      limits.maximum_request_option_value_bytes == 0 ||
      limits.maximum_total_bytes == 0 || limits.maximum_policy_effects == 0 ||
      limits.maximum_policy_scopes == 0 ||
      limits.maximum_automatic_tools == 0) {
    return failure(RunProvenanceErrorCode::invalid_limits,
                   "a provenance limit is zero");
  }
  if (!valid_identity(provenance.aiforge_version,
                      limits.maximum_identity_bytes)) {
    return failure(RunProvenanceErrorCode::invalid_identity,
                   "the aiforge version is empty or malformed");
  }
  if (!valid_identity(provenance.backend_id, limits.maximum_identity_bytes)) {
    return failure(RunProvenanceErrorCode::invalid_identity,
                   "the backend identity is empty or malformed");
  }
  if (provenance.backend_version &&
      !valid_identity(*provenance.backend_version,
                      limits.maximum_identity_bytes)) {
    return failure(RunProvenanceErrorCode::invalid_identity,
                   "the backend version is empty or malformed");
  }
  if (provenance.credential_source &&
      !valid_credential_identity(provenance.credential_source->identity,
                                 limits.maximum_identity_bytes)) {
    return failure(RunProvenanceErrorCode::invalid_credential_source,
                   "the credential source identity is empty or malformed");
  }
  if (auto configuration =
          validate_configuration(provenance.configuration, limits);
      !configuration) {
    return configuration;
  }
  if (provenance.components.size() > limits.maximum_components) {
    return failure(RunProvenanceErrorCode::too_many_entries,
                   "the runtime component count exceeds its limit");
  }
  for (const auto& component : provenance.components) {
    if (!valid_identity(component.component, limits.maximum_identity_bytes) ||
        !valid_identity(component.version, limits.maximum_identity_bytes)) {
      return failure(RunProvenanceErrorCode::invalid_component,
                     "a runtime component name or version is malformed");
    }
  }
  if (auto tools = validate_tools(provenance.tools, limits); !tools) {
    return tools;
  }
  if (auto profile = validate_tool_profile(provenance.tool_profile, limits);
      !profile) {
    return profile;
  }
  if (auto policy = validate_tool_policy_impl(provenance.tool_policy, limits);
      !policy) {
    return policy;
  }
  if (auto options = validate_request_options(
          provenance.effective_request_options, limits);
      !options) {
    return options;
  }
  if (total_bytes(provenance) > limits.maximum_total_bytes) {
    return failure(RunProvenanceErrorCode::resource_exhausted,
                   "the provenance record exceeds its total byte budget");
  }
  return {};
}

} // namespace aiforge::domain
