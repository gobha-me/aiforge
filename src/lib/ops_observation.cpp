#include <aiforge/domain/ops_observation.hpp>

#include <aiforge/detail/utf8_text.hpp>
#include <algorithm>
#include <limits>
#include <set>
#include <string_view>
#include <type_traits>
#include <utility>

namespace aiforge::domain {
namespace {
using Code = OpsObservationErrorCode;
using Status = std::expected<void, OpsObservationError>;
auto fail(Code code, std::string message) -> Status {
  return std::unexpected(OpsObservationError{code, std::move(message)});
}
constexpr auto maximum_counter =
    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
auto text(std::string_view value, std::size_t maximum, bool empty = false)
    -> bool {
  return (empty || !value.empty()) && value.size() <= maximum &&
         (value.empty() || detail::is_safe_utf8_text(value)) &&
         std::ranges::none_of(
             value, [](unsigned char c) { return c < 32 || c == 127; });
}
auto label(std::string_view value) -> bool {
  const auto alnum = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
  };
  return !value.empty() && value.size() <= 63 && alnum(value.front()) &&
         alnum(value.back()) && std::ranges::all_of(value, [&](char c) {
           return alnum(c) || c == '-';
         });
}
auto timestamp(EventTimestamp value) -> bool {
  return value.time_since_epoch().count() >= 0;
}
auto count(const std::optional<std::uint32_t>& value) -> bool {
  return !value || *value <= static_cast<std::uint32_t>(
                                 std::numeric_limits<std::int32_t>::max());
}
auto number(const std::optional<std::uint64_t>& value) -> bool {
  return !value || *value <= maximum_counter;
}
auto exit_status(const std::optional<std::int32_t>& value) -> bool {
  return !value || (*value >= 0 && *value <= 255);
}
template <class T> auto known(T value, T last) -> bool {
  return value >= static_cast<T>(0) && value <= last;
}
auto logs(OpsObservationOperation operation) -> bool {
  return operation == OpsObservationOperation::linux_service_logs ||
         operation == OpsObservationOperation::kubernetes_pod_logs;
}
auto payload_index(OpsObservationOperation operation) -> std::size_t {
  switch (operation) {
    case OpsObservationOperation::linux_health: return 0;
    case OpsObservationOperation::linux_services: return 1;
    case OpsObservationOperation::linux_service_health: return 2;
    case OpsObservationOperation::kubernetes_workloads: return 3;
    case OpsObservationOperation::kubernetes_pod_health: return 4;
    case OpsObservationOperation::kubernetes_events: return 5;
    case OpsObservationOperation::linux_service_logs:
    case OpsObservationOperation::kubernetes_pod_logs: return 6;
  }
  return std::variant_npos;
}
auto envelope(const OpsObservationRequest& request, const OpsObservation& value)
    -> Status {
  if (value.request != request)
    return fail(Code::request_mismatch,
                "Ops observation belongs to another captured request");
  if (!timestamp(value.started_at) || !timestamp(value.completed_at) ||
      value.completed_at < value.started_at)
    return fail(Code::invalid_timestamp,
                "Ops observation receipt timestamps are invalid");
  if (!known(value.completeness, OpsObservationCompleteness::truncated) ||
      !number(value.omitted_entries) ||
      value.unsupported_entries > maximum_counter)
    return fail(Code::invalid_completeness,
                "Ops completeness counters are invalid");
  if (value.completeness == OpsObservationCompleteness::complete &&
      (!value.omitted_entries || *value.omitted_entries != 0 ||
       value.unsupported_entries != 0))
    return fail(Code::invalid_completeness,
                "Complete Ops observations cannot hide omitted entries");
  if (value.source_version && !text(*value.source_version, 128))
    return fail(Code::invalid_payload, "Ops source version is invalid");
  if (ops_target_kind(request.target) == OpsTargetKind::kubernetes &&
      !logs(request.operation) && !value.source_version)
    return fail(Code::invalid_payload,
                "Kubernetes structured observations require a source version");
  if (value.payload.valueless_by_exception() ||
      value.payload.index() != payload_index(request.operation))
    return fail(Code::invalid_payload,
                "Ops payload does not match its operation");
  return {};
}
class Validator {
 public:
  Validator(const OpsObservationRequest& request, const OpsObservation& value)
      : m_request(request), m_value(value) {}
  auto run() -> std::expected<OpsObservationUsage, OpsObservationError> {
    // 4096 accounts conservatively for the entire bounded captured request,
    // timestamps, source version and completeness metadata. Each payload/row
    // adds 128 fixed bytes plus its exact bounded UTF-8 field bytes below.
    if (!bytes(4096))
      return std::unexpected(
          OpsObservationError{Code::resource_exhausted,
                              "Ops evidence exceeds its neutral byte budget"});
    auto valid = std::visit([&](const auto& payload) { return check(payload); },
                            m_value.payload);
    if (!valid) return std::unexpected(valid.error());
    return m_usage;
  }

