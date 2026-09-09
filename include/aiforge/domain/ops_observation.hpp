#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <aiforge/domain/events_fwd.hpp>
#include <aiforge/domain/ops_target.hpp>

namespace aiforge::domain {

inline constexpr std::size_t ops_observation_maximum_evidence_bytes =
    std::size_t{64} * 1024;

enum class OpsObservationCompleteness { complete, partial, truncated };
enum class OpsHealthState {
  unknown,
  healthy,
  degraded,
  unhealthy,
  unavailable
};
enum class OpsServiceState {
  unknown,
  active,
  inactive,
  activating,
  deactivating,
  failed
};
enum class OpsPodPhase { unknown, pending, running, succeeded, failed };
enum class OpsContainerState { unknown, waiting, running, terminated };
enum class OpsReadiness { unknown, not_ready, ready };
enum class OpsEventSeverity { unknown, normal, warning };
enum class OpsObservationReason {
  unknown,
  none,
  not_ready,
  crash_loop,
  image_pull_failed,
  out_of_memory,
  probe_failed,
  scheduling_failed,
  failed_exit,
  completed,
  unavailable
};
enum class OpsWorkloadKind {
  pod,
  deployment,
  stateful_set,
  daemon_set,
  job,
  cron_job
};

enum class OpsMemoryScope { unknown, kernel, cgroup };
struct LinuxMemoryObservation {
  OpsMemoryScope scope{OpsMemoryScope::unknown};
  std::uint64_t total_bytes{};
  std::uint64_t available_bytes{};
  auto operator==(const LinuxMemoryObservation&) const -> bool = default;
};
struct LinuxHealthObservation {
  OpsHealthState health{OpsHealthState::unknown};
  // Kernel uptime, not container/service age. Memory scope is independent of
  // the execution namespace and cannot be inferred from target identity.
  std::optional<std::uint64_t> kernel_uptime_seconds{};
  std::optional<LinuxMemoryObservation> memory{};
  std::optional<std::uint32_t> active_services{};
  std::optional<std::uint32_t> failed_services{};
  auto operator==(const LinuxHealthObservation&) const -> bool = default;
};
struct LinuxServiceObservation {
  LinuxServiceIdentity identity;
  OpsServiceState state{OpsServiceState::unknown};
  OpsObservationReason reason{OpsObservationReason::unknown};
  std::optional<std::int32_t> exit_status{};
  std::optional<std::uint32_t> restart_count{};
  auto operator==(const LinuxServiceObservation&) const -> bool = default;
};
struct LinuxServicesObservation {
  std::vector<LinuxServiceObservation> services;
  auto operator==(const LinuxServicesObservation&) const -> bool = default;
};

struct KubernetesObservedResource {
  OpsWorkloadKind kind{OpsWorkloadKind::pod};
  std::string namespace_name;
  std::string name;
  OpsResourceUid uid;
  auto operator==(const KubernetesObservedResource&) const -> bool = default;
};
struct KubernetesWorkloadObservation {
  KubernetesObservedResource identity;
  OpsHealthState health{OpsHealthState::unknown};
  // Absent counts are unknown/not applicable, never inferred as zero.
  std::optional<std::uint32_t> desired_count{};
  std::optional<std::uint32_t> ready_count{};
  // Rollout surge may make ready exceed desired; it cannot exceed observed.
  std::optional<std::uint32_t> observed_count{};
  auto operator==(const KubernetesWorkloadObservation&) const -> bool = default;
};
struct KubernetesWorkloadsObservation {
  std::vector<KubernetesWorkloadObservation> workloads;
  auto operator==(const KubernetesWorkloadsObservation&) const
      -> bool = default;
};
struct KubernetesContainerObservation {
  std::string name;
  // Waiting/not-yet-created containers may lack a runtime identity.
  std::optional<std::string> runtime_identity{};
  OpsContainerState state{OpsContainerState::unknown};
  OpsReadiness readiness{OpsReadiness::unknown};
  OpsObservationReason reason{OpsObservationReason::unknown};
  std::optional<std::uint32_t> restart_count{};
  std::optional<std::int32_t> exit_status{};
  auto operator==(const KubernetesContainerObservation&) const
      -> bool = default;
};
struct KubernetesPodObservation {
  KubernetesPodIdentity identity;
  OpsPodPhase phase{OpsPodPhase::unknown};
  std::vector<KubernetesContainerObservation> containers;
  auto operator==(const KubernetesPodObservation&) const -> bool = default;
};
struct KubernetesEventObservation {
  OpsResourceUid event_uid;
  KubernetesObservedResource regarding;
  OpsEventSeverity severity{OpsEventSeverity::unknown};
  OpsObservationReason reason{OpsObservationReason::unknown};
  std::optional<EventTimestamp> first_observed_at{};
  std::optional<EventTimestamp> last_observed_at{};
  std::uint64_t occurrences{1};
  auto operator==(const KubernetesEventObservation&) const -> bool = default;
};
struct KubernetesEventsObservation {
  std::vector<KubernetesEventObservation> events;
  auto operator==(const KubernetesEventsObservation&) const -> bool = default;
};
struct OpsLogLine {
  EventTimestamp timestamp;
  // One bounded UTF-8 line. Adapter excludes known credentials before delivery;
  // domain validation does not claim arbitrary text is perfectly redacted.
  std::string text;
  auto operator==(const OpsLogLine&) const -> bool = default;
};
struct OpsLogObservation {
  OpsResourceIdentity source;
  // Preserve source order; timestamps need not be monotonic. Timestamps outside
  // the requested window fail explicitly rather than being clamped or omitted.
  std::vector<OpsLogLine> lines;
  auto operator==(const OpsLogObservation&) const -> bool = default;
};
using OpsObservationPayload =
    std::variant<LinuxHealthObservation, LinuxServicesObservation,
                 LinuxServiceObservation, KubernetesWorkloadsObservation,
                 KubernetesPodObservation, KubernetesEventsObservation,
                 OpsLogObservation>;

struct OpsObservation {
  OpsObservationRequest request;
  EventTimestamp started_at;
  EventTimestamp completed_at;
  OpsObservationCompleteness completeness{OpsObservationCompleteness::complete};
  // Null means the omitted population is unknown; zero is an observed count.
  // Complete requires known zero omissions and zero unsupported entries.
  std::optional<std::uint64_t> omitted_entries{0};
  std::uint64_t unsupported_entries{};
  // Bounded non-secret source resource/list version. Required for Kubernetes
  // structured reads; logs/Linux sources may have no resource version.
  std::optional<std::string> source_version{};
  OpsObservationPayload payload;
  auto operator==(const OpsObservation&) const -> bool = default;
};

enum class OpsObservationErrorCode {
  invalid_request,
  request_mismatch,
  invalid_payload,
  source_mismatch,
  invalid_timestamp,
  invalid_completeness,
  resource_exhausted,
  internal_failure
};
struct OpsObservationError {
  OpsObservationErrorCode code;
  std::string message;
  auto operator==(const OpsObservationError&) const -> bool = default;
};
struct OpsObservationUsage {
  // Version-1 neutral accounting: bounded string bytes plus fixed field/row
  // envelopes. This is not a promise about a future JSON encoding's size.
  std::size_t evidence_bytes{};
  std::size_t entries{};
  auto operator==(const OpsObservationUsage&) const -> bool = default;
};

// Authority is the immutable snapshot captured for this operation. This pure
// check does not replace a live controller's current-selection/consent check
// before publishing a completion, and does not initiate or authorize new IO.
[[nodiscard]] auto validate_ops_observation(
    const OpsObservationAuthority& authority,
    const OpsObservationRequest& expected_request,
    const OpsObservation& observation)
    -> std::expected<OpsObservationUsage, OpsObservationError>;

} // namespace aiforge::domain
