#include <aiforge/runtime/ops_observation_history.hpp>
#include <aiforge/surfaces/manual_ops_session.hpp>
#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <stop_token>

namespace {
using namespace aiforge;
using namespace aiforge::domain;
using namespace aiforge::surfaces;
template <class T> auto id(std::string text) -> T {
  return T::from(std::move(text)).value();
}
struct History {
  std::vector<RunEvent> events;
  ObservationSubmission submission{id<RunId>("manual"),
                                   id<InvocationId>("invocation")};
  MessageId result{id<MessageId>("result")};
  OpsObservationRequest request{
      id<OpsOwnerId>("owner"),
      id<SessionId>("session"),
      id<OpsRequestId>("request"),
      {id<OpsTargetId>("target"), id<OpsConfigurationRevision>("revision"),
       LinuxOpsIdentity{LinuxExecutionScope::container,
                        "12345678-1234-1234-1234-123456789abc", 42, 43}},
      1,
      OpsObservationOperation::linux_health,
      {},
      1,
      {}};
  CapabilityScope scope{Effect::read, "ops.target", "target"};
  auto push(RunEventPayload payload, unsigned schema = 1, bool tool = true)
      -> void {
    const auto sequence = events.size() + 1;
    events.push_back(
        {{id<EventId>("event-" + std::to_string(sequence)),
          submission.run_id,
          sequence,
          schema,
          EventTimestamp{std::chrono::milliseconds{sequence}},
          {},
          {},
          tool ? std::optional<InvocationId>{submission.invocation_id}
               : std::nullopt},
         std::move(payload)});
  }
  auto begin() -> void {
    RunStarted start{id<SurfaceId>("admin"),
                     id<WorkspaceId>("ops"),
                     id<PermissionProfileId>("observe"),
                     {}};
    start.purpose = RunPurpose::control;
    start.manual_observation_required = true;
    push(start, 6, false);
    ToolProvenanceEntry tool{"observe_target",
                             {Effect::read},
                             {scope},
                             "sha256:" + std::string(64, 'a')};
    ToolPolicyProvenance policy{"aiforge.tool-launch-policy.v1",
                                id<PermissionProfileId>("observe"),
                                ToolRestrictionLevel::medium,
                                ToolApprovalMode::allow_all,
                                {Effect::read},
                                {scope},
                                {}};
    push(HumanObservationRequested{submission.invocation_id, request, tool,
                                   policy});
    ToolProposed proposal{submission.invocation_id,
                          "observe_target",
                          {"application/json", "{}"},
                          {Effect::read},
                          {},
                          true,
                          {scope},
                          {scope},
                          result};
    proposal.validated_arguments = proposal.arguments;
    proposal.observation_request = request;
    push(proposal, 3);
    push(ToolPolicyDecided{
        submission.invocation_id, PolicyDecision::allow, {scope}, {}});
    push(ToolStarted{submission.invocation_id});
  }
  auto observation() -> OpsObservation {
    OpsObservation observation{
        request,
        EventTimestamp{std::chrono::milliseconds{10}},
        EventTimestamp{std::chrono::milliseconds{11}},
        OpsObservationCompleteness::complete,
        0,
        0,
        {},
        LinuxHealthObservation{OpsHealthState::healthy, 12, {}, 1, 0}};
    return observation;
  }
  auto record_observation() -> void {
    push(OpsObservationRecorded{submission.invocation_id, observation()});
  }
  auto record_result() -> void {
    const auto observed = observation();
    auto content = runtime::format_ops_observation_content(observed);
    REQUIRE(content);
    push(ToolResultRecorded{submission.invocation_id, *content, result});
  }
  auto complete() -> void { push(RunCompleted{}, 1, false); }
  auto success() -> void {
    const auto observed = observation();
    auto content = runtime::format_ops_observation_content(observed);
    REQUIRE(content);
    push(OpsObservationRecorded{submission.invocation_id, observed});
    push(ToolResultRecorded{submission.invocation_id, *content, result});
    complete();
  }
  auto select(OpsTargetBinding target, std::uint64_t generation) -> void {
    const auto saved = submission;
    submission = {id<RunId>("selection-" + std::to_string(generation)),
                  id<InvocationId>("selection-metadata")};
    RunStarted start{id<SurfaceId>("admin"),
                     id<WorkspaceId>("ops"),
                     id<PermissionProfileId>("observe"),
                     {}};
    start.purpose = RunPurpose::control;
    push(start, 3, false);
    push(OpsTargetSelected{std::move(target), generation}, 1, false);
    push(RunCompleted{}, 1, false);
    submission = saved;
  }
  auto log() const -> SessionEventLog {
    SessionEventLog output{request.session_id};
    for (const auto& event : events)
      REQUIRE(output.append(event));
    return output;
  }
};
} // namespace
TEST_CASE("Manual projector rejects incomplete or substituted terminal proof",
          "[manual-projection]") {
  History h;
  h.begin();
  h.success();
  SECTION("missing observation") {
    h.events.erase(h.events.begin() + 5);
  }
  SECTION("missing result") {
    h.events.erase(h.events.begin() + 6);
  }
  SECTION("completion alone") {
    h.events.erase(h.events.begin() + 5, h.events.begin() + 7);
  }
  SECTION("request mismatch") {
    std::get<OpsObservationRecorded>(h.events[5].payload)
        .observation.request.selection_generation++;
  }
  SECTION("result invocation mismatch") {
    std::get<ToolResultRecorded>(h.events[6].payload).invocation_id =
        id<InvocationId>("foreign");
  }
  SECTION("canonical content mismatch") {
    std::get<ToolResultRecorded>(h.events[6].payload).content = {
        TextBlock{"unproved"}};
  }
  SECTION("completion missing") {
    h.events.pop_back();
  }
  SECTION("cancelled resurrection") {
    auto event = h.events[4];
    event.payload = RunCancelRequested{};
    event.metadata.invocation_id.reset();
    h.events.insert(h.events.begin() + 4, std::move(event));
    for (std::size_t index = 0; index < h.events.size(); ++index) {
      h.events[index].metadata.sequence = index + 1;
      h.events[index].metadata.event_id =
          id<EventId>("reindexed-" + std::to_string(index));
    }
  }
  auto result = project_manual_observations(h.log(), h.submission);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == ManualOpsErrorCode::invalid_history);
}
TEST_CASE("Manual projector refuses selecting foreign or ordinary work",
          "[manual-projection]") {
  History h;
  h.begin();
  h.success();
  auto selected = h.submission;
  SECTION("foreign invocation") {
    selected.invocation_id = id<InvocationId>("foreign");
  }
  SECTION("foreign run") {
    selected.run_id = id<RunId>("foreign");
  }
  auto result = project_manual_observations(h.log(), selected);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == ManualOpsErrorCode::wrong_operation);
}
TEST_CASE("Manual projection preserves prior proof when a later run fails",
          "[manual-projection]") {
  History h;
  h.begin();
  h.success();
  const auto initial = project_manual_observations(h.log(), h.submission);
  REQUIRE(initial);
  REQUIRE(initial->current->status == RunStatus::completed);
  REQUIRE(initial->current->observation_event_id);
  REQUIRE(initial->latest_success);
  REQUIRE(initial->catalog[0] == initial->latest_success);
  CHECK_FALSE(initial->historical_selection);
  CHECK(initial->maximum_selection_generation == 1);
  h.submission = {id<RunId>("later"), id<InvocationId>("later-invocation")};
  h.request.request_id = id<OpsRequestId>("later-request");
  h.result = id<MessageId>("later-result");
  h.begin();
  h.push(ToolErrored{h.submission.invocation_id,
                     {ErrorCode::unavailable, "private error", false},
                     h.result});
  h.push(RunFailed{{ErrorCode::unavailable, "private error", false}}, 1, false);
  auto projected = project_manual_observations(h.log(), h.submission);
  REQUIRE(projected);
  REQUIRE(projected->current->status == RunStatus::failed);
  REQUIRE(projected->current->failure == ErrorCode::unavailable);
  REQUIRE_FALSE(projected->current->observation_event_id);
  REQUIRE(projected->latest_success == initial->latest_success);
  REQUIRE(projected->catalog[0] == initial->catalog[0]);
}
TEST_CASE("Mixed conversation and manual histories expose only human evidence",
          "[manual-projection]") {
  History h;
  h.push(RunStarted{id<SurfaceId>("chat"),
                    id<WorkspaceId>("chat"),
                    id<PermissionProfileId>("observe"),
                    {}},
         1, false);
  h.push(RunCompleted{}, 1, false);
  h.submission = {id<RunId>("manual-after-chat"),
                  id<InvocationId>("human-invocation")};
  h.begin();
  h.success();
  const auto original = h.events;
  auto projected = project_manual_observations(h.log(), h.submission);
  REQUIRE(projected);
  REQUIRE(projected->latest_success->submission == h.submission);
  REQUIRE(projected->latest_success->observation.request == h.request);
  REQUIRE(projected->catalog[0] == projected->latest_success);
  REQUIRE(h.events == original);
}

