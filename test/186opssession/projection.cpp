#include <aiforge/runtime/ops_observation_history.hpp>
#include <aiforge/surfaces/manual_ops_session.hpp>
#include <algorithm>
#include <catch2/catch_test_macros.hpp>

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
  auto success() -> void {
    OpsObservation observation{
        request,
        EventTimestamp{std::chrono::milliseconds{10}},
        EventTimestamp{std::chrono::milliseconds{11}},
        OpsObservationCompleteness::complete,
        0,
        0,
        {},
        LinuxHealthObservation{OpsHealthState::healthy, 12, {}, 1, 0}};
    auto content = runtime::format_ops_observation_content(observation);
    REQUIRE(content);
    push(OpsObservationRecorded{submission.invocation_id, observation});
    push(ToolResultRecorded{submission.invocation_id, *content, result});
    push(RunCompleted{}, 1, false);
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
  REQUIRE(h.events == original);
}
