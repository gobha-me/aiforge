#include <aiforge/detail/sha256.hpp>
#include <aiforge/runtime/ask_user_tool.hpp>
#include <aiforge/runtime/run_kernel.hpp>
#include <aiforge/surfaces/agent.hpp>
#include <aiforge/testing/scripted_backend.hpp>

#include <algorithm>
#include <chrono>
#include <functional>
#include <span>
#include <thread>

#include <catch2/catch_test_macros.hpp>

namespace {
using namespace aiforge;
using namespace std::chrono_literals;
template <class Id> auto id(const std::string& value) -> Id {
  return Id::from(value).value();
}
auto digest(std::string_view text) -> domain::ContentDigest {
  detail::Sha256 hash;
  hash.update(std::as_bytes(std::span{text.data(), text.size()}));
  return {"sha256", hash.finish(), text.size()};
}
auto admission() -> domain::RepositoryContextAdmission {
  domain::RepositoryContextAdmission result{
      1,
      "root:123:456",
      {id<domain::RepositoryId>("repo"), digest("snapshot")},
      "",
      1,
      {4096, 512, 0},
      {},
      {},
      {}};
  REQUIRE(domain::seal_repository_context_admission(result));
  return result;
}
auto context() -> domain::ConstructedContext {
  return {{{id<domain::ContextEntryId>("runtime"),
            domain::ContextEntryKind::instruction,
            domain::InstructionLayer::application_runtime,
            {id<domain::MessageId>("runtime"),
             domain::Role::system,
             {domain::TextBlock{"contract"}},
             {}},
            {id<domain::ContextSourceId>("runtime"), {}, {}},
            0,
            1,
            2}},
          {{id<domain::ContextEntryId>("runtime"),
            domain::ContextDecision::admitted,
            {}}},
          {4096, 512, 0},
          2};
}
auto start() -> runtime::RunStart {
  runtime::RunStart result{id<domain::RunId>("run"),
                           {id<domain::SurfaceId>("test"),
                            id<domain::WorkspaceId>("code"),
                            id<domain::PermissionProfileId>("observe"),
                            {}},
                           {id<domain::MessageId>("user"),
                            domain::Role::user,
                            {domain::TextBlock{"hello"}},
                            {}},
                           {id<domain::InferenceId>("inference"),
                            id<domain::MessageId>("assistant"),
                            id<domain::ModelId>("model"),
                            context(),
                            {},
                            {}}};
  result.repository_admission = admission();
  return result;
}
auto add_sources(runtime::RunStart& value) -> void {
  auto& selected = *value.repository_admission;
  const auto instructions = digest("Project instruction");
  const auto evidence = digest("Untrusted evidence");
  selected.instructions.push_back(
      {id<domain::ProjectInstructionId>("project:root"),
       {selected.source_snapshot, "AGENTS.md", instructions, {}},
       "",
       0,
       1,
       instructions.byte_size,
       instructions});
  selected.evidence.push_back(
      {id<domain::EvidenceId>("file"),
       id<domain::ContextEntryId>("repository-context-entry-file"),
       id<domain::MessageId>("repository-context-message-file"),
       id<domain::ContextSourceId>("repository-context-source-file"),
       {selected.source_snapshot, "file.txt", evidence, {}},
       1,
       evidence.byte_size,
       domain::RepositoryContextDecision::admitted,
       evidence});
  REQUIRE(domain::seal_repository_context_admission(selected));
  value.request.context.entries.push_back(
      {id<domain::ContextEntryId>("project:root"),
       domain::ContextEntryKind::instruction,
       domain::InstructionLayer::project,
       {id<domain::MessageId>("project:root"),
        domain::Role::system,
        {domain::TextBlock{"Project instruction"}},
        {}},
       {id<domain::ContextSourceId>("project:root"), "AGENTS.md",
        "sha256:" + instructions.value},
       0,
       1,
       instructions.byte_size});
  value.request.context.entries.push_back(
      {selected.evidence.front().entry_id,
       domain::ContextEntryKind::evidence,
       {},
       {selected.evidence.front().message_id,
        domain::Role::evidence,
        {domain::TextBlock{"Untrusted evidence"}},
        {}},
       {selected.evidence.front().source_id, "file.txt",
        "sha256:" + evidence.value},
       0,
       1,
       evidence.byte_size});
  value.request.context.estimated_input_tokens +=
      instructions.byte_size + evidence.byte_size;
}
auto event(domain::RunEventPayload payload, std::uint64_t sequence)
    -> domain::RunEvent {
  return {{id<domain::EventId>("event-" + std::to_string(sequence)),
           id<domain::RunId>("run"),
           sequence,
           1,
           domain::EventTimestamp{100ms},
           {},
           {},
           {}},
          std::move(payload)};
}
auto log(std::vector<domain::RunEventPayload> payloads)
    -> domain::SessionEventLog {
  domain::SessionEventLog result{id<domain::SessionId>("session")};
  for (auto& payload : payloads)
    REQUIRE(
        result.append(event(std::move(payload), result.last_sequence() + 1)));
  return result;
}
class Store final : public storage::SessionStore {
 public:
  bool fail_append{};
  std::vector<domain::RunEvent> events;
  std::vector<std::vector<domain::RunEvent>> attempts;
  auto create_session(storage::SessionCreate, std::stop_token)
      -> std::expected<void, storage::SessionStoreError> override {
    return {};
  }
  auto open_session(const domain::SessionId& session, std::stop_token)
      -> std::expected<storage::SessionInfo,
                       storage::SessionStoreError> override {
    return storage::SessionInfo{
        session, domain::EventTimestamp{100ms}, domain::EventTimestamp{100ms},
        events.empty() ? 0 : events.back().metadata.sequence, 1};
  }
  auto list_sessions(std::size_t, std::stop_token)
      -> std::expected<std::vector<storage::SessionInfo>,
                       storage::SessionStoreError> override {
    return std::vector<storage::SessionInfo>{};
  }
  auto replay_events(const domain::SessionId&, std::stop_token)
      -> std::expected<std::vector<domain::RunEvent>,
                       storage::SessionStoreError> override {
    return events;
  }
  auto append_events(const domain::SessionId&,
                     const std::span<const domain::RunEvent> batch,
                     std::stop_token)
      -> std::expected<void, storage::SessionStoreError> override {
    attempts.emplace_back(batch.begin(), batch.end());
    if (fail_append)
      return std::unexpected(
          storage::SessionStoreError{storage::SessionStoreErrorCode::contention,
                                     "test append failure", true});
    events.insert(events.end(), batch.begin(), batch.end());
    return {};
  }
};
auto drain_until(runtime::RunKernel& kernel, const std::function<bool()>& done)
    -> void {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!done() && std::chrono::steady_clock::now() < deadline) {
    auto drained = kernel.drain();
    INFO((drained ? "drained" : drained.error().message));
    REQUIRE(drained);
    std::this_thread::sleep_for(1ms);
  }
  std::string failures;
  for (const auto& event : kernel.event_log().events()) {
    if (const auto* failed =
            std::get_if<domain::ToolPolicyFailed>(&event.payload))
      failures += "policy: " + failed->error.message + "; ";
    if (const auto* failed = std::get_if<domain::ToolErrored>(&event.payload))
      failures += "tool: " + failed->error.message + "; ";
    if (const auto* failed = std::get_if<domain::RunFailed>(&event.payload))
      failures += "run: " + failed->error.message + "; ";
  }
  INFO(failures);
  REQUIRE(done());
}
auto complete_script() -> testing::StreamScript {
  return {{testing::ScriptedStep{backend::ResponseStarted{"response"}},
           testing::ScriptedStep{
               backend::ResponseFinished{domain::FinishReason::stop}},
           testing::EndOfStream{}}};
}
} // namespace

