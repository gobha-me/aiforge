#include "kubernetes_observation_projection_internal.hpp"

#include <aiforge/adapters/ops_observation_json.hpp>
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <functional>
#include <limits>
#include <nlohmann/json.hpp>

namespace {
using namespace aiforge;
using namespace domain;
using Json = nlohmann::json;
using Failure = runtime::OpsObservationSourceError;
namespace projection = adapters::kubernetes_projection_detail;
template <class T> auto id(std::string value) -> T {
  return T::from(std::move(value)).value();
}
auto at(std::int64_t value) -> EventTimestamp {
  return EventTimestamp{std::chrono::milliseconds{value}};
}
auto request(OpsObservationOperation operation =
                 OpsObservationOperation::kubernetes_workloads)
    -> OpsObservationRequest {
  OpsObservationRequest result{
      id<OpsOwnerId>("owner"),
      id<SessionId>("session"),
      id<OpsRequestId>("request"),
      {id<OpsTargetId>("target"), id<OpsConfigurationRevision>("revision"),
       KubernetesOpsIdentity{"context",
                             "selected",
                             {"fixture.invalid", 6443},
                             "sha256:synthetic-public"}},
      1,
      operation,
      {},
      1,
      {}};
  if (operation == OpsObservationOperation::kubernetes_pod_health)
    result.resource = KubernetesPodIdentity{
        "selected", "pod-one", id<OpsResourceUid>("pod-uid"), {}};
  return result;
}
auto metadata(std::string name = "pod-one", std::string uid = "pod-uid")
    -> Json {
  return {{"namespace", "selected"},
          {"name", std::move(name)},
          {"uid", std::move(uid)},
          {"resourceVersion", "rv-one"}};
}
auto pod() -> Json {
  Json value{{"apiVersion", "v1"}, {"kind", "Pod"}, {"metadata", metadata()}};
  Json declaration{{"name", "main"}, {"image", "secret-image"}};
  declaration["env"] =
      Json::array({{{"name", "SECRET"}, {"value", "secret-env"}}});
  value["spec"]["containers"] = Json::array({std::move(declaration)});
  Json status{{"name", "main"},
              {"containerID", "containerd://one"},
              {"ready", true},
              {"restartCount", 2}};
  status["state"]["running"] = Json::object();
  status["lastState"]["terminated"]["message"] = "secret-last-state";
  value["status"]["phase"] = "Running";
  value["status"]["conditions"] =
      Json::array({{{"type", "Ready"}, {"status", "True"}}});
  value["status"]["containerStatuses"] = Json::array({std::move(status)});
  return value;
}
auto event() -> Json {
  return {{"apiVersion", "v1"},
          {"kind", "Event"},
          {"metadata", metadata("event-one", "event-uid")},
          {"involvedObject",
           {{"apiVersion", "v1"},
            {"kind", "Pod"},
            {"namespace", "selected"},
            {"name", "pod-one"},
            {"uid", "pod-uid"}}},
          {"count", 3},
          {"type", "Warning"},
          {"reason", "FailedScheduling"},
          {"message", "secret-message"},
          {"firstTimestamp", "2026-09-09T10:00:00Z"},
          {"lastTimestamp", "2026-09-09T10:00:01Z"}};
}
auto list(bool events = false) -> Json {
  return {{"apiVersion", "v1"},
          {"kind", events ? "EventList" : "PodList"},
          {"metadata", {{"resourceVersion", "list-rv"}}},
          {"items", Json::array({events ? event() : pod()})}};
}
auto run(const OpsObservationRequest& req, std::string_view input,
         std::stop_token stop = {}) {
  return adapters::project_kubernetes_observation(req, at(1000), at(2000),
                                                  input, stop);
}
auto run(const OpsObservationRequest& req, const std::string& input) {
  return run(req, std::string_view{input});
}
auto run(const OpsObservationRequest& req, const Json& input) {
  return run(req, input.dump());
}
auto rejects(const std::function<void()>& operation,
             Failure failure = Failure::resource_exhausted) -> void {
  try {
    operation();
    FAIL("expected fixed rejection");
  } catch (const projection::Rejected& rejected) {
    CHECK(rejected.failure == failure);
  }
}
} // namespace

