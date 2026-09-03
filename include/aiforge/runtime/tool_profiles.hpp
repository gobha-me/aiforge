#pragma once

#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <aiforge/domain/ids.hpp>
#include <aiforge/runtime/tool_policy.hpp>
#include <aiforge/runtime/tool_registry.hpp>

namespace aiforge::runtime {

struct ToolProfile {
  domain::ToolProfileId profile_id;
  std::string name;
  std::vector<std::string> tool_names;
  auto operator==(const ToolProfile&) const -> bool = default;
};

struct ToolProfileLimits {
  std::size_t maximum_profiles{32};
  std::size_t maximum_profile_id_bytes{64};
  std::size_t maximum_profile_name_bytes{128};
  std::size_t maximum_tools_per_profile{256};
  std::size_t maximum_tool_name_bytes{128};
  std::size_t maximum_total_bytes{std::size_t{64} * 1024U};
  auto operator==(const ToolProfileLimits&) const -> bool = default;
};

enum class ToolProfileErrorCode {
  invalid_limits,
  invalid_profile,
  duplicate_profile,
  duplicate_tool,
  unknown_profile,
  resource_exhausted,
  internal_failure,
};

struct ToolProfileError {
  ToolProfileErrorCode code{ToolProfileErrorCode::internal_failure};
  std::string message;
  auto operator==(const ToolProfileError&) const -> bool = default;
};

enum class ToolProfileAvailabilityReason {
  available,
  tool_not_registered,
  profile_contract_mismatch,
  session_tool_disabled,
  model_profile_limit,
  persona_profile_limit,
  launch_policy_denied,
  model_tool_calling_unsupported,
  model_tool_calling_unknown,
};

[[nodiscard]] auto tool_profile_availability_reason_text(
    ToolProfileAvailabilityReason reason) noexcept -> std::string_view;

struct ToolProfileToolAvailability {
  std::string tool_name;
  ToolProfileAvailabilityReason reason{
      ToolProfileAvailabilityReason::tool_not_registered};
  auto operator==(const ToolProfileToolAvailability&) const -> bool = default;
};

struct ToolProfileSelection {
  domain::ToolProfileId selected_profile_id;
  // Absence preserves the complete selected-profile membership. Presence is
  // an exact, possibly empty, session-local narrowing subset.
  std::optional<std::vector<std::string>> desired_tool_names;
  std::optional<domain::ToolProfileId> model_maximum_profile_id;
  std::optional<domain::ToolProfileId> persona_maximum_profile_id;
  std::optional<bool> model_tool_calling_support;
  auto operator==(const ToolProfileSelection&) const -> bool = default;
};

struct ToolProfileResolution {
  ToolProfile selected_profile;
  ToolRegistrySnapshot effective_tools;
  std::vector<ToolProfileToolAvailability> tool_availability;
  ToolProfileSelection selection;
};

[[nodiscard]] auto validate_tool_profiles(std::span<const ToolProfile> profiles,
                                          ToolProfileLimits limits = {})
    -> std::expected<void, ToolProfileError>;

[[nodiscard]] auto builtin_tool_profiles() noexcept
    -> std::span<const ToolProfile>;

// Expands one category to exact names already contained by the selected
// built-in profile. Registration metadata supplies grouping only and never
// grants profile membership or authority.
[[nodiscard]] auto tool_profile_category_members(
    const ToolRegistrySnapshot& full_registry,
    const domain::ToolProfileId& selected_profile_id, ToolCategory category,
    ToolProfileLimits limits = {})
    -> std::expected<std::vector<std::string>, ToolProfileError>;

// Resolves every current narrowing dimension. The launch policy is observed
// through bounded neutral provenance only; resolution never evaluates an
// invocation, requests approval, or mutates policy state.
[[nodiscard]] auto resolve_tool_profile(
    const ToolRegistrySnapshot& full_registry, ToolProfileSelection selection,
    const ToolPolicy& launch_policy, ToolProfileLimits limits = {})
    -> std::expected<ToolProfileResolution, ToolProfileError>;

// `model_tool_calling_support` must be explicitly true before any selected
// declaration becomes effective. A false or unknown value retains the desired
// profile and reports why each registered member is unavailable.
[[nodiscard]] auto resolve_tool_profile(
    const ToolRegistrySnapshot& full_registry,
    const domain::ToolProfileId& selected_profile_id,
    std::optional<bool> model_tool_calling_support,
    std::span<const ToolProfile> profiles = builtin_tool_profiles(),
    ToolProfileLimits limits = {})
    -> std::expected<ToolProfileResolution, ToolProfileError>;

} // namespace aiforge::runtime
