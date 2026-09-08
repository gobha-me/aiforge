#include "../131summarykernel/fixture.hpp"
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
           std::holds_alternative<domain::RunStarted>(payload) &&
                   std::get<domain::RunStarted>(payload)
                       .local_context_admission_required
               ? 5U
               : 1U,
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

namespace {
auto local_admission() -> domain::LocalContextAdmission {
  const auto bytes = digest("Local exact evidence");
  domain::LocalContextAdmission result{
      1,
      id<domain::SessionId>("session"),
      1,
      {4096, 512, 0},
      {{id<domain::EvidenceId>("local-evidence-" + std::string(64, 'a')),
        id<domain::ContextEntryId>("local-context-entry-" +
                                   std::string(64, 'a')),
        id<domain::MessageId>("local-context-message-" + std::string(64, 'a')),
        id<domain::ContextSourceId>("local-context-source-" +
                                    std::string(64, 'a')),
        {{1, std::string(64, 'a')}, "notes.txt", bytes},
        2,
        bytes.byte_size,
        domain::LocalContextDecision::admitted}},
      {}};
  REQUIRE(domain::seal_local_context_admission(result));
  return result;
}
auto local_start() -> runtime::RunStart {
  auto result = start();
  result.attributes.workspace_id = id<domain::WorkspaceId>("chat");
  result.repository_admission.reset();
  result.attributes.local_context_admission_required = true;
  result.request.context.entries.push_back(
      {id<domain::ContextEntryId>("user-entry"),
       domain::ContextEntryKind::conversation,
       {},
       result.user_message,
       {id<domain::ContextSourceId>("user-source"), {}, {}},
       0,
       1,
       5});
  result.request.context.estimated_input_tokens += 5;
  result.local_admission = local_admission();
  const auto& item = result.local_admission->evidence.front();
  result.request.context.entries.push_back(
      {item.entry_id,
       domain::ContextEntryKind::evidence,
       {},
       {item.message_id,
        domain::Role::evidence,
        {domain::TextBlock{"Local exact evidence"}},
        {}},
       {item.source_id,
        domain::local_context_source_location(item.source).value(),
        "sha256:" + item.source.content_digest.value},
       0,
       item.order,
       item.estimated_tokens});
  result.request.context.estimated_input_tokens += item.estimated_tokens;
  return result;
}
auto recalculate(domain::ConstructedContext& value) -> void {
  value.estimated_input_tokens = value.capacity.reserved_input_tokens;
  for (const auto& entry : value.entries)
    value.estimated_input_tokens += entry.estimated_tokens;
}
auto mixed_start() -> runtime::RunStart {
  auto value = local_start();
  value.attributes.workspace_id = id<domain::WorkspaceId>("code");
  value.repository_admission = admission();
  add_sources(value);
  value.repository_admission->evidence.front().order = 3;
  value.request.context.entries.back().order = 3;
  REQUIRE(
      domain::seal_repository_context_admission(*value.repository_admission));
  return value;
}
} // namespace

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
       3,
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
  runtime::RunStart initial{local_start()};
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