TEST_CASE(
    "Kubernetes projection rejects request framing and typed JSON failures") {
  const auto req = request();
  for (const std::string input : {"", "[]", "null", "{}", "{", "{}{}"})
    CHECK_FALSE(run(req, input));
  auto trailing = list().dump() + " {}";
  CHECK_FALSE(run(req, trailing));
  auto duplicate = list().dump();
  duplicate.insert(1, "\"kind\":\"PodList\",");
  CHECK_FALSE(run(req, duplicate));
  for (const auto suffix :
       {R"(,"ignored":{"a":1,"\u0061":2})", R"(,"ignored":[{"a":1,"a":2}])"}) {
    auto text = list().dump();
    text.insert(text.size() - 1, suffix);
    CHECK_FALSE(run(req, text));
  }
  for (const auto path : {"/apiVersion", "/kind", "/metadata",
                          "/metadata/resourceVersion", "/items"}) {
    for (const auto& replacement : {Json(nullptr), Json(3), Json(true)}) {
      auto value = list();
      value[Json::json_pointer{path}] = replacement;
      CHECK_FALSE(run(req, value));
    }
  }
  auto malformed = list().dump();
  malformed.insert(malformed.size() - 1, ",\"ignored\":\"\xff\"");
  CHECK_FALSE(run(req, malformed));
  for (const auto field : {"apiVersion", "kind"}) {
    auto value = list();
    value["items"][0][field] = "";
    CHECK_FALSE(run(req, value));
  }
  auto invalid_request = req;
  invalid_request.selection_generation = 0;
  CHECK_FALSE(run(invalid_request, list()));
  invalid_request = req;
  invalid_request.operation = OpsObservationOperation::kubernetes_pod_logs;
  CHECK_FALSE(run(invalid_request, list()));
  std::stop_source stopped;
  stopped.request_stop();
  const auto cancelled = run(req, "", stopped.get_token());
  REQUIRE_FALSE(cancelled);
  CHECK(cancelled.error() == Failure::cancelled);
  CHECK_FALSE(adapters::project_kubernetes_observation(req, at(-1), at(0),
                                                       list().dump()));
  CHECK_FALSE(adapters::project_kubernetes_observation(req, at(2), at(1),
                                                       list().dump()));
}

TEST_CASE(
    "Kubernetes projection enforces actual and defensive parser ceilings") {
  auto req = request();
  auto bytes = list().dump();
  req.limits.maximum_bytes = bytes.size();
  CHECK(run(req, bytes));
  CHECK_FALSE(run(req, bytes + " "));
  req = request();
  bytes = list().dump();
  bytes.resize(projection::input_limit, ' ');
  CHECK(run(req, bytes));
  bytes.push_back(' ');
  const auto too_big = run(req, bytes);
  REQUIRE_FALSE(too_big);
  CHECK(too_big.error() == Failure::resource_exhausted);
  for (const auto size :
       {projection::scalar_limit, projection::scalar_limit + 1}) {
    auto value = list();
    value["ignored"] = std::string(size, 'x');
    CHECK(run(req, value).has_value() == (size == projection::scalar_limit));
  }
  for (const auto size :
       {projection::scalar_limit, projection::scalar_limit + 1}) {
    auto value = list().dump();
    const auto literal = std::string{"0."} + std::string(size - 3, '0') + "1";
    value.insert(value.size() - 1, ",\"ignored\":" + literal);
    CHECK(run(req, value).has_value() == (size == projection::scalar_limit));
  }
  for (const auto depth : {31, 32}) {
    auto value = list();
    Json nested = 0;
    for (int index{}; index < depth; ++index)
      nested = Json::array({std::move(nested)});
    value["ignored"] = std::move(nested);
    CHECK(run(req, value).has_value() == (depth == 31));
  }
  SECTION("event count") {
    projection::Budget budget{{}};
    for (std::size_t index{}; index < projection::event_limit; ++index)
      budget.event();
    rejects([&] { budget.event(); });
    auto value = list();
    value["ignored"] = Json::array();
    for (std::size_t index{}; index < projection::event_limit; ++index)
      value["ignored"].push_back(0);
    CHECK_FALSE(run(req, value));
  }
  SECTION("active keys and release") {
    projection::Budget budget{{}};
    budget.key(projection::key_limit);
    rejects([&] { budget.key(1); });
    budget.release_keys(projection::key_limit);
    budget.key(projection::key_limit);
  }
  SECTION("retained values independent of keys") {
    projection::Budget budget{{}};
    budget.value(projection::value_limit);
    rejects([&] { budget.value(1); });
  }
  SECTION("intermediate records") {
    projection::Budget budget{{}};
    for (std::size_t index{}; index < projection::record_limit; ++index)
      budget.record();
    rejects([&] { budget.record(); });
  }
  SECTION("callback stop gate after earlier progress") {
    std::stop_source stop;
    projection::Budget budget{stop.get_token()};
    budget.event();
    stop.request_stop();
    rejects([&] { budget.event(); }, Failure::cancelled);
  }
}

