#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include <aiforge/runtime/ask_user_tool.hpp>
#include <aiforge/runtime/run_kernel.hpp>
#include <aiforge/testing/scripted_backend.hpp>
#include <aiforge/testing/scripted_tool_executor.hpp>

namespace {

using namespace std::chrono_literals;
using namespace aiforge;

template <typename IdType> auto id(const std::string& value) -> IdType {
  return IdType::from(value).value();
}

auto context(std::vector<domain::Message> tool_messages = {})
    -> domain::ConstructedContext {
  std::vector<domain::ContextEntry> entries{
      {id<domain::ContextEntryId>("runtime-entry"),
       domain::ContextEntryKind::instruction,
       domain::InstructionLayer::application_runtime,
       {id<domain::MessageId>("runtime-message"),
        domain::Role::system,
        {domain::TextBlock{"runtime"}},
        std::nullopt},
       {id<domain::ContextSourceId>("runtime-source"), std::nullopt,
        std::nullopt},
       0,
       1,
       1}};
  std::uint64_t order{2};
  for (auto& message : tool_messages) {
    entries.push_back(
        {id<domain::ContextEntryId>("tool-entry-" + std::to_string(order)),
         domain::ContextEntryKind::tool_result,
         std::nullopt,
         std::move(message),
         {id<domain::ContextSourceId>("tool-source-" + std::to_string(order)),
          std::nullopt, std::nullopt},
         0,
         order++,
         1});
  }
  return {std::move(entries), {}, {4096, 512, 0}, order};
}

auto request(std::string inference, std::string message,
             const runtime::ToolRegistrySnapshot& tools,
             std::vector<domain::Message> results = {})
    -> backend::BackendRequest {
  return {id<domain::InferenceId>(inference),
          id<domain::MessageId>(message),
          id<domain::ModelId>("model"),
          context(std::move(results)),
          tools.declarations(),
          {}};
}

class Wake final : public runtime::RunWakeSink {
 public:
  auto wake() noexcept -> void override {
    {
      std::lock_guard lock(m_mutex);
      ++m_generation;
    }
    m_changed.notify_all();
  }

  auto wait(std::size_t generation) -> void {
    std::unique_lock lock(m_mutex);
    static_cast<void>(m_changed.wait_for(
        lock, 1s, [&] { return m_generation != generation; }));
  }

  auto generation() -> std::size_t {
    std::lock_guard lock(m_mutex);
    return m_generation;
  }

 private:
  std::mutex m_mutex;
  std::condition_variable m_changed;
  std::size_t m_generation{};
};

auto drain_until_questions(runtime::RunKernel& kernel, Wake& wake) -> void {
  for (int attempt = 0; attempt < 100 && !kernel.pending_question_input();
       ++attempt) {
    const auto generation = wake.generation();
    REQUIRE(kernel.drain());
    if (!kernel.pending_question_input()) wake.wait(generation);
  }
  REQUIRE(kernel.pending_question_input());
}

auto drain_until_done(runtime::RunKernel& kernel, Wake& wake) -> void {
  for (int attempt = 0; attempt < 100 && kernel.active_run_id(); ++attempt) {
    const auto generation = wake.generation();
    REQUIRE(kernel.drain());
    if (kernel.active_run_id()) wake.wait(generation);
  }
  REQUIRE_FALSE(kernel.active_run_id());
}

class MemoryStore final : public storage::SessionStore {
 public:
  domain::SessionId session_id{id<domain::SessionId>("session")};
  domain::EventTimestamp created{std::chrono::milliseconds{1}};
  std::vector<domain::RunEvent> events;
  bool fail_next_append{};

