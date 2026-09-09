#include <aiforge/runtime/ops_observation_history.hpp>
#include <array>
#include <charconv>
#include <concepts>
#include <exception>
#include <string_view>
#include <system_error>

namespace aiforge::runtime {
namespace {
class ContentLimit final : public std::exception {};
class InvalidContent final : public std::exception {};
class Writer {
 public:
  auto render(const domain::OpsObservation& value) -> std::string {
    append("AIForge Ops observation v1\n");
    write("observation", value);
    return std::move(m_text);
  }

 private:
  auto append(std::string_view value) -> void {
    if (value.size() > maximum_ops_observation_content_bytes - m_text.size())
      throw ContentLimit{};
    m_text.append(value);
  }
  auto scalar(const std::string& key, std::string_view value) -> void {
    append(key);
    append("=");
    append(value);
    append("\n");
  }
  auto write(const std::string& key, std::string_view value) -> void {
    append(key);
    append("=\"");
    for (char c : value) {
      if (c == '\\' || c == '"') append("\\");
      append(std::string_view{&c, 1});
    }
    append("\"\n");
  }
  auto write(const std::string& key, const std::string& value) -> void {
    write(key, std::string_view{value});
  }
  template <std::integral T>
  auto write(const std::string& key, T value) -> void {
    std::array<char, 32> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.end(), value);
    if (result.ec != std::errc{}) throw InvalidContent{};
    scalar(key, std::string_view{buffer.data(), result.ptr});
  }
  template <class Tag>
  auto write(const std::string& key, const domain::Id<Tag>& value) -> void {
    write(key, value.value());
  }
  template <class T>
  auto write(const std::string& key, const std::optional<T>& value) -> void {
    if (value)
      write(key, *value);
    else
      scalar(key, "unknown");
  }
  template <class T>
  auto write(const std::string& key, const std::vector<T>& values) -> void {
    write(key + ".count", values.size());
    for (std::size_t i = 0; i < values.size(); ++i)
      write(key + "[" + std::to_string(i) + "]", values[i]);
  }
  template <class Rep, class Period>
  auto write(const std::string& key, std::chrono::duration<Rep, Period> value)
      -> void {
    write(key, value.count());
  }
  auto write(const std::string& key, domain::EventTimestamp value) -> void {
    write(key, value.time_since_epoch());
  }
  auto write(const std::string& key, std::monostate) -> void {
    scalar(key, "none");
  }
  auto write(const std::string& key, const domain::LinuxExecutionScope& value)
      -> void {
    switch (value) {
      case domain::LinuxExecutionScope::host: scalar(key, "host"); return;
      case domain::LinuxExecutionScope::container:
        scalar(key, "container");
        return;
      case domain::LinuxExecutionScope::unknown: scalar(key, "unknown"); return;
    }
    throw InvalidContent{};
  }
  auto write(const std::string& key,
             const domain::OpsObservationOperation& value) -> void {
    switch (value) {
      case domain::OpsObservationOperation::linux_health:
        scalar(key, "linux_health");
        return;
      case domain::OpsObservationOperation::linux_services:
        scalar(key, "linux_services");
        return;
      case domain::OpsObservationOperation::linux_service_health:
        scalar(key, "linux_service_health");
        return;
      case domain::OpsObservationOperation::linux_service_logs:
        scalar(key, "linux_service_logs");
        return;
      case domain::OpsObservationOperation::kubernetes_workloads:
        scalar(key, "kubernetes_workloads");
        return;
      case domain::OpsObservationOperation::kubernetes_pod_health:
        scalar(key, "kubernetes_pod_health");
        return;
      case domain::OpsObservationOperation::kubernetes_events:
        scalar(key, "kubernetes_events");
        return;
      case domain::OpsObservationOperation::kubernetes_pod_logs:
        scalar(key, "kubernetes_pod_logs");
        return;
    }
    throw InvalidContent{};
  }
  auto write(const std::string& key,
             const domain::OpsObservationCompleteness& value) -> void {
    switch (value) {
      case domain::OpsObservationCompleteness::complete:
        scalar(key, "complete");
        return;
      case domain::OpsObservationCompleteness::partial:
        scalar(key, "partial");
        return;
      case domain::OpsObservationCompleteness::truncated:
        scalar(key, "truncated");
        return;
    }
    throw InvalidContent{};
  }
  auto write(const std::string& key, const domain::OpsHealthState& value)
      -> void {
    switch (value) {
      case domain::OpsHealthState::unknown: scalar(key, "unknown"); return;
      case domain::OpsHealthState::healthy: scalar(key, "healthy"); return;
      case domain::OpsHealthState::degraded: scalar(key, "degraded"); return;
      case domain::OpsHealthState::unhealthy: scalar(key, "unhealthy"); return;
      case domain::OpsHealthState::unavailable:
        scalar(key, "unavailable");
        return;
    }
    throw InvalidContent{};
  }
  auto write(const std::string& key, const domain::OpsServiceState& value)
      -> void {
    switch (value) {
      case domain::OpsServiceState::unknown: scalar(key, "unknown"); return;
      case domain::OpsServiceState::active: scalar(key, "active"); return;
      case domain::OpsServiceState::inactive: scalar(key, "inactive"); return;
      case domain::OpsServiceState::activating:
        scalar(key, "activating");
        return;
      case domain::OpsServiceState::deactivating:
        scalar(key, "deactivating");
        return;
      case domain::OpsServiceState::failed: scalar(key, "failed"); return;
    }
    throw InvalidContent{};
  }
  auto write(const std::string& key, const domain::OpsPodPhase& value) -> void {
    switch (value) {
      case domain::OpsPodPhase::unknown: scalar(key, "unknown"); return;
      case domain::OpsPodPhase::pending: scalar(key, "pending"); return;
      case domain::OpsPodPhase::running: scalar(key, "running"); return;
      case domain::OpsPodPhase::succeeded: scalar(key, "succeeded"); return;
      case domain::OpsPodPhase::failed: scalar(key, "failed"); return;
    }
    throw InvalidContent{};
  }
  auto write(const std::string& key, const domain::OpsContainerState& value)
      -> void {
    switch (value) {
      case domain::OpsContainerState::unknown: scalar(key, "unknown"); return;
      case domain::OpsContainerState::waiting: scalar(key, "waiting"); return;
      case domain::OpsContainerState::running: scalar(key, "running"); return;
      case domain::OpsContainerState::terminated:
        scalar(key, "terminated");
        return;
    }
    throw InvalidContent{};
  }
  auto write(const std::string& key, const domain::OpsReadiness& value)
      -> void {
    switch (value) {
      case domain::OpsReadiness::unknown: scalar(key, "unknown"); return;
      case domain::OpsReadiness::not_ready: scalar(key, "not_ready"); return;
      case domain::OpsReadiness::ready: scalar(key, "ready"); return;
    }
    throw InvalidContent{};
  }
  auto write(const std::string& key, const domain::OpsEventSeverity& value)
      -> void {
    switch (value) {
      case domain::OpsEventSeverity::unknown: scalar(key, "unknown"); return;
      case domain::OpsEventSeverity::normal: scalar(key, "normal"); return;
      case domain::OpsEventSeverity::warning: scalar(key, "warning"); return;
    }
    throw InvalidContent{};
  }
  auto write(const std::string& key, const domain::OpsObservationReason& value)
      -> void {
    switch (value) {
      case domain::OpsObservationReason::unknown:
        scalar(key, "unknown");
        return;
      case domain::OpsObservationReason::none: scalar(key, "none"); return;
      case domain::OpsObservationReason::not_ready:
        scalar(key, "not_ready");
        return;
      case domain::OpsObservationReason::crash_loop:
        scalar(key, "crash_loop");
        return;
      case domain::OpsObservationReason::image_pull_failed:
        scalar(key, "image_pull_failed");
        return;
      case domain::OpsObservationReason::out_of_memory:
        scalar(key, "out_of_memory");
        return;
      case domain::OpsObservationReason::probe_failed:
        scalar(key, "probe_failed");
        return;
      case domain::OpsObservationReason::scheduling_failed:
        scalar(key, "scheduling_failed");
        return;
      case domain::OpsObservationReason::failed_exit:
        scalar(key, "failed_exit");
        return;
      case domain::OpsObservationReason::completed:
        scalar(key, "completed");
        return;
      case domain::OpsObservationReason::unavailable:
        scalar(key, "unavailable");
        return;
    }
    throw InvalidContent{};
  }
  auto write(const std::string& key, const domain::OpsWorkloadKind& value)
      -> void {
    switch (value) {
      case domain::OpsWorkloadKind::pod: scalar(key, "pod"); return;
      case domain::OpsWorkloadKind::deployment:
        scalar(key, "deployment");
        return;
      case domain::OpsWorkloadKind::stateful_set:
        scalar(key, "stateful_set");
        return;
      case domain::OpsWorkloadKind::daemon_set:
        scalar(key, "daemon_set");
        return;
      case domain::OpsWorkloadKind::job: scalar(key, "job"); return;
      case domain::OpsWorkloadKind::cron_job: scalar(key, "cron_job"); return;
    }
    throw InvalidContent{};
  }
  auto write(const std::string& key, const domain::OpsMemoryScope& value)
      -> void {
    switch (value) {
      case domain::OpsMemoryScope::unknown: scalar(key, "unknown"); return;
      case domain::OpsMemoryScope::kernel: scalar(key, "kernel"); return;
      case domain::OpsMemoryScope::cgroup: scalar(key, "cgroup"); return;
    }
    throw InvalidContent{};
  }
  auto write(const std::string& key, const domain::LinuxOpsIdentity& value)
      -> void {
    write(key + ".scope", value.scope);
    write(key + ".boot_id", value.boot_id);
    write(key + ".pid_namespace", value.pid_namespace);
    write(key + ".mount_namespace", value.mount_namespace);
  }
  auto write(const std::string& key, const domain::OpsHttpsEndpoint& value)
      -> void {
    write(key + ".host", value.host);
    write(key + ".port", value.port);
  }
  auto write(const std::string& key, const domain::KubernetesOpsIdentity& value)
      -> void {
    write(key + ".context_name", value.context_name);
    write(key + ".namespace_name", value.namespace_name);
    write(key + ".endpoint", value.endpoint);
    write(key + ".trust_identity", value.trust_identity);
  }
  auto write(const std::string& key, const domain::CephOpsIdentity& value)
      -> void {
    write(key + ".cluster_identity", value.cluster_identity);
  }
  auto write(const std::string& key, const domain::OpsTargetIdentity& value)
      -> void {
    if (const auto* item = std::get_if<domain::LinuxOpsIdentity>(&value)) {
      scalar(key + ".kind", "linux_local");
      write(key + ".value", *item);
      return;
    }
    if (const auto* item = std::get_if<domain::KubernetesOpsIdentity>(&value)) {
      scalar(key + ".kind", "kubernetes");
      write(key + ".value", *item);
      return;
    }
    if (const auto* item = std::get_if<domain::CephOpsIdentity>(&value)) {
      scalar(key + ".kind", "ceph");
      write(key + ".value", *item);
      return;
    }
    throw InvalidContent{};
  }
  auto write(const std::string& key, const domain::OpsTargetBinding& value)
      -> void {
    write(key + ".target_id", value.target_id);
    write(key + ".configuration_revision", value.configuration_revision);
    write(key + ".identity", value.identity);
  }
  auto write(const std::string& key, const domain::LinuxServiceIdentity& value)
      -> void {
    write(key + ".unit_name", value.unit_name);
    write(key + ".invocation_id", value.invocation_id);
  }
  auto write(const std::string& key,
             const domain::KubernetesContainerIdentity& value) -> void {
    write(key + ".name", value.name);
    write(key + ".runtime_identity", value.runtime_identity);
  }
  auto write(const std::string& key, const domain::KubernetesPodIdentity& value)
      -> void {
    write(key + ".namespace_name", value.namespace_name);
    write(key + ".name", value.name);
    write(key + ".uid", value.uid);
    write(key + ".container", value.container);
  }
  auto write(const std::string& key, const domain::OpsResourceIdentity& value)
      -> void {
    if (const auto* item = std::get_if<std::monostate>(&value)) {
      scalar(key + ".kind", "none");
      write(key + ".value", *item);
      return;
    }
    if (const auto* item = std::get_if<domain::LinuxServiceIdentity>(&value)) {
      scalar(key + ".kind", "linux_service");
      write(key + ".value", *item);
      return;
    }
    if (const auto* item = std::get_if<domain::KubernetesPodIdentity>(&value)) {
      scalar(key + ".kind", "kubernetes_pod");
      write(key + ".value", *item);
      return;
    }
    throw InvalidContent{};
  }
  auto write(const std::string& key, const domain::OpsObservationLimits& value)
      -> void {
    write(key + ".maximum_entries", value.maximum_entries);
    write(key + ".maximum_bytes", value.maximum_bytes);
    write(key + ".timeout_ms", value.timeout);
    write(key + ".maximum_log_lines", value.maximum_log_lines);
    write(key + ".maximum_log_bytes", value.maximum_log_bytes);
    write(key + ".maximum_log_age_seconds", value.maximum_log_age);
  }
  auto write(const std::string& key, const domain::OpsObservationRequest& value)
      -> void {
    write(key + ".owner_id", value.owner_id);
    write(key + ".session_id", value.session_id);
    write(key + ".request_id", value.request_id);
    write(key + ".target", value.target);
    write(key + ".selection_generation", value.selection_generation);
    write(key + ".operation", value.operation);
    write(key + ".resource", value.resource);
    write(key + ".log_policy_revision", value.log_policy_revision);
    write(key + ".limits", value.limits);
  }
  auto write(const std::string& key,
             const domain::LinuxMemoryObservation& value) -> void {
    write(key + ".scope", value.scope);
    write(key + ".total_bytes", value.total_bytes);
    write(key + ".available_bytes", value.available_bytes);
  }
  auto write(const std::string& key,
             const domain::LinuxHealthObservation& value) -> void {
    write(key + ".health", value.health);
    write(key + ".kernel_uptime_seconds", value.kernel_uptime_seconds);
    write(key + ".memory", value.memory);
    write(key + ".active_services", value.active_services);
    write(key + ".failed_services", value.failed_services);
  }
  auto write(const std::string& key,
             const domain::LinuxServiceObservation& value) -> void {
    write(key + ".identity", value.identity);
    write(key + ".state", value.state);
    write(key + ".reason", value.reason);
    write(key + ".exit_status", value.exit_status);
    write(key + ".restart_count", value.restart_count);
  }
  auto write(const std::string& key,
             const domain::LinuxServicesObservation& value) -> void {
    write(key + ".services", value.services);
  }
  auto write(const std::string& key,
             const domain::KubernetesObservedResource& value) -> void {
    write(key + ".kind", value.kind);
    write(key + ".namespace_name", value.namespace_name);
    write(key + ".name", value.name);
    write(key + ".uid", value.uid);
  }
  auto write(const std::string& key,
             const domain::KubernetesWorkloadObservation& value) -> void {
    write(key + ".identity", value.identity);
    write(key + ".health", value.health);
    write(key + ".desired_count", value.desired_count);
    write(key + ".ready_count", value.ready_count);
    write(key + ".observed_count", value.observed_count);
  }
  auto write(const std::string& key,
             const domain::KubernetesWorkloadsObservation& value) -> void {
    write(key + ".workloads", value.workloads);
  }
  auto write(const std::string& key,
             const domain::KubernetesContainerObservation& value) -> void {
    write(key + ".name", value.name);
    write(key + ".runtime_identity", value.runtime_identity);
    write(key + ".state", value.state);
    write(key + ".readiness", value.readiness);
    write(key + ".reason", value.reason);
    write(key + ".restart_count", value.restart_count);
    write(key + ".exit_status", value.exit_status);
  }
  auto write(const std::string& key,
             const domain::KubernetesPodObservation& value) -> void {
    write(key + ".identity", value.identity);
    write(key + ".phase", value.phase);
    write(key + ".containers", value.containers);
  }
  auto write(const std::string& key,
             const domain::KubernetesEventObservation& value) -> void {
    write(key + ".event_uid", value.event_uid);
    write(key + ".regarding", value.regarding);
    write(key + ".severity", value.severity);
    write(key + ".reason", value.reason);
    write(key + ".first_observed_at_ms", value.first_observed_at);
    write(key + ".last_observed_at_ms", value.last_observed_at);
    write(key + ".occurrences", value.occurrences);
  }
  auto write(const std::string& key,
             const domain::KubernetesEventsObservation& value) -> void {
    write(key + ".events", value.events);
  }
  auto write(const std::string& key, const domain::OpsLogLine& value) -> void {
    write(key + ".timestamp_ms", value.timestamp);
    write(key + ".text", value.text);
  }
  auto write(const std::string& key, const domain::OpsLogObservation& value)
      -> void {
    write(key + ".source", value.source);
    write(key + ".lines", value.lines);
  }
  auto write(const std::string& key, const domain::OpsObservationPayload& value)
      -> void {
    if (const auto* item =
            std::get_if<domain::LinuxHealthObservation>(&value)) {
      scalar(key + ".kind", "linux_health");
      write(key + ".value", *item);
      return;
    }
    if (const auto* item =
            std::get_if<domain::LinuxServicesObservation>(&value)) {
      scalar(key + ".kind", "linux_services");
      write(key + ".value", *item);
      return;
    }
    if (const auto* item =
            std::get_if<domain::LinuxServiceObservation>(&value)) {
      scalar(key + ".kind", "linux_service");
      write(key + ".value", *item);
      return;
    }
    if (const auto* item =
            std::get_if<domain::KubernetesWorkloadsObservation>(&value)) {
      scalar(key + ".kind", "kubernetes_workloads");
      write(key + ".value", *item);
      return;
    }
    if (const auto* item =
            std::get_if<domain::KubernetesPodObservation>(&value)) {
      scalar(key + ".kind", "kubernetes_pod");
      write(key + ".value", *item);
      return;
    }
    if (const auto* item =
            std::get_if<domain::KubernetesEventsObservation>(&value)) {
      scalar(key + ".kind", "kubernetes_events");
      write(key + ".value", *item);
      return;
    }
    if (const auto* item = std::get_if<domain::OpsLogObservation>(&value)) {
      scalar(key + ".kind", "logs");
      write(key + ".value", *item);
      return;
    }
    throw InvalidContent{};
  }
  auto write(const std::string& key, const domain::OpsObservation& value)
      -> void {
    write(key + ".request", value.request);
    write(key + ".started_at_ms", value.started_at);
    write(key + ".completed_at_ms", value.completed_at);
    write(key + ".completeness", value.completeness);
    write(key + ".omitted_entries", value.omitted_entries);
    write(key + ".unsupported_entries", value.unsupported_entries);
    write(key + ".source_version", value.source_version);
    write(key + ".payload", value.payload);
  }
  std::string m_text;
};
} // namespace

auto format_ops_observation_content(const domain::OpsObservation& observation)
    -> std::expected<std::vector<domain::ContentBlock>, OpsHistoryError> {
  try {
    if (!domain::validate_recorded_ops_observation(observation))
      return std::unexpected(OpsHistoryError{
          OpsHistoryErrorCode::invalid_history, "Ops observation is invalid"});
    return std::vector<domain::ContentBlock>{
        domain::TextBlock{Writer{}.render(observation)}};
  } catch (const ContentLimit&) {
    return std::unexpected(
        OpsHistoryError{OpsHistoryErrorCode::resource_exhausted,
                        "Ops observation content exceeds its byte limit"});
  } catch (...) {
    return std::unexpected(
        OpsHistoryError{OpsHistoryErrorCode::internal_failure,
                        "Ops observation content formatting failed"});
  }
}
} // namespace aiforge::runtime
