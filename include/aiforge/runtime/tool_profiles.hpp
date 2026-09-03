#pragma once

#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <aiforge/domain/ids.hpp>
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

struct ToolProfileResolution {
  ToolProfile selected_profile;
  ToolRegistrySnapshot effective_tools;
  std::vector<ToolProfileToolAvailability> tool_availability;
};

[[nodiscard]] auto validate_tool_profiles(std::span<const ToolProfile> profiles,
                                          ToolProfileLimits limits = {})
    -> std::expected<void, ToolProfileError>;

[[nodiscard]] auto builtin_tool_profiles() noexcept
    -> std::span<const ToolProfile>;

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