TEST_CASE("repository startup rejects missing corrupt and mismatched admission "
          "before effects",
          "[repository][admission][runtime][failure]") {
  auto value = start();
  add_sources(value);
  SECTION("missing admission") {
    value.repository_admission.reset();
  }
  SECTION("empty Dev admission required even without sources") {
    value = start();
    value.repository_admission.reset();
  }
  SECTION("orphan project context in ordinary Chat") {
    value.attributes.workspace_id = id<domain::WorkspaceId>("chat");
    value.repository_admission.reset();
  }
  SECTION("changed seal") {
    value.repository_admission->selection_revision++;
  }
  SECTION("changed project text") {
    value.request.context.entries[1].message.content = {
        domain::TextBlock{"replacement"}};
  }
  SECTION("changed evidence text") {
    value.request.context.entries[2].message.content = {
        domain::TextBlock{"replacement"}};
  }
  SECTION("missing evidence entry") {
    value.request.context.entries.pop_back();
  }
  SECTION("duplicated evidence entry") {
    value.request.context.entries.push_back(
        value.request.context.entries.back());
  }
  SECTION("evidence promoted to instruction") {
    value.request.context.entries[2].message.role = domain::Role::system;
  }
  SECTION("changed estimate") {
    value.request.context.entries[2].estimated_tokens++;
  }
  SECTION("omitted evidence secretly sent") {
    value.repository_admission->evidence.front().decision =
        domain::RepositoryContextDecision::omitted_budget;
    REQUIRE(
        domain::seal_repository_context_admission(*value.repository_admission));
  }
  testing::ScriptedBackend backend{{}};
  Store store;
  auto kernel = runtime::RunKernel::open_durable(
      {id<domain::SessionId>("session"), runtime::DurableSessionMode::create,
       domain::EventTimestamp{100ms}},
      store, backend);
  REQUIRE(kernel);
  const auto result = (*kernel)->start(std::move(value));
  REQUIRE_FALSE(result);
  CHECK(result.error().code == runtime::RunKernelErrorCode::invalid_start);
  CHECK(store.attempts.empty());
  CHECK((*kernel)->event_log().events().empty());
  CHECK(backend.recorded_requests().empty());
}