TEST_CASE("local startup rejects absent corrupt foreign or mismatched proof "
          "before effects",
          "[local][admission][kernel][failure]") {
  auto value = local_start();
  SECTION("missing proof for local context") {
    value.local_admission.reset();
  }
  SECTION("tampered seal") {
    ++value.local_admission->selection_revision;
  }
  SECTION("foreign session with valid seal") {
    value.local_admission->session_id = id<domain::SessionId>("foreign");
    REQUIRE(domain::seal_local_context_admission(*value.local_admission));
  }
  SECTION("future version") {
    value.local_admission->version = 2;
  }
  SECTION("control purpose cannot admit local evidence") {
    value.attributes.purpose = domain::RunPurpose::control;
  }
  SECTION("missing context entry") {
    value.request.context.entries.pop_back();
  }
  SECTION("duplicated context entry") {
    value.request.context.entries.push_back(
        value.request.context.entries.back());
  }
  SECTION("changed text") {
    value.request.context.entries.back().message.content = {
        domain::TextBlock{"Different evidence"}};
  }
  SECTION("evidence promoted to instruction") {
    auto& entry = value.request.context.entries.back();
    entry.kind = domain::ContextEntryKind::instruction;
    entry.instruction_layer = domain::InstructionLayer::project;
    entry.message.role = domain::Role::system;
  }
  SECTION("changed source provenance") {
    value.request.context.entries.back().provenance.source_location =
        "other.txt";
  }
  SECTION("changed estimate") {
    ++value.request.context.entries.back().estimated_tokens;
  }
  SECTION("omitted source secretly included") {
    value.local_admission->evidence.front().decision =
        domain::LocalContextDecision::omitted_budget;
    REQUIRE(domain::seal_local_context_admission(*value.local_admission));
  }
  SECTION("unrecorded local entry") {
    auto extra = value.request.context.entries.back();
    extra.entry_id = id<domain::ContextEntryId>("local-context-entry-" +
                                                std::string(64, 'b'));
    extra.message.message_id =
        id<domain::MessageId>("local-context-message-" + std::string(64, 'b'));
    extra.provenance.source_id = id<domain::ContextSourceId>(
        "local-context-source-" + std::string(64, 'b'));
    extra.order = 3;
    value.request.context.entries.push_back(std::move(extra));
  }
  recalculate(value.request.context);
  testing::ScriptedBackend backend{{}};
  Store store;
  auto kernel =
      runtime::RunKernel::open_durable({id<domain::SessionId>("session"),
                                        runtime::DurableSessionMode::create,
                                        {}},
                                       store, backend);
  REQUIRE(kernel);
  const auto result = (*kernel)->start(std::move(value));
  REQUIRE_FALSE(result);
  CHECK(result.error().code == runtime::RunKernelErrorCode::invalid_start);
  CHECK(store.attempts.empty());
  CHECK((*kernel)->event_log().events().empty());
  CHECK(backend.recorded_requests().empty());
}

TEST_CASE(
    "local admission is staged atomically before repository and inference",
    "[local][admission][kernel][failure]") {
  auto value = local_start();
  SECTION("local only") {
  }
  SECTION("mixed repository and local") {
    value = mixed_start();
  }
  Store store;
  store.fail_append = true;
  testing::ScriptedBackend backend{{}};
  auto opened =
      runtime::RunKernel::open_durable({id<domain::SessionId>("session"),
                                        runtime::DurableSessionMode::create,
                                        {}},
                                       store, backend);
  REQUIRE(opened);
  const auto started = (*opened)->start(value);
  REQUIRE_FALSE(started);
  CHECK(started.error().code == runtime::RunKernelErrorCode::storage_failure);
  REQUIRE(store.attempts.size() == 1);
  const auto& batch = store.attempts.front();
  auto next = std::ranges::find_if(batch, [](const auto& event) {
    return std::holds_alternative<domain::LocalContextAdmitted>(event.payload);
  });
  REQUIRE(next != batch.end());
  CHECK(std::get<domain::LocalContextAdmitted>(next->payload).admission ==
        *value.local_admission);
  CHECK(std::get<domain::LocalContextAdmitted>(next->payload).inference_id ==
        value.request.inference_id);
  REQUIRE(++next != batch.end());
  if (value.repository_admission) {
    REQUIRE(std::holds_alternative<domain::RepositoryContextAdmitted>(
        next->payload));
    CHECK(std::get<domain::RepositoryContextAdmitted>(next->payload)
              .inference_id == value.request.inference_id);
    REQUIRE(++next != batch.end());
  }
  REQUIRE(std::holds_alternative<domain::InferenceStarted>(next->payload));
  CHECK(std::get<domain::InferenceStarted>(next->payload).inference_id ==
        value.request.inference_id);
  CHECK(store.events.empty());
  CHECK((*opened)->event_log().events().empty());
  CHECK(backend.recorded_requests().empty());
}

