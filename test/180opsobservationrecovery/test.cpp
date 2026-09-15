// Failure matrix: malformed manual admission/proof and partial terminal pairs
// reject replay before append or calls; every valid unfinished policy/approval/
// execution/cancellation prefix becomes one atomic interruption, without retry,
// renewed approval or backend work. Store failure preserves the original log;
// generated ID collision and sequence overflow fail before append. Completed
// manual history remains unchanged. Mixed conversation recovery is preserved;
// manual/control tool and artifact events never become provider continuation
// turns, including projected conversation spans without retained RunStarted.

#include <aiforge/runtime/ops_observation_history.hpp>
#include <aiforge/runtime/run_kernel.hpp>
#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace {
using namespace aiforge;
using namespace aiforge::domain;
template <class T> auto id(std::string value) -> T {
  return T::from(std::move(value)).value();
}

class Store final : public storage::SessionStore {
 public:
  std::vector<RunEvent> history;
  std::vector<std::vector<RunEvent>> batches;
  bool fail_append{};
  unsigned attempts{};
  auto create_session(storage::SessionCreate, std::stop_token)
      -> std::expected<void, storage::SessionStoreError> override {
    return {};
  }
  auto open_session(const SessionId& session, std::stop_token)
      -> std::expected<storage::SessionInfo,
                       storage::SessionStoreError> override {
    return storage::SessionInfo{
        session,
        {},
        {},
        history.empty() ? 0 : history.back().metadata.sequence,
        1};
  }
  auto list_sessions(std::size_t, std::stop_token)
      -> std::expected<std::vector<storage::SessionInfo>,
                       storage::SessionStoreError> override {
    return std::vector<storage::SessionInfo>{};
  }
  auto append_events(const SessionId&, std::span<const RunEvent> events,
                     std::stop_token)
      -> std::expected<void, storage::SessionStoreError> override {
    ++attempts;
    if (fail_append)
      return std::unexpected(
          storage::SessionStoreError{storage::SessionStoreErrorCode::io_failure,
                                     "fixture refused append", true});
    batches.emplace_back(events.begin(), events.end());
    history.insert(history.end(), events.begin(), events.end());
    return {};
  }
  auto replay_events(const SessionId&, std::stop_token)
      -> std::expected<std::vector<RunEvent>,
                       storage::SessionStoreError> override {
    return history;
  }
};
class Backend final : public backend::Backend {
 public:
  std::atomic<unsigned> calls{};
  auto start(backend::BackendRequest, std::stop_token)
      -> std::expected<std::unique_ptr<backend::BackendStream>,
                       backend::BackendError> override {
    ++calls;
    return std::unexpected(
        backend::BackendError{backend::BackendErrorKind::script_mismatch,
                              "unexpected backend call",
                              false,
                              {}});
  }
};
class Executor final : public runtime::ToolExecutor {
 public:
  mutable std::atomic<unsigned> validations{};
  std::atomic<unsigned> starts{};
  auto validate(const StructuredDataBlock& arguments) const
      -> std::expected<runtime::ValidatedToolArguments,
                       runtime::ToolExecutionError> override {
    ++validations;
    return runtime::ValidatedToolArguments{arguments};
  }
  auto start(runtime::ToolInvocation, std::stop_token)
      -> std::expected<std::unique_ptr<runtime::ToolExecutionStream>,
                       runtime::ToolExecutionError> override {
    ++starts;
    return std::unexpected(runtime::ToolExecutionError{
        runtime::ToolExecutionErrorCode::internal_failure,
        "unexpected collection", false});
  }
};
class Policy final : public runtime::ToolPolicy {
 public:
  mutable unsigned inspections{};
  unsigned evaluations{};
  unsigned approvals{};
  auto evaluate(const runtime::ToolPolicyRequest&)
      -> std::expected<runtime::ToolPolicyResolution,
                       runtime::ToolPolicyError> override {
    ++evaluations;
    return runtime::ToolPolicyResolution{};
  }
  auto approve(const runtime::ToolPolicyRequest&, runtime::ToolPolicyApproval)
      -> std::expected<runtime::ToolPolicyResolution,
                       runtime::ToolPolicyError> override {
    ++approvals;
    return runtime::ToolPolicyResolution{};
  }
  auto provenance() const noexcept -> const ToolPolicyProvenance* override {
    ++inspections;
    return nullptr;
  }
};