TEST_CASE("repository admission and inference append atomically before backend "
          "launch",
          "[repository][admission][runtime][failure]") {
  auto value = start();
  Store store;
  store.fail_append = true;
  testing::ScriptedBackend backend{{}};
  auto kernel = runtime::RunKernel::open_durable(
      {id<domain::SessionId>("session"), runtime::DurableSessionMode::create,
       domain::EventTimestamp{100ms}},
      store, backend);
  REQUIRE(kernel);
  const auto result = (*kernel)->start(value);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == runtime::RunKernelErrorCode::storage_failure);
  REQUIRE(store.attempts.size() == 1);
  const auto& batch = store.attempts.front();
  const auto admitted = std::ranges::find_if(batch, [](const auto& item) {
    return std::holds_alternative<domain::RepositoryContextAdmitted>(
        item.payload);
  });
  REQUIRE(admitted != batch.end());
  REQUIRE(std::next(admitted) != batch.end());
  REQUIRE(std::holds_alternative<domain::InferenceStarted>(
      std::next(admitted)->payload));
  CHECK(std::get<domain::RepositoryContextAdmitted>(admitted->payload)
            .admission == *value.repository_admission);
  CHECK(store.events.empty());
  CHECK((*kernel)->event_log().events().empty());
  CHECK(backend.recorded_requests().empty());
}