TEST_CASE(
    "Pod inventory rejects wrong identity and does not infer missing health") {
  const auto req = request();
  for (const auto path : {"namespace", "name", "uid"}) {
    auto value = list();
    value["items"][0]["metadata"].erase(path);
    CHECK_FALSE(run(req, value));
  }
  auto value = list();
  value["items"][0]["metadata"]["namespace"] = "foreign";
  CHECK_FALSE(run(req, value));
  value = list();
  value["items"].push_back(value["items"][0]);
  CHECK_FALSE(run(req, value));
  value = list();
  value["items"][0].erase("status");
  auto result = run(req, value);
  REQUIRE(result);
  const auto& unknown =
      std::get<KubernetesWorkloadsObservation>(result->payload)
          .workloads.front();
  CHECK(unknown.health == OpsHealthState::unknown);
  CHECK_FALSE(unknown.desired_count);
  CHECK_FALSE(unknown.ready_count);
  CHECK(unknown.observed_count == 1);
  value = list();
  value["items"][0]["metadata"]["deletionTimestamp"] = "2026-09-09T10:00:00Z";
  result = run(req, value);
  REQUIRE(result);
  CHECK(std::get<KubernetesWorkloadsObservation>(result->payload)
            .workloads.front()
            .health == OpsHealthState::unknown);
  value = list();
  value["items"][0]["status"]["phase"] = "FuturePhase";
  result = run(req, value);
  REQUIRE(result);
  CHECK(std::get<KubernetesWorkloadsObservation>(result->payload)
            .workloads.front()
            .health == OpsHealthState::unknown);
  value = list();
  value["items"][0].erase("kind");
  value["items"][0].erase("apiVersion");
  CHECK(run(req, value));
}

TEST_CASE("Finite Kubernetes lists never turn estimated omissions into exact "
          "evidence") {
  const auto req = request();
  for (const auto& count : {Json(nullptr), Json(0), Json(42)}) {
    auto value = list();
    value["metadata"]["continue"] = "opaque-not-fetched";
    if (!count.is_null()) value["metadata"]["remainingItemCount"] = count;
    auto result = run(req, value);
    REQUIRE(result);
    CHECK(result->completeness == OpsObservationCompleteness::partial);
    CHECK_FALSE(result->omitted_entries);
  }
  auto value = list();
  value["metadata"]["remainingItemCount"] = -1;
  CHECK_FALSE(run(req, value));
  value = list();
  value["items"] = Json::array();
  auto empty = run(req, value);
  REQUIRE(empty);
  CHECK(empty->completeness == OpsObservationCompleteness::complete);
  CHECK(empty->omitted_entries == 0);
  CHECK(empty->source_version == "list-rv");
  value["metadata"].erase("resourceVersion");
  CHECK_FALSE(run(req, value));
  auto limited = req;
  limited.limits.maximum_entries = 1;
  value = list();
  auto other = pod();
  other["metadata"] = metadata("other", "other-uid");
  value["items"].push_back(other);
  CHECK_FALSE(run(limited, value));
}