 private:
  auto bytes(std::size_t amount) -> bool {
    if (amount >
        ops_observation_maximum_evidence_bytes - m_usage.evidence_bytes)
      return false;
    m_usage.evidence_bytes += amount;
    return true;
  }
  auto row() -> bool {
    if (m_usage.entries >= m_request.limits.maximum_entries || !bytes(128))
      return false;
    ++m_usage.entries;
    return true;
  }
  auto resource(const LinuxServiceIdentity& value) -> bool {
    if (value.unit_name.size() > 255) return false;
    return validate_ops_resource_identity(m_request.target,
                                          OpsResourceIdentity{value})
        .has_value();
  }
  auto resource(const KubernetesObservedResource& value) -> bool {
    if (!known(value.kind, OpsWorkloadKind::cron_job) ||
        value.namespace_name.size() > 63 || value.name.size() > 253)
      return false;
    return validate_ops_resource_identity(
               m_request.target,
               KubernetesPodIdentity{
                   value.namespace_name, value.name, value.uid, {}})
        .has_value();
  }
  auto charge(const LinuxServiceIdentity& value) -> bool {
    return bytes(value.unit_name.size()) &&
           (!value.invocation_id || bytes(value.invocation_id->value().size()));
  }
  auto charge(const KubernetesObservedResource& value) -> bool {
    return bytes(value.namespace_name.size()) && bytes(value.name.size()) &&
           bytes(value.uid.value().size());
  }
  auto charge(const KubernetesPodIdentity& value) -> bool {
    return bytes(value.namespace_name.size()) && bytes(value.name.size()) &&
           bytes(value.uid.value().size()) &&
           (!value.container ||
            (bytes(value.container->name.size()) &&
             bytes(value.container->runtime_identity.size())));
  }
  auto bounds() -> Status {
    return fail(Code::resource_exhausted,
                "Ops evidence exceeds aggregate entry or byte bounds");
  }
  auto malformed() -> Status {
    return fail(Code::invalid_payload,
                "Ops observation contains invalid status or numeric fields");
  }
  auto source_error() -> Status {
    return fail(Code::source_mismatch,
                "Ops observation source identity does not match its request");
  }
  auto check(const LinuxHealthObservation& value) -> Status {
    if (!known(value.health, OpsHealthState::unavailable) ||
        !number(value.kernel_uptime_seconds) || !count(value.active_services) ||
        !count(value.failed_services))
      return malformed();
    if (value.memory &&
        ((value.memory->scope != OpsMemoryScope::kernel &&
          value.memory->scope != OpsMemoryScope::cgroup) ||
         value.memory->total_bytes > maximum_counter ||
         value.memory->available_bytes > value.memory->total_bytes))
      return malformed();
    const bool factual = value.kernel_uptime_seconds || value.memory ||
                         value.active_services || value.failed_services;
    if (!factual &&
        (m_value.completeness == OpsObservationCompleteness::complete ||
         value.health == OpsHealthState::healthy))
      return malformed();
    if (value.health == OpsHealthState::healthy &&
        value.failed_services.value_or(0) != 0)
      return malformed();
    return row() ? Status{} : bounds();
  }
  auto service(const LinuxServiceObservation& value) -> Status {
    if (!resource(value.identity)) return source_error();
    if (!known(value.state, OpsServiceState::failed) ||
        !known(value.reason, OpsObservationReason::unavailable) ||
        !exit_status(value.exit_status) || !count(value.restart_count))
      return malformed();
    return row() && charge(value.identity) ? Status{} : bounds();
  }
  auto check(const LinuxServiceObservation& value) -> Status {
    const auto* expected =
        std::get_if<LinuxServiceIdentity>(&m_request.resource);
    if (expected == nullptr || value.identity != *expected)
      return source_error();
    return service(value);
  }
  auto check(const LinuxServicesObservation& value) -> Status {
    if (value.services.size() > m_request.limits.maximum_entries)
      return bounds();
    std::set<std::string_view> names;
    for (const auto& entry : value.services) {
      if (auto valid = service(entry); !valid) return valid;
      if (!names.insert(entry.identity.unit_name).second) return source_error();
    }
    return {};
  }
  auto check(const KubernetesWorkloadsObservation& value) -> Status {
    if (value.workloads.size() > m_request.limits.maximum_entries)
      return bounds();
    std::set<std::string_view> uids;
    std::set<std::pair<OpsWorkloadKind, std::string_view>> names;
    for (const auto& entry : value.workloads) {
      if (!resource(entry.identity)) return source_error();
      if (!known(entry.health, OpsHealthState::unavailable) ||
          !count(entry.desired_count) || !count(entry.ready_count) ||
          !count(entry.observed_count) ||
          (entry.ready_count && entry.observed_count &&
           *entry.ready_count > *entry.observed_count))
        return malformed();
      if (!row() || !charge(entry.identity)) return bounds();
      if (!uids.insert(entry.identity.uid.value()).second ||
          !names.emplace(entry.identity.kind, entry.identity.name).second)
        return source_error();
    }
    return {};
  }
  auto container(const KubernetesContainerObservation& value) -> Status {
    if (!label(value.name) ||
        (value.runtime_identity && !text(*value.runtime_identity, 512)))
      return source_error();
    if (!known(value.state, OpsContainerState::terminated) ||
        !known(value.readiness, OpsReadiness::ready) ||
        !known(value.reason, OpsObservationReason::unavailable) ||
        !count(value.restart_count) || !exit_status(value.exit_status) ||
        (value.readiness == OpsReadiness::ready &&
         value.state != OpsContainerState::running))
      return malformed();
    return row() && bytes(value.name.size()) &&
                   (!value.runtime_identity ||
                    bytes(value.runtime_identity->size()))
               ? Status{}
               : bounds();
  }
  auto check(const KubernetesPodObservation& value) -> Status {
    const auto* expected =
        std::get_if<KubernetesPodIdentity>(&m_request.resource);
    if (expected == nullptr || value.identity != *expected)
      return source_error();
    if (!known(value.phase, OpsPodPhase::failed)) return malformed();
    if (value.containers.size() > m_request.limits.maximum_entries || !row() ||
        !charge(value.identity))
      return bounds();
    std::set<std::string_view> names;
    std::set<std::string_view> runtime_ids;
    bool requested_container = !expected->container;
    for (const auto& entry : value.containers) {
      if (auto valid = container(entry); !valid) return valid;
      if (!names.insert(entry.name).second ||
          (entry.runtime_identity &&
           !runtime_ids.insert(*entry.runtime_identity).second))
        return source_error();
      if (expected->container && entry.name == expected->container->name &&
          entry.runtime_identity == expected->container->runtime_identity)
        requested_container = true;
    }
    return requested_container ? Status{} : source_error();
  }
  auto event_source(const KubernetesEventObservation& value) -> bool {
    if (!resource(value.regarding) || !text(value.event_uid.value(), 128))
      return false;
    const auto* expected =
        std::get_if<KubernetesPodIdentity>(&m_request.resource);
    return expected == nullptr ||
           (value.regarding.kind == OpsWorkloadKind::pod &&
            value.regarding.namespace_name == expected->namespace_name &&
            value.regarding.name == expected->name &&
            value.regarding.uid == expected->uid);
  }
  auto event(const KubernetesEventObservation& value) -> Status {
    if (!event_source(value)) return source_error();
    if (!known(value.severity, OpsEventSeverity::warning) ||
        !known(value.reason, OpsObservationReason::unavailable) ||
        value.occurrences == 0 || value.occurrences > maximum_counter)
      return malformed();
    if ((value.first_observed_at && !timestamp(*value.first_observed_at)) ||
        (value.last_observed_at && !timestamp(*value.last_observed_at)) ||
        (value.first_observed_at && value.last_observed_at &&
         *value.first_observed_at > *value.last_observed_at))
      return fail(Code::invalid_timestamp,
                  "Ops source event timestamps are invalid");
    return row() && charge(value.regarding) &&
                   bytes(value.event_uid.value().size())
               ? Status{}
               : bounds();
  }
  auto check(const KubernetesEventsObservation& value) -> Status {
    if (value.events.size() > m_request.limits.maximum_entries) return bounds();
    std::set<std::string_view> uids;
    for (const auto& entry : value.events) {
      if (auto valid = event(entry); !valid) return valid;
      if (!uids.insert(entry.event_uid.value()).second) return source_error();
    }
    return {};
  }
  auto check(const OpsLogObservation& value) -> Status {
    if (value.source != m_request.resource ||
        !validate_ops_resource_identity(m_request.target, value.source, true))
      return source_error();
    if (value.lines.size() > m_request.limits.maximum_log_lines ||
        value.lines.size() > m_request.limits.maximum_entries)
      return bounds();
    const auto charged = std::visit(
        [&](const auto& source) {
          if constexpr (std::is_same_v<std::decay_t<decltype(source)>,
                                       std::monostate>)
            return false;
          else
            return charge(source);
        },
        value.source);
    if (!charged || !bytes(128)) return bounds();
    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
        m_request.limits.maximum_log_age);
    std::uint64_t log_bytes{};
    for (const auto& entry : value.lines) {
      if (!timestamp(entry.timestamp) ||
          entry.timestamp > m_value.completed_at ||
          m_value.completed_at - entry.timestamp > age)
        return fail(Code::invalid_timestamp,
                    "Ops log timestamp is outside the requested window or "
                    "source clock is skewed");
      if (entry.text.size() > m_request.limits.maximum_log_bytes - log_bytes)
        return bounds();
      if (!text(entry.text,
                static_cast<std::size_t>(m_request.limits.maximum_log_bytes),
                true))
        return malformed();
      log_bytes += entry.text.size();
      if (!row() || !bytes(entry.text.size())) return bounds();
    }
    return {};
  }
  const OpsObservationRequest& m_request;
  const OpsObservation& m_value;
  OpsObservationUsage m_usage;
};
} // namespace
auto validate_recorded_ops_observation(const OpsObservation& observation)
    -> std::expected<OpsObservationUsage, OpsObservationError> {
  try {
    if (!validate_recorded_ops_request(observation.request))
      return std::unexpected(OpsObservationError{
          Code::invalid_request, "Recorded Ops request is invalid"});
    if (auto valid = envelope(observation.request, observation); !valid)
      return std::unexpected(valid.error());
    return Validator{observation.request, observation}.run();
  } catch (...) {
    return std::unexpected(OpsObservationError{
        Code::internal_failure, "Recorded Ops observation validation failed"});
  }
}
auto validate_ops_observation(const OpsObservationAuthority& authority,
                              const OpsObservationRequest& expected_request,
                              const OpsObservation& observation)
    -> std::expected<OpsObservationUsage, OpsObservationError> {
  try {
    if (!authority.validate(expected_request))
      return std::unexpected(OpsObservationError{
          Code::invalid_request,
          "Ops observation request is outside its captured authority"});
    if (auto valid = envelope(expected_request, observation); !valid)
      return std::unexpected(valid.error());
    return Validator{expected_request, observation}.run();
  } catch (...) {
    return std::unexpected(OpsObservationError{
        Code::internal_failure, "Ops observation validation failed"});
  }
}
} // namespace aiforge::domain