TEST_CASE(
    "repository recovery history rejects broken inference admission pairs",
    "[repository][admission][recovery][failure]") {
  auto value = start();
  const domain::RepositoryContextAdmitted admitted{value.request.inference_id,
                                                   *value.repository_admission};
  const domain::InferenceStarted inference{value.request.inference_id,
                                           value.request.model_id};
  std::vector<domain::RunEventPayload> payloads{value.attributes, admitted,
                                                inference};
  SECTION("missing") {
    payloads.erase(payloads.begin() + 1);
  }
  SECTION("orphan admission") {
    payloads.pop_back();
  }
  SECTION("wrong inference") {
    std::get<domain::RepositoryContextAdmitted>(payloads[1]).inference_id =
        id<domain::InferenceId>("wrong");
  }
  SECTION("duplicate admission") {
    payloads.insert(payloads.begin() + 1, admitted);
  }
  SECTION("nonadjacent pair") {
    payloads.insert(payloads.begin() + 2, domain::RunCompletionRequested{});
  }
  SECTION("corrupt seal") {
    std::get<domain::RepositoryContextAdmitted>(payloads[1])
        .admission.selection_revision++;
  }
  SECTION("unknown future admission") {
    payloads[1] = domain::UnknownEvent{"run.repository_context_admitted",
                                       {"application/json", "{}"}};
  }
  SECTION("next inference has no admission") {
    payloads.push_back(domain::InferenceStarted{id<domain::InferenceId>("next"),
                                                value.request.model_id});
  }
  SECTION("successor changes target") {
    auto next = admitted;
    next.inference_id = id<domain::InferenceId>("next");
    next.admission.target_subtree = "src";
    REQUIRE(domain::seal_repository_context_admission(next.admission));
    payloads.push_back(next);
    payloads.push_back(
        domain::InferenceStarted{next.inference_id, value.request.model_id});
  }
  const auto history = log(std::move(payloads));
  const auto result =
      runtime::recorded_repository_context_admission(history, value.run_id);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == runtime::RunKernelErrorCode::replay_rejected);
}

TEST_CASE("repository exact successor keeps selections while observing "
          "unrelated snapshot drift",
          "[repository][admission][recovery]") {
  auto value = start();
  add_sources(value);
  auto next = *value.repository_admission;
  next.source_snapshot.fingerprint = digest("unrelated change");
  for (auto& item : next.instructions)
    item.source.snapshot = next.source_snapshot;
  for (auto& item : next.evidence)
    item.source.snapshot = next.source_snapshot;
  REQUIRE(domain::seal_repository_context_admission(next));
  const auto history = log(
      {value.attributes,
       domain::RepositoryContextAdmitted{value.request.inference_id,
                                         *value.repository_admission},
       domain::InferenceStarted{value.request.inference_id,
                                value.request.model_id},
       domain::RepositoryContextAdmitted{id<domain::InferenceId>("next"), next},
       domain::InferenceStarted{id<domain::InferenceId>("next"),
                                value.request.model_id}});
  const auto latest =
      runtime::recorded_repository_context_admission(history, value.run_id);
  REQUIRE(latest);
  REQUIRE(*latest);
  CHECK(**latest == next);
}

TEST_CASE("plain legacy and explicit empty Dev admissions remain distinct",
          "[repository][admission][runtime]") {
  auto value = start();
  SECTION("plain legacy") {
    value.attributes.workspace_id = id<domain::WorkspaceId>("chat");
    value.repository_admission.reset();
  }
  SECTION("new empty Dev") {
  }
  SECTION("Dev selected sources") {
    add_sources(value);
  }
  const auto expected = value.repository_admission;
  testing::ScriptedBackend backend{{{value.request, complete_script()}}};
  runtime::RunKernel kernel{id<domain::SessionId>("session"), backend};
  const auto result = kernel.start(value);
  INFO((result ? "started" : result.error().message));
  REQUIRE(result);
  drain_until(kernel, [&] { return !kernel.active_run_id(); });
  const auto latest = runtime::recorded_repository_context_admission(
      kernel.event_log(), value.run_id);
  REQUIRE(latest);
  CHECK(*latest == expected);
  CHECK(backend.recorded_requests().size() == 1);
}