TEST_CASE("Exact Pod health rejects replacement and malformed current "
          "container state") {
  auto req = request(OpsObservationOperation::kubernetes_pod_health);
  for (const auto field : {"name", "uid", "namespace"}) {
    auto value = pod();
    value["metadata"][field] = "changed";
    CHECK_FALSE(run(req, value));
  }
  for (const auto field : {"resourceVersion", "uid"}) {
    auto value = pod();
    value["metadata"].erase(field);
    CHECK_FALSE(run(req, value));
  }
  auto value = pod();
  value["status"]["containerStatuses"][0]["state"]["waiting"] = Json::object();
  CHECK_FALSE(run(req, value));
  value = pod();
  value["status"]["containerStatuses"][0]["state"] = {
      {"terminated", {{"exitCode", 0}}}};
  CHECK_FALSE(run(req, value));
  for (const auto& bad :
       {Json(-1), Json(2147483648LL), Json(1.0), Json("1"), Json(nullptr)}) {
    value = pod();
    value["status"]["containerStatuses"][0]["restartCount"] = bad;
    CHECK_FALSE(run(req, value));
  }
  value = pod();
  value["status"]["containerStatuses"][0]["name"] = "undeclared";
  CHECK_FALSE(run(req, value));
  value = pod();
  value["spec"]["initContainers"] = Json::array({{{"name", "main"}}});
  CHECK_FALSE(run(req, value));
  value = pod();
  value["status"]["containerStatuses"].push_back(
      value["status"]["containerStatuses"][0]);
  CHECK_FALSE(run(req, value));
  std::get<KubernetesPodIdentity>(req.resource).container =
      KubernetesContainerIdentity{"main", "containerd://changed"};
  CHECK_FALSE(run(req, pod()));
  req = request(OpsObservationOperation::kubernetes_pod_health);
  req.limits.maximum_entries = 1;
  CHECK_FALSE(run(req, pod()));
}

TEST_CASE("Exact Pod health retains unknown declared containers and no secret "
          "spec values") {
  auto req = request(OpsObservationOperation::kubernetes_pod_health);
  auto value = pod();
  value["spec"]["initContainers"] = Json::array({{{"name", "setup"}}});
  value["spec"]["ephemeralContainers"] = Json::array({{{"name", "debug"}}});
  auto result = run(req, value);
  REQUIRE(result);
  const auto& output = std::get<KubernetesPodObservation>(result->payload);
  REQUIRE(output.containers.size() == 3);
  CHECK(output.containers[0].name == "main");
  CHECK(output.containers[1].name == "setup");
  CHECK(output.containers[2].name == "debug");
  CHECK(output.containers[1].state == OpsContainerState::unknown);
  CHECK(output.containers[1].readiness == OpsReadiness::unknown);
  CHECK_FALSE(output.containers[1].runtime_identity);
  CHECK_FALSE(output.containers[1].restart_count);
  auto encoded = adapters::encode_ops_observation(*result);
  REQUIRE(encoded);
  CHECK(encoded->find("secret-") == std::string::npos);
  value = pod();
  value.erase("status");
  result = run(req, value);
  REQUIRE(result);
  CHECK(std::get<KubernetesPodObservation>(result->payload).phase ==
        OpsPodPhase::unknown);
  CHECK(std::get<KubernetesPodObservation>(result->payload)
            .containers.front()
            .state == OpsContainerState::unknown);
}

TEST_CASE(
    "Events reject identity mismatches even when the row would be omitted") {
  auto req = request(OpsObservationOperation::kubernetes_events);
  auto value = list(true);
  value["items"][0]["involvedObject"]["kind"] = "UnsupportedKind";
  auto result = run(req, value);
  REQUIRE(result);
  CHECK(result->unsupported_entries == 1);
  CHECK(result->omitted_entries == 1);
  CHECK(result->completeness == OpsObservationCompleteness::partial);
  value["items"][0]["involvedObject"]["uid"] = "";
  CHECK_FALSE(run(req, value));
  value = list(true);
  value["items"][0].erase("count");
  value["items"][0]["involvedObject"]["namespace"] = "foreign";
  CHECK_FALSE(run(req, value));
  value = list(true);
  value["items"].push_back(value["items"][0]);
  CHECK_FALSE(run(req, value));
  req.resource = KubernetesPodIdentity{
      "selected", "pod-one", id<OpsResourceUid>("pod-uid"), {}};
  for (const auto field : {"name", "uid", "namespace", "kind", "apiVersion"}) {
    value = list(true);
    value["items"][0]["involvedObject"][field] = "foreign";
    CHECK_FALSE(run(req, value));
  }
  value = list(true);
  value["items"][0]["involvedObject"].erase("apiVersion");
  CHECK(run(req, value));
}

