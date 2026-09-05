#pragma once

#include <expected>
#include <memory>
#include <string>

#include <aiforge/runtime/application_launch_context.hpp>
#include <aiforge/runtime/tool_policy.hpp>
#include <aiforge/runtime/tool_registry.hpp>

namespace aiforge::runtime {

struct ToolLaunchPolicyConfiguration {
  domain::PermissionProfileId permission_profile_id;
  ApplicationLaunchContext launch_context;
  // Required only by `automatic`. The compiled rules and their counters remain
  // application-lifetime state; only safe matcher/rule identities are durable.
  std::shared_ptr<AutomaticApprovalMatcher> automatic_matcher;
};

// Builds a policy bound to an exact registry snapshot. Registered declaration
// effects and scopes remain the ceiling even when a restriction/mode would
// otherwise allow more authority.
[[nodiscard]] auto make_tool_launch_policy(
    const ToolRegistrySnapshot& registered_tools,
    ToolLaunchPolicyConfiguration configuration)
    -> std::expected<std::shared_ptr<ToolPolicy>, ToolPolicyError>;

} // namespace aiforge::runtime