namespace {
auto continuation_for(const runtime::RunStart& initial)
    -> backend::BackendRequest {
  auto request = initial.request;
  request.inference_id = id<domain::InferenceId>("continuation");
  request.assistant_message_id = id<domain::MessageId>("continued-assistant");
  request.context.entries.push_back(
      {id<domain::ContextEntryId>("result"),
       domain::ContextEntryKind::tool_result,
       {},
       {id<domain::MessageId>("result"),
        domain::Role::tool,
        {domain::TextBlock{"yes"}},
        id<domain::InvocationId>("ask-call")},
       {id<domain::ContextSourceId>("result"), {}, {}},
       0,
       2,
       2});
  request.context.estimated_input_tokens += 2;
  return request;
}
class ApprovalPolicy final : public runtime::ToolPolicy {
 public:
  domain::ToolPolicyProvenance identity{
      "aiforge.tool-launch-policy.v1",
      id<domain::PermissionProfileId>("observe"),
      domain::ToolRestrictionLevel::none,
      domain::ToolApprovalMode::prompt,
      {domain::Effect::read},
      {{domain::Effect::read, "filesystem.root", "/repo"}},
      {}};
  auto provenance() const noexcept
      -> const domain::ToolPolicyProvenance* override {
    return &identity;
  }
  auto selected_restriction() const noexcept
      -> std::optional<runtime::RestrictionLevel> override {
    return runtime::RestrictionLevel::none;
  }
  std::size_t approvals{};
  bool allow_initial{};
  auto evaluate(const runtime::ToolPolicyRequest& request)
      -> std::expected<runtime::ToolPolicyResolution,
                       runtime::ToolPolicyError> override {
    return runtime::ToolPolicyResolution{
        allow_initial ? domain::PolicyDecision::allow
                      : domain::PolicyDecision::require_approval,
        request.scopes, "test approval"};
  }
  auto approve(const runtime::ToolPolicyRequest& request,
               runtime::ToolPolicyApproval)
      -> std::expected<runtime::ToolPolicyResolution,
                       runtime::ToolPolicyError> override {
    ++approvals;
    return runtime::ToolPolicyResolution{domain::PolicyDecision::allow,
                                         request.scopes, "approved"};
  }
};
struct Suspended {
  Store store;
  runtime::RunStart initial{start()};
  runtime::ToolRegistrySnapshot tools;
  std::unique_ptr<testing::ScriptedBackend> backend;
  std::shared_ptr<ApprovalPolicy> policy;
  std::unique_ptr<runtime::RunKernel> kernel;
  explicit Suspended(const bool approval = false,
                     const bool authority = false) {
    if (approval || authority) {
      policy = std::make_shared<ApprovalPolicy>();
      policy->allow_initial = !approval;
    }
    add_sources(initial);
    runtime::ToolRegistry registry;
    REQUIRE(runtime::register_ask_user_tool(registry, true));
    tools = registry.snapshot().value();
    if (approval || authority) {
      // A synthetic effect makes this fake executor's approval state valid;
      // ordinary ask_user remains authority-free in the production registry.
      auto question_tool = *tools.find("ask_user");
      question_tool.declaration.effects = {domain::Effect::read};
      question_tool.declaration.capability_scopes = {
          {domain::Effect::read, "filesystem.root", "/repo"}};
      auto replaced = tools.replace(std::move(question_tool));
      INFO((replaced ? "replaced" : replaced.error().message));
      REQUIRE(replaced);
      tools = std::move(*replaced);
    }
    initial.request.tools = tools.declarations();
    initial.provenance = domain::RunProvenance{
        "test", "fake", {}, initial.request.model_id, {}, {}, {}, {}};
    const testing::StreamScript question{
        {testing::ScriptedStep{backend::ResponseStarted{"response"}},
         testing::ScriptedStep{backend::ToolCallDelta{
             id<domain::InvocationId>("ask-call"), "ask_user",
             R"({"questions":[{"id":"choice","prompt":"Choose","kind":"one","required":true,"minimum_selections":1,"maximum_selections":1,"options":[{"id":"yes","label":"Yes"}]}]})"}},
         testing::ScriptedStep{
             backend::ResponseFinished{domain::FinishReason::tool_call}},
         testing::EndOfStream{}}};
    backend = std::make_unique<testing::ScriptedBackend>(
        std::vector<testing::ScriptedExchange>{
            {initial.request, question},
            {continuation_for(initial), complete_script()}});
    open(runtime::DurableSessionMode::create);
    const auto begun = kernel->start(initial);
    INFO((begun ? "started" : begun.error().message));
    REQUIRE(begun);
    drain_until(*kernel, [&] {
      return approval ? kernel->pending_tool_approval().has_value()
                      : kernel->pending_question_input().has_value();
    });
  }
  auto open(runtime::DurableSessionMode mode) -> void {
    auto opened = runtime::RunKernel::open_durable(
        {id<domain::SessionId>("session"), mode, domain::EventTimestamp{100ms}},
        store, *backend, nullptr, {}, {}, tools, policy);
    INFO((opened ? "opened" : opened.error().message));
    REQUIRE(opened);
    kernel = std::move(*opened);
  }
  auto answer() -> std::expected<void, runtime::RunKernelError> {
    return kernel->answer_questions(
        initial.run_id, id<domain::InvocationId>("ask-call"),
        {{id<domain::QuestionId>("choice"), {"yes"}, {}}});
  }
};
} // namespace

