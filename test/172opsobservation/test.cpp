// Failure matrix written before implementation:
// Exact captured request/owner/session/target/source identities must match.
// Payload kind must match the closed operation and the captured authority.
// Reject invalid enums, malformed UTF-8, oversized names and missing versions.
// Reject duplicate resources/containers/events and cross-namespace identities.
// Bound every collection and the aggregate rows/neutral evidence bytes.
// Reject integer overflow and contradictory readiness/memory/status counts.
// Require ordered receipt/source event pairs; preserve nonmonotonic log order.
// Complete observations cannot hide omitted or unsupported entries.
// Log excerpts require exact consented source and time/line/byte bounds.
// Missing data remains unknown; no collection or replacement is inferred.
// No transport, secret discovery, redaction claims, persistence or UI here.

#include <aiforge/domain/ops_observation.hpp>
#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace {
using namespace aiforge::domain;
template <class T> auto id(std::string value) -> T {
  return T::from(std::move(value)).value();
}
auto at(std::int64_t value) -> EventTimestamp {
  return EventTimestamp{std::chrono::milliseconds{value}};
}
auto service() -> LinuxServiceIdentity {
  return {"test.service", id<OpsResourceUid>("invocation")};
}
auto pod(bool container = false) -> KubernetesPodIdentity {
  KubernetesPodIdentity result{
      "chosen", "test-pod", id<OpsResourceUid>("pod-uid"), {}};
  if (container)
    result.container =
        KubernetesContainerIdentity{"main", "containerd://runtime"};
  return result;
}
auto pod_resource() -> KubernetesObservedResource {
  return {OpsWorkloadKind::pod, "chosen", "test-pod",
          id<OpsResourceUid>("pod-uid")};
}
struct Fixture {
  OpsObservationAuthoritySpec specification;
  OpsObservationAuthority authority;
  OpsObservationRequest request;
  explicit Fixture(OpsObservationOperation operation)
      : specification(grant(operation)),
        authority(OpsObservationAuthority::create(specification).value()),
        request{specification.owner_id,
                specification.session_id,
                id<OpsRequestId>("request"),
                specification.target,
                1,
                operation,
                {},
                1,
                {}} {
    if (operation == OpsObservationOperation::linux_service_health ||
        operation == OpsObservationOperation::linux_service_logs)
      request.resource = service();
    if (operation == OpsObservationOperation::kubernetes_pod_health)
      request.resource = pod();
    if (operation == OpsObservationOperation::kubernetes_pod_logs)
      request.resource = pod(true);
  }
  static auto grant(OpsObservationOperation operation)
      -> OpsObservationAuthoritySpec {
    const bool kube =
        operation >= OpsObservationOperation::kubernetes_workloads;
    OpsTargetBinding target{
        id<OpsTargetId>("target"), id<OpsConfigurationRevision>("revision"),
        LinuxOpsIdentity{LinuxExecutionScope::container,
                         "12345678-1234-1234-1234-123456789abc", 42, 43}};
    if (kube)
      target.identity = KubernetesOpsIdentity{
          "context", "chosen", {"api.example.test", 6443}, "ca-public"};
    return {id<OpsOwnerId>("owner"),
            id<SessionId>("session"),
            target,
            1,
            {operation},
            {1,
             true,
             {kube ? OpsResourceIdentity{pod(true)}
                   : OpsResourceIdentity{service()}}},
            {}};
  }
  auto observation() -> OpsObservation {
    OpsObservation result{
        request, at(1000000), at(1000100), OpsObservationCompleteness::complete,
        0,       0,           {},          LinuxHealthObservation{}};
    if (ops_target_kind(request.target) == OpsTargetKind::kubernetes)
      result.source_version = "resource-version";
    switch (request.operation) {
      case OpsObservationOperation::linux_health:
        result.payload = LinuxHealthObservation{
            OpsHealthState::healthy, 12,
            LinuxMemoryObservation{OpsMemoryScope::cgroup, 1000, 500}, 1, 0};
        break;
      case OpsObservationOperation::linux_services:
        result.payload = LinuxServicesObservation{
            {LinuxServiceObservation{service(),
                                     OpsServiceState::active,
                                     OpsObservationReason::none,
                                     {},
                                     0}}};
        break;
      case OpsObservationOperation::linux_service_health:
        result.payload = LinuxServiceObservation{service(),
                                                 OpsServiceState::active,
                                                 OpsObservationReason::none,
                                                 {},
                                                 0};
        break;
      case OpsObservationOperation::kubernetes_workloads:
        result.payload =
            KubernetesWorkloadsObservation{{KubernetesWorkloadObservation{
                pod_resource(), OpsHealthState::healthy, 1, 1, 1}}};
        break;
      case OpsObservationOperation::kubernetes_pod_health:
        result.payload = KubernetesPodObservation{pod(),
                                                  OpsPodPhase::running,
                                                  {{"main",
                                                    "containerd://runtime",
                                                    OpsContainerState::running,
                                                    OpsReadiness::ready,
                                                    OpsObservationReason::none,
                                                    0,
                                                    {}}}};
        break;
      case OpsObservationOperation::kubernetes_events:
        result.payload =
            KubernetesEventsObservation{{KubernetesEventObservation{
                id<OpsResourceUid>("event-uid"), pod_resource(),
                OpsEventSeverity::normal, OpsObservationReason::completed,
                at(999000), at(999100), 1}}};
        break;
      case OpsObservationOperation::linux_service_logs:
      case OpsObservationOperation::kubernetes_pod_logs:
        result.source_version.reset();
        result.payload = OpsLogObservation{
            request.resource, {{at(999000), "ordinary application line"}}};
        break;
    }
    return result;
  }
  auto validate(const OpsObservation& result) const {
    return validate_ops_observation(authority, request, result);
  }
};
} // namespace