struct Fixture {
  SessionId session{id<SessionId>("session")};
  RunId run{id<RunId>("manual")};
  InvocationId invocation{id<InvocationId>("manual-invocation")};
  MessageId message{id<MessageId>("manual-result")};
  CapabilityScope scope{Effect::read, "filesystem.root", "/fixture"};
  OpsObservationRequest request{
      id<OpsOwnerId>("owner"),
      session,
      id<OpsRequestId>("request"),
      {id<OpsTargetId>("target"), id<OpsConfigurationRevision>("revision"),
       LinuxOpsIdentity{LinuxExecutionScope::container,
                        "12345678-1234-1234-1234-123456789abc", 42, 43}},
      1,
      OpsObservationOperation::linux_health,
      {},
      1,
      {}};
  ToolProvenanceEntry tool{"observe_target",
                           {Effect::read},
                           {scope},
                           "sha256:" + std::string(64, 'a')};
  ToolPolicyProvenance provenance{"aiforge.tool-launch-policy.v1",
                                  id<PermissionProfileId>("observe"),
                                  ToolRestrictionLevel::medium,
                                  ToolApprovalMode::prompt,
                                  {Effect::read},
                                  {scope},
                                  {}};
  Store store;
  Backend backend;
  std::shared_ptr<Executor> executor{std::make_shared<Executor>()};
  std::shared_ptr<Policy> policy{std::make_shared<Policy>()};
  runtime::ToolRegistrySnapshot tools;
  Fixture() {
    runtime::ToolRegistry registry;
    REQUIRE(registry.register_tool(
        {"observe_target",
         "Unreachable observation fixture",
         {"application/schema+json", R"({"type":"object"})"},
         {Effect::read},
         {scope}},
        executor));
    auto captured = registry.snapshot();
    REQUIRE(captured);
    tools = std::move(*captured);
  }
  auto push(RunEventPayload payload, unsigned schema = 1,
            bool tool_event = true) -> void {
    const auto sequence = store.history.size() + 1;
    store.history.push_back(
        {{id<EventId>("event-" + std::to_string(sequence)),
          run,
          sequence,
          schema,
          EventTimestamp{std::chrono::milliseconds{sequence}},
          {},
          {},
          tool_event ? std::optional<InvocationId>{invocation} : std::nullopt},
         std::move(payload)});
  }
  auto admission() -> void {
    RunStarted start{id<SurfaceId>("test"),
                     id<WorkspaceId>("ops"),
                     id<PermissionProfileId>("observe"),
                     {}};
    start.purpose = RunPurpose::control;
    start.manual_observation_required = true;
    push(start, 6, false);
    push(HumanObservationRequested{invocation, request, tool, provenance});
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
    proposal.observation_request = request;
    push(proposal, 3);
  }
  auto model_admission() -> void {
    admission();
    auto& start = std::get<RunStarted>(store.history.front().payload);
    start.manual_observation_required = false;
    start.purpose = RunPurpose::conversation;
    store.history.front().metadata.schema_version = 1;
    RunProvenance recorded{"test", "fake", {}, id<ModelId>("fake"),
                           {},     {},     {}, {tool}};
    recorded.tool_policy = provenance;
    store.history[1].payload = RunProvenanceRecorded{recorded};
    store.history[1].metadata.invocation_id.reset();
  }
  auto allow() -> void {
    push(ToolPolicyDecided{invocation, PolicyDecision::allow, {scope}, {}});
  }
  auto approval() -> void {
    push(ToolPolicyDecided{
        invocation, PolicyDecision::require_approval, {scope}, {}});
    push(ToolApprovalRequested{invocation, {scope}, {}});
  }
  auto success() -> void {
    OpsObservation observation{
        request,
        EventTimestamp{std::chrono::milliseconds{1000}},
        EventTimestamp{std::chrono::milliseconds{1100}},
        OpsObservationCompleteness::complete,
        0,
        0,
        {},
        LinuxHealthObservation{OpsHealthState::healthy, 12, {}, 1, 0}};
    const auto content = runtime::format_ops_observation_content(observation);
    REQUIRE(content);
    push(OpsObservationRecorded{invocation, observation});
    push(ToolResultRecorded{invocation, *content, message});
    push(RunCompleted{}, 1, false);
  }
  auto log() const -> SessionEventLog {
    SessionEventLog result{session};
    for (const auto& event : store.history)
      REQUIRE(result.append(event));
    return result;
  }
  auto reopen() {
    return runtime::RunKernel::open_durable(
        {session, runtime::DurableSessionMode::resume, {}}, store, backend,
        nullptr, [] { return EventTimestamp{std::chrono::milliseconds{2000}}; },
        {}, tools, policy);
  }
  auto no_calls() const -> void {
    REQUIRE(backend.calls == 0);
    REQUIRE(executor->validations == 0);
    REQUIRE(executor->starts == 0);
    REQUIRE(policy->inspections == 0);
    REQUIRE(policy->evaluations == 0);
    REQUIRE(policy->approvals == 0);
  }
};
} // namespace