TEST_CASE(
    "repository continuation rejects reselection and changed exact admission",
    "[repository][admission][continuation][failure]") {
  Suspended fixture;
  REQUIRE(fixture.answer());
  auto request = continuation_for(fixture.initial);
  auto next = fixture.initial.repository_admission;
  bool invalid_shape{};
  SECTION("missing admission") {
    next.reset();
  }
  SECTION("changed root binding") {
    next->root_binding = "root:changed";
  }
  SECTION("changed selection revision") {
    next->selection_revision++;
  }
  SECTION("changed capacity") {
    next->capacity.context_window_tokens++;
    request.context.capacity = next->capacity;
  }
  SECTION("changed estimate with matching request") {
    next->evidence.front().estimated_tokens++;
    invalid_shape = true;
    request.context.entries[2].estimated_tokens++;
  }
  SECTION("changed evidence omission with matching request") {
    next->evidence.front().decision =
        domain::RepositoryContextDecision::omitted_budget;
    request.context.entries.erase(request.context.entries.begin() + 2);
  }
  SECTION("changed project membership with matching request") {
    next->instructions.clear();
    request.context.entries.erase(request.context.entries.begin() + 1);
  }
  SECTION("changed source bytes with matching request") {
    const auto changed = digest("Changed evidence");
    next->evidence.front().source.content_digest = changed;
    next->evidence.front().text_digest = changed;
    next->evidence.front().estimated_tokens = changed.byte_size;
    request.context.entries[2].estimated_tokens = changed.byte_size;
    request.context.entries[2].message.content = {
        domain::TextBlock{"Changed evidence"}};
    request.context.entries[2].provenance.digest = "sha256:" + changed.value;
  }
  if (next)
    CHECK(domain::seal_repository_context_admission(*next).has_value() !=
          invalid_shape);
  const auto before = fixture.kernel->event_log().events();
  const auto attempts = fixture.store.attempts.size();
  const auto continued =
      fixture.kernel->continue_run(fixture.initial.run_id, request, {}, next);
  REQUIRE_FALSE(continued);
  CHECK(continued.error().code ==
        runtime::RunKernelErrorCode::continuation_not_ready);
  CHECK(fixture.kernel->event_log().events() == before);
  CHECK(fixture.store.attempts.size() == attempts);
  CHECK(fixture.backend->recorded_requests().size() == 1);
  REQUIRE(fixture.kernel->cancel_run(fixture.initial.run_id, "test cleanup"));
}

TEST_CASE("repository continuation persistence failure starts no inference",
          "[repository][admission][continuation][failure]") {
  Suspended fixture;
  REQUIRE(fixture.answer());
  const auto before = fixture.store.events;
  fixture.store.fail_append = true;
  const auto continued = fixture.kernel->continue_run(
      fixture.initial.run_id, continuation_for(fixture.initial), {},
      fixture.initial.repository_admission);
  REQUIRE_FALSE(continued);
  CHECK(continued.error().code == runtime::RunKernelErrorCode::storage_failure);
  CHECK(fixture.store.events == before);
  CHECK(fixture.backend->recorded_requests().size() == 1);
  REQUIRE(fixture.store.attempts.back().size() >= 2);
  CHECK(std::holds_alternative<domain::RepositoryContextAdmitted>(
      fixture.store.attempts.back()[0].payload));
  CHECK(std::holds_alternative<domain::InferenceStarted>(
      fixture.store.attempts.back()[1].payload));
}