  auto create_session(storage::SessionCreate session, std::stop_token)
      -> std::expected<void, storage::SessionStoreError> override {
    session_id = std::move(session.session_id);
    created = session.created_at;
    return {};
  }
  auto open_session(const domain::SessionId& requested, std::stop_token)
      -> std::expected<storage::SessionInfo,
                       storage::SessionStoreError> override {
    if (requested != session_id) {
      return std::unexpected(storage::SessionStoreError{
          storage::SessionStoreErrorCode::not_found, "missing", false});
    }
    const auto last =
        events.empty() ? created : events.back().metadata.timestamp;
    return storage::SessionInfo{
        session_id, created, last,
        events.empty() ? 0 : events.back().metadata.sequence};
  }
  auto list_sessions(std::size_t, std::stop_token)
      -> std::expected<std::vector<storage::SessionInfo>,
                       storage::SessionStoreError> override {
    return std::vector<storage::SessionInfo>{};
  }
  auto append_events(const domain::SessionId& requested,
                     std::span<const domain::RunEvent> additions,
                     std::stop_token)
      -> std::expected<void, storage::SessionStoreError> override {
    if (requested != session_id) {
      return std::unexpected(storage::SessionStoreError{
          storage::SessionStoreErrorCode::not_found, "missing", false});
    }
    if (std::exchange(fail_next_append, false)) {
      return std::unexpected(storage::SessionStoreError{
          storage::SessionStoreErrorCode::io_failure, "failed", true});
    }
    events.insert(events.end(), additions.begin(), additions.end());
    return {};
  }
  auto replay_events(const domain::SessionId&, std::stop_token)
      -> std::expected<std::vector<domain::RunEvent>,
                       storage::SessionStoreError> override {
    return events;
  }
};

constexpr std::string_view arguments =
    R"({"questions":[{"id":"format","prompt":"Choose output","kind":"one","required":true,"minimum_selections":1,"maximum_selections":1,"options":[{"id":"short","label":"Short","recommended":true},{"id":"long","label":"Long"}],"other":{"label":"Other","placeholder":"Describe it"}}]})";

auto snapshot() -> runtime::ToolRegistrySnapshot {
  runtime::ToolRegistry registry;
  REQUIRE(runtime::register_ask_user_tool(registry, true));
  auto result = registry.snapshot();
  REQUIRE(result);
  return std::move(*result);
}

auto provenance() -> domain::RunProvenance {
  return {"test-version",
          "test-backend",
          std::nullopt,
          id<domain::ModelId>("model"),
          std::nullopt,
          {},
          {},
          {}};
}

} // namespace

TEST_CASE("ask_user registration requires an interactive input protocol",
          "[questions][registry][failure]") {
  runtime::ToolRegistry registry;
  auto unavailable = runtime::register_ask_user_tool(registry, false);
  REQUIRE_FALSE(unavailable);
  REQUIRE(unavailable.error().code ==
          runtime::ToolRegistryErrorCode::interactive_input_unavailable);

  runtime::AskUserLimits excessive;
  excessive.questions = 4;
  auto invalid_limits =
      runtime::register_ask_user_tool(registry, true, excessive);
  REQUIRE_FALSE(invalid_limits);
  REQUIRE(invalid_limits.error().code ==
          runtime::ToolRegistryErrorCode::invalid_declaration);

  REQUIRE(runtime::register_ask_user_tool(registry, true));
  auto tools = registry.snapshot();
  REQUIRE(tools);
  const auto* registered = tools->find("ask_user");
  REQUIRE(registered != nullptr);
  REQUIRE(registered->declaration.effects.empty());
  const runtime::ToolExecutorContract ask_user_contract{
      "aiforge.runtime.ask_user", "1"};
  REQUIRE(registered->executor_contract == ask_user_contract);
  REQUIRE(registered->executor->validate(
      {"application/json", std::string{arguments}}));
  REQUIRE_FALSE(registered->executor->validate(
      {"application/json", R"({"questions":[]})"}));
  REQUIRE_FALSE(registered->executor->validate(
      {"application/json",
       R"({"questions":[{"id":"same","prompt":"First","kind":"one","required":true,"minimum_selections":1,"maximum_selections":1,"options":[{"id":"x","label":"X"}]},{"id":"same","prompt":"Second","kind":"one","required":true,"minimum_selections":1,"maximum_selections":1,"options":[{"id":"y","label":"Y"}]}]})"}));
  REQUIRE_FALSE(registered->executor->validate(
      {"application/json",
       R"({"questions":[{"id":"q","prompt":"Impossible","kind":"many","required":true,"minimum_selections":2,"maximum_selections":1,"options":[{"id":"x","label":"X"}]}]})"}));
  REQUIRE_FALSE(registered->executor->validate(
      {"application/json",
       R"({"questions":[{"id":"q","prompt":"Unknown","kind":"one","required":true,"minimum_selections":1,"maximum_selections":1,"options":[{"id":"x","label":"X"}],"unexpected":true}]})"}));
  REQUIRE_FALSE(registered->executor->validate(
      {"application/json",
       R"({"questions":[{"id":"q","prompt":"Control\u001btext","kind":"one","required":true,"minimum_selections":1,"maximum_selections":1,"options":[{"id":"x","label":"X"}]}]})"}));
  REQUIRE_FALSE(registered->executor->validate(
      {"application/json",
       R"({"questions":[{"id":"q","prompt":"Other","kind":"one","required":true,"minimum_selections":1,"maximum_selections":1,"options":[{"id":"x","label":"X"}],"other":{"label":""}}]})"}));
  const auto oversized =
      std::string{R"({"questions":[{"id":"q","prompt":")"} +
      std::string(1025, 'x') +
      R"(","kind":"one","required":true,"minimum_selections":1,"maximum_selections":1,"options":[{"id":"x","label":"X"}]}]})";
  REQUIRE_FALSE(
      registered->executor->validate({"application/json", oversized}));
}

