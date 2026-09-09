// Failure matrix before codec implementation:
// Empty/oversized/deep/malformed/duplicate-key UTF-8 JSON; unknown schema,
// keys, variants and enums; exact integer types and overflow; missing/null
// fields; invalid target/request/resource/time/completeness/row/log data;
// explicit encoded-byte bound separate from neutral accounting; safe fixed
// errors. Historical validation does not reconstruct authority, restore log
// consent, compare old IDs to current selection or collect any source.
// Roundtrip every payload and operation, preserving null versus zero, row order
// and source provenance, then a final smoke case.

#include <aiforge/adapters/ops_observation_json.hpp>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <nlohmann/json.hpp>

namespace {
using namespace aiforge;
using namespace aiforge::domain;
using Json = nlohmann::json;
using Error = adapters::OpsObservationJsonError;
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

auto document(OpsObservationOperation operation =
                  OpsObservationOperation::linux_health) -> Json {
  const auto encoded =
      adapters::encode_ops_observation(Fixture{operation}.observation());
  REQUIRE(encoded);
  return Json::parse(*encoded);
}
auto rejected(Json value) -> void {
  CHECK_FALSE(adapters::decode_ops_observation(value.dump()));
}
} // namespace
TEST_CASE("Ops JSON refuses invalid syntax ambiguity and parser bounds") {
  for (const auto* text :
       {"", "null", "[]", "{}", "{", "{} {}", "{\"version\":1,\"version\":1}",
        "{\"version\":1,\"ver\\u0073ion\":1}", "{\"version\":NaN}"}) {
    const auto result = adapters::decode_ops_observation(text);
    INFO(text);
    REQUIRE_FALSE(result);
    CHECK(result.error() == Error::invalid_document);
  }
  const auto large = adapters::decode_ops_observation(
      std::string(adapters::maximum_ops_observation_json_bytes + 1, ' '));
  REQUIRE_FALSE(large);
  CHECK(large.error() == Error::resource_exhausted);
  const auto deep = adapters::decode_ops_observation(
      std::string(24, '[') + "0" + std::string(24, ']'));
  REQUIRE_FALSE(deep);
  CHECK(deep.error() == Error::resource_exhausted);
  std::string broad{"["};
  for (unsigned index = 0; index < 32768; ++index) {
    if (index != 0) broad += ',';
    broad += '0';
  }
  broad += ']';
  const auto many_events = adapters::decode_ops_observation(broad);
  REQUIRE_FALSE(many_events);
  CHECK(many_events.error() == Error::resource_exhausted);
  auto text = document().dump();
  text.insert(text.find("linux_health"), 1, static_cast<char>(0xff));
  CHECK_FALSE(adapters::decode_ops_observation(text));
  text = document().dump();
  const auto target = text.find("\"owner_id\":");
  REQUIRE(target != std::string::npos);
  text.insert(target, "\"owner_id\":\"shadow\",");
  CHECK_FALSE(adapters::decode_ops_observation(text));
}
TEST_CASE("Ops JSON requires exact schema keys variants and scalar types") {
  for (const auto& invalid : {Json(nullptr), Json(true), Json(-1), Json(1.0),
                              Json("1"), Json(2), Json(4294967296ULL)}) {
    auto value = document();
    value["version"] = invalid;
    rejected(value);
  }
  for (unsigned mutation = 0; mutation < 15; ++mutation) {
    auto value = document();
    auto& obs = value["observation"];
    auto& req = obs["request"];
    switch (mutation) {
      case 0: value["unexpected"] = "ignored?"; break;
      case 1: value.erase("version"); break;
      case 2: obs.erase("source_version"); break;
      case 3: req["credentials"] = "fake forbidden field"; break;
      case 4: req["target"]["identity"]["kind"] = "future"; break;
      case 5: obs["payload"]["kind"] = "future"; break;
      case 6: obs["payload"]["value"]["health"] = "perfect"; break;
      case 7: req["operation"] = 0; break;
      case 8: req["resource"]["value"] = Json::object(); break;
      case 9: req["selection_generation"] = -1; break;
      case 10: req["limits"]["maximum_entries"] = true; break;
      case 11: obs["started_at_ms"] = 1.25; break;
      case 12:
        obs["payload"]["value"]["active_services"] = 4294967296ULL;
        break;
      case 13:
        obs["started_at_ms"] = std::numeric_limits<std::uint64_t>::max();
        break;
      case 14: obs["payload"]["value"]["health"] = nullptr; break;
    }
    INFO(mutation);
    rejected(value);
  }
  auto kube = document(OpsObservationOperation::kubernetes_workloads);
  kube["observation"]["request"]["target"]["identity"]["value"]["endpoint"]
      ["port"] = 65536;
  rejected(kube);
}
TEST_CASE(
    "Ops JSON refuses structurally invalid historical requests and results") {
  for (unsigned mutation = 0; mutation < 13; ++mutation) {
    auto value = document();
    auto& obs = value["observation"];
    auto& req = obs["request"];
    switch (mutation) {
      case 0: req["owner_id"] = ""; break;
      case 1: req["session_id"] = "line\nfeed"; break;
      case 2: req["target"]["identity"]["value"]["pid_namespace"] = 0; break;
      case 3: req["selection_generation"] = 0; break;
      case 4: req["log_policy_revision"] = 0; break;
      case 5: req["operation"] = "kubernetes_workloads"; break;
      case 6: req["operation"] = "linux_service_health"; break;
      case 7: req["limits"]["maximum_entries"] = 257; break;
      case 8: obs["completed_at_ms"] = 0; break;
      case 9: obs["omitted_entries"] = nullptr; break;
      case 10: obs["unsupported_entries"] = 1; break;
      case 11: obs["payload"]["value"]["failed_services"] = 1; break;
      case 12:
        req["target"]["identity"] = {{"kind", "ceph"},
                                     {"value", {{"cluster_identity", "fsid"}}}};
        break;
    }
    INFO(mutation);
    const auto result = adapters::decode_ops_observation(value.dump());
    REQUIRE_FALSE(result);
    CHECK(result.error() ==
          (mutation < 2 ? Error::invalid_document : Error::invalid_record));
  }
  auto value = Fixture{OpsObservationOperation::linux_health}.observation();
  value.request.selection_generation = 0;
  const auto encoded = adapters::encode_ops_observation(value);
  REQUIRE_FALSE(encoded);
  CHECK(encoded.error() == Error::invalid_record);
}
TEST_CASE(
    "Ops JSON applies row log and encoded text bounds before acceptance") {
  auto services = document(OpsObservationOperation::linux_services);
  auto& rows = services["observation"]["payload"]["value"]["services"];
  const auto row = rows.front();
  while (rows.size() < 257)
    rows.push_back(row);
  const auto many = adapters::decode_ops_observation(services.dump());
  REQUIRE_FALSE(many);
  CHECK(many.error() == Error::resource_exhausted);
  auto logs = document(OpsObservationOperation::linux_service_logs);
  logs["observation"]["payload"]["value"]["lines"][0]["text"] =
      std::string(32769, 'x');
  const auto long_line = adapters::decode_ops_observation(logs.dump());
  REQUIRE_FALSE(long_line);
  CHECK(long_line.error() == Error::resource_exhausted);
  logs = document(OpsObservationOperation::linux_service_logs);
  logs["observation"]["payload"]["value"]["lines"][0]["timestamp_ms"] = 1;
  rejected(logs);
  auto value =
      Fixture{OpsObservationOperation::linux_service_logs}.observation();
  std::get<OpsLogObservation>(value.payload).lines[0].text =
      std::string(32768, '\\');
  const auto encoded = adapters::encode_ops_observation(value);
  REQUIRE(encoded);
  CHECK(encoded->size() > 65536);
  CHECK(encoded->size() <= adapters::maximum_ops_observation_json_bytes);
  const auto decoded = adapters::decode_ops_observation(*encoded);
  REQUIRE(decoded);
  CHECK(*decoded == value);
  CHECK_FALSE(adapters::decode_ops_observation(
      encoded->substr(0, encoded->size() - 1)));
}
TEST_CASE(
    "Historical Ops decoding preserves revoked provenance without a grant") {
  Fixture f{OpsObservationOperation::linux_service_logs};
  const auto observation = f.observation();
  f.specification.logs.enabled = false;
  f.specification.logs.permitted_sources.clear();
  ++f.specification.logs.revision;
  f.specification.target.configuration_revision =
      id<OpsConfigurationRevision>("new-config");
  auto current = OpsObservationAuthority::create(f.specification);
  REQUIRE(current);
  CHECK_FALSE(
      validate_ops_observation(*current, observation.request, observation));
  REQUIRE(validate_recorded_ops_observation(observation));
  const auto encoded = adapters::encode_ops_observation(observation);
  REQUIRE(encoded);
  const auto decoded = adapters::decode_ops_observation(*encoded);
  REQUIRE(decoded);
  CHECK(*decoded == observation);
  CHECK_FALSE(current->validate(decoded->request));
}
TEST_CASE("Ops JSON preserves optional zero unknown timestamps and row order") {
  auto logs =
      Fixture{OpsObservationOperation::kubernetes_pod_logs}.observation();
  logs.completeness = OpsObservationCompleteness::truncated;
  logs.omitted_entries.reset();
  std::get<OpsLogObservation>(logs.payload).lines = {
      {at(999010), "quote \" \\"},
      {at(999000), ""},
      {at(999005), "UTF-8 café"}};
  const auto encoded = adapters::encode_ops_observation(logs);
  const auto structural = validate_recorded_ops_observation(logs);
  INFO(
      (structural ? "valid historical structure" : structural.error().message));
  REQUIRE(encoded);
  const auto decoded = adapters::decode_ops_observation(*encoded);
  REQUIRE(decoded);
  CHECK(*decoded == logs);
  const auto canonical = adapters::encode_ops_observation(*decoded);
  REQUIRE(canonical);
  CHECK(*canonical == *encoded);
  CHECK(Json::parse(*encoded)["observation"]["omitted_entries"].is_null());
}
TEST_CASE("Ops JSON roundtrips every supported operation without collection") {
  for (const auto operation : {OpsObservationOperation::linux_health,
                               OpsObservationOperation::linux_services,
                               OpsObservationOperation::linux_service_health,
                               OpsObservationOperation::linux_service_logs,
                               OpsObservationOperation::kubernetes_workloads,
                               OpsObservationOperation::kubernetes_pod_health,
                               OpsObservationOperation::kubernetes_events,
                               OpsObservationOperation::kubernetes_pod_logs}) {
    const auto expected = Fixture{operation}.observation();
    const auto encoded = adapters::encode_ops_observation(expected);
    REQUIRE(encoded);
    const auto decoded = adapters::decode_ops_observation(*encoded);
    INFO(static_cast<int>(operation));
    REQUIRE(decoded);
    CHECK(*decoded == expected);
  }
}