TEST_CASE("malformed recovered Dev manifest blocks answers while exact "
          "cancellation remains usable",
          "[repository][admission][recovery][failure]") {
  Suspended fixture;
  fixture.kernel.reset();
  SECTION("missing manifest") {
    std::erase_if(fixture.store.events, [](const auto& item) {
      return std::holds_alternative<domain::RepositoryContextAdmitted>(
          item.payload);
    });
  }
  SECTION("corrupt manifest") {
    for (auto& item : fixture.store.events)
      if (auto* admitted =
              std::get_if<domain::RepositoryContextAdmitted>(&item.payload))
        admitted->admission.selection_revision++;
  }
  fixture.open(runtime::DurableSessionMode::resume);
  REQUIRE(fixture.kernel->pending_question_input());
  const auto before = fixture.store.events;
  const auto answered = fixture.answer();
  REQUIRE_FALSE(answered);
  CHECK(answered.error().code ==
        runtime::RunKernelErrorCode::invalid_tool_state);
  CHECK(fixture.store.events == before);
  const auto cancelled_question = fixture.kernel->cancel_questions(
      fixture.initial.run_id, id<domain::InvocationId>("ask-call"),
      "cancel input");
  REQUIRE_FALSE(cancelled_question);
  CHECK(cancelled_question.error().code ==
        runtime::RunKernelErrorCode::invalid_tool_state);
  CHECK(fixture.store.events == before);
  REQUIRE(fixture.kernel->cancel_run(fixture.initial.run_id,
                                     "cancel invalid source manifest"));
  CHECK(std::holds_alternative<domain::RunCancelled>(
      fixture.store.events.back().payload));
  CHECK(fixture.backend->recorded_requests().size() == 1);
}

TEST_CASE("repository continuation admits exact selected bytes after unrelated "
          "repository drift",
          "[repository][admission][continuation]") {
  Suspended fixture;
  REQUIRE(fixture.answer());
  auto next = *fixture.initial.repository_admission;
  next.source_snapshot.fingerprint = digest("unrelated changed file");
  for (auto& item : next.instructions)
    item.source.snapshot = next.source_snapshot;
  for (auto& item : next.evidence)
    item.source.snapshot = next.source_snapshot;
  REQUIRE(domain::seal_repository_context_admission(next));
  const auto continued = fixture.kernel->continue_run(
      fixture.initial.run_id, continuation_for(fixture.initial), {}, next);
  INFO((continued ? "continued" : continued.error().message));
  REQUIRE(continued);
  drain_until(*fixture.kernel,
              [&] { return !fixture.kernel->active_run_id(); });
  const auto latest = runtime::recorded_repository_context_admission(
      fixture.kernel->event_log(), fixture.initial.run_id);
  REQUIRE(latest);
  REQUIRE(*latest);
  CHECK(**latest == next);
  CHECK(fixture.backend->recorded_requests().size() == 2);
  CHECK(fixture.kernel->projection(fixture.initial.run_id)->status() ==
        domain::RunStatus::completed);
}

TEST_CASE("malformed recovered Dev manifest blocks approval before authority "
          "dispatch",
          "[repository][admission][recovery][approval][failure]") {
  Suspended fixture{true};
  REQUIRE(fixture.kernel->pending_tool_approval());
  fixture.kernel.reset();
  std::erase_if(fixture.store.events, [](const auto& item) {
    return std::holds_alternative<domain::RepositoryContextAdmitted>(
        item.payload);
  });
  fixture.open(runtime::DurableSessionMode::resume);
  REQUIRE(fixture.kernel->pending_tool_approval());
  const auto before = fixture.store.events;
  const auto approved = fixture.kernel->decide_approval(
      fixture.initial.run_id, id<domain::InvocationId>("ask-call"),
      {domain::ApprovalDecision::approved, {}});
  REQUIRE_FALSE(approved);
  CHECK(approved.error().code ==
        runtime::RunKernelErrorCode::invalid_tool_state);
  CHECK(fixture.policy->approvals == 0);
  CHECK_FALSE(fixture.kernel->pending_question_input());
  CHECK(fixture.store.events == before);
  REQUIRE(fixture.kernel->cancel_run(fixture.initial.run_id, "test cleanup"));
  CHECK(fixture.backend->recorded_requests().size() == 1);
}