TEST_CASE("malformed manual histories reject before recovery writes",
          "[ops][recovery]") {
  Fixture f;
  f.admission();
  SECTION("missing intent") {
    f.store.history.erase(f.store.history.begin() + 1);
  }
  SECTION("missing proposal") {
    f.store.history.pop_back();
  }
  SECTION("missing typed proof") {
    std::get<ToolProposed>(f.store.history.back().payload)
        .observation_request.reset();
  }
  SECTION("unmarked manual start") {
    std::get<RunStarted>(f.store.history.front().payload)
        .manual_observation_required = false;
    f.store.history.front().metadata.schema_version = 3;
  }
  SECTION("terminal error without atomic run failure") {
    f.push(ToolErrored{
        f.invocation, {ErrorCode::invalid_state, "failed", false}, f.message});
  }
  SECTION("observation without atomic result") {
    f.allow();
    f.push(ToolStarted{f.invocation});
    f.success();
    f.store.history.erase(f.store.history.end() - 2, f.store.history.end());
  }
  SECTION("result without atomic run completion") {
    f.allow();
    f.push(ToolStarted{f.invocation});
    f.success();
    f.store.history.pop_back();
  }
  const auto original = f.store.history;
  const auto classified = runtime::classify_recoverable_run(f.log());
  REQUIRE_FALSE(classified);
  REQUIRE(classified.error().code ==
          runtime::RunKernelErrorCode::replay_rejected);
  const auto opened = f.reopen();
  REQUIRE_FALSE(opened);
  REQUIRE(opened.error().code == runtime::RunKernelErrorCode::replay_rejected);
  REQUIRE(f.store.history == original);
  REQUIRE(f.store.attempts == 0);
  f.no_calls();
}

TEST_CASE("valid unfinished manual prefixes become one atomic interruption",
          "[ops][recovery]") {
  Fixture f;
  f.admission();
  SECTION("admitted before policy") {
  }
  SECTION("policy allowed") {
    f.allow();
  }
  SECTION("awaiting approval") {
    f.approval();
  }
  SECTION("approved but unstarted") {
    f.approval();
    f.push(ToolApprovalDecided{
        f.invocation, ApprovalDecision::approved, {f.scope}});
  }
  SECTION("collection running") {
    f.allow();
    f.push(ToolStarted{f.invocation});
  }
  SECTION("cancelled before start") {
    f.allow();
    f.push(RunCancelRequested{}, 1, false);
  }
  SECTION("cancelled during collection") {
    f.allow();
    f.push(ToolStarted{f.invocation});
    f.push(RunCancelRequested{}, 1, false);
  }
  const auto classified = runtime::classify_recoverable_run(f.log());
  REQUIRE(classified);
  REQUIRE_FALSE(*classified);
  auto opened = f.reopen();
  INFO((opened ? "opened" : opened.error().message));
  REQUIRE(opened);
  REQUIRE(f.store.batches.size() == 1);
  REQUIRE(f.store.batches.front().size() == 2);
  const auto& error =
      std::get<ToolErrored>(f.store.batches.front().front().payload);
  REQUIRE(error.invocation_id == f.invocation);
  REQUIRE(error.result_message_id == f.message);
  REQUIRE(std::holds_alternative<RunFailed>(
      f.store.batches.front().back().payload));
  REQUIRE((*opened)->projection(f.run)->status() == RunStatus::failed);
  REQUIRE_FALSE((*opened)->active_run_id());
  REQUIRE_FALSE((*opened)->pending_tool_dispatch());
  REQUIRE_FALSE((*opened)->pending_tool_approval());
  REQUIRE((*opened)->drain());
  REQUIRE(runtime::recorded_ops_observations((*opened)->event_log()));
  auto continuation =
      runtime::tool_continuation_messages((*opened)->event_log().events());
  REQUIRE(continuation);
  REQUIRE(continuation->empty());
  opened->reset();
  auto repeated = f.reopen();
  REQUIRE(repeated);
  REQUIRE(f.store.attempts == 1);
  f.no_calls();
}

