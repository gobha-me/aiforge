#pragma once

#include <expected>
#include <memory>
#include <string>
#include <vector>

#include <aiforge/runtime/tool_policy.hpp>
#include <aiforge/runtime/tool_registry.hpp>

namespace aiforge::runtime {

// Restriction names describe how much authority remains available. `high` is
// therefore the most restrictive setting and `none` the least restrictive.
enum class RestrictionLevel {
  high,
  medium,
  low,
  none,
};

enum class ApprovalMode {
  prompt,
  automatic,
  allow_all,
};

struct ToolLaunchPolicyConfiguration {
  domain::PermissionProfileId permission_profile_id;
  RestrictionLevel restriction_level{RestrictionLevel::high};
  ApprovalMode approval_mode{ApprovalMode::prompt};
  // Used only by `automatic`. Names must belong to the exact registry snapshot
  // captured by the resulting policy.
  std::vector<std::string> automatically_eligible_tools;
  auto operator==(const ToolLaunchPolicyConfiguration&) const -> bool = default;
};

// Builds a policy bound to an exact registry snapshot. Registered declaration
// effects and scopes remain the ceiling even when a restriction/mode would
// otherwise allow more authority.
[[nodiscard]] auto make_tool_launch_policy(
    const ToolRegistrySnapshot& registered_tools,
    ToolLaunchPolicyConfiguration configuration)
    -> std::expected<std::shared_ptr<ToolPolicy>, ToolPolicyError>;

} // namespace aiforge::runtime