TEST_CASE("ask_user resolves once and continues inference in the same run",
          "[questions][runtime]") {
  const auto tools = snapshot();
  const auto invocation = id<domain::InvocationId>("ask-call");
  auto initial = request("inference-1", "assistant-1", tools);
  const auto result_message = domain::Message{
      id<domain::MessageId>("tool-message-6"),
      domain::Role::tool,
      {domain::StructuredDataBlock{
          "application/json",
          R"({"answers":[{"other":null,"question_id":"format","selected_option_ids":["short"]}],"status":"answered"})"}},
      invocation};
  auto continuation =
      request("inference-2", "assistant-2", tools, {result_message});
  testing::ScriptedBackend backend{{
      {initial, testing::StreamScript{{
                    backend::ResponseStarted{"response-1"},
                    backend::ToolCallDelta{invocation, "ask_user",
                                           std::string{arguments}},
                    backend::ResponseFinished{domain::FinishReason::tool_call},
                    testing::EndOfStream{},
                }}},
      {continuation,
       testing::StreamScript{{
           backend::ResponseStarted{"response-2"},
           backend::ContentDelta{continuation.assistant_message_id,
                                 domain::TextBlock{"done"}},
           backend::ResponseFinished{domain::FinishReason::stop},
           testing::EndOfStream{},
       }}},
  }};
  Wake wake;
  runtime::RunKernel kernel{
      id<domain::SessionId>("session"), backend, &wake, {}, {}, tools};
  REQUIRE(kernel.start(
      {id<domain::RunId>("run"),
       {id<domain::SurfaceId>("tui"), id<domain::WorkspaceId>("chat"),
        id<domain::PermissionProfileId>("observe"), std::nullopt},
       {id<domain::MessageId>("user"),
        domain::Role::user,
        {domain::TextBlock{"hello"}},
        std::nullopt},
       initial}));
  drain_until_questions(kernel, wake);

  const auto pending = kernel.pending_question_input();
  REQUIRE(pending->invocation_id == invocation);
  REQUIRE(pending->questions.size() == 1);
  REQUIRE(pending->questions.front().options.front().recommended);

  auto invalid = kernel.answer_questions(
      id<domain::RunId>("run"), invocation,
      {{id<domain::QuestionId>("format"), {"missing"}, std::nullopt}});
  REQUIRE_FALSE(invalid);
  REQUIRE_FALSE(kernel.answer_questions(
      id<domain::RunId>("run"), invocation,
      {{id<domain::QuestionId>("format"), {}, std::nullopt}}));
  REQUIRE_FALSE(kernel.answer_questions(
      id<domain::RunId>("run"), invocation,
      {{id<domain::QuestionId>("format"), {"short"}, "also other"}}));
  REQUIRE_FALSE(kernel.answer_questions(
      id<domain::RunId>("run"), invocation,
      {{id<domain::QuestionId>("format"), {}, std::string(4097, 'x')}}));
  REQUIRE(kernel.answer_questions(
      id<domain::RunId>("run"), invocation,
      {{id<domain::QuestionId>("format"), {"short"}, std::nullopt}}));
  REQUIRE_FALSE(kernel.pending_question_input());
  REQUIRE_FALSE(kernel.answer_questions(
      id<domain::RunId>("run"), invocation,
      {{id<domain::QuestionId>("format"), {"short"}, std::nullopt}}));
  const auto continued =
      kernel.continue_run(id<domain::RunId>("run"), continuation);
  INFO((continued ? std::string{} : continued.error().message));
  REQUIRE(continued);
  drain_until_done(kernel, wake);
  REQUIRE(backend.remaining_exchanges() == 0);
}