TEST_CASE("local history rejects malformed inference proof grammar",
          "[local][admission][history][failure]") {
  const auto value = local_start();
  const domain::LocalContextAdmitted admitted{value.request.inference_id,
                                              *value.local_admission};
  const domain::InferenceStarted inference{value.request.inference_id,
                                           value.request.model_id};
  std::vector<domain::RunEventPayload> payloads{value.attributes, admitted,
                                                inference};
  SECTION("orphan proof") {
    payloads.pop_back();
  }
  SECTION("missing initial required proof") {
    payloads.erase(payloads.begin() + 1);
  }
  SECTION("proof without required marker") {
    std::get<domain::RunStarted>(payloads.front())
        .local_context_admission_required = false;
  }
  SECTION("wrong inference") {
    std::get<domain::LocalContextAdmitted>(payloads[1]).inference_id =
        id<domain::InferenceId>("wrong");
  }
  SECTION("duplicate proof") {
    payloads.insert(payloads.begin() + 1, admitted);
  }
  SECTION("intervening unrelated event") {
    payloads.insert(payloads.begin() + 2, domain::RunCompletionRequested{});
  }
  SECTION("corrupt seal") {
    ++std::get<domain::LocalContextAdmitted>(payloads[1])
          .admission.selection_revision;
  }
  SECTION("foreign session") {
    auto& proof = std::get<domain::LocalContextAdmitted>(payloads[1]).admission;
    proof.session_id = id<domain::SessionId>("foreign");
    REQUIRE(domain::seal_local_context_admission(proof));
  }
  SECTION("unknown future local event") {
    payloads[1] = domain::UnknownEvent{"run.local_context_admitted",
                                       {"application/json", "{}"}};
  }
  SECTION("missing successor proof") {
    payloads.push_back(domain::InferenceStarted{id<domain::InferenceId>("next"),
                                                value.request.model_id});
  }
  SECTION("proof introduced after plain inference") {
    payloads.insert(payloads.begin() + 1,
                    domain::InferenceStarted{id<domain::InferenceId>("earlier"),
                                             value.request.model_id});
  }
  SECTION("wrong intervening repository inference") {
    payloads.insert(payloads.begin() + 2,
                    domain::RepositoryContextAdmitted{
                        id<domain::InferenceId>("wrong"), admission()});
  }
  SECTION("duplicate intervening repository") {
    const domain::RepositoryContextAdmitted repository{
        value.request.inference_id, admission()};
    payloads.insert(payloads.begin() + 2, repository);
    payloads.insert(payloads.begin() + 2, repository);
  }
  SECTION("changed successor selection") {
    auto next = admitted;
    next.inference_id = id<domain::InferenceId>("next");
    ++next.admission.selection_revision;
    REQUIRE(domain::seal_local_context_admission(next.admission));
    payloads.push_back(next);
    payloads.push_back(
        domain::InferenceStarted{next.inference_id, value.request.model_id});
  }
  const auto result = runtime::recorded_local_context_admission(
      log(std::move(payloads)), value.run_id);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == runtime::RunKernelErrorCode::replay_rejected);
}

TEST_CASE("local history validates schema and invocation envelopes",
          "[local][admission][history][failure]") {
  const auto value = local_start();
  domain::SessionEventLog history{id<domain::SessionId>("session")};
  REQUIRE(history.append(event(value.attributes, 1)));
  auto proof = event(domain::LocalContextAdmitted{value.request.inference_id,
                                                  *value.local_admission},
                     2);
  SECTION("future schema") {
    proof.metadata.schema_version = 2;
  }
  SECTION("invocation scoped proof") {
    proof.metadata.invocation_id = id<domain::InvocationId>("unrelated");
  }
  REQUIRE(history.append(std::move(proof)));
  REQUIRE(
      history.append(event(domain::InferenceStarted{value.request.inference_id,
                                                    value.request.model_id},
                           3)));
  REQUIRE_FALSE(
      runtime::recorded_local_context_admission(history, value.run_id));
}