TEST_CASE("Namespace events admit only matched built-in kinds without invented "
          "count one") {
  const auto req = request(OpsObservationOperation::kubernetes_events);
  for (const auto& pair : std::array{
           std::pair{"Pod", "v1"}, std::pair{"Deployment", "apps/v1"},
           std::pair{"StatefulSet", "apps/v1"},
           std::pair{"DaemonSet", "apps/v1"}, std::pair{"Job", "batch/v1"},
           std::pair{"CronJob", "batch/v1"}}) {
    auto value = list(true);
    value["items"][0]["involvedObject"]["kind"] = pair.first;
    value["items"][0]["involvedObject"]["apiVersion"] = pair.second;
    auto result = run(req, value);
    REQUIRE(result);
    CHECK(result->unsupported_entries == 0);
    CHECK(
        std::get<KubernetesEventsObservation>(result->payload).events.size() ==
        1);
    value["items"][0]["involvedObject"]["apiVersion"] = "foreign.example/v1";
    result = run(req, value);
    REQUIRE(result);
    CHECK(result->unsupported_entries == 1);
  }
  auto value = list(true);
  value["items"][0].erase("count");
  auto result = run(req, value);
  REQUIRE(result);
  CHECK(result->unsupported_entries == 1);
  CHECK(std::get<KubernetesEventsObservation>(result->payload).events.empty());
  value = list(true);
  value["items"][0]["series"] = {{"lastObservedTime", "2026-09-09T10:00:02Z"}};
  result = run(req, value);
  REQUIRE(result);
  CHECK(result->unsupported_entries == 1);
  value["items"][0]["series"]["count"] = 7;
  result = run(req, value);
  REQUIRE(result);
  CHECK(std::get<KubernetesEventsObservation>(result->payload)
            .events.front()
            .occurrences == 7);
  for (const auto& bad : {Json(0), Json(-1), Json(2147483648LL), Json(2.0),
                          Json("2"), Json(nullptr)}) {
    value = list(true);
    value["items"][0]["count"] = bad;
    CHECK_FALSE(run(req, value));
  }
}

TEST_CASE(
    "Event timestamp projection rejects malformed and reversed evidence") {
  const auto req = request(OpsObservationOperation::kubernetes_events);
  for (const auto& bad :
       {"2026-02-30T00:00:00Z", "2026-09-09T25:00:00Z", "2026-09-09T00:00:60Z",
        "2026-09-09T00:00:00.1234567890Z", "1969-01-01T00:00:00Z",
        "2026-09-09T00:00:00+24:00", "arbitrary"}) {
    auto value = list(true);
    value["items"][0]["firstTimestamp"] = bad;
    CHECK_FALSE(run(req, value));
  }
  auto value = list(true);
  value["items"][0]["firstTimestamp"] = "2026-09-09T10:00:02Z";
  CHECK_FALSE(run(req, value));
  value = list(true);
  value["items"][0].erase("firstTimestamp");
  value["items"][0]["lastTimestamp"] = nullptr;
  auto result = run(req, value);
  REQUIRE(result);
  const auto& row =
      std::get<KubernetesEventsObservation>(result->payload).events.front();
  CHECK_FALSE(row.first_observed_at);
  CHECK_FALSE(row.last_observed_at);
  value = list(true);
  value["items"][0]["eventTime"] = "2026-09-09T11:00:00.123456+01:00";
  result = run(req, value);
  REQUIRE(result);
  CHECK(std::get<KubernetesEventsObservation>(result->payload)
                .events.front()
                .first_observed_at->time_since_epoch()
                .count() %
            1000 ==
        123);
}

TEST_CASE("Kubernetes final smoke retains explicit observed status and "
          "existing domain bounds") {
  for (const auto operation : {OpsObservationOperation::kubernetes_workloads,
                               OpsObservationOperation::kubernetes_pod_health,
                               OpsObservationOperation::kubernetes_events}) {
    auto req = request(operation);
    const auto value =
        operation == OpsObservationOperation::kubernetes_pod_health
            ? pod()
            : list(operation == OpsObservationOperation::kubernetes_events);
    const auto result = run(req, value);
    REQUIRE(result);
    CHECK(validate_recorded_ops_observation(*result));
    CHECK(result->request == req);
    CHECK(result->started_at == at(1000));
    CHECK(result->completed_at == at(2000));
    auto encoded = adapters::encode_ops_observation(*result);
    REQUIRE(encoded);
    CHECK(encoded->find("secret-") == std::string::npos);
  }
}