TEST_CASE("legacy pending ask_user history without provenance is rejected",
          "[questions][replay][failure]") {
  const auto tools = snapshot();
  const auto invocation = id<domain::InvocationId>("ask-call");
  auto initial = request("inference-1", "assistant-1", tools);
  testing::ScriptedBackend backend{{
      {initial,
       testing::StreamScript{
           {backend::ResponseStarted{"response"},
            backend::ToolCallDelta{invocation, "ask_user",
                                   std::string{arguments}},
            backend::ResponseFinished{domain::FinishReason::tool_call},
            testing::EndOfStream{}}}},
  }};
  Wake wake;
  runtime::RunKernel original{
      id<domain::SessionId>("session"), backend, &wake, {}, {}, tools};
  REQUIRE(original.start(
      {id<domain::RunId>("run"),
       {id<domain::SurfaceId>("tui"), id<domain::WorkspaceId>("chat"),
        id<domain::PermissionProfileId>("observe"), std::nullopt},
       {id<domain::MessageId>("user"),
        domain::Role::user,
        {domain::TextBlock{"hello"}},
        std::nullopt},
       initial}));
  drain_until_questions(original, wake);

  MemoryStore store;
  store.events = original.event_log().events();
  REQUIRE(original.cancel_run(id<domain::RunId>("run"), "backend cancelled"));
  REQUIRE_FALSE(original.pending_question_input());
  REQUIRE(std::ranges::count_if(
              original.event_log().events(), [](const auto& event) {
                return std::holds_alternative<domain::QuestionCancelled>(
                    event.payload);
              }) == 1);
  REQUIRE(std::ranges::count_if(
              original.event_log().events(), [](const auto& event) {
                return std::holds_alternative<domain::RunCancelled>(
                    event.payload);
              }) == 1);
  testing::ScriptedBackend replay_backend{{}};
  auto rejected = runtime::RunKernel::open_durable(
      {id<domain::SessionId>("session"), runtime::DurableSessionMode::resume,
       store.created},
      store, replay_backend, nullptr, {}, {}, tools);
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().code ==
          runtime::RunKernelErrorCode::replay_rejected);
}