TEST_CASE("Manual projection retains only the latest successful proof per slot",
          "[manual-projection][catalog]") {
  History h;
  h.begin();
  h.success();
  const auto first = project_manual_observations(h.log());
  REQUIRE(first);
  REQUIRE(first->catalog[0]);
  const auto first_event = first->catalog[0]->observation_event_id;

  h.submission = {id<RunId>("replacement"),
                  id<InvocationId>("replacement-invocation")};
  h.request.request_id = id<OpsRequestId>("replacement-request");
  h.result = id<MessageId>("replacement-result");
  h.begin();
  h.success();
  const auto projected = project_manual_observations(h.log());
  REQUIRE(projected);
  REQUIRE(projected->catalog[0]);
  CHECK(projected->catalog[0]->observation_event_id != first_event);
  CHECK(projected->catalog[0] == projected->latest_success);
  for (std::size_t slot{1}; slot < projected->catalog.size(); ++slot)
    CHECK_FALSE(projected->catalog[slot]);
}

TEST_CASE("Manual projection keeps A evidence beside later B selection",
          "[manual-projection][catalog][selection]") {
  History h;
  h.begin();
  h.success();
  const auto captured = h.request.target;
  auto selected = captured;
  selected.target_id = id<OpsTargetId>("target-b");
  selected.configuration_revision = id<OpsConfigurationRevision>("revision-b");
  h.select(selected, 2);

  const auto projected = project_manual_observations(h.log());
  REQUIRE(projected);
  REQUIRE(projected->catalog[0]);
  CHECK(projected->catalog[0]->observation.request.target == captured);
  REQUIRE(projected->historical_selection);
  CHECK(projected->historical_selection->target == selected);
  CHECK(projected->maximum_selection_generation == 2);
}