TEST_CASE("failed interruption persistence leaves no partial recovery",
          "[ops][recovery]") {
  Fixture f;
  f.admission();
  f.approval();
  const auto original = f.store.history;
  f.store.fail_append = true;
  const auto opened = f.reopen();
  REQUIRE_FALSE(opened);
  REQUIRE(opened.error().code == runtime::RunKernelErrorCode::storage_failure);
  REQUIRE(opened.error().retryable);
  REQUIRE(f.store.attempts == 1);
  REQUIRE(f.store.batches.empty());
  REQUIRE(f.store.history == original);
  f.no_calls();
  f.store.fail_append = false;
  REQUIRE(f.reopen());
  REQUIRE(f.store.batches.size() == 1);
  f.no_calls();
}

TEST_CASE("manual interruption preserves event identity and sequence guards",
          "[ops][recovery]") {
  Fixture f;
  f.admission();
  SECTION("generated event ID already exists") {
    f.store.history.front().metadata.event_id = id<EventId>("event-4");
  }
  SECTION("no sequence remains") {
    f.store.history.back().metadata.sequence =
        std::numeric_limits<std::uint64_t>::max();
  }
  const auto original = f.store.history;
  REQUIRE_FALSE(f.reopen());
  REQUIRE(f.store.attempts == 0);
  REQUIRE(f.store.history == original);
  f.no_calls();
}

TEST_CASE("completed manual evidence reopens without any new event or call",
          "[ops][recovery]") {
  Fixture f;
  f.admission();
  f.allow();
  f.push(ToolStarted{f.invocation});
  SECTION("successful evidence") {
    f.success();
  }
  SECTION("failed observation") {
    f.push(ToolErrored{
        f.invocation, {ErrorCode::invalid_state, "failed", false}, f.message});
    f.push(RunFailed{{ErrorCode::invalid_state, "failed", false}}, 1, false);
  }
  const auto original = f.store.history;
  auto opened = f.reopen();
  REQUIRE(opened);
  REQUIRE((*opened)->drain());
  REQUIRE(f.store.history == original);
  REQUIRE(f.store.attempts == 0);
  f.no_calls();
}

TEST_CASE(
    "manual recovery candidates do not hide unrelated awaiting conversation",
    "[ops][recovery]") {
  Fixture f;
  f.admission();
  f.approval();
  f.run = id<RunId>("conversation");
  f.push(RunStarted{id<SurfaceId>("test"),
                    id<WorkspaceId>("chat"),
                    id<PermissionProfileId>("observe"),
                    {}},
         1, false);
  f.push(RunAwaitingInput{id<QuestionId>("input")}, 1, false);
  const auto classified = runtime::classify_recoverable_run(f.log());
  REQUIRE(classified);
  REQUIRE(*classified);
  REQUIRE((*classified)->run_id == f.run);
  f.no_calls();
}