TEST_CASE(
    "legacy absence explicit empty and mixed local proofs remain distinct",
    "[local][admission][kernel]") {
  auto value = local_start();
  SECTION("legacy absence") {
    value.local_admission.reset();
    value.attributes.local_context_admission_required = false;
    value.request.context.entries.pop_back();
  }
  SECTION("kernel derives required marker from valid proof") {
    value.attributes.local_context_admission_required = false;
  }
  SECTION("explicit empty") {
    value.local_admission->evidence.clear();
    REQUIRE(domain::seal_local_context_admission(*value.local_admission));
    value.request.context.entries.pop_back();
  }
  SECTION("local evidence") {
  }
  SECTION("mixed evidence") {
    value = mixed_start();
  }
  recalculate(value.request.context);
  testing::ScriptedBackend backend{{{value.request, complete_script()}}};
  Store store;
  auto opened =
      runtime::RunKernel::open_durable({id<domain::SessionId>("session"),
                                        runtime::DurableSessionMode::create,
                                        {}},
                                       store, backend);
  REQUIRE(opened);
  auto started = (*opened)->start(value);
  INFO((started ? "started" : started.error().message));
  REQUIRE(started);
  drain_until(**opened, [&] { return !(*opened)->active_run_id(); });
  const auto recorded = runtime::recorded_local_context_admission(
      (*opened)->event_log(), value.run_id);
  REQUIRE(recorded);
  CHECK(*recorded == value.local_admission);
  REQUIRE(backend.recorded_requests().size() == 1);
  CHECK(backend.recorded_requests().front().context == value.request.context);
  const auto repository = runtime::recorded_repository_context_admission(
      (*opened)->event_log(), value.run_id);
  REQUIRE(repository);
  CHECK(*repository == value.repository_admission);
  opened->reset();
  auto replayed =
      runtime::RunKernel::open_durable({id<domain::SessionId>("session"),
                                        runtime::DurableSessionMode::resume,
                                        {}},
                                       store, backend);
  REQUIRE(replayed);
  REQUIRE((*replayed)->drain());
  CHECK(backend.recorded_requests().size() == 1);
}

TEST_CASE(
    "local continuation rejects omitted changed and newly selected sources",
    "[local][admission][continuation][failure]") {
  Suspended fixture;
  REQUIRE(fixture.answer());
  auto request = continuation_for(fixture.initial);
  auto next = fixture.initial.local_admission;
  SECTION("missing proof") {
    next.reset();
  }
  SECTION("changed revision") {
    ++next->selection_revision;
  }
  SECTION("changed root") {
    next->evidence.front().source.root.binding[0] = 'b';
    request.context.entries[2].provenance.source_location =
        domain::local_context_source_location(next->evidence.front().source)
            .value();
  }
  SECTION("changed path and matching request") {
    next->evidence.front().source.relative_path = "replacement.txt";
    request.context.entries[2].provenance.source_location =
        domain::local_context_source_location(next->evidence.front().source)
            .value();
  }
  SECTION("changed bytes and matching request") {
    const auto changed = digest("Replaced local bytes");
    auto& source = next->evidence.front();
    source.source.content_digest = changed;
    source.estimated_tokens = changed.byte_size;
    auto& entry = request.context.entries[2];
    entry.message.content = {domain::TextBlock{"Replaced local bytes"}};
    entry.provenance.digest = "sha256:" + changed.value;
    entry.estimated_tokens = changed.byte_size;
  }
  SECTION("omission decision and matching request") {
    next->evidence.front().decision =
        domain::LocalContextDecision::omitted_budget;
    request.context.entries.erase(request.context.entries.begin() + 2);
  }
  SECTION("dropped membership and matching request") {
    next->evidence.clear();
    request.context.entries.erase(request.context.entries.begin() + 2);
  }
  SECTION("capacity change") {
    ++next->capacity.context_window_tokens;
    request.context.capacity = next->capacity;
  }
  if (next) REQUIRE(domain::seal_local_context_admission(*next));
  recalculate(request.context);
  const auto before = fixture.store.events;
  const auto attempts = fixture.store.attempts.size();
  const auto continued = fixture.kernel->continue_run(fixture.initial.run_id,
                                                      request, {}, {}, next);
  REQUIRE_FALSE(continued);
  CHECK(continued.error().code ==
        runtime::RunKernelErrorCode::continuation_not_ready);
  CHECK(fixture.store.events == before);
  CHECK(fixture.store.attempts.size() == attempts);
  CHECK(fixture.backend->recorded_requests().size() == 1);
  REQUIRE(fixture.kernel->cancel_run(fixture.initial.run_id, "cleanup"));
}

