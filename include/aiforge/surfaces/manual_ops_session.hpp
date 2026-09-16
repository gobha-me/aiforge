#pragma once

#include <aiforge/runtime/run_kernel.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <stop_token>

namespace aiforge::surfaces {
enum class ManualOpsErrorCode {
  invalid_input,
  busy,
  unavailable,
  closed,
  wrong_operation,
  cancelled,
  storage_failure,
  invalid_history,
  operation_failed,
  resource_exhausted,
  internal_failure
};
struct ManualOpsFailure {
  ManualOpsErrorCode code{ManualOpsErrorCode::internal_failure};
  std::optional<runtime::RunKernelErrorCode> kernel{};
  std::optional<runtime::OpsBrokerError> broker{};
  std::optional<runtime::OpsObservationSourceError> source{};
  auto operator==(const ManualOpsFailure&) const -> bool = default;
};
enum class ManualOpsSourceConnection {
  unbound,
  historical_unverified,
  connected,
  disconnected,
};
struct ObservationSubmission {
  domain::RunId run_id;
  domain::InvocationId invocation_id;
  auto operator==(const ObservationSubmission&) const -> bool = default;
};
struct ManualObservationProgress {
  ObservationSubmission submission;
  domain::RunStatus status{domain::RunStatus::not_started};
  std::optional<domain::ErrorCode> failure{};
  // Present only after exact recorded observation/result and RunCompleted
  // proof.
  std::optional<domain::EventId> observation_event_id{};
  auto operator==(const ManualObservationProgress&) const -> bool = default;
};
struct CommittedOpsObservation {
  ObservationSubmission submission;
  domain::EventId observation_event_id;
  domain::EventId result_event_id;
  domain::OpsObservation observation;
  auto operator==(const CommittedOpsObservation&) const -> bool = default;
};
inline constexpr std::size_t manual_ops_catalog_slots = 8;
inline constexpr std::size_t maximum_manual_ops_catalog_bytes =
    std::size_t{512} * 1024U;
static_assert(maximum_manual_ops_catalog_bytes ==
              manual_ops_catalog_slots *
                  domain::ops_observation_maximum_evidence_bytes);
[[nodiscard]] constexpr auto manual_ops_catalog_slot(
    domain::OpsObservationOperation operation) -> std::size_t {
  switch (operation) {
    case domain::OpsObservationOperation::linux_health: return 0;
    case domain::OpsObservationOperation::linux_services: return 1;
    case domain::OpsObservationOperation::linux_service_health: return 2;
    case domain::OpsObservationOperation::kubernetes_workloads: return 3;
    case domain::OpsObservationOperation::kubernetes_pod_health: return 4;
    case domain::OpsObservationOperation::kubernetes_events: return 5;
    case domain::OpsObservationOperation::linux_service_logs: return 6;
    case domain::OpsObservationOperation::kubernetes_pod_logs: return 7;
  }
  return manual_ops_catalog_slots;
}
struct ManualObservationProjection {
  std::uint64_t last_sequence{};
  std::optional<ManualObservationProgress> current{};
  std::optional<CommittedOpsObservation> latest_success{};
  // Final latest successful terminal evidence only. Slot order is fixed by
  // manual_ops_catalog_slot; total neutral evidence is capped at 512 KiB.
  std::array<std::optional<CommittedOpsObservation>, manual_ops_catalog_slots>
      catalog{};
  std::optional<runtime::RecordedOpsTargetSelection> historical_selection{};
  std::uint64_t maximum_selection_generation{};
  auto operator==(const ManualObservationProjection&) const -> bool = default;
};
// Pure, bounded committed-history projection shared by standalone and chat.
// No current authority, source collection, persistence or provider work.
[[nodiscard]] auto project_manual_observations(
    const domain::SessionEventLog& log,
    const std::optional<ObservationSubmission>& current = {},
    std::stop_token stop = {})
    -> std::expected<ManualObservationProjection, ManualOpsFailure>;

struct ManualOpsInspection {
  ManualObservationProjection projection;
  std::optional<runtime::PendingToolApproval> approval{};
  std::optional<domain::OpsTargetBinding> selection{};
  std::uint64_t selection_generation{};
  std::optional<ManualOpsFailure> problem{};
  ManualOpsSourceConnection source_connection{
      ManualOpsSourceConnection::unbound};
  bool busy{};
  bool available{};
  bool closed{};
};
// Borrowed by controllers/widgets on the session owner thread. Inspection is
// cached and performs no IO; its reference expires at the next owner mutation.
class ManualOpsSession {
 public:
  virtual ~ManualOpsSession() = default;
  [[nodiscard]] virtual auto submit_observation(
      runtime::OpsObservationIntent intent)
      -> std::expected<ObservationSubmission, ManualOpsFailure> = 0;
  [[nodiscard]] virtual auto cancel_observation(const domain::RunId& run_id)
      -> std::expected<void, ManualOpsFailure> = 0;
  [[nodiscard]] virtual auto decide_observation_approval(
      const domain::RunId& run_id, const domain::InvocationId& invocation_id,
      runtime::ToolApprovalResolution decision)
      -> std::expected<void, ManualOpsFailure> = 0;
  [[nodiscard]] virtual auto pump_observations()
      -> std::expected<void, ManualOpsFailure> = 0;
  [[nodiscard]] virtual auto inspect_observations() const noexcept
      -> const ManualOpsInspection& = 0;
};
} // namespace aiforge::surfaces