TEST_CASE("manual and control spans never become provider tool turns",
          "[ops][recovery][continuation]") {
  Fixture manual;
  manual.admission();
  manual.allow();
  manual.push(ToolStarted{manual.invocation});
  manual.success();
  Fixture conversation;
  conversation.run = id<RunId>("conversation");
  conversation.invocation = id<InvocationId>("conversation-tool");
  const auto assistant = id<MessageId>("assistant");
  const auto inference = id<InferenceId>("inference");
  // Reuse a message ID across runs to prove excluded result events cannot
  // overwrite the conversation content in the artifact projection pass.
  const auto result_id = manual.message;
  conversation.push(RunStarted{id<SurfaceId>("test"),
                               id<WorkspaceId>("chat"),
                               id<PermissionProfileId>("observe"),
                               {}},
                    1, false);
  conversation.push(AssistantContentStarted{assistant, inference}, 1, false);
  conversation.push(ToolProposed{conversation.invocation,
                                 "read",
                                 {"application/json", "{}"},
                                 {Effect::read}});
  conversation.push(AssistantContentFinished{assistant, inference}, 1, false);
  conversation.push(ToolResultRecorded{conversation.invocation,
                                       {TextBlock{"conversation evidence"}},
                                       result_id});
  const auto expected =
      runtime::tool_continuation_messages(conversation.store.history);
  REQUIRE(expected);
  REQUIRE(expected->size() == 2);
  SECTION("interleaved complete manual history") {
    conversation.store.history.insert(conversation.store.history.begin() + 3,
                                      manual.store.history.begin(),
                                      manual.store.history.end());
  }
  SECTION("projected conversation span lacks RunStarted") {
    conversation.store.history.erase(conversation.store.history.begin());
    conversation.store.history.insert(conversation.store.history.begin() + 2,
                                      manual.store.history.begin(),
                                      manual.store.history.end());
  }
  SECTION(
      "ordinary control malformed tool terminal is excluded in both passes") {
    manual.store.history.clear();
    RunStarted start{id<SurfaceId>("test"),
                     id<WorkspaceId>("ops"),
                     id<PermissionProfileId>("observe"),
                     {}};
    start.purpose = RunPurpose::control;
    manual.push(ToolResultRecorded{manual.invocation, {}, {}});
    // The purpose map is preliminary, so placement of the retained start
    // cannot turn another run's result into provider history.
    manual.push(start, 3, false);
    conversation.store.history.insert(conversation.store.history.end(),
                                      manual.store.history.begin(),
                                      manual.store.history.end());
  }
  SECTION("summary run cannot add a synthetic assistant turn") {
    manual.store.history.clear();
    RunStarted start{id<SurfaceId>("test"),
                     id<WorkspaceId>("ops"),
                     id<PermissionProfileId>("observe"),
                     {}};
    start.purpose = RunPurpose::summary;
    manual.push(start, 3, false);
    manual.push(AssistantContentStarted{assistant, inference}, 1, false);
    conversation.store.history.insert(conversation.store.history.end(),
                                      manual.store.history.begin(),
                                      manual.store.history.end());
  }
  const auto actual =
      runtime::tool_continuation_messages(conversation.store.history);
  REQUIRE(actual);
  REQUIRE(*actual == *expected);
  manual.no_calls();
  conversation.no_calls();
}

TEST_CASE("explicit non-conversation start excludes a conflicting partial span",
          "[ops][recovery][continuation]") {
  Fixture f;
  RunStarted start{id<SurfaceId>("test"),
                   id<WorkspaceId>("ops"),
                   id<PermissionProfileId>("observe"),
                   {}};
  f.push(start, 1, false);
  start.purpose = RunPurpose::control;
  f.push(start, 3, false);
  auto result = runtime::tool_continuation_messages(f.store.history);
  REQUIRE(result);
  REQUIRE(result->empty());
  f.no_calls();
}

TEST_CASE(
    "model Ops proof cannot restore an ordinary executor with the same name",
    "[ops][recovery]") {
  Fixture f;
  f.model_admission();
  SECTION("proposed") {
  }
  SECTION("allowed") {
    f.allow();
  }
  SECTION("awaiting approval") {
    f.approval();
  }
  SECTION("started") {
    f.allow();
    f.push(ToolStarted{f.invocation});
  }
  REQUIRE(runtime::recorded_ops_observations(f.log()));
  const auto original = f.store.history;
  const auto classified = runtime::classify_recoverable_run(f.log());
  REQUIRE_FALSE(classified);
  REQUIRE(classified.error().code ==
          runtime::RunKernelErrorCode::replay_rejected);
  const auto opened = f.reopen();
  REQUIRE_FALSE(opened);
  REQUIRE(opened.error().code == runtime::RunKernelErrorCode::replay_rejected);
  REQUIRE(f.store.attempts == 0);
  REQUIRE(f.store.history == original);
  f.no_calls();
}

TEST_CASE("completed model Ops observations remain readable history",
          "[ops][recovery]") {
  Fixture f;
  f.model_admission();
  f.allow();
  f.push(ToolStarted{f.invocation});
  f.success();
  const auto original = f.store.history;
  REQUIRE(runtime::recorded_ops_observations(f.log()));
  const auto classified = runtime::classify_recoverable_run(f.log());
  REQUIRE(classified);
  REQUIRE_FALSE(*classified);
  auto opened = f.reopen();
  REQUIRE(opened);
  REQUIRE((*opened)->drain());
  REQUIRE(f.store.history == original);
  REQUIRE(f.store.attempts == 0);
  f.no_calls();
}
