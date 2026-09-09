#include <aiforge/adapters/ops_observation_json.hpp>
#include <algorithm>
#include <array>
#include <concepts>
#include <exception>
#include <nlohmann/json.hpp>
#include <set>
#include <type_traits>
#include <utility>

namespace aiforge::adapters {
namespace {
using Json = nlohmann::json;
using Error = OpsObservationJsonError;
class InvalidDocument final : public std::exception {};
class DocumentLimit final : public std::exception {};
auto fields(const Json& value, std::initializer_list<std::string_view> names)
    -> void {
  if (!value.is_object() || value.size() != names.size())
    throw InvalidDocument{};
  for (auto name : names)
    if (!value.contains(name)) throw InvalidDocument{};
}
template <class T> struct Codec;
template <class T> auto encode(const T& value) -> Json {
  return Codec<T>::write(value);
}
template <class T> auto decode(const Json& value) -> T {
  return Codec<T>::read(value);
}
template <std::integral T> struct Codec<T> {
  static auto write(T value) -> Json { return value; }
  static auto read(const Json& value) -> T {
    if constexpr (std::same_as<T, bool>) {
      if (!value.is_boolean()) throw InvalidDocument{};
      return value.get<bool>();
    } else {
      if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (!std::in_range<T>(number)) throw InvalidDocument{};
        return static_cast<T>(number);
      }
      if (!value.is_number_integer()) throw InvalidDocument{};
      const auto number = value.get<std::int64_t>();
      if (!std::in_range<T>(number)) throw InvalidDocument{};
      return static_cast<T>(number);
    }
  }
};
template <> struct Codec<std::string> {
  static auto write(const std::string& value) -> Json { return value; }
  static auto read(const Json& value) -> std::string {
    if (!value.is_string()) throw InvalidDocument{};
    const auto& text = value.get_ref<const std::string&>();
    if (text.size() > 32768) throw DocumentLimit{};
    return text;
  }
};
template <class Tag> struct Codec<domain::Id<Tag>> {
  static auto write(const domain::Id<Tag>& value) -> Json {
    return value.value();
  }
  static auto read(const Json& value) -> domain::Id<Tag> {
    auto result = domain::Id<Tag>::from(decode<std::string>(value));
    if (!result) throw InvalidDocument{};
    return std::move(*result);
  }
};
template <class T> struct Codec<std::optional<T>> {
  static auto write(const std::optional<T>& value) -> Json {
    return value ? encode(*value) : Json(nullptr);
  }
  static auto read(const Json& value) -> std::optional<T> {
    return value.is_null() ? std::nullopt : std::optional<T>{decode<T>(value)};
  }
};
template <class T> struct Codec<std::vector<T>> {
  static auto write(const std::vector<T>& values) -> Json {
    auto result = Json::array();
    for (const auto& value : values)
      result.push_back(encode(value));
    return result;
  }
  static auto read(const Json& value) -> std::vector<T> {
    if (!value.is_array()) throw InvalidDocument{};
    if (value.size() > 256) throw DocumentLimit{};
    std::vector<T> result;
    result.reserve(value.size());
    for (const auto& row : value)
      result.push_back(decode<T>(row));
    return result;
  }
};
template <class Rep, class Period>
struct Codec<std::chrono::duration<Rep, Period>> {
  using T = std::chrono::duration<Rep, Period>;
  static auto write(T value) -> Json { return encode(value.count()); }
  static auto read(const Json& value) -> T { return T{decode<Rep>(value)}; }
};
template <> struct Codec<domain::EventTimestamp> {
  static auto write(domain::EventTimestamp value) -> Json {
    return encode(value.time_since_epoch());
  }
  static auto read(const Json& value) -> domain::EventTimestamp {
    return domain::EventTimestamp{decode<std::chrono::milliseconds>(value)};
  }
};
template <> struct Codec<std::monostate> {
  static auto write(std::monostate) -> Json { return nullptr; }
  static auto read(const Json& value) -> std::monostate {
    if (!value.is_null()) throw InvalidDocument{};
    return {};
  }
};
template <> struct Codec<domain::LinuxExecutionScope> {
  using T = domain::LinuxExecutionScope;
  static auto write(T value) -> Json {
    switch (value) {
      case T::host: return "host";
      case T::container: return "container";
      case T::unknown: return "unknown";
    }
    throw InvalidDocument{};
  }
  static auto read(const Json& value) -> T {
    const auto name = decode<std::string>(value);
    if (name == "host") return T::host;
    if (name == "container") return T::container;
    if (name == "unknown") return T::unknown;
    throw InvalidDocument{};
  }
};
template <> struct Codec<domain::OpsObservationOperation> {
  using T = domain::OpsObservationOperation;
  static auto write(T value) -> Json {
    switch (value) {
      case T::linux_health: return "linux_health";
      case T::linux_services: return "linux_services";
      case T::linux_service_health: return "linux_service_health";
      case T::linux_service_logs: return "linux_service_logs";
      case T::kubernetes_workloads: return "kubernetes_workloads";
      case T::kubernetes_pod_health: return "kubernetes_pod_health";
      case T::kubernetes_events: return "kubernetes_events";
      case T::kubernetes_pod_logs: return "kubernetes_pod_logs";
    }
    throw InvalidDocument{};
  }
  static auto read(const Json& value) -> T {
    const auto name = decode<std::string>(value);
    if (name == "linux_health") return T::linux_health;
    if (name == "linux_services") return T::linux_services;
    if (name == "linux_service_health") return T::linux_service_health;
    if (name == "linux_service_logs") return T::linux_service_logs;
    if (name == "kubernetes_workloads") return T::kubernetes_workloads;
    if (name == "kubernetes_pod_health") return T::kubernetes_pod_health;
    if (name == "kubernetes_events") return T::kubernetes_events;
    if (name == "kubernetes_pod_logs") return T::kubernetes_pod_logs;
    throw InvalidDocument{};
  }
};
template <> struct Codec<domain::OpsObservationCompleteness> {
  using T = domain::OpsObservationCompleteness;
  static auto write(T value) -> Json {
    switch (value) {
      case T::complete: return "complete";
      case T::partial: return "partial";
      case T::truncated: return "truncated";
    }
    throw InvalidDocument{};
  }
  static auto read(const Json& value) -> T {
    const auto name = decode<std::string>(value);
    if (name == "complete") return T::complete;
    if (name == "partial") return T::partial;
    if (name == "truncated") return T::truncated;
    throw InvalidDocument{};
  }
};
template <> struct Codec<domain::OpsHealthState> {
  using T = domain::OpsHealthState;
  static auto write(T value) -> Json {
    switch (value) {
      case T::unknown: return "unknown";
      case T::healthy: return "healthy";
      case T::degraded: return "degraded";
      case T::unhealthy: return "unhealthy";
      case T::unavailable: return "unavailable";
    }
    throw InvalidDocument{};
  }
  static auto read(const Json& value) -> T {
    const auto name = decode<std::string>(value);
    if (name == "unknown") return T::unknown;
    if (name == "healthy") return T::healthy;
    if (name == "degraded") return T::degraded;
    if (name == "unhealthy") return T::unhealthy;
    if (name == "unavailable") return T::unavailable;
    throw InvalidDocument{};
  }
};
template <> struct Codec<domain::OpsServiceState> {
  using T = domain::OpsServiceState;
  static auto write(T value) -> Json {
    switch (value) {
      case T::unknown: return "unknown";
      case T::active: return "active";
      case T::inactive: return "inactive";
      case T::activating: return "activating";
      case T::deactivating: return "deactivating";
      case T::failed: return "failed";
    }
    throw InvalidDocument{};
  }
  static auto read(const Json& value) -> T {
    const auto name = decode<std::string>(value);
    if (name == "unknown") return T::unknown;
    if (name == "active") return T::active;
    if (name == "inactive") return T::inactive;
    if (name == "activating") return T::activating;
    if (name == "deactivating") return T::deactivating;
    if (name == "failed") return T::failed;
    throw InvalidDocument{};
  }
};
template <> struct Codec<domain::OpsPodPhase> {
  using T = domain::OpsPodPhase;
  static auto write(T value) -> Json {
    switch (value) {
      case T::unknown: return "unknown";
      case T::pending: return "pending";
      case T::running: return "running";
      case T::succeeded: return "succeeded";
      case T::failed: return "failed";
    }
    throw InvalidDocument{};
  }
  static auto read(const Json& value) -> T {
    const auto name = decode<std::string>(value);
    if (name == "unknown") return T::unknown;
    if (name == "pending") return T::pending;
    if (name == "running") return T::running;
    if (name == "succeeded") return T::succeeded;
    if (name == "failed") return T::failed;
    throw InvalidDocument{};
  }
};
template <> struct Codec<domain::OpsContainerState> {
  using T = domain::OpsContainerState;
  static auto write(T value) -> Json {
    switch (value) {
      case T::unknown: return "unknown";
      case T::waiting: return "waiting";
      case T::running: return "running";
      case T::terminated: return "terminated";
    }
    throw InvalidDocument{};
  }
  static auto read(const Json& value) -> T {
    const auto name = decode<std::string>(value);
    if (name == "unknown") return T::unknown;
    if (name == "waiting") return T::waiting;
    if (name == "running") return T::running;
    if (name == "terminated") return T::terminated;
    throw InvalidDocument{};
  }
};
template <> struct Codec<domain::OpsReadiness> {
  using T = domain::OpsReadiness;
  static auto write(T value) -> Json {
    switch (value) {
      case T::unknown: return "unknown";
      case T::not_ready: return "not_ready";
      case T::ready: return "ready";
    }
    throw InvalidDocument{};
  }
  static auto read(const Json& value) -> T {
    const auto name = decode<std::string>(value);
    if (name == "unknown") return T::unknown;
    if (name == "not_ready") return T::not_ready;
    if (name == "ready") return T::ready;
    throw InvalidDocument{};
  }
};
template <> struct Codec<domain::OpsEventSeverity> {
  using T = domain::OpsEventSeverity;
  static auto write(T value) -> Json {
    switch (value) {
      case T::unknown: return "unknown";
      case T::normal: return "normal";
      case T::warning: return "warning";
    }
    throw InvalidDocument{};
  }
  static auto read(const Json& value) -> T {
    const auto name = decode<std::string>(value);
    if (name == "unknown") return T::unknown;
    if (name == "normal") return T::normal;
    if (name == "warning") return T::warning;
    throw InvalidDocument{};
  }
};
template <> struct Codec<domain::OpsObservationReason> {
  using T = domain::OpsObservationReason;
  static auto write(T value) -> Json {
    switch (value) {
      case T::unknown: return "unknown";
      case T::none: return "none";
      case T::not_ready: return "not_ready";
      case T::crash_loop: return "crash_loop";
      case T::image_pull_failed: return "image_pull_failed";
      case T::out_of_memory: return "out_of_memory";
      case T::probe_failed: return "probe_failed";
      case T::scheduling_failed: return "scheduling_failed";
      case T::failed_exit: return "failed_exit";
      case T::completed: return "completed";
      case T::unavailable: return "unavailable";
    }
    throw InvalidDocument{};
  }
  static auto read(const Json& value) -> T {
    const auto name = decode<std::string>(value);
    if (name == "unknown") return T::unknown;
    if (name == "none") return T::none;
    if (name == "not_ready") return T::not_ready;
    if (name == "crash_loop") return T::crash_loop;
    if (name == "image_pull_failed") return T::image_pull_failed;
    if (name == "out_of_memory") return T::out_of_memory;
    if (name == "probe_failed") return T::probe_failed;
    if (name == "scheduling_failed") return T::scheduling_failed;
    if (name == "failed_exit") return T::failed_exit;
    if (name == "completed") return T::completed;
    if (name == "unavailable") return T::unavailable;
    throw InvalidDocument{};
  }
};
template <> struct Codec<domain::OpsWorkloadKind> {
  using T = domain::OpsWorkloadKind;
  static auto write(T value) -> Json {
    switch (value) {
      case T::pod: return "pod";
      case T::deployment: return "deployment";
      case T::stateful_set: return "stateful_set";
      case T::daemon_set: return "daemon_set";
      case T::job: return "job";
      case T::cron_job: return "cron_job";
    }
    throw InvalidDocument{};
  }
  static auto read(const Json& value) -> T {
    const auto name = decode<std::string>(value);
    if (name == "pod") return T::pod;
    if (name == "deployment") return T::deployment;
    if (name == "stateful_set") return T::stateful_set;
    if (name == "daemon_set") return T::daemon_set;
    if (name == "job") return T::job;
    if (name == "cron_job") return T::cron_job;
    throw InvalidDocument{};
  }
};
template <> struct Codec<domain::OpsMemoryScope> {
  using T = domain::OpsMemoryScope;
  static auto write(T value) -> Json {
    switch (value) {
      case T::unknown: return "unknown";
      case T::kernel: return "kernel";
      case T::cgroup: return "cgroup";
    }
    throw InvalidDocument{};
  }
  static auto read(const Json& value) -> T {
    const auto name = decode<std::string>(value);
    if (name == "unknown") return T::unknown;
    if (name == "kernel") return T::kernel;
    if (name == "cgroup") return T::cgroup;
    throw InvalidDocument{};
  }
};
template <> struct Codec<domain::LinuxOpsIdentity> {
  using T = domain::LinuxOpsIdentity;
  static auto write(const T& value) -> Json {
    return {{"scope", encode(value.scope)},
            {"boot_id", encode(value.boot_id)},
            {"pid_namespace", encode(value.pid_namespace)},
            {"mount_namespace", encode(value.mount_namespace)}};
  }
  static auto read(const Json& value) -> T {
    fields(value, {"scope", "boot_id", "pid_namespace", "mount_namespace"});
    return {decode<domain::LinuxExecutionScope>(value.at("scope")),
            decode<std::string>(value.at("boot_id")),
            decode<std::uint64_t>(value.at("pid_namespace")),
            decode<std::uint64_t>(value.at("mount_namespace"))};
  }
};
template <> struct Codec<domain::OpsHttpsEndpoint> {
  using T = domain::OpsHttpsEndpoint;
  static auto write(const T& value) -> Json {
    return {{"host", encode(value.host)}, {"port", encode(value.port)}};
  }
  static auto read(const Json& value) -> T {
    fields(value, {"host", "port"});
    return {decode<std::string>(value.at("host")),
            decode<std::uint16_t>(value.at("port"))};
  }
};
template <> struct Codec<domain::KubernetesOpsIdentity> {
  using T = domain::KubernetesOpsIdentity;
  static auto write(const T& value) -> Json {
    return {{"context_name", encode(value.context_name)},
            {"namespace_name", encode(value.namespace_name)},
            {"endpoint", encode(value.endpoint)},
            {"trust_identity", encode(value.trust_identity)}};
  }
  static auto read(const Json& value) -> T {
    fields(value,
           {"context_name", "namespace_name", "endpoint", "trust_identity"});
    return {decode<std::string>(value.at("context_name")),
            decode<std::string>(value.at("namespace_name")),
            decode<domain::OpsHttpsEndpoint>(value.at("endpoint")),
            decode<std::string>(value.at("trust_identity"))};
  }
};
template <> struct Codec<domain::CephOpsIdentity> {
  using T = domain::CephOpsIdentity;
  static auto write(const T& value) -> Json {
    return {{"cluster_identity", encode(value.cluster_identity)}};
  }
  static auto read(const Json& value) -> T {
    fields(value, {"cluster_identity"});
    return {decode<std::string>(value.at("cluster_identity"))};
  }
};
template <> struct Codec<domain::OpsTargetIdentity> {
  using T = domain::OpsTargetIdentity;
  static auto write(const T& value) -> Json {
    if (const auto* item = std::get_if<domain::LinuxOpsIdentity>(&value))
      return {{"kind", "linux_local"}, {"value", encode(*item)}};
    if (const auto* item = std::get_if<domain::KubernetesOpsIdentity>(&value))
      return {{"kind", "kubernetes"}, {"value", encode(*item)}};
    if (const auto* item = std::get_if<domain::CephOpsIdentity>(&value))
      return {{"kind", "ceph"}, {"value", encode(*item)}};
    throw InvalidDocument{};
  }
  static auto read(const Json& value) -> T {
    fields(value, {"kind", "value"});
    const auto kind = decode<std::string>(value.at("kind"));
    if (kind == "linux_local")
      return decode<domain::LinuxOpsIdentity>(value.at("value"));
    if (kind == "kubernetes")
      return decode<domain::KubernetesOpsIdentity>(value.at("value"));
    if (kind == "ceph")
      return decode<domain::CephOpsIdentity>(value.at("value"));
    throw InvalidDocument{};
  }
};
template <> struct Codec<domain::OpsTargetBinding> {
  using T = domain::OpsTargetBinding;
  static auto write(const T& value) -> Json {
    return {{"target_id", encode(value.target_id)},
            {"configuration_revision", encode(value.configuration_revision)},
            {"identity", encode(value.identity)}};
  }
  static auto read(const Json& value) -> T {
    fields(value, {"target_id", "configuration_revision", "identity"});
    return {decode<domain::OpsTargetId>(value.at("target_id")),
            decode<domain::OpsConfigurationRevision>(
                value.at("configuration_revision")),
            decode<domain::OpsTargetIdentity>(value.at("identity"))};
  }
};
template <> struct Codec<domain::LinuxServiceIdentity> {
  using T = domain::LinuxServiceIdentity;
  static auto write(const T& value) -> Json {
    return {{"unit_name", encode(value.unit_name)},
            {"invocation_id", encode(value.invocation_id)}};
  }
  static auto read(const Json& value) -> T {
    fields(value, {"unit_name", "invocation_id"});
    return {decode<std::string>(value.at("unit_name")),
            decode<std::optional<domain::OpsResourceUid>>(
                value.at("invocation_id"))};
  }
};
template <> struct Codec<domain::KubernetesContainerIdentity> {
  using T = domain::KubernetesContainerIdentity;
  static auto write(const T& value) -> Json {
    return {{"name", encode(value.name)},
            {"runtime_identity", encode(value.runtime_identity)}};
  }
  static auto read(const Json& value) -> T {
    fields(value, {"name", "runtime_identity"});
    return {decode<std::string>(value.at("name")),
            decode<std::string>(value.at("runtime_identity"))};
  }
};
template <> struct Codec<domain::KubernetesPodIdentity> {
  using T = domain::KubernetesPodIdentity;
  static auto write(const T& value) -> Json {
    return {{"namespace_name", encode(value.namespace_name)},
            {"name", encode(value.name)},
            {"uid", encode(value.uid)},
            {"container", encode(value.container)}};
  }
  static auto read(const Json& value) -> T {
    fields(value, {"namespace_name", "name", "uid", "container"});
    return {decode<std::string>(value.at("namespace_name")),
            decode<std::string>(value.at("name")),
            decode<domain::OpsResourceUid>(value.at("uid")),
            decode<std::optional<domain::KubernetesContainerIdentity>>(
                value.at("container"))};
  }
};
template <> struct Codec<domain::OpsResourceIdentity> {
  using T = domain::OpsResourceIdentity;
  static auto write(const T& value) -> Json {
    if (const auto* item = std::get_if<std::monostate>(&value))
      return {{"kind", "none"}, {"value", encode(*item)}};
    if (const auto* item = std::get_if<domain::LinuxServiceIdentity>(&value))
      return {{"kind", "linux_service"}, {"value", encode(*item)}};
    if (const auto* item = std::get_if<domain::KubernetesPodIdentity>(&value))
      return {{"kind", "kubernetes_pod"}, {"value", encode(*item)}};
    throw InvalidDocument{};
  }
  static auto read(const Json& value) -> T {
    fields(value, {"kind", "value"});
    const auto kind = decode<std::string>(value.at("kind"));
    if (kind == "none") return decode<std::monostate>(value.at("value"));
    if (kind == "linux_service")
      return decode<domain::LinuxServiceIdentity>(value.at("value"));
    if (kind == "kubernetes_pod")
      return decode<domain::KubernetesPodIdentity>(value.at("value"));
    throw InvalidDocument{};
  }
};
template <> struct Codec<domain::OpsObservationLimits> {
  using T = domain::OpsObservationLimits;
  static auto write(const T& value) -> Json {
    return {{"maximum_entries", encode(value.maximum_entries)},
            {"maximum_bytes", encode(value.maximum_bytes)},
            {"timeout_ms", encode(value.timeout)},
            {"maximum_log_lines", encode(value.maximum_log_lines)},
            {"maximum_log_bytes", encode(value.maximum_log_bytes)},
            {"maximum_log_age_seconds", encode(value.maximum_log_age)}};
  }
  static auto read(const Json& value) -> T {
    fields(value, {"maximum_entries", "maximum_bytes", "timeout_ms",
                   "maximum_log_lines", "maximum_log_bytes",
                   "maximum_log_age_seconds"});
    return {decode<std::size_t>(value.at("maximum_entries")),
            decode<std::uint64_t>(value.at("maximum_bytes")),
            decode<std::chrono::milliseconds>(value.at("timeout_ms")),
            decode<std::size_t>(value.at("maximum_log_lines")),
            decode<std::uint64_t>(value.at("maximum_log_bytes")),
            decode<std::chrono::seconds>(value.at("maximum_log_age_seconds"))};
  }
};
template <> struct Codec<domain::OpsObservationRequest> {
  using T = domain::OpsObservationRequest;
  static auto write(const T& value) -> Json {
    return {{"owner_id", encode(value.owner_id)},
            {"session_id", encode(value.session_id)},
            {"request_id", encode(value.request_id)},
            {"target", encode(value.target)},
            {"selection_generation", encode(value.selection_generation)},
            {"operation", encode(value.operation)},
            {"resource", encode(value.resource)},
            {"log_policy_revision", encode(value.log_policy_revision)},
            {"limits", encode(value.limits)}};
  }
  static auto read(const Json& value) -> T {
    fields(value, {"owner_id", "session_id", "request_id", "target",
                   "selection_generation", "operation", "resource",
                   "log_policy_revision", "limits"});
    return {decode<domain::OpsOwnerId>(value.at("owner_id")),
            decode<domain::SessionId>(value.at("session_id")),
            decode<domain::OpsRequestId>(value.at("request_id")),
            decode<domain::OpsTargetBinding>(value.at("target")),
            decode<std::uint64_t>(value.at("selection_generation")),
            decode<domain::OpsObservationOperation>(value.at("operation")),
            decode<domain::OpsResourceIdentity>(value.at("resource")),
            decode<std::uint64_t>(value.at("log_policy_revision")),
            decode<domain::OpsObservationLimits>(value.at("limits"))};
  }
};
template <> struct Codec<domain::LinuxMemoryObservation> {
  using T = domain::LinuxMemoryObservation;
  static auto write(const T& value) -> Json {
    return {{"scope", encode(value.scope)},
            {"total_bytes", encode(value.total_bytes)},
            {"available_bytes", encode(value.available_bytes)}};
  }
  static auto read(const Json& value) -> T {
    fields(value, {"scope", "total_bytes", "available_bytes"});
    return {decode<domain::OpsMemoryScope>(value.at("scope")),
            decode<std::uint64_t>(value.at("total_bytes")),
            decode<std::uint64_t>(value.at("available_bytes"))};
  }
};
template <> struct Codec<domain::LinuxHealthObservation> {
  using T = domain::LinuxHealthObservation;
  static auto write(const T& value) -> Json {
    return {{"health", encode(value.health)},
            {"kernel_uptime_seconds", encode(value.kernel_uptime_seconds)},
            {"memory", encode(value.memory)},
            {"active_services", encode(value.active_services)},
            {"failed_services", encode(value.failed_services)}};
  }
  static auto read(const Json& value) -> T {
    fields(value, {"health", "kernel_uptime_seconds", "memory",
                   "active_services", "failed_services"});
    return {
        decode<domain::OpsHealthState>(value.at("health")),
        decode<std::optional<std::uint64_t>>(value.at("kernel_uptime_seconds")),
        decode<std::optional<domain::LinuxMemoryObservation>>(
            value.at("memory")),
        decode<std::optional<std::uint32_t>>(value.at("active_services")),
        decode<std::optional<std::uint32_t>>(value.at("failed_services"))};
  }
};
template <> struct Codec<domain::LinuxServiceObservation> {
  using T = domain::LinuxServiceObservation;
  static auto write(const T& value) -> Json {
    return {{"identity", encode(value.identity)},
            {"state", encode(value.state)},
            {"reason", encode(value.reason)},
            {"exit_status", encode(value.exit_status)},
            {"restart_count", encode(value.restart_count)}};
  }
  static auto read(const Json& value) -> T {
    fields(value,
           {"identity", "state", "reason", "exit_status", "restart_count"});
    return {decode<domain::LinuxServiceIdentity>(value.at("identity")),
            decode<domain::OpsServiceState>(value.at("state")),
            decode<domain::OpsObservationReason>(value.at("reason")),
            decode<std::optional<std::int32_t>>(value.at("exit_status")),
            decode<std::optional<std::uint32_t>>(value.at("restart_count"))};
  }
};
template <> struct Codec<domain::LinuxServicesObservation> {
  using T = domain::LinuxServicesObservation;
  static auto write(const T& value) -> Json {
    return {{"services", encode(value.services)}};
  }
  static auto read(const Json& value) -> T {
    fields(value, {"services"});
    return {decode<std::vector<domain::LinuxServiceObservation>>(
        value.at("services"))};
  }
};
template <> struct Codec<domain::KubernetesObservedResource> {
  using T = domain::KubernetesObservedResource;
  static auto write(const T& value) -> Json {
    return {{"kind", encode(value.kind)},
            {"namespace_name", encode(value.namespace_name)},
            {"name", encode(value.name)},
            {"uid", encode(value.uid)}};
  }
  static auto read(const Json& value) -> T {
    fields(value, {"kind", "namespace_name", "name", "uid"});
    return {decode<domain::OpsWorkloadKind>(value.at("kind")),
            decode<std::string>(value.at("namespace_name")),
            decode<std::string>(value.at("name")),
            decode<domain::OpsResourceUid>(value.at("uid"))};
  }
};
template <> struct Codec<domain::KubernetesWorkloadObservation> {
  using T = domain::KubernetesWorkloadObservation;
  static auto write(const T& value) -> Json {
    return {{"identity", encode(value.identity)},
            {"health", encode(value.health)},
            {"desired_count", encode(value.desired_count)},
            {"ready_count", encode(value.ready_count)},
            {"observed_count", encode(value.observed_count)}};
  }
  static auto read(const Json& value) -> T {
    fields(value, {"identity", "health", "desired_count", "ready_count",
                   "observed_count"});
    return {decode<domain::KubernetesObservedResource>(value.at("identity")),
            decode<domain::OpsHealthState>(value.at("health")),
            decode<std::optional<std::uint32_t>>(value.at("desired_count")),
            decode<std::optional<std::uint32_t>>(value.at("ready_count")),
            decode<std::optional<std::uint32_t>>(value.at("observed_count"))};
  }
};
template <> struct Codec<domain::KubernetesWorkloadsObservation> {
  using T = domain::KubernetesWorkloadsObservation;
  static auto write(const T& value) -> Json {
    return {{"workloads", encode(value.workloads)}};
  }
  static auto read(const Json& value) -> T {
    fields(value, {"workloads"});
    return {decode<std::vector<domain::KubernetesWorkloadObservation>>(
        value.at("workloads"))};
  }
};
template <> struct Codec<domain::KubernetesContainerObservation> {
  using T = domain::KubernetesContainerObservation;
  static auto write(const T& value) -> Json {
    return {{"name", encode(value.name)},
            {"runtime_identity", encode(value.runtime_identity)},
            {"state", encode(value.state)},
            {"readiness", encode(value.readiness)},
            {"reason", encode(value.reason)},
            {"restart_count", encode(value.restart_count)},
            {"exit_status", encode(value.exit_status)}};
  }
  static auto read(const Json& value) -> T {
    fields(value, {"name", "runtime_identity", "state", "readiness", "reason",
                   "restart_count", "exit_status"});
    return {decode<std::string>(value.at("name")),
            decode<std::optional<std::string>>(value.at("runtime_identity")),
            decode<domain::OpsContainerState>(value.at("state")),
            decode<domain::OpsReadiness>(value.at("readiness")),
            decode<domain::OpsObservationReason>(value.at("reason")),
            decode<std::optional<std::uint32_t>>(value.at("restart_count")),
            decode<std::optional<std::int32_t>>(value.at("exit_status"))};
  }
};
template <> struct Codec<domain::KubernetesPodObservation> {
  using T = domain::KubernetesPodObservation;
  static auto write(const T& value) -> Json {
    return {{"identity", encode(value.identity)},
            {"phase", encode(value.phase)},
            {"containers", encode(value.containers)}};
  }
  static auto read(const Json& value) -> T {
    fields(value, {"identity", "phase", "containers"});
    return {decode<domain::KubernetesPodIdentity>(value.at("identity")),
            decode<domain::OpsPodPhase>(value.at("phase")),
            decode<std::vector<domain::KubernetesContainerObservation>>(
                value.at("containers"))};
  }
};
template <> struct Codec<domain::KubernetesEventObservation> {
  using T = domain::KubernetesEventObservation;
  static auto write(const T& value) -> Json {
    return {{"event_uid", encode(value.event_uid)},
            {"regarding", encode(value.regarding)},
            {"severity", encode(value.severity)},
            {"reason", encode(value.reason)},
            {"first_observed_at_ms", encode(value.first_observed_at)},
            {"last_observed_at_ms", encode(value.last_observed_at)},
            {"occurrences", encode(value.occurrences)}};
  }
  static auto read(const Json& value) -> T {
    fields(value,
           {"event_uid", "regarding", "severity", "reason",
            "first_observed_at_ms", "last_observed_at_ms", "occurrences"});
    return {decode<domain::OpsResourceUid>(value.at("event_uid")),
            decode<domain::KubernetesObservedResource>(value.at("regarding")),
            decode<domain::OpsEventSeverity>(value.at("severity")),
            decode<domain::OpsObservationReason>(value.at("reason")),
            decode<std::optional<domain::EventTimestamp>>(
                value.at("first_observed_at_ms")),
            decode<std::optional<domain::EventTimestamp>>(
                value.at("last_observed_at_ms")),
            decode<std::uint64_t>(value.at("occurrences"))};
  }
};
template <> struct Codec<domain::KubernetesEventsObservation> {
  using T = domain::KubernetesEventsObservation;
  static auto write(const T& value) -> Json {
    return {{"events", encode(value.events)}};
  }
  static auto read(const Json& value) -> T {
    fields(value, {"events"});
    return {decode<std::vector<domain::KubernetesEventObservation>>(
        value.at("events"))};
  }
};
template <> struct Codec<domain::OpsLogLine> {
  using T = domain::OpsLogLine;
  static auto write(const T& value) -> Json {
    return {{"timestamp_ms", encode(value.timestamp)},
            {"text", encode(value.text)}};
  }
  static auto read(const Json& value) -> T {
    fields(value, {"timestamp_ms", "text"});
    return {decode<domain::EventTimestamp>(value.at("timestamp_ms")),
            decode<std::string>(value.at("text"))};
  }
};
template <> struct Codec<domain::OpsLogObservation> {
  using T = domain::OpsLogObservation;
  static auto write(const T& value) -> Json {
    return {{"source", encode(value.source)}, {"lines", encode(value.lines)}};
  }
  static auto read(const Json& value) -> T {
    fields(value, {"source", "lines"});
    return {decode<domain::OpsResourceIdentity>(value.at("source")),
            decode<std::vector<domain::OpsLogLine>>(value.at("lines"))};
  }
};
template <> struct Codec<domain::OpsObservationPayload> {
  using T = domain::OpsObservationPayload;
  static auto write(const T& value) -> Json {
    if (const auto* item = std::get_if<domain::LinuxHealthObservation>(&value))
      return {{"kind", "linux_health"}, {"value", encode(*item)}};
    if (const auto* item =
            std::get_if<domain::LinuxServicesObservation>(&value))
      return {{"kind", "linux_services"}, {"value", encode(*item)}};
    if (const auto* item = std::get_if<domain::LinuxServiceObservation>(&value))
      return {{"kind", "linux_service"}, {"value", encode(*item)}};
    if (const auto* item =
            std::get_if<domain::KubernetesWorkloadsObservation>(&value))
      return {{"kind", "kubernetes_workloads"}, {"value", encode(*item)}};
    if (const auto* item =
            std::get_if<domain::KubernetesPodObservation>(&value))
      return {{"kind", "kubernetes_pod"}, {"value", encode(*item)}};
    if (const auto* item =
            std::get_if<domain::KubernetesEventsObservation>(&value))
      return {{"kind", "kubernetes_events"}, {"value", encode(*item)}};
    if (const auto* item = std::get_if<domain::OpsLogObservation>(&value))
      return {{"kind", "logs"}, {"value", encode(*item)}};
    throw InvalidDocument{};
  }
  static auto read(const Json& value) -> T {
    fields(value, {"kind", "value"});
    const auto kind = decode<std::string>(value.at("kind"));
    if (kind == "linux_health")
      return decode<domain::LinuxHealthObservation>(value.at("value"));
    if (kind == "linux_services")
      return decode<domain::LinuxServicesObservation>(value.at("value"));
    if (kind == "linux_service")
      return decode<domain::LinuxServiceObservation>(value.at("value"));
    if (kind == "kubernetes_workloads")
      return decode<domain::KubernetesWorkloadsObservation>(value.at("value"));
    if (kind == "kubernetes_pod")
      return decode<domain::KubernetesPodObservation>(value.at("value"));
    if (kind == "kubernetes_events")
      return decode<domain::KubernetesEventsObservation>(value.at("value"));
    if (kind == "logs")
      return decode<domain::OpsLogObservation>(value.at("value"));
    throw InvalidDocument{};
  }
};
template <> struct Codec<domain::OpsObservation> {
  using T = domain::OpsObservation;
  static auto write(const T& value) -> Json {
    return {{"request", encode(value.request)},
            {"started_at_ms", encode(value.started_at)},
            {"completed_at_ms", encode(value.completed_at)},
            {"completeness", encode(value.completeness)},
            {"omitted_entries", encode(value.omitted_entries)},
            {"unsupported_entries", encode(value.unsupported_entries)},
            {"source_version", encode(value.source_version)},
            {"payload", encode(value.payload)}};
  }
  static auto read(const Json& value) -> T {
    fields(value, {"request", "started_at_ms", "completed_at_ms",
                   "completeness", "omitted_entries", "unsupported_entries",
                   "source_version", "payload"});
    return {
        decode<domain::OpsObservationRequest>(value.at("request")),
        decode<domain::EventTimestamp>(value.at("started_at_ms")),
        decode<domain::EventTimestamp>(value.at("completed_at_ms")),
        decode<domain::OpsObservationCompleteness>(value.at("completeness")),
        decode<std::optional<std::uint64_t>>(value.at("omitted_entries")),
        decode<std::uint64_t>(value.at("unsupported_entries")),
        decode<std::optional<std::string>>(value.at("source_version")),
        decode<domain::OpsObservationPayload>(value.at("payload"))};
  }
};

auto parse(std::string_view document) -> Json {
  std::vector<std::set<std::string>> keys;
  std::size_t events{};
  const auto callback = [&](int depth, Json::parse_event_t event, Json& value) {
    if (++events > 32768 || depth > 16) throw DocumentLimit{};
    if (event == Json::parse_event_t::object_start)
      keys.emplace_back();
    else if (event == Json::parse_event_t::key) {
      if (keys.empty() || !keys.back().insert(value.get<std::string>()).second)
        throw InvalidDocument{};
    } else if (event == Json::parse_event_t::object_end) {
      if (keys.empty()) throw InvalidDocument{};
      keys.pop_back();
    }
    return true;
  };
  return Json::parse(document.begin(), document.end(), callback, true, false);
}
} // namespace