TEST_CASE("pending ask_user replay retains the run tool subset",
          "[questions][replay][tools][failure]") {
  runtime::ToolRegistry registry;
  REQUIRE(runtime::register_ask_user_tool(registry, true));
  REQUIRE(registry.register_tool(
      {"hidden",
       "Not advertised for this run",
       {"application/schema+json", R"({"type":"object"})"},
       {},
       {}},
      std::make_shared<testing::ScriptedToolExecutor>(
          std::vector<testing::ScriptedToolExchange>{})));
  auto full = registry.snapshot();
  REQUIRE(full);
  const std::vector<std::string> selected_names{"ask_user"};
  auto selected = full->subset(selected_names);
  REQUIRE(selected);

  const auto invocation = id<domain::InvocationId>("ask-call");
  auto initial = request("inference-1", "assistant-1", *selected);
  const auto result_message = domain::Message{
      id<domain::MessageId>("tool-message-6"),
      domain::Role::tool,
      {domain::StructuredDataBlock{
          "application/json",
          R"({"answers":[{"other":null,"question_id":"format","selected_option_ids":["short"]}],"status":"answered"})"}},
      invocation};
  auto continuation =
      request("inference-2", "assistant-2", *selected, {result_message});
  testing::ScriptedBackend initial_backend{{
      {initial, testing::StreamScript{{
                    backend::ResponseStarted{"response-1"},
                    backend::ToolCallDelta{invocation, "ask_user",
                                           std::string{arguments}},
                    backend::ResponseFinished{domain::FinishReason::tool_call},
                    testing::EndOfStream{},
                }}},
  }};
  Wake initial_wake;
  MemoryStore store;
  auto original = runtime::RunKernel::open_durable(
      {id<domain::SessionId>("session"), runtime::DurableSessionMode::create,
       store.created},
      store, initial_backend, &initial_wake, {}, {}, *full);
  REQUIRE(original);
  auto start = runtime::RunStart{
      id<domain::RunId>("run"),
      {id<domain::SurfaceId>("tui"), id<domain::WorkspaceId>("chat"),
       id<domain::PermissionProfileId>("observe"), std::nullopt},
      {id<domain::MessageId>("user"),
       domain::Role::user,
       {domain::TextBlock{"hello"}},
       std::nullopt},
      initial};
  const auto missing_provenance = (*original)->start(start);
  REQUIRE_FALSE(missing_provenance);
  REQUIRE(missing_provenance.error().code ==
          runtime::RunKernelErrorCode::invalid_start);
  REQUIRE(store.events.empty());
  auto unversioned = start;
  unversioned.request.tools = full->declarations();
  unversioned.provenance = provenance();
  const auto unversioned_start = (*original)->start(std::move(unversioned));
  REQUIRE_FALSE(unversioned_start);
  REQUIRE(unversioned_start.error().code ==
          runtime::RunKernelErrorCode::invalid_start);
  REQUIRE(store.events.empty());
  start.provenance = provenance();
  REQUIRE((*original)->start(std::move(start)));
  drain_until_questions(**original, initial_wake);

  original->reset();

  MemoryStore tampered;
  tampered.events = store.events;
  const auto recorded =
      std::ranges::find_if(tampered.events, [](const auto& event) {
        return std::holds_alternative<domain::RunProvenanceRecorded>(
            event.payload);
      });
  REQUIRE(recorded != tampered.events.end());
  std::get<domain::RunProvenanceRecorded>(recorded->payload)
      .provenance.tools.clear();
  testing::ScriptedBackend rejected_backend{{}};
  auto rejected_replay = runtime::RunKernel::open_durable(
      {id<domain::SessionId>("session"), runtime::DurableSessionMode::resume,
       tampered.created},
      tampered, rejected_backend, nullptr, {}, {}, *full);
  REQUIRE_FALSE(rejected_replay);
  REQUIRE(rejected_replay.error().code ==
          runtime::RunKernelErrorCode::replay_rejected);

  MemoryStore missing_digest;
  missing_digest.events = store.events;
  const auto missing_recorded =
      std::ranges::find_if(missing_digest.events, [](const auto& event) {
        return std::holds_alternative<domain::RunProvenanceRecorded>(
            event.payload);
      });
  REQUIRE(missing_recorded != missing_digest.events.end());
  std::get<domain::RunProvenanceRecorded>(missing_recorded->payload)
      .provenance.tools.front()
      .registration_digest.reset();
  auto missing_digest_replay = runtime::RunKernel::open_durable(
      {id<domain::SessionId>("session"), runtime::DurableSessionMode::resume,
       missing_digest.created},
      missing_digest, rejected_backend, nullptr, {}, {}, *full);
  REQUIRE_FALSE(missing_digest_replay);
  REQUIRE(missing_digest_replay.error().code ==
          runtime::RunKernelErrorCode::replay_rejected);

  MemoryStore widened_scopes;
  widened_scopes.events = store.events;
  const auto policy =
      std::ranges::find_if(widened_scopes.events, [](const auto& event) {
        return std::holds_alternative<domain::ToolPolicyDecided>(event.payload);
      });
  REQUIRE(policy != widened_scopes.events.end());
  std::get<domain::ToolPolicyDecided>(policy->payload)
      .scopes.push_back({domain::Effect::read, "filesystem.root", "/outside"});
  auto widened_scope_replay = runtime::RunKernel::open_durable(
      {id<domain::SessionId>("session"), runtime::DurableSessionMode::resume,
       widened_scopes.created},
      widened_scopes, rejected_backend, nullptr, {}, {}, *full);
  REQUIRE_FALSE(widened_scope_replay);
  REQUIRE(widened_scope_replay.error().code ==
          runtime::RunKernelErrorCode::replay_rejected);

  MemoryStore duplicate_policy;
  duplicate_policy.events = store.events;
  const auto original_policy =
      std::ranges::find_if(duplicate_policy.events, [](const auto& event) {
        return std::holds_alternative<domain::ToolPolicyDecided>(event.payload);
      });
  REQUIRE(original_policy != duplicate_policy.events.end());
  auto repeated_policy = *original_policy;
  repeated_policy.metadata.event_id =
      id<domain::EventId>("duplicate-policy-decision");
  repeated_policy.metadata.sequence += 1;
  for (auto current = std::next(original_policy);
       current != duplicate_policy.events.end(); ++current) {
    current->metadata.sequence += 1;
  }
  duplicate_policy.events.insert(std::next(original_policy),
                                 std::move(repeated_policy));
  auto duplicate_policy_replay = runtime::RunKernel::open_durable(
      {id<domain::SessionId>("session"), runtime::DurableSessionMode::resume,
       duplicate_policy.created},
      duplicate_policy, rejected_backend, nullptr, {}, {}, *full);
  REQUIRE_FALSE(duplicate_policy_replay);
  REQUIRE(duplicate_policy_replay.error().code ==
          runtime::RunKernelErrorCode::replay_rejected);

  const auto* original_registration = full->find("ask_user");
  REQUIRE(original_registration != nullptr);
  REQUIRE(original_registration->executor_contract);
  const auto rejects_drift = [&](backend::ToolDeclaration declaration,
                                 runtime::ToolExecutionLimits limits,
                                 runtime::ToolExecutorContract contract) {
    runtime::ToolRegistry drifted_registry;
    REQUIRE(drifted_registry.register_tool(std::move(declaration),
                                           original_registration->executor,
                                           limits, std::move(contract)));
    auto drifted = drifted_registry.snapshot();
    REQUIRE(drifted);
    testing::ScriptedBackend drifted_backend{{}};
    auto result = runtime::RunKernel::open_durable(
        {id<domain::SessionId>("session"), runtime::DurableSessionMode::resume,
         store.created},
        store, drifted_backend, nullptr, {}, {}, std::move(*drifted));
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code ==
            runtime::RunKernelErrorCode::replay_rejected);
  };

  auto schema_drift = original_registration->declaration;
  schema_drift.input_schema.data.push_back(' ');
  rejects_drift(std::move(schema_drift), original_registration->limits,
                *original_registration->executor_contract);
  auto limit_drift = original_registration->limits;
  ++limit_drift.output_bytes;
  rejects_drift(original_registration->declaration, limit_drift,
                *original_registration->executor_contract);
  auto version_drift = *original_registration->executor_contract;
  version_drift.version = "2";
  rejects_drift(original_registration->declaration,
                original_registration->limits, std::move(version_drift));

  testing::ScriptedBackend replay_backend{{
      {continuation,
       testing::StreamScript{{
           backend::ResponseStarted{"response-2"},
           backend::ContentDelta{continuation.assistant_message_id,
                                 domain::TextBlock{"done"}},
           backend::ResponseFinished{domain::FinishReason::stop},
           testing::EndOfStream{},
       }}},
  }};
  Wake replay_wake;
  auto resumed = runtime::RunKernel::open_durable(
      {id<domain::SessionId>("session"), runtime::DurableSessionMode::resume,
       store.created},
      store, replay_backend, &replay_wake, {}, {}, *full);
  REQUIRE(resumed);
  REQUIRE((*resumed)->pending_question_input());
  REQUIRE((*resumed)->answer_questions(
      id<domain::RunId>("run"), invocation,
      {{id<domain::QuestionId>("format"), {"short"}, std::nullopt}}));

  auto widened = continuation;
  widened.tools = full->declarations();
  const auto rejected =
      (*resumed)->continue_run(id<domain::RunId>("run"), std::move(widened));
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().code ==
          runtime::RunKernelErrorCode::continuation_not_ready);
  REQUIRE((*resumed)->continue_run(id<domain::RunId>("run"), continuation));
  drain_until_done(**resumed, replay_wake);
  REQUIRE(replay_backend.remaining_exchanges() == 0);
}