TEST_CASE("Target selection cannot cross an observation publication",
          "[manual-projection][catalog][selection][failure]") {
  History h;
  h.begin();
  auto selected = h.request.target;
  selected.target_id = id<OpsTargetId>("target-b");
  selected.configuration_revision = id<OpsConfigurationRevision>("revision-b");
  SECTION("before observation") {
    h.select(selected, 2);
    h.success();
  }
  SECTION("between observation and result") {
    h.record_observation();
    h.select(selected, 2);
    h.record_result();
    h.complete();
  }
  const auto projected = project_manual_observations(h.log());
  REQUIRE_FALSE(projected);
  CHECK(projected.error().code == ManualOpsErrorCode::invalid_history);
}

TEST_CASE("Catalog latest follows successful completion order",
          "[manual-projection][catalog][ordering]") {
  History h;
  h.begin();
  const auto first = h.submission;
  h.record_observation();
  h.record_result();

  h.submission = {id<RunId>("second"), id<InvocationId>("second-invocation")};
  h.request.request_id = id<OpsRequestId>("second-request");
  h.result = id<MessageId>("second-result");
  h.begin();
  h.success();
  h.submission = first;
  h.complete();

  const auto projected = project_manual_observations(h.log());
  REQUIRE(projected);
  REQUIRE(projected->latest_success);
  REQUIRE(projected->catalog[0]);
  CHECK(projected->latest_success->submission == first);
  CHECK(projected->catalog[0]->submission == first);
}

TEST_CASE("Manual projection cancellation performs no partial hydration",
          "[manual-projection][catalog][cancel]") {
  History h;
  h.begin();
  h.success();
  std::stop_source stopped;
  stopped.request_stop();
  const auto projected =
      project_manual_observations(h.log(), {}, stopped.get_token());
  REQUIRE_FALSE(projected);
  CHECK(projected.error().code == ManualOpsErrorCode::cancelled);
}

TEST_CASE("Catalog slot mapping covers the final eight operations exactly",
          "[manual-projection][catalog][bounds]") {
  const std::array operations{OpsObservationOperation::linux_health,
                              OpsObservationOperation::linux_services,
                              OpsObservationOperation::linux_service_health,
                              OpsObservationOperation::kubernetes_workloads,
                              OpsObservationOperation::kubernetes_pod_health,
                              OpsObservationOperation::kubernetes_events,
                              OpsObservationOperation::linux_service_logs,
                              OpsObservationOperation::kubernetes_pod_logs};
  for (std::size_t slot{}; slot < operations.size(); ++slot)
    CHECK(manual_ops_catalog_slot(operations[slot]) == slot);
  CHECK(operations.size() == manual_ops_catalog_slots);
  CHECK(maximum_manual_ops_catalog_bytes == std::size_t{512} * 1024U);
}
