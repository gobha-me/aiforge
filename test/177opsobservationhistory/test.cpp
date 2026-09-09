// Failure matrix before implementation:
// Schema 6 missing/false/null/wrong-type manual marker, non-control purpose,
// incompatible admissions; schemas 1-5 reserved marker; schema 3 proposal
// missing/null proof, normalized arguments, extra fields or spend; legacy
// proposal compatibility. Invalid historical request/observation/provenance.
// Missing, duplicated, misplaced or foreign human intent; proposal identity,
// policy, request and scope mismatch; forged ordinary or additional manual
// work; lifecycle ordering; receipt/result mismatch and partial atomic pairs;
// observation following error; terminal success missing evidence. Unknown
// events retain ordering but cannot satisfy required known events.
// Canonical field/enum/optional/quote/UTF-8/row-order/blank-log preservation;
// byte and invocation bounds; no collection on replay; unfinished admitted
// manual prefixes are reported without renewed authority; malformed atomic
// prefixes are rejected. SQLite corruption and rollback, then smoke cases.

#include <aiforge/adapters/ops_observation_json.hpp>
#include <aiforge/adapters/sqlite_session_store.hpp>
#include <aiforge/runtime/ops_observation_history.hpp>
#include <aiforge/runtime/tool_policy.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

namespace {
using namespace aiforge;
using namespace aiforge::domain;
using Json = nlohmann::json;

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

using runtime::OpsInvocationPhase;
struct History {
  Fixture fixture{OpsObservationOperation::linux_service_logs};
  RunId run{id<RunId>("manual")};
  InvocationId invocation{id<InvocationId>("invocation")};
  MessageId message{id<MessageId>("result")};
  SessionEventLog log{id<SessionId>("session")};
  std::vector<RunEvent> events;
  CapabilityScope scope{Effect::read, "filesystem.root", "/fixture"};
  ToolProvenanceEntry tool{
      "ops.read", {Effect::read}, {scope}, "sha256:" + std::string(64, 'a')};
  ToolPolicyProvenance policy{"aiforge.tool-launch-policy.v1",
                              id<PermissionProfileId>("observe"),
                              ToolRestrictionLevel::medium,
                              ToolApprovalMode::prompt,
                              {Effect::read},
                              {scope},
                              {}};
  auto push(RunEventPayload payload, unsigned schema = 1,
            bool tool_event = true) -> void {
    const auto sequence = events.size() + 1;
    events.push_back(
        {{id<EventId>("event-" + std::to_string(sequence)),
          run,
          sequence,
          schema,
          at(static_cast<std::int64_t>(sequence)),
          {},
          {},
          tool_event ? std::optional<InvocationId>{invocation} : std::nullopt},
         std::move(payload)});
  }
  auto admission(bool manual = true) -> void {
    RunStarted start{id<SurfaceId>("test"),
                     id<WorkspaceId>("ops"),
                     id<PermissionProfileId>("observe"),
                     {}};
    start.purpose = manual ? RunPurpose::control : RunPurpose::conversation;
    start.manual_observation_required = manual;
    push(start, manual ? 6 : 1, false);
    if (manual)
      push(
          HumanObservationRequested{invocation, fixture.request, tool, policy});
    else {
      RunProvenance provenance{"test", "fake", {}, id<ModelId>("fake"),
                               {},     {},     {}, {tool}};
      provenance.tool_policy = policy;
      push(RunProvenanceRecorded{provenance}, 1, false);
    }
    ToolProposed proposal{invocation,
                          tool.tool_name,
                          {"application/json", "{}"},
                          {Effect::read},
                          {},
                          true,
                          {scope},
                          {scope},
                          message};
    proposal.validated_arguments = proposal.arguments;
    proposal.observation_request = fixture.request;
    push(proposal, 3);
  }
  auto allow() -> void {
    push(ToolPolicyDecided{invocation, PolicyDecision::allow, {scope}, {}});
  }
  auto approval() -> void {
    push(ToolPolicyDecided{
        invocation, PolicyDecision::require_approval, {scope}, {}});
    push(ToolApprovalRequested{invocation, {scope}, {}});
  }
  auto started() -> void { push(ToolStarted{invocation}); }
  auto success(bool manual = true) -> void {
    const auto observation = fixture.observation();
    push(OpsObservationRecorded{invocation, observation});
    auto content = runtime::format_ops_observation_content(observation);
    REQUIRE(content);
    push(ToolResultRecorded{invocation, *content, message});
    if (manual) push(RunCompleted{}, 1, false);
  }
  auto failure(bool cancelled = false) -> void {
    push(ToolErrored{
        invocation, {ErrorCode::cancelled, "interrupted", false}, message});
    if (cancelled)
      push(RunCancelled{}, 1, false);
    else
      push(RunFailed{{ErrorCode::invalid_state, "observation failed", false}},
           1, false);
  }
  auto validate() const {
    return runtime::recorded_ops_observations(log, events);
  }
};
class Database {
 public:
  Database() {
    auto pattern =
        (std::filesystem::temp_directory_path() / "aiforge-ops-history-XXXXXX")
            .string();
    REQUIRE(::mkdtemp(pattern.data()) != nullptr);
    directory = pattern;
    path = directory / "sessions.sqlite3";
    open();
    REQUIRE(store->create_session({id<SessionId>("session"), at(0)}));
  }
  ~Database() {
    store.reset();
    std::error_code error;
    std::filesystem::remove_all(directory, error);
  }
  auto open() -> void {
    auto result = adapters::SqliteSessionStore::open(path);
    REQUIRE(result);
    store = std::move(*result);
  }
  auto mutate(const std::string& sql) -> void {
    store.reset();
    sqlite3* connection{};
    REQUIRE(sqlite3_open(path.c_str(), &connection) == SQLITE_OK);
    const auto result =
        sqlite3_exec(connection, sql.c_str(), nullptr, nullptr, nullptr);
    const auto closed = sqlite3_close(connection);
    REQUIRE(result == SQLITE_OK);
    REQUIRE(closed == SQLITE_OK);
    open();
  }
  std::filesystem::path directory, path;
  std::unique_ptr<adapters::SqliteSessionStore> store;
};
} // namespace
TEST_CASE("Ops history rejects missing atomic proof and foreign results") {
  for (unsigned mutation = 0; mutation < 16; ++mutation) {
    History history;
    history.admission();
    history.allow();
    history.started();
    history.success();
    REQUIRE(history.validate());
    switch (mutation) {
      case 0: history.events.erase(history.events.begin() + 1); break;
      case 1: history.events.erase(history.events.begin() + 2); break;
      case 2:
        std::get<RunStarted>(history.events[0].payload)
            .manual_observation_required = false;
        break;
      case 3: history.events[0].metadata.schema_version = 3; break;
      case 4:
        std::get<RunStarted>(history.events[0].payload).purpose =
            RunPurpose::conversation;
        break;
      case 5:
        std::get<ToolProposed>(history.events[2].payload)
            .observation_request.reset();
        break;
      case 6: history.events[2].metadata.schema_version = 2; break;
      case 7:
        std::get<HumanObservationRequested>(history.events[1].payload)
            .request.selection_generation = 2;
        break;
      case 8:
        std::get<OpsObservationRecorded>(history.events[5].payload)
            .observation.request.selection_generation = 2;
        break;
      case 9:
        std::get<ToolResultRecorded>(history.events[6].payload).content = {
            TextBlock{"forged"}};
        break;
      case 10: history.events.erase(history.events.begin() + 5); break;
      case 11: history.events.erase(history.events.begin() + 6); break;
      case 12: history.events.pop_back(); break;
      case 13: history.events[6].metadata.invocation_id.reset(); break;
      case 14: history.events[5].metadata.run_id = id<RunId>("foreign"); break;
      case 15:
        std::get<HumanObservationRequested>(history.events[1].payload)
            .policy.permission_profile_id = id<PermissionProfileId>("foreign");
        break;
    }
    INFO(mutation);
    const auto result = history.validate();
    REQUIRE_FALSE(result);
    CHECK(result.error().code == runtime::OpsHistoryErrorCode::invalid_history);
  }
}
TEST_CASE("Ops history denies ordinary or repeated work in manual runs") {
  for (unsigned mutation = 0; mutation < 7; ++mutation) {
    History history;
    history.admission();
    switch (mutation) {
      case 0:
        history.push(ToolProgressed{history.invocation, {TextBlock{"text"}}});
        break;
      case 1:
        history.push(RunProvenanceRecorded{RunProvenance{
                         "v", "fake", {}, id<ModelId>("fake"), {}, {}, {}, {}}},
                     1, false);
        break;
      case 2:
        history.push(HumanObservationRequested{history.invocation,
                                               history.fixture.request,
                                               history.tool, history.policy});
        break;
      case 3: history.push(ToolStarted{history.invocation}); break;
      case 4: history.push(RunCompleted{}, 1, false); break;
      case 5:
        history.push(ToolResultRecorded{
            history.invocation, {TextBlock{"unproved"}}, history.message});
        break;
      case 6: history.events.push_back(history.events[2]); break;
    }
    INFO(mutation);
    CHECK_FALSE(history.validate());
  }
  History absent;
  absent.admission();
  absent.events.erase(absent.events.begin() + 1, absent.events.end());
  CHECK_FALSE(absent.validate());
  absent.events.clear();
  absent.admission();
  absent.events.erase(absent.events.begin() + 2, absent.events.end());
  CHECK_FALSE(absent.validate());
}
TEST_CASE(
    "Ops history cannot resurrect a denied approval or widen scope effects") {
  History history;
  history.admission();
  history.approval();
  REQUIRE(history.validate());
  history.push(
      ToolApprovalDecided{history.invocation, ApprovalDecision::denied, {}});
  CHECK_FALSE(history.validate());
  history.push(ToolApprovalRequested{history.invocation, {history.scope}, {}});
  history.push(ToolApprovalDecided{
      history.invocation, ApprovalDecision::approved, {history.scope}});
  CHECK_FALSE(history.validate());
  history.events.erase(history.events.begin() + 6, history.events.end());
  history.failure();
  REQUIRE(history.validate());
  History widened;
  widened.admission();
  const CapabilityScope execution{Effect::execute, "process.command",
                                  "/usr/bin/fixture"};
  auto& marker = std::get<HumanObservationRequested>(widened.events[1].payload);
  marker.tool.declared_effects.push_back(Effect::execute);
  marker.tool.capability_scopes.push_back(execution);
  marker.policy.restriction_level = ToolRestrictionLevel::none;
  marker.policy.effect_ceiling.push_back(Effect::execute);
  marker.policy.capability_ceiling.push_back(execution);
  REQUIRE(validate_tool_policy_provenance(marker.policy));
  auto& proposal = std::get<ToolProposed>(widened.events[2].payload);
  proposal.requested_scopes.push_back(execution);
  proposal.validated_required_scopes.push_back(execution);
  CHECK_FALSE(widened.validate());
}
TEST_CASE("Ops history reports admitted unfinished reads without restoring "
          "authority") {
  for (unsigned prefix = 0; prefix < 3; ++prefix) {
    History history;
    history.admission();
    if (prefix == 1) history.approval();
    if (prefix == 2) {
      history.allow();
      history.started();
    }
    const auto result = history.validate();
    REQUIRE(result);
    CHECK(result->unfinished_manual_runs == std::vector<RunId>{history.run});
    CHECK(result->invocations.size() == 1);
    CHECK_FALSE(result->invocations[0].observation_event_id);
    history.failure(prefix == 1);
    const auto terminal = history.validate();
    REQUIRE(terminal);
    CHECK(terminal->unfinished_manual_runs.empty());
    CHECK(terminal->invocations[0].phase == OpsInvocationPhase::failed);
  }
  History observed;
  observed.admission();
  observed.allow();
  observed.started();
  observed.push(OpsObservationRecorded{observed.invocation,
                                       observed.fixture.observation()});
  CHECK_FALSE(observed.validate());
}
TEST_CASE("Ops request codec has independent strict bounds") {
  const auto request =
      Fixture{OpsObservationOperation::kubernetes_pod_logs}.request;
  auto encoded = adapters::encode_recorded_ops_request(request);
  REQUIRE(encoded);
  const auto decoded = adapters::decode_recorded_ops_request(*encoded);
  REQUIRE(decoded);
  CHECK(*decoded == request);
  for (const auto* document :
       {"", "{}", "{\"version\":1,\"version\":1}", "[]", "{} {}"})
    CHECK_FALSE(adapters::decode_recorded_ops_request(document));
  auto invalid = Json::parse(*encoded);
  invalid["extra"] = true;
  CHECK_FALSE(adapters::decode_recorded_ops_request(invalid.dump()));
  const auto large = adapters::decode_recorded_ops_request(
      std::string(adapters::maximum_ops_request_json_bytes + 1, ' '));
  REQUIRE_FALSE(large);
  CHECK(large.error() == adapters::OpsObservationJsonError::resource_exhausted);
  auto bad = request;
  bad.selection_generation = 0;
  CHECK_FALSE(adapters::encode_recorded_ops_request(bad));
}
TEST_CASE("Ops event SQLite schemas reject stripped or misplaced proof") {
  for (unsigned mutation = 0; mutation < 10; ++mutation) {
    Database database;
    History history;
    history.admission();
    REQUIRE(database.store->append_events(history.log.session_id(),
                                          history.events));
    std::string sql;
    switch (mutation) {
      case 0:
        sql = "UPDATE events SET "
              "payload_json=json_remove(payload_json,'$.manual_observation_"
              "required') WHERE sequence=1";
        break;
      case 1:
        sql = "UPDATE events SET "
              "payload_json=json_set(payload_json,'$.manual_observation_"
              "required',json('false')) WHERE sequence=1";
        break;
      case 2:
        sql = "UPDATE events SET "
              "payload_json=json_set(payload_json,'$.manual_observation_"
              "required',1) WHERE sequence=1";
        break;
      case 3:
        sql = "UPDATE events SET schema_version=3 WHERE sequence=1";
        break;
      case 4:
        sql = "UPDATE events SET "
              "payload_json=json_set(payload_json,'$.purpose','conversation') "
              "WHERE sequence=1";
        break;
      case 5:
        sql = "UPDATE events SET "
              "payload_json=json_remove(payload_json,'$.observation_request') "
              "WHERE sequence=3";
        break;
      case 6:
        sql = "UPDATE events SET "
              "payload_json=json_set(payload_json,'$.observation_request',null)"
              " WHERE sequence=3";
        break;
      case 7:
        sql = "UPDATE events SET schema_version=2 WHERE sequence=3";
        break;
      case 8:
        sql = "UPDATE events SET "
              "payload_json=json_set(payload_json,'$.unexpected',true) WHERE "
              "sequence=3";
        break;
      case 9:
        sql = "UPDATE events SET "
              "payload_json=json_set(payload_json,'$.validated_arguments',null)"
              " WHERE sequence=3";
        break;
    }
    INFO(mutation);
    database.mutate(sql);
    CHECK_FALSE(database.store->replay_events(history.log.session_id()));
  }
}
TEST_CASE("Ops event schema mismatch rolls back the complete SQLite batch") {
  Database database;
  History history;
  history.admission();
  history.allow();
  history.started();
  history.success();
  auto mismatched = history.events;
  mismatched[2].metadata.schema_version = 2;
  CHECK_FALSE(
      database.store->append_events(history.log.session_id(), mismatched));
  const auto empty = database.store->replay_events(history.log.session_id());
  REQUIRE(empty);
  CHECK(empty->empty());
  REQUIRE(
      database.store->append_events(history.log.session_id(), history.events));
  const auto replay = database.store->replay_events(history.log.session_id());
  REQUIRE(replay);
  CHECK(*replay == history.events);
  SessionEventLog log{history.log.session_id()};
  for (const auto& event : *replay)
    REQUIRE(log.append(event));
  const auto result = runtime::recorded_ops_observations(log);
  REQUIRE(result);
  CHECK(result->unfinished_manual_runs.empty());
  CHECK(result->invocations[0].phase == OpsInvocationPhase::succeeded);
}
TEST_CASE("Single tool provenance validation preserves whole-run semantics") {
  History history;
  RunProvenance provenance{"test", "fake", {}, id<ModelId>("fake"),
                           {},     {},     {}, {history.tool}};
  REQUIRE(validate_tool_provenance_entry(history.tool));
  REQUIRE(validate_run_provenance(provenance));
  for (unsigned mutation = 0; mutation < 4; ++mutation) {
    auto tool = history.tool;
    if (mutation == 0) tool.tool_name = "";
    if (mutation == 1) tool.registration_digest = "bad";
    if (mutation == 2) tool.capability_scopes[0].kind = "";
    if (mutation == 3) tool.capability_scopes[0].value = std::string(4097, 'x');
    provenance.tools = {tool};
    const auto one = validate_tool_provenance_entry(tool);
    const auto whole = validate_run_provenance(provenance);
    REQUIRE_FALSE(one);
    REQUIRE_FALSE(whole);
    CHECK(one.error() == whole.error());
  }
  RunProvenanceLimits limits;
  limits.maximum_tools = 0;
  CHECK_FALSE(validate_tool_provenance_entry(history.tool, limits));
}
TEST_CASE("Canonical Ops text preserves quotes unknowns all variants and "
          "source order") {
  for (const auto operation : {OpsObservationOperation::linux_health,
                               OpsObservationOperation::linux_services,
                               OpsObservationOperation::linux_service_health,
                               OpsObservationOperation::linux_service_logs,
                               OpsObservationOperation::kubernetes_workloads,
                               OpsObservationOperation::kubernetes_pod_health,
                               OpsObservationOperation::kubernetes_events,
                               OpsObservationOperation::kubernetes_pod_logs}) {
    auto observation = Fixture{operation}.observation();
    auto formatted = runtime::format_ops_observation_content(observation);
    REQUIRE(formatted);
    REQUIRE(formatted->size() == 1);
    const auto& text = std::get<TextBlock>(formatted->front()).text;
    CHECK(text.starts_with("AIForge Ops observation v1\n"));
    CHECK(text.find("observation.request.owner_id=\"owner\"\n") !=
          std::string::npos);
    CHECK(text.find("observation.started_at_ms=1000000\n") !=
          std::string::npos);
    CHECK(text.size() <= runtime::maximum_ops_observation_content_bytes);
    CHECK(runtime::format_ops_observation_content(observation) == formatted);
  }
  auto observation =
      Fixture{OpsObservationOperation::linux_service_logs}.observation();
  std::get<OpsLogObservation>(observation.payload).lines = {
      {at(999005), "quoted \" slash \\"},
      {at(999000), ""},
      {at(999003), "café"}};
  observation.completeness = OpsObservationCompleteness::truncated;
  observation.omitted_entries.reset();
  auto formatted = runtime::format_ops_observation_content(observation);
  REQUIRE(formatted);
  const auto& text = std::get<TextBlock>(formatted->front()).text;
  CHECK(text.find("observation.omitted_entries=unknown\n") !=
        std::string::npos);
  CHECK(text.find("lines[1].text=\"\"\n") != std::string::npos);
  CHECK(text.find("lines[0].timestamp_ms=999005") <
        text.find("lines[1].timestamp_ms=999000"));
  CHECK(text.find("quoted \\\" slash \\\\") != std::string::npos);
  std::get<OpsLogObservation>(observation.payload).lines[0].text = "line\nfeed";
  CHECK_FALSE(runtime::format_ops_observation_content(observation));
}
TEST_CASE(
    "Manual Ops cancellation cannot resume collection or publish a result") {
  for (bool running : {false, true}) {
    History history;
    history.admission();
    history.allow();
    if (running) history.started();
    history.push(RunCancelRequested{}, 1, false);
    const auto cancelled_prefix = history.events;
    if (!running) history.started();
    history.success();
    CHECK_FALSE(history.validate());
    history.events = cancelled_prefix;
    history.failure(true);
    REQUIRE(history.validate());
  }
}
TEST_CASE(
    "Unknown events cannot replace required known Ops admission records") {
  for (unsigned replacement : {1U, 2U}) {
    History history;
    history.admission();
    history.events[replacement].payload =
        UnknownEvent{"future.fact", {"application/json", "{}"}};
    CHECK_FALSE(history.validate());
  }
  History history;
  history.admission();
  history.push(UnknownEvent{"future.fact", {"application/json", "{}"}}, 99,
               false);
  history.allow();
  history.started();
  history.success();
  const auto result = history.validate();
  REQUIRE(result);
  CHECK(result->last_sequence == history.events.back().metadata.sequence);
  history.events.back().metadata.sequence =
      history.events.front().metadata.sequence;
  CHECK_FALSE(history.validate());
}
TEST_CASE(
    "Ops history accepts its exact invocation budget and rejects overflow") {
  History history;
  history.events.reserve((runtime::maximum_ops_history_invocations + 1) * 5);
  for (std::size_t i = 0; i <= runtime::maximum_ops_history_invocations; ++i) {
    const auto suffix = std::to_string(i);
    history.run = id<RunId>("run-" + suffix);
    history.invocation = id<InvocationId>("invocation-" + suffix);
    history.message = id<MessageId>("message-" + suffix);
    history.fixture.request.request_id = id<OpsRequestId>("request-" + suffix);
    history.admission();
    history.failure();
    if (i + 1 == runtime::maximum_ops_history_invocations) {
      const auto exact = history.validate();
      REQUIRE(exact);
      CHECK(exact->invocations.size() ==
            runtime::maximum_ops_history_invocations);
    }
  }
  const auto overflow = history.validate();
  REQUIRE_FALSE(overflow);
  CHECK(overflow.error().code ==
        runtime::OpsHistoryErrorCode::resource_exhausted);
}
TEST_CASE("Ops requests cannot be reused by another invocation") {
  History history;
  history.admission();
  history.failure();
  history.run = id<RunId>("second-run");
  history.invocation = id<InvocationId>("second-invocation");
  history.message = id<MessageId>("second-message");
  history.admission();
  history.failure();
  CHECK_FALSE(history.validate());
}
TEST_CASE("Manual and model Ops history complete without collection") {
  for (bool manual : {false, true}) {
    History history;
    history.admission(manual);
    history.allow();
    history.started();
    history.success(manual);
    const auto result = history.validate();
    REQUIRE(result);
    REQUIRE(result->invocations.size() == 1);
    CHECK(result->invocations[0].human_origin == manual);
    CHECK(result->invocations[0].request == history.fixture.request);
    CHECK(result->invocations[0].phase == OpsInvocationPhase::succeeded);
    CHECK(result->unfinished_manual_runs.empty());
  }
}