auto encode_ops_observation(const domain::OpsObservation& value)
    -> std::expected<std::string, Error> {
  try {
    if (!domain::validate_recorded_ops_observation(value))
      return std::unexpected(Error::invalid_record);
    auto document = Json{{"version", 1}, {"observation", encode(value)}}.dump();
    if (document.size() > maximum_ops_observation_json_bytes)
      return std::unexpected(Error::resource_exhausted);
    return document;
  } catch (const DocumentLimit&) {
    return std::unexpected(Error::resource_exhausted);
  } catch (...) {
    return std::unexpected(Error::internal_failure);
  }
}
auto decode_ops_observation(std::string_view document)
    -> std::expected<domain::OpsObservation, Error> {
  try {
    if (document.size() > maximum_ops_observation_json_bytes)
      return std::unexpected(Error::resource_exhausted);
    const auto parsed = parse(document);
    fields(parsed, {"version", "observation"});
    if (decode<std::uint32_t>(parsed.at("version")) != 1)
      throw InvalidDocument{};
    auto result = decode<domain::OpsObservation>(parsed.at("observation"));
    if (!domain::validate_recorded_ops_observation(result))
      return std::unexpected(Error::invalid_record);
    return result;
  } catch (const DocumentLimit&) {
    return std::unexpected(Error::resource_exhausted);
  } catch (const InvalidDocument&) {
    return std::unexpected(Error::invalid_document);
  } catch (const Json::exception&) {
    return std::unexpected(Error::invalid_document);
  } catch (...) {
    return std::unexpected(Error::internal_failure);
  }
}
} // namespace aiforge::adapters