TEST_CASE("ask_user blocks a queued tool until the question resolves",
          "[questions][runtime][ordering]") {
  const auto ask = id<domain::InvocationId>("ask-call");
  const auto queued = id<domain::InvocationId>("queued-call");
  const runtime::ToolExecutionLimits limits{4096, 4, 1s};
  auto queued_executor = std::make_shared<testing::ScriptedToolExecutor>(
      std::vector<testing::ScriptedToolExchange>{
          {runtime::ToolInvocation{
               queued,
               std::nullopt,
               "after_question",
               runtime::ValidatedToolArguments{{"application/json", "{}"}},
               {},
               limits},
           testing::ToolStreamScript{{
               runtime::ToolExecutionEvent{
                   runtime::ToolResult{{domain::TextBlock{"after"}}}},
               testing::ToolEndOfStream{},
           }}}});
  runtime::ToolRegistry registry;
  REQUIRE(runtime::register_ask_user_tool(registry, true));
  REQUIRE(registry.register_tool(
      {"after_question",
       "Run after the question",
       {"application/schema+json", R"({"type":"object"})"},
       {},
       {}},
      queued_executor, limits,
      runtime::ToolExecutorContract{"test.after_question", "1"}));
  auto tools = registry.snapshot();
  REQUIRE(tools);
  auto initial = request("inference-1", "assistant-1", *tools);
  testing::ScriptedBackend backend{{
      {initial,
       testing::StreamScript{{
           backend::ResponseStarted{"response"},
           backend::ToolCallDelta{ask, "ask_user", std::string{arguments}},
           backend::ToolCallDelta{queued, "after_question", "{}"},
           backend::ResponseFinished{domain::FinishReason::tool_call},
           testing::EndOfStream{},
       }}},
  }};
  Wake wake;
  runtime::RunKernel kernel{
      id<domain::SessionId>("session"), backend, &wake, {}, {}, *tools};
  auto start = runtime::RunStart{
      id<domain::RunId>("run"),
      {id<domain::SurfaceId>("tui"), id<domain::WorkspaceId>("chat"),
       id<domain::PermissionProfileId>("observe"), std::nullopt},
      {id<domain::MessageId>("user"),
       domain::Role::user,
       {domain::TextBlock{"hello"}},
       std::nullopt},
      initial};
  start.provenance = provenance();
  REQUIRE(kernel.start(std::move(start)));
  drain_until_questions(kernel, wake);
  REQUIRE(queued_executor->recorded_invocations().empty());

  MemoryStore store;
  store.events = kernel.event_log().events();
  testing::ScriptedBackend replay_backend{{}};
  Wake resumed_wake;
  auto resumed = runtime::RunKernel::open_durable(
      {id<domain::SessionId>("session"), runtime::DurableSessionMode::resume,
       store.created},
      store, replay_backend, &resumed_wake, {}, {}, *tools);
  REQUIRE(resumed);
  REQUIRE((*resumed)->pending_question_input());
  REQUIRE(queued_executor->recorded_invocations().empty());

  REQUIRE((*resumed)->answer_questions(
      id<domain::RunId>("run"), ask,
      {{id<domain::QuestionId>("format"), {"short"}, std::nullopt}}));
  for (int attempt = 0;
       attempt < 100 && queued_executor->remaining_exchanges() != 0;
       ++attempt) {
    const auto generation = resumed_wake.generation();
    REQUIRE((*resumed)->drain());
    if (queued_executor->remaining_exchanges() != 0) {
      resumed_wake.wait(generation);
    }
  }
  REQUIRE(queued_executor->remaining_exchanges() == 0);
  for (int attempt = 0; attempt < 100; ++attempt) {
    REQUIRE((*resumed)->drain());
    const auto results =
        runtime::tool_result_messages((*resumed)->event_log().events());
    REQUIRE(results);
    if (results->size() == 2) break;
    resumed_wake.wait(resumed_wake.generation());
  }
  const auto results =
      runtime::tool_result_messages((*resumed)->event_log().events());
  REQUIRE(results);
  REQUIRE(results->size() == 2);
  REQUIRE((*resumed)->cancel_run(id<domain::RunId>("run"), "cleanup"));
  REQUIRE(kernel.cancel_run(id<domain::RunId>("run"), "cleanup"));
}