TEST_CASE("local continuation append failure dispatches no next inference",
          "[local][admission][continuation][failure]") {
  Suspended fixture;
  REQUIRE(fixture.answer());
  fixture.store.fail_append = true;
  const auto before = fixture.store.events;
  const auto result = fixture.kernel->continue_run(
      fixture.initial.run_id, continuation_for(fixture.initial), {}, {},
      fixture.initial.local_admission);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == runtime::RunKernelErrorCode::storage_failure);
  CHECK(fixture.store.events == before);
  CHECK(fixture.backend->recorded_requests().size() == 1);
  const auto& batch = fixture.store.attempts.back();
  REQUIRE(batch.size() >= 2);
  CHECK(std::holds_alternative<domain::LocalContextAdmitted>(batch[0].payload));
  CHECK(std::holds_alternative<domain::InferenceStarted>(batch[1].payload));
}

TEST_CASE("corrupt recovered local proof blocks direct question and approval "
          "decisions",
          "[local][admission][recovery][failure]") {
  bool approval{};
  SECTION("question") {
  }
  SECTION("approval") {
    approval = true;
  }
  Suspended fixture{approval};
  fixture.kernel.reset();
  for (auto& item : fixture.store.events)
    if (auto* local = std::get_if<domain::LocalContextAdmitted>(&item.payload))
      ++local->admission.selection_revision;
  fixture.open(runtime::DurableSessionMode::resume);
  const auto before = fixture.store.events;
  if (approval) {
    REQUIRE(fixture.kernel->pending_tool_approval());
    const auto result = fixture.kernel->decide_approval(
        fixture.initial.run_id, id<domain::InvocationId>("ask-call"),
        {domain::ApprovalDecision::approved, {}});
    REQUIRE_FALSE(result);
    CHECK(fixture.policy->approvals == 0);
  } else {
    REQUIRE(fixture.kernel->pending_question_input());
    REQUIRE_FALSE(fixture.answer());
    REQUIRE_FALSE(fixture.kernel->cancel_questions(
        fixture.initial.run_id, id<domain::InvocationId>("ask-call"),
        "cancel input"));
  }
  CHECK(fixture.store.events == before);
  CHECK(fixture.backend->recorded_requests().size() == 1);
  REQUIRE(fixture.kernel->cancel_run(fixture.initial.run_id,
                                     "cancel invalid proof"));
  CHECK(std::holds_alternative<domain::RunCancelled>(
      fixture.store.events.back().payload));
}

TEST_CASE("exact local evidence survives real question tool continuation",
          "[local][admission][continuation]") {
  Suspended fixture;
  REQUIRE(fixture.answer());
  const auto request = continuation_for(fixture.initial);
  auto continued = fixture.kernel->continue_run(
      fixture.initial.run_id, request, {}, {}, fixture.initial.local_admission);
  INFO((continued ? "continued" : continued.error().message));
  REQUIRE(continued);
  drain_until(*fixture.kernel,
              [&] { return !fixture.kernel->active_run_id(); });
  REQUIRE(fixture.backend->recorded_requests().size() == 2);
  const auto& actual = fixture.backend->recorded_requests().back().context;
  REQUIRE(actual.entries.size() ==
          fixture.initial.request.context.entries.size() + 1);
  CHECK(actual.entries[2] == fixture.initial.request.context.entries[2]);
  const auto recorded = runtime::recorded_local_context_admission(
      fixture.kernel->event_log(), fixture.initial.run_id);
  REQUIRE(recorded);
  REQUIRE(*recorded);
  CHECK(**recorded == *fixture.initial.local_admission);
}

