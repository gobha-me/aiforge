#include <aiforge/runtime/context_builder.hpp>
#include <aiforge/runtime/conversation_context.hpp>
#include <aiforge/runtime/run_kernel.hpp>
#include <aiforge/runtime/tool_registry.hpp>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <thread>
#include <utility>

namespace {
using namespace aiforge;
template <class Id> auto id(const std::string& text) -> Id {
  return Id::from(text).value();
}

class RejectingExecutor final : public runtime::ToolExecutor {
 public:
  auto validate(const domain::StructuredDataBlock&) const
      -> std::expected<runtime::ValidatedToolArguments,
                       runtime::ToolExecutionError> override {
    return std::unexpected(runtime::ToolExecutionError{
        runtime::ToolExecutionErrorCode::invalid_arguments,
        "arguments rejected", false});
  }
  auto start(runtime::ToolInvocation, std::stop_token)
      -> std::expected<std::unique_ptr<runtime::ToolExecutionStream>,
                       runtime::ToolExecutionError> override {
    return std::unexpected(runtime::ToolExecutionError{
        runtime::ToolExecutionErrorCode::internal_failure, "must not execute",
        false});
  }
};
auto tool_snapshot() -> runtime::ToolRegistrySnapshot {
  runtime::ToolRegistry registry;
  REQUIRE(registry.register_tool(
      {"read",
       "Read a value",
       {"application/schema+json", R"({"type":"object"})"},
       {domain::Effect::read},
       {}},
      std::make_shared<RejectingExecutor>()));
  auto snapshot = registry.snapshot();
  REQUIRE(snapshot);
  return std::move(*snapshot);
}

class ResponseStream final : public backend::BackendStream {
 public:
  explicit ResponseStream(const domain::MessageId& message, bool tools) {
    m_events.push_back(backend::ResponseStarted{"fake-response"});
    if (tools) {
      m_events.push_back(backend::ToolCallDelta{
          id<domain::InvocationId>("active-call"), "read", "{}"});
      m_events.push_back(
          backend::ResponseFinished{domain::FinishReason::tool_call});
    } else {
      m_events.push_back(
          backend::ContentDelta{message, domain::TextBlock{"answer"}});
      m_events.push_back(backend::ResponseFinished{domain::FinishReason::stop});
    }
  }
  auto next(std::stop_token)
      -> std::expected<std::optional<backend::BackendEvent>,
                       backend::BackendError> override {
    if (m_index == m_events.size()) return std::nullopt;
    return m_events[m_index++];
  }