TEST_CASE("Ops evidence refuses forged captured request and wrong payload "
          "operation") {
  Fixture f{OpsObservationOperation::linux_health};
  for (unsigned bad = 0; bad < 7; ++bad) {
    auto value = f.observation();
    switch (bad) {
      case 0: value.request.owner_id = id<OpsOwnerId>("other"); break;
      case 1: value.request.session_id = id<SessionId>("other"); break;
      case 2: value.request.request_id = id<OpsRequestId>("other"); break;
      case 3: value.request.selection_generation++; break;
      case 4:
        value.request.target.configuration_revision =
            id<OpsConfigurationRevision>("other");
        break;
      case 5: value.request.log_policy_revision++; break;
      case 6: value.payload = LinuxServicesObservation{}; break;
    }
    CHECK_FALSE(f.validate(value));
  }
  auto other = f.specification;
  other.session_id = id<SessionId>("other");
  auto authority = OpsObservationAuthority::create(other);
  REQUIRE(authority);
  CHECK_FALSE(validate_ops_observation(*authority, f.request, f.observation()));
}
TEST_CASE("Ops evidence rejects malformed time completeness versions and "
          "unknown enums") {
  Fixture f{OpsObservationOperation::kubernetes_workloads};
  for (unsigned bad = 0; bad < 9; ++bad) {
    auto value = f.observation();
    switch (bad) {
      case 0: value.started_at = at(-1); break;
      case 1:
        value.completed_at = value.started_at - std::chrono::milliseconds{1};
        break;
      case 2:
        value.completeness = static_cast<OpsObservationCompleteness>(99);
        break;
      case 3: value.omitted_entries = 1; break;
      case 4: value.omitted_entries.reset(); break;
      case 5: value.unsupported_entries = 1; break;
      case 6: value.source_version.reset(); break;
      case 7: value.source_version = std::string(129, 'x'); break;
      case 8:
        std::get<KubernetesWorkloadsObservation>(value.payload)
            .workloads.front()
            .health = static_cast<OpsHealthState>(99);
        break;
    }
    CHECK_FALSE(f.validate(value));
  }
  auto value = f.observation();
  value.completeness = OpsObservationCompleteness::partial;
  value.omitted_entries.reset();
  value.unsupported_entries = 1;
  REQUIRE(f.validate(value));
  value.unsupported_entries = std::numeric_limits<std::uint64_t>::max();
  CHECK_FALSE(f.validate(value));
}
TEST_CASE("Ops evidence retains scoped factual Linux metrics without false "
          "empty health") {
  Fixture f{OpsObservationOperation::linux_health};
  for (unsigned bad = 0; bad < 5; ++bad) {
    auto value = f.observation();
    auto& health = std::get<LinuxHealthObservation>(value.payload);
    switch (bad) {
      case 0: health.memory->scope = OpsMemoryScope::unknown; break;
      case 1: health.memory->available_bytes = 1001; break;
      case 2:
        health.kernel_uptime_seconds =
            std::numeric_limits<std::uint64_t>::max();
        break;
      case 3: health.failed_services = 1; break;
      case 4: health = LinuxHealthObservation{}; break;
    }
    CHECK_FALSE(f.validate(value));
  }
  auto value = f.observation();
  value.completeness = OpsObservationCompleteness::partial;
  value.omitted_entries.reset();
  value.payload = LinuxHealthObservation{};
  REQUIRE(f.validate(value));
}
TEST_CASE("Ops lists and exact reads reject replaced foreign and duplicate "
          "resource identities") {
  Fixture f{OpsObservationOperation::kubernetes_workloads};
  auto value = f.observation();
  auto& rows =
      std::get<KubernetesWorkloadsObservation>(value.payload).workloads;
  rows.push_back(rows.front());
  CHECK_FALSE(f.validate(value));
  rows.pop_back();
  rows.front().identity.namespace_name = "foreign";
  CHECK_FALSE(f.validate(value));
  value = f.observation();
  std::get<KubernetesWorkloadsObservation>(value.payload)
      .workloads.front()
      .identity.kind = static_cast<OpsWorkloadKind>(99);
  CHECK_FALSE(f.validate(value));
  Fixture exact{OpsObservationOperation::linux_service_health};
  auto single = exact.observation();
  std::get<LinuxServiceObservation>(single.payload).identity.invocation_id =
      id<OpsResourceUid>("restarted");
  CHECK_FALSE(exact.validate(single));
  Fixture pod_fixture{OpsObservationOperation::kubernetes_pod_health};
  auto pod_value = pod_fixture.observation();
  std::get<KubernetesPodObservation>(pod_value.payload).identity.uid =
      id<OpsResourceUid>("replacement");
  CHECK_FALSE(pod_fixture.validate(pod_value));
}
TEST_CASE("Ops container status and counts remain bounded without rejecting "
          "rollout surge") {
  Fixture f{OpsObservationOperation::kubernetes_pod_health};
  for (unsigned bad = 0; bad < 5; ++bad) {
    auto value = f.observation();
    auto& containers =
        std::get<KubernetesPodObservation>(value.payload).containers;
    switch (bad) {
      case 0: containers.push_back(containers.front()); break;
      case 1: containers.front().state = OpsContainerState::waiting; break;
      case 2: containers.front().exit_status = -1; break;
      case 3:
        containers.front().restart_count =
            std::numeric_limits<std::uint32_t>::max();
        break;
      case 4:
        containers.front().name = std::string(1, static_cast<char>(0xff));
        break;
    }
    CHECK_FALSE(f.validate(value));
  }
  Fixture workloads{OpsObservationOperation::kubernetes_workloads};
  auto value = workloads.observation();
  auto& row =
      std::get<KubernetesWorkloadsObservation>(value.payload).workloads.front();
  row.desired_count = 2;
  row.ready_count = 3;
  row.observed_count = 3;
  REQUIRE(workloads.validate(value));
  row.observed_count = 2;
  CHECK_FALSE(workloads.validate(value));
}
TEST_CASE("Ops evidence enforces aggregate neutral budgets independently of "
          "private input") {
  Fixture f{OpsObservationOperation::kubernetes_pod_health};
  auto value = f.observation();
  auto& rows = std::get<KubernetesPodObservation>(value.payload).containers;
  const auto original = rows.front();
  rows.clear();
  for (unsigned i = 0; i < 110; ++i) {
    auto row = original;
    row.name = "container-" + std::to_string(i);
    row.runtime_identity = std::to_string(i);
    row.runtime_identity->resize(512, 'x');
    rows.push_back(std::move(row));
  }
  auto bounded = f.validate(value);
  REQUIRE_FALSE(bounded);
  CHECK(bounded.error().code == OpsObservationErrorCode::resource_exhausted);
  rows.resize(20);
  REQUIRE(f.validate(value));
  rows.resize(257, original);
  CHECK_FALSE(f.validate(value));
}
TEST_CASE("Ops event source times may be absent and filters preserve exact pod "
          "identity") {
  Fixture f{OpsObservationOperation::kubernetes_events};
  auto value = f.observation();
  auto& event =
      std::get<KubernetesEventsObservation>(value.payload).events.front();
  event.first_observed_at.reset();
  event.last_observed_at.reset();
  REQUIRE(f.validate(value));
  event.first_observed_at = at(100);
  event.last_observed_at = at(99);
  CHECK_FALSE(f.validate(value));
  event.last_observed_at = at(100);
  event.occurrences = 0;
  CHECK_FALSE(f.validate(value));
  event.occurrences = 1;
  f.request.resource = pod();
  value.request = f.request;
  REQUIRE(f.validate(value));
  event.regarding.uid = id<OpsResourceUid>("other-pod");
  CHECK_FALSE(f.validate(value));
}
TEST_CASE("Ops logs preserve nonmonotonic source order while enforcing exact "
          "consent and window") {
  for (auto operation : {OpsObservationOperation::linux_service_logs,
                         OpsObservationOperation::kubernetes_pod_logs}) {
    Fixture f{operation};
    auto value = f.observation();
    auto& logs = std::get<OpsLogObservation>(value.payload);
    logs.lines.push_back({at(998000), "earlier source timestamp"});
    REQUIRE(f.validate(value));
    for (unsigned bad = 0; bad < 6; ++bad) {
      auto invalid = value;
      auto& excerpt = std::get<OpsLogObservation>(invalid.payload);
      switch (bad) {
        case 0: excerpt.source = {}; break;
        case 1: excerpt.lines.front().timestamp = at(1000101); break;
        case 2: excerpt.lines.front().timestamp = at(400099); break;
        case 3: excerpt.lines.front().text = "bad\nline"; break;
        case 4:
          excerpt.lines.front().text = std::string(1, static_cast<char>(0xff));
          break;
        case 5: excerpt.lines.front().text = std::string(32769, 'x'); break;
      }
      CHECK_FALSE(f.validate(invalid));
    }
    logs.lines.resize(201, logs.lines.front());
    CHECK_FALSE(f.validate(value));
  }
}
TEST_CASE("Ops exact container selection rejects replacement or missing "
          "runtime identity") {
  Fixture f{OpsObservationOperation::kubernetes_pod_health};
  f.request.resource = pod(true);
  auto value = f.observation();
  auto& result = std::get<KubernetesPodObservation>(value.payload);
  result.identity = pod(true);
  REQUIRE(f.validate(value));
  result.containers.front().runtime_identity = "containerd://replacement";
  CHECK_FALSE(f.validate(value));
  result.containers.front().runtime_identity.reset();
  CHECK_FALSE(f.validate(value));
  result.containers.clear();
  CHECK_FALSE(f.validate(value));
}
TEST_CASE("Ops empty lists and absent metrics remain explicit unknown data") {
  Fixture f{OpsObservationOperation::kubernetes_workloads};
  auto value = f.observation();
  auto& rows =
      std::get<KubernetesWorkloadsObservation>(value.payload).workloads;
  rows.front().health = OpsHealthState::unknown;
  rows.front().desired_count.reset();
  rows.front().ready_count.reset();
  rows.front().observed_count.reset();
  REQUIRE(f.validate(value));
  rows.clear();
  const auto result = f.validate(value);
  REQUIRE(result);
  CHECK(result->entries == 0);
  value.completeness = OpsObservationCompleteness::truncated;
  value.omitted_entries = 4;
  REQUIRE(f.validate(value));
  value.omitted_entries = std::numeric_limits<std::uint64_t>::max();
  CHECK_FALSE(f.validate(value));
}
TEST_CASE(
    "Ops pure resource validation does not require log authority for health") {
  Fixture f{OpsObservationOperation::linux_service_health};
  const OpsResourceIdentity inactive =
      LinuxServiceIdentity{"inactive.service", {}};
  REQUIRE(validate_ops_resource_identity(f.request.target, inactive));
  CHECK_FALSE(validate_ops_resource_identity(f.request.target, inactive, true));
  CHECK_FALSE(validate_ops_resource_identity(f.request.target, pod()));
  auto invalid_target = f.request.target;
  std::get<LinuxOpsIdentity>(invalid_target.identity).pid_namespace = 0;
  CHECK_FALSE(validate_ops_resource_identity(invalid_target, inactive));
}
TEST_CASE("Ops observation validates each closed operation with explicit "
          "unknown values") {
  for (auto operation : {OpsObservationOperation::linux_health,
                         OpsObservationOperation::linux_services,
                         OpsObservationOperation::linux_service_health,
                         OpsObservationOperation::kubernetes_workloads,
                         OpsObservationOperation::kubernetes_pod_health,
                         OpsObservationOperation::kubernetes_events,
                         OpsObservationOperation::linux_service_logs,
                         OpsObservationOperation::kubernetes_pod_logs}) {
    Fixture f{operation};
    auto value = f.observation();
    const auto result = f.validate(value);
    INFO(static_cast<int>(operation));
    REQUIRE(result);
    CHECK(result->evidence_bytes <= ops_observation_maximum_evidence_bytes);
    CHECK(result->entries > 0);
  }
}
