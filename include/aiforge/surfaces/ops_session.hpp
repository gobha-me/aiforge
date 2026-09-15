#pragma once

#include <aiforge/runtime/tool_launch_policy.hpp>
#include <aiforge/surfaces/manual_ops_session.hpp>
#include <functional>
#include <memory>

namespace aiforge::surfaces {
struct OpsSessionOpen {
  domain::SessionId session_id;
  domain::EventTimestamp created_at;
  domain::SurfaceId surface_id;
  domain::WorkspaceId workspace_id;
};
using OpsIdentitySuffixSource = std::function<std::uint64_t()>;
struct OpsSessionDependencies {
  runtime::ToolLaunchPolicyConfiguration launch_policy;
  std::shared_ptr<runtime::OpsObservationBroker> broker;
  OpsIdentitySuffixSource identity_suffix_source;
  runtime::TimestampSource timestamp_source{};
  runtime::RunKernelLimits run_limits{};
  runtime::RunWakeSink* wake_sink{};
};
// Owns exactly one fresh durable kernel. No resume/attach/second-writer lease.
// The application owns activation/shutdown and keeps store, wake sink and its
// shared broker/source graph alive past the session. No public provider port.
class OpsSession final : public ManualOpsSession {
 public:
  [[nodiscard]] static auto open(OpsSessionOpen request,
                                 storage::SessionStore& store,
                                 OpsSessionDependencies dependencies)
      -> std::expected<std::unique_ptr<OpsSession>, ManualOpsFailure>;
  ~OpsSession() override;
  OpsSession(const OpsSession&) = delete;
  auto operator=(const OpsSession&) -> OpsSession& = delete;
  // Application owner-only selection, deliberately absent from the widget port.
  [[nodiscard]] auto bind_observation(
      domain::OpsObservationAuthority authority,
      std::shared_ptr<runtime::OpsObservationSource> source,
      std::shared_ptr<runtime::OpsObservationEndpoint> endpoint)
      -> std::expected<void, ManualOpsFailure>;
  [[nodiscard]] auto submit_observation(runtime::OpsObservationIntent intent)
      -> std::expected<ObservationSubmission, ManualOpsFailure> override;
  [[nodiscard]] auto cancel_observation(const domain::RunId& run_id)
      -> std::expected<void, ManualOpsFailure> override;
  [[nodiscard]] auto decide_observation_approval(
      const domain::RunId& run_id, const domain::InvocationId& invocation_id,
      runtime::ToolApprovalResolution decision)
      -> std::expected<void, ManualOpsFailure> override;
  [[nodiscard]] auto pump_observations()
      -> std::expected<void, ManualOpsFailure> override;
  [[nodiscard]] auto inspect_observations() const noexcept
      -> const ManualOpsInspection& override;
  // Stops admission; preserves cancellation where storage permits. Subsequent
  // pumping may retire ToolEnded. Never deactivates a replacement broker
  // issuer.
  [[nodiscard]] auto close() -> std::expected<void, ManualOpsFailure>;

 private:
  struct Impl;
  explicit OpsSession(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> m_impl;
};
} // namespace aiforge::surfaces