 private:
  std::vector<backend::BackendEvent> m_events;
  std::size_t m_index{};
};
class RecordingBackend final : public backend::Backend {
 public:
  auto start(backend::BackendRequest request, std::stop_token)
      -> std::expected<std::unique_ptr<backend::BackendStream>,
                       backend::BackendError> override {
    const bool tools = request.assistant_message_id ==
                       id<domain::MessageId>("active-assistant");
    requests.push_back(request);
    return std::make_unique<ResponseStream>(request.assistant_message_id,
                                            tools);
  }
  std::vector<backend::BackendRequest> requests;
};
auto drain_inference(runtime::RunKernel& kernel) -> void {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{3};
  while (kernel.active_inference_id() &&
         std::chrono::steady_clock::now() < deadline) {
    REQUIRE(kernel.drain());
    if (kernel.active_inference_id())
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  REQUIRE_FALSE(kernel.active_inference_id());
}

auto start_request(
    const domain::SessionEventLog& log, const std::string& suffix,
    const std::vector<backend::ToolDeclaration>& declarations = {})
    -> runtime::RunStart {
  domain::Message instruction{
      id<domain::MessageId>(suffix + "-runtime-message"),
      domain::Role::system,
      {domain::TextBlock{"Runtime contract"}},
      {}};
  domain::Message user{id<domain::MessageId>(suffix + "-user-message"),
                       domain::Role::user,
                       {domain::TextBlock{"New question"}},
                       {}};
  auto instruction_tokens = runtime::estimate_conversation_message(instruction);
  auto user_tokens = runtime::estimate_conversation_message(user);
  REQUIRE(instruction_tokens);
  REQUIRE(user_tokens);
  const domain::ContextCapacity capacity{4096, 100, 100};
  auto prepared =
      runtime::prepare_conversation_context({log,
                                             id<domain::ModelId>("model"),
                                             capacity,
                                             *instruction_tokens + *user_tokens,
                                             17,
                                             {},
                                             {}});
  REQUIRE(prepared);
  domain::ContextBuildInput input{capacity, {}, {}};
  input.instructions.push_back(
      {id<domain::ContextEntryId>(suffix + "-runtime-entry"),
       domain::InstructionLayer::application_runtime,
       domain::InstructionOperation::add,
       {},
       instruction,
       {id<domain::ContextSourceId>(suffix + "-runtime-source"), {}, {}},
       0,
       1,
       *instruction_tokens});
  for (const auto& group : prepared->selection.selected_groups)
    for (const auto& entry : group.entries)
      input.content.push_back(entry.content);
  input.content.push_back(
      {id<domain::ContextEntryId>(suffix + "-user-entry"),
       domain::ContextContentKind::conversation,
       user,
       {id<domain::ContextSourceId>(suffix + "-user-source"), {}, {}},
       17 + prepared->selection.selected_entry_count,
       *user_tokens});
  auto context = runtime::ContextBuilder{}.build(std::move(input));
  REQUIRE(context);
  runtime::RunStart result{id<domain::RunId>(suffix + "-run"),
                           {id<domain::SurfaceId>("chat"),
                            id<domain::WorkspaceId>("chat"),
                            id<domain::PermissionProfileId>("observe"),
                            {}},
                           user,
                           {id<domain::InferenceId>(suffix + "-inference"),
                            id<domain::MessageId>(suffix + "-assistant"),
                            id<domain::ModelId>("model"),
                            std::move(*context),
                            declarations,
                            {}}};
  result.attributes.conversation_admission = std::move(prepared->admission);
  return result;
}

struct Fixture {
  runtime::ToolRegistrySnapshot tools{tool_snapshot()};
  RecordingBackend backend;
  runtime::RunKernel kernel{
      id<domain::SessionId>("session"), backend, nullptr, {}, {}, tools};
  std::optional<runtime::RunStart> active_start;
  Fixture() {
    REQUIRE(kernel.start(start_request(kernel.event_log(), "source")));
    drain_inference(kernel);
    REQUIRE_FALSE(kernel.active_run_id());
    active_start =
        start_request(kernel.event_log(), "active", tools.declarations());
    REQUIRE(kernel.start(*active_start));
    drain_inference(kernel);
    REQUIRE(kernel.active_run_id() == active_start->run_id);
    REQUIRE(backend.requests.size() == 2);
  }
  auto continuation() -> backend::BackendRequest {
    auto request = active_start->request;
    request.inference_id = id<domain::InferenceId>("continuation-inference");
    request.assistant_message_id =
        id<domain::MessageId>("continuation-assistant");
    auto messages = runtime::reconstruct_active_tool_continuation(
        kernel.event_log(), active_start->run_id);
    REQUIRE(messages);
    REQUIRE(messages->size() == 2);
    REQUIRE(messages->front().role == domain::Role::assistant);
    REQUIRE(messages->front().tool_calls.size() == 1);
    REQUIRE(messages->back().role == domain::Role::tool);
    auto order = request.context.entries.back().order;
    std::size_t index{};
    for (const auto& message : *messages) {
      auto estimate = runtime::estimate_conversation_message(message);
      REQUIRE(estimate);
      const auto suffix = std::to_string(++index);
      request.context.entries.push_back(
          {id<domain::ContextEntryId>("active-entry-" + suffix),
           message.role == domain::Role::tool
               ? domain::ContextEntryKind::tool_result
               : domain::ContextEntryKind::conversation,
           {},
           message,
           {id<domain::ContextSourceId>("active-source-" + suffix), {}, {}},
           0,
           ++order,
           *estimate});
      request.context.estimated_input_tokens += *estimate;
    }
    return request;
  }
  auto reject(backend::BackendRequest request) -> void {
    const auto events = kernel.event_log().events();
    const auto dispatched = backend.requests.size();
    const auto result =
        kernel.continue_run(active_start->run_id, std::move(request));
    REQUIRE_FALSE(result);
    CHECK(result.error().code ==
          runtime::RunKernelErrorCode::continuation_not_ready);
    CHECK(kernel.event_log().events() == events);
    CHECK(backend.requests.size() == dispatched);
    CHECK(kernel.active_run_id() == active_start->run_id);
    CHECK_FALSE(kernel.active_inference_id());
  }
  auto change_policy() -> void {
    REQUIRE(kernel.record_conversation_policy(
        {id<domain::RunId>("policy-run"),
         {id<domain::SurfaceId>("chat"),
          id<domain::WorkspaceId>("chat"),
          id<domain::PermissionProfileId>("observe"),
          {},
          {},
          domain::RunPurpose::control},
         0,
         domain::ConversationMode::rolling,
         {id<domain::RunId>("source-run")}}));
  }
};
} // namespace

TEST_CASE("sealed continuation rejects changed frozen context before dispatch",
          "[conversation][continuation][failure]") {
  Fixture fixture;
  auto request = fixture.continuation();
  REQUIRE(request.context.entries.size() == 6);
  SECTION("changed historical message") {
    request.context.entries[1].message.content = {
        domain::TextBlock{"replacement history"}};
  }
  SECTION("omitted historical message") {
    request.context.entries.erase(request.context.entries.begin() + 1);
  }
  SECTION("injected historical conversation") {
    auto extra = request.context.entries[1];
    extra.entry_id = id<domain::ContextEntryId>("injected-history");
    extra.message.message_id = id<domain::MessageId>("injected-message");
    request.context.entries.insert(request.context.entries.begin() + 1,
                                   std::move(extra));
  }
  SECTION("changed original user") {
    request.context.entries[3].message.content = {
        domain::TextBlock{"replacement question"}};
  }
  SECTION("changed model") {
    request.model_id = id<domain::ModelId>("other-model");
  }
  SECTION("changed capacity") {
    ++request.context.capacity.context_window_tokens;
  }
  SECTION("changed frozen mandatory budget") {
    ++request.context.entries.front().estimated_tokens;
  }
  SECTION("incorrect aggregate input estimate") {
    --request.context.estimated_input_tokens;
  }
  SECTION("unadmitted memory identity cannot be introduced") {
    // Preserve the original token totals and messages: only the memory
    // identity changes, so this exercises the original selection guard.
    request.context.entries.front().entry_id =
        id<domain::ContextEntryId>("memory-entry-injected");
    request.context.entries.front().provenance.source_location =
        "memory:injected";
  }
  fixture.reject(std::move(request));
}

TEST_CASE("sealed continuation requires exact complete active tool messages",
          "[conversation][continuation][tools][failure]") {
  Fixture fixture;
  auto request = fixture.continuation();
  SECTION("changed tool result") {
    request.context.entries.back().message.content = {
        domain::TextBlock{"forged output"}};
  }
  SECTION("changed assistant call arguments") {
    request.context.entries[4].message.tool_calls.front().arguments.data =
        R"({"forged":true})";
  }
  SECTION("omitted assistant tool call") {
    request.context.entries.erase(request.context.entries.begin() + 4);
  }
  SECTION("omitted tool result") {
    request.context.entries.pop_back();
  }
  SECTION("tool result ordered before call") {
    std::swap(request.context.entries[4], request.context.entries[5]);
  }
  SECTION("incorrect active message estimate") {
    ++request.context.entries.back().estimated_tokens;
    ++request.context.estimated_input_tokens;
  }
  SECTION("active result order precedes original user") {
    request.context.entries.back().order = 1;
  }
  SECTION("duplicated active result") {
    auto duplicate = request.context.entries.back();
    duplicate.entry_id = id<domain::ContextEntryId>("duplicated-result");
    request.context.entries.push_back(std::move(duplicate));
  }
  fixture.reject(std::move(request));
}

TEST_CASE("later policy cannot replace a sealed active run base",
          "[conversation][continuation][policy]") {
  Fixture fixture;
  auto request = fixture.continuation();
  const auto snapshot = fixture.active_start->attributes.conversation_admission
                            ->source_snapshot_sequence;
  fixture.change_policy();
  REQUIRE(fixture.kernel.event_log().last_sequence() > snapshot);
  SECTION("old admission remains usable") {
    const auto expected = request;
    REQUIRE(fixture.kernel.continue_run(fixture.active_start->run_id,
                                        std::move(request)));
    drain_inference(fixture.kernel);
    REQUIRE(fixture.backend.requests.size() == 3);
    CHECK(fixture.backend.requests.back() == expected);
    CHECK_FALSE(fixture.kernel.active_run_id());
  }
  SECTION("removing original history still fails") {
    request.context.entries.erase(request.context.entries.begin() + 1,
                                  request.context.entries.begin() + 3);
    fixture.reject(std::move(request));
  }
}

TEST_CASE("sealed continuation dispatches exact buffered early tool errors",
          "[conversation][continuation][roundtrip]") {
  Fixture fixture;
  const auto request = fixture.continuation();
  const auto events_before = fixture.kernel.event_log().events().size();
  const auto& error_message = request.context.entries.back().message;
  CHECK(error_message.invocation_id == id<domain::InvocationId>("active-call"));
  REQUIRE(fixture.kernel.continue_run(fixture.active_start->run_id, request));
  drain_inference(fixture.kernel);
  REQUIRE(fixture.backend.requests.size() == 3);
  CHECK(fixture.backend.requests.back() == request);
  CHECK(fixture.kernel.event_log().events().size() > events_before);
  CHECK_FALSE(fixture.kernel.active_run_id());
}
