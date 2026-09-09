#pragma once

#include <aiforge/runtime/tool_registry.hpp>

namespace aiforge::runtime {

// Human and model intent use the same closed operation vocabulary. Runtime
// preparation supplies owner/session/configuration/log-policy/request identity
// from the frozen registration and the kernel-assigned invocation.
struct OpsObservationIntent {
  domain::OpsTargetId target_id;
  std::uint64_t selection_generation;
  domain::OpsObservationOperation operation;
  domain::OpsResourceIdentity resource{};
  domain::OpsObservationLimits limits{};
  auto operator==(const OpsObservationIntent&) const -> bool = default;
};

// The final native type is the kernel's Observe execution contract. Category,
// declared tool name, or a version string alone never enables manual dispatch
// or observation receipt publication for an ordinary executor.
class OpsObservationTool final : public ToolExecutor {
 public:
  [[nodiscard]] auto validate(
      const domain::StructuredDataBlock& arguments) const
      -> std::expected<ValidatedToolArguments, ToolExecutionError> override;
  [[nodiscard]] auto prepare(const domain::InvocationId& invocation,
                             const domain::StructuredDataBlock& arguments) const
      -> std::expected<ValidatedToolArguments, ToolExecutionError> override;
  [[nodiscard]] auto prepare(const domain::InvocationId& invocation,
                             const OpsObservationIntent& intent) const
      -> std::expected<ValidatedToolArguments, ToolExecutionError>;
  // Owner-thread only, no IO. Binds the frozen endpoint to the kernel's broker
  // and rechecks exact normalized/invocation proof against current authority.
  [[nodiscard]] auto check_current(const OpsObservationBroker& broker,
                                   const domain::InvocationId& invocation,
                                   const ValidatedToolArguments& arguments)
      const -> std::expected<void, ToolExecutionError>;
  [[nodiscard]] auto start(ToolInvocation invocation, std::stop_token stop)
      -> std::expected<std::unique_ptr<ToolExecutionStream>,
                       ToolExecutionError> override;

 private:
  friend auto register_ops_observation_tool(
      ToolRegistry&, domain::OpsObservationAuthority,
      std::shared_ptr<OpsObservationEndpoint>)
      -> std::expected<void, ToolRegistryError>;
  OpsObservationTool(domain::OpsObservationAuthority authority,
                     std::shared_ptr<OpsObservationEndpoint> endpoint);
  domain::OpsObservationAuthority m_authority;
  std::shared_ptr<OpsObservationEndpoint> m_endpoint;
};

// One immutable registration per current target selection. Existing active
// runs retain their old registration; the broker invalidates its authority
// when selection changes. Install a replacement only when the kernel is idle.
[[nodiscard]] auto register_ops_observation_tool(
    ToolRegistry& registry, domain::OpsObservationAuthority authority,
    std::shared_ptr<OpsObservationEndpoint> endpoint)
    -> std::expected<void, ToolRegistryError>;

} // namespace aiforge::runtime