namespace {
auto key_object(std::size_t bytes) -> Json {
  Json result = Json::object();
  std::size_t index{};
  while (bytes != 0) {
    const auto size = std::min<std::size_t>(1024, bytes);
    const auto prefix = std::to_string(index++) + ':';
    REQUIRE(size >= prefix.size());
    result[prefix + std::string(size - prefix.size(), 'k')] = nullptr;
    bytes -= size;
  }
  return result;
}
} // namespace

TEST_CASE("SAX active-key budget applies to ignored mappings and releases "
          "sibling frames") {
  // Root keys apiVersion/kind/metadata/items total27 decoded bytes. The first
  // ignored member adds3; the second adds another3. Child metadata has closed.
  const std::string prefix =
      R"({"apiVersion":"v1","kind":"PodList","metadata":{"resourceVersion":"rv"},"items":[])";
  const auto first = key_object(projection::key_limit - 30).dump();
  const auto req = request();
  CHECK(run(req, prefix + ",\"one\":" + first + '}'));
  const auto too_large = key_object(projection::key_limit - 29).dump();
  const auto failed = run(req, prefix + ",\"one\":" + too_large + '}');
  REQUIRE_FALSE(failed);
  CHECK(failed.error() == Failure::resource_exhausted);
  const auto sibling = key_object(projection::key_limit - 33).dump();
  CHECK(run(req, prefix + ",\"one\":" + first + ",\"two\":" + sibling + '}'));
}

TEST_CASE("SAX retained-value budget applies before copying selected condition "
          "fields") {
  auto value = list();
  value["metadata"]["resourceVersion"] = "v";
  value["items"][0].erase("apiVersion");
  value["items"][0].erase("kind");
  value["items"][0].erase("spec");
  value["items"][0]["metadata"].erase("resourceVersion");
  value["items"][0]["status"] = {{"conditions", Json::array()}};
  // v1+PodList+v+selected+pod-one+pod-uid =32 retained bytes. Other values
  // below are real selected condition.type strings, unlike arbitrary ignored
  // fields.
  std::size_t remaining = projection::value_limit - 32;
  auto& conditions = value["items"][0]["status"]["conditions"];
  while (remaining != 0) {
    const auto size = std::min<std::size_t>(128, remaining);
    conditions.push_back({{"type", std::string(size, 'x')}});
    remaining -= size;
  }
  CHECK(run(request(), value));
  conditions.back()["type"] =
      conditions.back()["type"].get<std::string>() + 'x';
  const auto failed = run(request(), value);
  REQUIRE_FALSE(failed);
  CHECK(failed.error() == Failure::resource_exhausted);
}

TEST_CASE("SAX intermediate record guard counts container declarations and "
          "status captures") {
  // This deliberately exceeds the later neutral row ceiling. Exercise the
  // actual private SAX frontend directly so the earlier storage guard cannot
  // be hidden by final-domain rejection of both boundary cases.
  auto value = pod();
  value["status"]["containerStatuses"] = Json::array();
  for (const auto field :
       {"containers", "initContainers", "ephemeralContainers"}) {
    value["spec"][field] = Json::array();
    for (std::size_t index{}; index < 256; ++index)
      value["spec"][field].push_back({{"name", "c" + std::to_string(index)}});
  }
  for (std::size_t index{}; index < 255; ++index)
    value["status"]["containerStatuses"].push_back(
        {{"name", "c" + std::to_string(index)}});
  const auto parse = [&] {
    return projection::parse(
        value.dump(), OpsObservationOperation::kubernetes_pod_health, 256, {});
  };
  REQUIRE_NOTHROW(parse()); // root1 + declarations768 + status255 =1024
  value["status"]["containerStatuses"].push_back({{"name", "c255"}});
  rejects([&] { (void)parse(); });
}