TEST_CASE(
    "recovered unstarted tool validates repository admission before dispatch",
    "[repository][admission][recovery][failure]") {
  Suspended fixture{false, true};
  fixture.kernel.reset();
  const auto started =
      std::ranges::find_if(fixture.store.events, [](const auto& item) {
        return std::holds_alternative<domain::ToolStarted>(item.payload);
      });
  REQUIRE(started != fixture.store.events.end());
  fixture.store.events.erase(started, fixture.store.events.end());
  bool invalid = true;
  SECTION("missing admission") {
    std::erase_if(fixture.store.events, [](const auto& item) {
      return std::holds_alternative<domain::RepositoryContextAdmitted>(
          item.payload);
    });
  }
  SECTION("corrupt admission") {
    for (auto& item : fixture.store.events)
      if (auto* admitted =
              std::get_if<domain::RepositoryContextAdmitted>(&item.payload))
        ++admitted->admission.selection_revision;
  }
  SECTION("intact admission dispatch smoke") {
    invalid = false;
  }
  fixture.open(runtime::DurableSessionMode::resume);
  REQUIRE(fixture.kernel->active_run_id());
  REQUIRE_FALSE(fixture.kernel->pending_question_input());
  const auto before = fixture.store.events;
  const auto drained = fixture.kernel->drain();
  if (invalid) {
    REQUIRE_FALSE(drained);
    CHECK(drained.error().code ==
          runtime::RunKernelErrorCode::invalid_tool_state);
    CHECK(fixture.store.events == before);
    CHECK_FALSE(fixture.kernel->pending_question_input());
    // Failure leaves dispatch pending but never grants a retry through drain.
    REQUIRE_FALSE(fixture.kernel->drain());
    CHECK(fixture.store.events == before);
  } else {
    INFO((drained ? "drained" : drained.error().message));
    REQUIRE(drained);
    drain_until(*fixture.kernel, [&] {
      return fixture.kernel->pending_question_input().has_value();
    });
  }
  REQUIRE(fixture.kernel->cancel_run(fixture.initial.run_id, "test cleanup"));
  CHECK(fixture.backend->recorded_requests().size() == 1);
}

TEST_CASE("agent repository admission projection preserves references without "
          "source content or root lease",
          "[repository][admission][agent]") {
  auto value = start();
  add_sources(value);
  const auto recorded =
      event(domain::RepositoryContextAdmitted{value.request.inference_id,
                                              *value.repository_admission},
            1);
  const auto encoded =
      surfaces::agent_event_record(id<domain::SessionId>("session"), recorded);
  REQUIRE(encoded);
  CHECK(encoded->find("repository_context_admitted") != std::string::npos);
  CHECK(encoded->find("project:root") != std::string::npos);
  CHECK(encoded->find("repository-context-entry-file") != std::string::npos);
  CHECK(encoded->find(value.repository_admission->admission_digest->value) !=
        std::string::npos);
  CHECK(encoded->find("root:123:456") == std::string::npos);
  CHECK(encoded->find("Project instruction") == std::string::npos);
  CHECK(encoded->find("Untrusted evidence") == std::string::npos);
  auto corrupt = recorded;
  std::get<domain::RepositoryContextAdmitted>(corrupt.payload)
      .admission.selection_revision++;
  CHECK_FALSE(
      surfaces::agent_event_record(id<domain::SessionId>("session"), corrupt));
}
