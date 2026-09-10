#include "kubernetes_observation_projection_internal.hpp"

#include <aiforge/detail/utf8_text.hpp>
#include <algorithm>
#include <chrono>
#include <limits>
#include <new>
#include <set>
#include <utility>

namespace aiforge::adapters::kubernetes_projection_detail {
namespace {
using namespace domain;
auto plain(std::string_view value, std::size_t maximum) -> bool {
  return !value.empty() && value.size() <= maximum &&
         detail::is_safe_utf8_text(value) &&
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
auto name(std::string_view value) -> bool {
  if (value.empty() || value.size() > 253) return false;
  while (!value.empty()) {
    const auto dot = value.find('.');
    if (!label(value.substr(0, dot))) return false;
    if (dot == std::string_view::npos) return true;
    value.remove_prefix(dot + 1);
  }
  return false;
}
auto validate_metadata(const Metadata& metadata,
                       std::string_view namespace_name) -> void {
  require(label(metadata.namespace_name) && name(metadata.name) &&
          plain(metadata.uid, 128));
  require(metadata.namespace_name == namespace_name, Failure::source_changed);
  require(metadata.version.empty() || plain(metadata.version, 128));
}
auto uid(std::string value) -> OpsResourceUid {
  auto result = OpsResourceUid::from(std::move(value));
  require(result.has_value());
  return std::move(*result);
}
auto resource(const Metadata& metadata, OpsWorkloadKind kind)
    -> KubernetesObservedResource {
  return {kind, metadata.namespace_name, metadata.name, uid(metadata.uid)};
}
auto reason(std::string_view value) -> OpsObservationReason {
  if (value == "CrashLoopBackOff") return OpsObservationReason::crash_loop;
  if (value == "ImagePullBackOff" || value == "ErrImagePull")
    return OpsObservationReason::image_pull_failed;
  if (value == "OOMKilled") return OpsObservationReason::out_of_memory;
  if (value == "FailedScheduling")
    return OpsObservationReason::scheduling_failed;
  if (value == "Completed") return OpsObservationReason::completed;
  if (value == "Error") return OpsObservationReason::failed_exit;
  return OpsObservationReason::unknown;
}
auto phase(std::string_view value) -> OpsPodPhase {
  if (value == "Pending") return OpsPodPhase::pending;
  if (value == "Running") return OpsPodPhase::running;
  if (value == "Succeeded") return OpsPodPhase::succeeded;
  if (value == "Failed") return OpsPodPhase::failed;
  return OpsPodPhase::unknown;
}
auto validate_pod(const Pod& pod, std::string_view namespace_name, bool exact)
    -> void {
  require((pod.kind.empty() && !exact) || pod.kind == "Pod");
  require((pod.api_version.empty() && !exact) || pod.api_version == "v1");
  validate_metadata(pod.metadata, namespace_name);
  if (exact) require(plain(pod.metadata.version, 128));
}
auto health(const Pod& pod) -> OpsHealthState {
  if (pod.phase == "Failed") return OpsHealthState::unhealthy;
  if (pod.phase != "Running" || !pod.ready || pod.metadata.deleting)
    return OpsHealthState::unknown;
  return *pod.ready ? OpsHealthState::healthy : OpsHealthState::degraded;
}
auto workloads(const Document& document, std::string_view namespace_name,
               std::stop_token stop) -> KubernetesWorkloadsObservation {
  KubernetesWorkloadsObservation result;
  for (const auto& pod : document.pods) {
    require(!stop.stop_requested(), Failure::cancelled);
    validate_pod(pod, namespace_name, false);
    KubernetesWorkloadObservation row{
        resource(pod.metadata, OpsWorkloadKind::pod), health(pod), {}, {}, 1};
    if (pod.ready) row.ready_count = *pod.ready ? 1U : 0U;
    result.workloads.push_back(std::move(row));
  }
  return result;
}
auto counter(std::optional<std::int64_t> value, bool positive = false)
    -> std::optional<std::uint32_t> {
  if (!value) return {};
  require(*value >= (positive ? 1 : 0) &&
          *value <= std::numeric_limits<std::int32_t>::max());
  return static_cast<std::uint32_t>(*value);
}
auto container(const Container& input) -> KubernetesContainerObservation {
  require(label(input.name));
  KubernetesContainerObservation result{input.name,
                                        {},
                                        input.state,
                                        OpsReadiness::unknown,
                                        reason(input.reason),
                                        counter(input.restarts),
                                        {}};
  if (!input.runtime.empty()) {
    require(plain(input.runtime, 512));
    result.runtime_identity = input.runtime;
  }
  if (input.ready)
    result.readiness =
        *input.ready ? OpsReadiness::ready : OpsReadiness::not_ready;
  if (input.exit_status) {
    require(*input.exit_status >= 0 && *input.exit_status <= 255);
    result.exit_status = static_cast<std::int32_t>(*input.exit_status);
  }
  return result;
}
auto append_containers(const Pod& input, std::size_t category,
                       KubernetesPodObservation& output,
                       std::set<std::string_view>& names, std::stop_token stop)
    -> void {
  const auto& declared = input.declared[category];
  const auto& statuses = input.statuses[category];
  std::set<std::string_view> status_names;
  for (const auto& status : statuses) {
    require(std::ranges::find(declared, status.name) != declared.end());
    require(status_names.insert(status.name).second);
    (void)container(
        status); // Validate all reported status, not just the selected one.
  }
  for (const auto& declared_name : declared) {
    require(!stop.stop_requested(), Failure::cancelled);
    require(label(declared_name) && names.insert(declared_name).second);
    const auto status =
        std::ranges::find(statuses, declared_name, &Container::name);
    if (status == statuses.end())
      output.containers.push_back(
          KubernetesContainerObservation{declared_name});
    else
      output.containers.push_back(container(*status));
  }
}
auto pod_health(const Document& document, const OpsObservationRequest& request,
                std::string_view namespace_name, std::stop_token stop)
    -> KubernetesPodObservation {
  require(document.pods.size() == 1);
  const auto& input = document.pods.front();
  validate_pod(input, namespace_name, true);
  const auto* expected = std::get_if<KubernetesPodIdentity>(&request.resource);
  require(expected != nullptr);
  require(input.metadata.name == expected->name &&
              input.metadata.uid == expected->uid.value(),
          Failure::source_changed);
  require(input.spec_seen && input.regular_seen && !input.declared[0].empty());
  KubernetesPodObservation result{*expected, phase(input.phase), {}};
  std::set<std::string_view> names;
  for (std::size_t category{}; category < input.declared.size(); ++category)
    append_containers(input, category, result, names, stop);
  return result;
}
auto digits(std::string_view value) -> unsigned {
  require(!value.empty());
  unsigned result{};
  for (const char c : value) {
    require(c >= '0' && c <= '9');
    result = result * 10U + static_cast<unsigned>(c - '0');
  }
  return result;
}
auto zone_offset(std::string_view value) -> std::chrono::minutes {
  if (value == "Z" || value == "z") return {};
  require(value.size() == 6 && (value[0] == '+' || value[0] == '-') &&
          value[3] == ':');
  const auto hours = digits(value.substr(1, 2));
  const auto minutes = digits(value.substr(4, 2));
  require(hours <= 23 && minutes <= 59);
  const auto total = static_cast<int>((hours * 60U) + minutes);
  return std::chrono::minutes{value[0] == '-' ? -total : total};
}
auto timestamp(std::string_view value) -> std::optional<EventTimestamp> {
  if (value.empty()) return {};
  require(value.size() >= 20 && value[4] == '-' && value[7] == '-' &&
          (value[10] == 'T' || value[10] == 't') && value[13] == ':' &&
          value[16] == ':');
  const auto year = digits(value.substr(0, 4));
  const auto month = digits(value.substr(5, 2));
  const auto day = digits(value.substr(8, 2));
  const auto hour = digits(value.substr(11, 2));
  const auto minute = digits(value.substr(14, 2));
  const auto second = digits(value.substr(17, 2));
  const std::chrono::year_month_day date{
      std::chrono::year{static_cast<int>(year)}, std::chrono::month{month},
      std::chrono::day{day}};
  require(date.ok() && hour <= 23 && minute <= 59 && second <= 59);
  std::size_t offset = 19;
  unsigned millis{};
  if (value[offset] == '.') {
    const auto begin = ++offset;
    while (offset < value.size() && value[offset] >= '0' &&
           value[offset] <= '9')
      ++offset;
    const auto fraction = value.substr(begin, offset - begin);
    require(!fraction.empty() && fraction.size() <= 9);
    for (std::size_t index{}; index < 3; ++index)
      millis =
          millis * 10U + (index < fraction.size()
                              ? static_cast<unsigned>(fraction[index] - '0')
                              : 0U);
  }
  const auto result =
      std::chrono::sys_days{date} + std::chrono::hours{hour} +
      std::chrono::minutes{minute} + std::chrono::seconds{second} +
      std::chrono::milliseconds{millis} - zone_offset(value.substr(offset));
  require(result.time_since_epoch().count() >= 0);
  return result;
}
struct Kind {
  OpsWorkloadKind kind;
  std::string_view api_version;
};
auto kind(std::string_view value) -> std::optional<Kind> {
  if (value == "Pod") return Kind{OpsWorkloadKind::pod, "v1"};
  if (value == "Deployment")
    return Kind{OpsWorkloadKind::deployment, "apps/v1"};
  if (value == "StatefulSet")
    return Kind{OpsWorkloadKind::stateful_set, "apps/v1"};
  if (value == "DaemonSet") return Kind{OpsWorkloadKind::daemon_set, "apps/v1"};
  if (value == "Job") return Kind{OpsWorkloadKind::job, "batch/v1"};
  if (value == "CronJob") return Kind{OpsWorkloadKind::cron_job, "batch/v1"};
  return {};
}
auto validate_event(const Event& event, const OpsObservationRequest& request,
                    std::string_view namespace_name) -> std::optional<Kind> {
  require(event.api_version.empty() || event.api_version == "v1");
  require(event.kind.empty() || event.kind == "Event");
  validate_metadata(event.metadata, namespace_name);
  validate_metadata(event.regarding, namespace_name);
  require(plain(event.regarding_kind, 128));
  require(event.regarding_api.empty() || plain(event.regarding_api, 128));
  auto resource_kind = kind(event.regarding_kind);
  if (resource_kind && !event.regarding_api.empty() &&
      event.regarding_api != resource_kind->api_version)
    resource_kind.reset();
  if (const auto* expected =
          std::get_if<KubernetesPodIdentity>(&request.resource))
    require(resource_kind && resource_kind->kind == OpsWorkloadKind::pod &&
                event.regarding.name == expected->name &&
                event.regarding.uid == expected->uid.value(),
            Failure::source_changed);
  return resource_kind;
}
auto event_row(const Event& input, OpsWorkloadKind resource_kind)
    -> KubernetesEventObservation {
  const auto first = timestamp(input.first_time);
  const auto last = timestamp(input.last_time);
  const auto initial = timestamp(input.event_time);
  const auto series_time = timestamp(input.series_time);
  const auto count = counter(input.count, true);
  const auto series_count = counter(input.series_count, true);
  const auto occurrences = input.series_seen ? series_count : count;
  if (!occurrences) reject();
  const auto severity = input.type == "Normal"    ? OpsEventSeverity::normal
                        : input.type == "Warning" ? OpsEventSeverity::warning
                                                  : OpsEventSeverity::unknown;
  return {uid(input.metadata.uid),
          resource(input.regarding, resource_kind),
          severity,
          reason(input.reason),
          initial ? initial : first,
          input.series_seen ? series_time : last,
          *occurrences};
}
auto events(const Document& document, const OpsObservationRequest& request,
            std::string_view namespace_name, std::uint64_t& unsupported,
            std::stop_token stop) -> KubernetesEventsObservation {
  KubernetesEventsObservation result;
  std::set<std::string_view> uids;
  for (const auto& input : document.events) {
    require(!stop.stop_requested(), Failure::cancelled);
    const auto resource_kind = validate_event(input, request, namespace_name);
    require(uids.insert(input.metadata.uid).second);
    // Even omitted rows must not conceal malformed numbers or timestamps.
    (void)counter(input.count, true);
    (void)counter(input.series_count, true);
    const auto first = timestamp(input.event_time.empty() ? input.first_time
                                                          : input.event_time);
    const auto last =
        timestamp(input.series_seen ? input.series_time : input.last_time);
    (void)timestamp(input.first_time);
    (void)timestamp(input.last_time);
    (void)timestamp(input.event_time);
    (void)timestamp(input.series_time);
    require(!first || !last || *first <= *last);
    if (!resource_kind ||
        !(input.series_seen ? input.series_count : input.count)) {
      ++unsupported;
      continue;
    }
    result.events.push_back(event_row(input, resource_kind->kind));
  }
  return result;
}
auto project(const OpsObservationRequest& request, EventTimestamp started,
             EventTimestamp completed, std::string_view bytes,
             std::stop_token stop) -> OpsObservation {
  require(!stop.stop_requested(), Failure::cancelled);
  require(validate_recorded_ops_request(request).has_value());
  const auto* target =
      std::get_if<KubernetesOpsIdentity>(&request.target.identity);
  require(target != nullptr);
  require(request.operation == OpsObservationOperation::kubernetes_workloads ||
              request.operation ==
                  OpsObservationOperation::kubernetes_pod_health ||
              request.operation == OpsObservationOperation::kubernetes_events,
          Failure::unsupported);
  require(started.time_since_epoch().count() >= 0 && completed >= started);
  require(bytes.size() <= input_limit &&
              bytes.size() <= request.limits.maximum_bytes,
          Failure::resource_exhausted);
  auto document =
      parse(bytes, request.operation, request.limits.maximum_entries, stop);
  OpsObservation result{
      request, started, completed, OpsObservationCompleteness::complete,
      0,       0,       {},        LinuxHealthObservation{}};
  if (request.operation == OpsObservationOperation::kubernetes_pod_health) {
    result.payload =
        pod_health(document, request, target->namespace_name, stop);
    result.source_version = document.pods.front().metadata.version;
  } else {
    const auto expected_kind =
        request.operation == OpsObservationOperation::kubernetes_workloads
            ? "PodList"
            : "EventList";
    require(document.api_version == "v1" && document.kind == expected_kind &&
            document.items_seen && plain(document.version, 128));
    result.source_version = document.version;
    if (request.operation == OpsObservationOperation::kubernetes_workloads)
      result.payload = workloads(document, target->namespace_name, stop);
    else
      result.payload = events(document, request, target->namespace_name,
                              result.unsupported_entries, stop);
    if (document.continued || result.unsupported_entries != 0)
      result.completeness = OpsObservationCompleteness::partial;
    result.omitted_entries = document.continued
                                 ? std::nullopt
                                 : std::optional{result.unsupported_entries};
  }
  require(!stop.stop_requested(), Failure::cancelled);
  const auto checked = validate_recorded_ops_observation(result);
  if (!checked)
    reject(checked.error().code == OpsObservationErrorCode::resource_exhausted
               ? Failure::resource_exhausted
               : Failure::invalid_result);
  return result;
}
} // namespace
} // namespace aiforge::adapters::kubernetes_projection_detail

namespace aiforge::adapters {
auto project_kubernetes_observation(
    const domain::OpsObservationRequest& request,
    domain::EventTimestamp started_at, domain::EventTimestamp completed_at,
    std::string_view response_json, std::stop_token stop) noexcept
    -> std::expected<domain::OpsObservation,
                     runtime::OpsObservationSourceError> {
  try {
    return kubernetes_projection_detail::project(
        request, started_at, completed_at, response_json, stop);
  } catch (const kubernetes_projection_detail::Rejected& failure) {
    return std::unexpected(failure.failure);
  } catch (const std::bad_alloc&) {
    return std::unexpected(
        runtime::OpsObservationSourceError::resource_exhausted);
  } catch (...) {
    return std::unexpected(
        runtime::OpsObservationSourceError::internal_failure);
  }
}
} // namespace aiforge::adapters