TEST_CASE("question answer persistence failure cannot publish a result",
          "[questions][storage][failure]") {
  const auto tools = snapshot();
  const auto invocation = id<domain::InvocationId>("ask-call");
  auto initial = request("inference-1", "assistant-1", tools);
  testing::ScriptedBackend backend{{
      {initial,
       testing::StreamScript{
           {backend::ResponseStarted{"response"},
            backend::ToolCallDelta{invocation, "ask_user",
                                   std::string{arguments}},
            backend::ResponseFinished{domain::FinishReason::tool_call},
            testing::EndOfStream{}}}},
  }};
  MemoryStore store;
  Wake wake;
  auto durable = runtime::RunKernel::open_durable(
      {id<domain::SessionId>("session"), runtime::DurableSessionMode::create,
       store.created},
      store, backend, &wake, {}, {}, tools);
  REQUIRE(durable);
  auto start = runtime::RunStart{
      id<domain::RunId>("run"),
      {id<domain::SurfaceId>("tui"), id<domain::WorkspaceId>("chat"),
       id<domain::PermissionProfileId>("observe"), std::nullopt},
      {id<domain::MessageId>("user"),
       domain::Role::user,
       {domain::TextBlock{"hello"}},
       std::nullopt},
      initial};
  start.provenance = provenance();
  REQUIRE((*durable)->start(std::move(start)));
  drain_until_questions(**durable, wake);
  const auto stored_before = store.events.size();
  store.fail_next_append = true;
  const auto answered = (*durable)->answer_questions(
      id<domain::RunId>("run"), invocation,
      {{id<domain::QuestionId>("format"), {"short"}, std::nullopt}});
  REQUIRE_FALSE(answered);
  REQUIRE(answered.error().code ==
          runtime::RunKernelErrorCode::storage_failure);
  REQUIRE(store.events.size() == stored_before);
  REQUIRE(std::ranges::none_of(store.events, [](const auto& event) {
    return std::holds_alternative<domain::QuestionAnswered>(event.payload) ||
           std::holds_alternative<domain::ToolResultRecorded>(event.payload);
  }));
  REQUIRE_FALSE((*durable)->active_run_id());
}
