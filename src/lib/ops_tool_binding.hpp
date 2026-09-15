#pragma once

#include <aiforge/runtime/tool_policy.hpp>
#include <aiforge/runtime/tool_registry.hpp>

namespace aiforge::runtime::ops_binding_detail {

// Private preparation only. The replacement must be the exact native
// registration; unrelated entries in each input snapshot remain unchanged.
[[nodiscard]] auto replace_ops_registration(const ToolRegistrySnapshot& current,
                                            const RegisteredTool& replacement)
    -> std::expected<ToolRegistrySnapshot, ToolRegistryError>;

// Recognizes the genuine private LaunchPolicy and preserves its own unrelated
// ceilings, configuration and shared matcher. No neutral provenance recovery.
[[nodiscard]] auto rebind_ops_launch_policy(const ToolPolicy& current,
                                            const RegisteredTool& replacement)
    -> std::expected<std::shared_ptr<ToolPolicy>, ToolPolicyError>;

} // namespace aiforge::runtime::ops_binding_detail
