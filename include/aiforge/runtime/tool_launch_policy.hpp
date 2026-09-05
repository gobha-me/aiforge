#pragma once

#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <aiforge/runtime/application_launch_context.hpp>
#include <aiforge/runtime/tool_policy.hpp>
#include <aiforge/runtime/tool_registry.hpp>

namespace aiforge::runtime {

struct ToolLaunchPolicyConfiguration {
  domain::PermissionProfileId permission_profile_id;
  ApplicationLaunchContext launch_context;
  // Used only by `automatic`. Names must belong to the exact registry snapshot
  // captured by the resulting policy. Only the matcher-policy identity enters
  // durable provenance; this raw configuration remains launch-lifetime state.
  std::vector<std::string> automatically_eligible_tools;
  auto operator==(const ToolLaunchPolicyConfiguration&) const -> bool = default;
};

// Returns a digest identity for the canonical exact-name allow-list. The
// names themselves remain launch-lifetime configuration and are not durable.
[[nodiscard]] auto exact_tool_allowlist_matcher_identity(
    std::span<const std::string> tool_names)
    -> std::expected<std::string, ToolPolicyError>;

// Builds a policy bound to an exact registry snapshot. Registered declaration
// effects and scopes remain the ceiling even when a restriction/mode would
// otherwise allow more authority.
[[nodiscard]] auto make_tool_launch_policy(
    const ToolRegistrySnapshot& registered_tools,
    ToolLaunchPolicyConfiguration configuration)
    -> std::expected<std::shared_ptr<ToolPolicy>, ToolPolicyError>;

} // namespace aiforge::runtime