TEST_CASE("summary generation refuses even an empty local admission",
          "[local][admission][purpose][failure]") {
  summary_kernel_test::Fixture fixture;
  auto value = fixture.request();
  auto proof = local_admission();
  proof.evidence.clear();
  proof.capacity = value.request.context.capacity;
  REQUIRE(domain::seal_local_context_admission(proof));
  value.local_admission = std::move(proof);
  fixture.reject(std::move(value));
}

TEST_CASE("local agent projection exposes bounded references without source "
          "text or leases",
          "[local][admission][agent][failure]") {
  const auto value = local_start();
  auto recorded = event(domain::LocalContextAdmitted{value.request.inference_id,
                                                     *value.local_admission},
                        1);
  const auto encoded =
      surfaces::agent_event_record(id<domain::SessionId>("session"), recorded);
  REQUIRE(encoded);
  CHECK(encoded->find("local_context_admitted") != std::string::npos);
  CHECK(encoded->find("notes.txt") != std::string::npos);
  CHECK(
      encoded->find(value.local_admission->evidence.front().entry_id.value()) !=
      std::string::npos);
  CHECK(encoded->find(value.local_admission->admission_digest->value) !=
        std::string::npos);
  CHECK(encoded->find("Local exact evidence") == std::string::npos);
  CHECK(encoded->find("lease_generation") == std::string::npos);
  CHECK(encoded->find("\"decision\":\"admitted\"") != std::string::npos);
  SECTION("tampered proof") {
    ++std::get<domain::LocalContextAdmitted>(recorded.payload)
          .admission.selection_revision;
  }
  SECTION("oversized unsealed references") {
    auto& proof =
        std::get<domain::LocalContextAdmitted>(recorded.payload).admission;
    proof.evidence.resize(65, proof.evidence.front());
  }
  CHECK_FALSE(
      surfaces::agent_event_record(id<domain::SessionId>("session"), recorded));
}

TEST_CASE(
    "recovered unstarted tool requires intact local proof before dispatch",
    "[local][admission][recovery][failure]") {
  Suspended fixture{false, true};
  fixture.kernel.reset();
  const auto started =
      std::ranges::find_if(fixture.store.events, [](const auto& item) {
        return std::holds_alternative<domain::ToolStarted>(item.payload);
      });
  REQUIRE(started != fixture.store.events.end());
  fixture.store.events.erase(started, fixture.store.events.end());
  bool invalid = true;
  SECTION("missing initial required proof") {
    std::erase_if(fixture.store.events, [](const auto& item) {
      return std::holds_alternative<domain::LocalContextAdmitted>(item.payload);
    });
  }
  SECTION("corrupt proof") {
    for (auto& item : fixture.store.events)
      if (auto* local =
              std::get_if<domain::LocalContextAdmitted>(&item.payload))
        ++local->admission.selection_revision;
  }
  SECTION("intact proof dispatches the recorded tool") {
    invalid = false;
  }
  fixture.open(runtime::DurableSessionMode::resume);
  REQUIRE(fixture.kernel->active_run_id());
  REQUIRE_FALSE(fixture.kernel->pending_question_input());
  const auto before = fixture.store.events;
  CHECK(fixture.backend->recorded_requests().size() == 1);
  const auto drained = fixture.kernel->drain();
  if (invalid) {
    REQUIRE_FALSE(drained);
    CHECK(drained.error().code ==
          runtime::RunKernelErrorCode::invalid_tool_state);
    CHECK(fixture.store.events == before);
    CHECK_FALSE(fixture.kernel->pending_question_input());
    REQUIRE_FALSE(fixture.kernel->drain());
    CHECK(fixture.store.events == before);
  } else {
    INFO((drained ? "drained" : drained.error().message));
    REQUIRE(drained);
    drain_until(*fixture.kernel, [&] {
      return fixture.kernel->pending_question_input().has_value();
    });
  }
  REQUIRE(fixture.kernel->cancel_run(fixture.initial.run_id, "cleanup"));
  CHECK(fixture.backend->recorded_requests().size() == 1);
}
