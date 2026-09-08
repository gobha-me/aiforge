#include "../131summarykernel/fixture.hpp"
#include "../135summarycontext/fixture.hpp"
#include <aiforge/runtime/context_builder.hpp>
#include <aiforge/runtime/conversation_policy.hpp>
#include <aiforge/runtime/tool_registry.hpp>

namespace {
using namespace summary_context_test;
class Executor final : public runtime::ToolExecutor {
 public:
  auto validate(const domain::StructuredDataBlock&) const
      -> std::expected<runtime::ValidatedToolArguments,
                       runtime::ToolExecutionError> override {
    return std::unexpected(runtime::ToolExecutionError{
        runtime::ToolExecutionErrorCode::invalid_arguments, "fixture rejection",
        false});
  }
  auto start(runtime::ToolInvocation, std::stop_token)
      -> std::expected<std::unique_ptr<runtime::ToolExecutionStream>,
                       runtime::ToolExecutionError> override {
    return std::unexpected(runtime::ToolExecutionError{
        runtime::ToolExecutionErrorCode::internal_failure, "must not execute",
        false});
  }
};
class Backend final : public backend::Backend {
 public:
  auto start(backend::BackendRequest request, std::stop_token)
      -> std::expected<std::unique_ptr<backend::BackendStream>,
                       backend::BackendError> override {
    const auto message = request.assistant_message_id;
    const bool tools = message == id<domain::MessageId>("active-assistant");
    std::lock_guard lock{mutex};
    requests.push_back(std::move(request));
    return std::make_unique<summary_kernel_test::Stream>(message, tools);
  }
  auto count() -> std::size_t {
    std::lock_guard lock{mutex};
    return requests.size();
  }
  std::mutex mutex;
  std::vector<backend::BackendRequest> requests;
};
struct KernelFixture {
  Fixture source;
  summary_kernel_test::Store store;
  Backend backend;
  runtime::ToolRegistrySnapshot tools;
  std::unique_ptr<runtime::RunKernel> kernel;
  std::optional<domain::ConversationSummaryActivation> active;
  std::optional<runtime::RunStart> started;
  KernelFixture() {
    runtime::ToolRegistry registry;
    REQUIRE(registry.register_tool(
        {"unavailable-tool",
         "Rejected fixture",
         {"application/schema+json", R"({"type":"object"})"},
         {domain::Effect::read},
         {{domain::Effect::read, "fixture", "value"}}},
        std::make_shared<Executor>()));
    auto snapshot = registry.snapshot();
    REQUIRE(snapshot);
    tools = *snapshot;
    source.source("original", "Preserve exact original facts.");
    const auto summary =
        source.summary("reviewed", {id<domain::RunId>("original")});
    active = source.activate(summary);
    source.policy(domain::ConversationMode::rolling);
    store.history = source.log.events();
    reopen();
  }
  auto reopen() -> void {
    kernel.reset();
    auto result = runtime::RunKernel::open_durable(
        {source.log.session_id(), runtime::DurableSessionMode::resume, {}},
        store, backend, nullptr, {}, {}, tools);
    INFO((result ? "opened" : result.error().message));
    REQUIRE(result);
    kernel = std::move(*result);
  }
  auto request() -> runtime::RunStart {
    auto input = mandatory();
    input.capacity.reserved_input_tokens = 1000;
    auto context =
        runtime::prepare_session_context({kernel->event_log(),
                                          id<domain::ModelId>("model"),
                                          input,
                                          nullptr,
                                          {},
                                          {},
                                          {}});
    REQUIRE(context);
    REQUIRE(context->conversation_admission.summaries.size() == 1);
    auto built = runtime::ContextBuilder{}.build(context->input);
    REQUIRE(built);
    auto attributes =
        summary_kernel_test::attributes(domain::RunPurpose::conversation);
    attributes.conversation_admission = context->conversation_admission;
    return {id<domain::RunId>("active-run"),
            std::move(attributes),
            input.content.front().message,
            {id<domain::InferenceId>("active-inference"),
             id<domain::MessageId>("active-assistant"),
             id<domain::ModelId>("model"),
             std::move(*built),
             tools.declarations(),
             {}}};
  }
  auto drain() -> void {
    for (unsigned i = 0; i < 1000 && kernel->active_inference_id(); ++i) {
      auto result = kernel->drain();
      INFO((result ? "drained" : result.error().message));
      REQUIRE(result);
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    REQUIRE_FALSE(kernel->active_inference_id());
  }
  auto pending() -> void {
    started = request();
    auto result = kernel->start(*started);
    INFO((result ? "started" : result.error().message));
    REQUIRE(result);
    drain();
    REQUIRE(kernel->active_run_id() == started->run_id);
  }
  auto disable() -> void {
    auto policy = runtime::recorded_conversation_policy(kernel->event_log());
    REQUIRE(policy);
    REQUIRE(kernel->disable_conversation_summary(
        {id<domain::RunId>("disable"),
         summary_kernel_test::attributes(domain::RunPurpose::control),
         kernel->event_log().last_sequence(), policy->policy.revision,
         active->candidate, active->activation_event_id}));
  }
  auto continuation() -> backend::BackendRequest {
    auto request = started->request;
    request.inference_id = id<domain::InferenceId>("next-inference");
    request.assistant_message_id = id<domain::MessageId>("next-assistant");
    auto messages = runtime::reconstruct_active_tool_continuation(
        kernel->event_log(), started->run_id);
    REQUIRE(messages);
    REQUIRE(messages->size() == 2);
    auto order = request.context.entries.back().order;
    for (const auto& message : *messages) {
      const auto estimate = runtime::estimate_conversation_message(message);
      REQUIRE(estimate);
      ++order;
      request.context.entries.push_back(
          {id<domain::ContextEntryId>("tool-entry-" + std::to_string(order)),
           message.role == domain::Role::tool
               ? domain::ContextEntryKind::tool_result
               : domain::ContextEntryKind::conversation,
           {},
           message,
           {id<domain::ContextSourceId>("tool-source"), {}, {}},
           0,
           order,
           *estimate});
      request.context.estimated_input_tokens += *estimate;
    }
    return request;
  }
};
} // namespace
TEST_CASE(
    "kernel rejects changed or omitted exact summary evidence before dispatch",
    "[summaryadmission][failure]") {
  KernelFixture f;
  auto request = f.request();
  const auto found = std::ranges::find_if(
      request.request.context.entries, [](const auto& entry) {
        return entry.message.role == domain::Role::evidence;
      });
  REQUIRE(found != request.request.context.entries.end());
  SECTION("changed text") {
    found->message.content = {domain::TextBlock{"forged"}};
  }
  SECTION("changed provenance") {
    found->provenance.source_location = "forged";
  }
  SECTION("changed order") {
    ++found->order;
  }
  SECTION("missing evidence") {
    request.request.context.entries.erase(found);
  }
  SECTION("re-sealed foreign activation") {
    auto& admission = *request.attributes.conversation_admission;
    admission.summaries.front().activation_event_id =
        id<domain::EventId>("foreign");
    REQUIRE(domain::seal_conversation_admission(admission));
  }
  const auto before = f.store.history;
  CHECK_FALSE(f.kernel->start(std::move(request)));
  CHECK(f.store.history == before);
  CHECK(f.backend.count() == 0);
}
TEST_CASE("resolved live summary context survives disable while unresolved "
          "restart fails",
          "[summaryadmission][recovery]") {
  KernelFixture f;
  f.pending();
  bool resolved = true;
  SECTION("live run remains pinned") {
  }
  SECTION("explicit recovery resolution remains pinned") {
    f.reopen();
    REQUIRE(f.kernel->pin_conversation_summaries(f.started->run_id));
  }
  SECTION("unresolved restart cannot use disabled evidence") {
    f.reopen();
    resolved = false;
  }
  f.disable();
  const auto before = f.store.history;
  const auto result =
      f.kernel->continue_run(f.started->run_id, f.continuation());
  if (resolved) {
    INFO((result ? "continued" : result.error().message));
    REQUIRE(result);
    f.drain();
    CHECK(f.backend.count() == 2);
  } else {
    CHECK_FALSE(result);
    CHECK_FALSE(f.kernel->pin_conversation_summaries(f.started->run_id));
    CHECK(f.store.history == before);
    CHECK(f.backend.count() == 1);
  }
}
TEST_CASE("summary pinning requires the exact active run",
          "[summaryadmission][failure]") {
  KernelFixture f;
  CHECK_FALSE(
      f.kernel->pin_conversation_summaries(id<domain::RunId>("active-run")));
  f.pending();
  const auto before = f.store.history;
  CHECK_FALSE(
      f.kernel->pin_conversation_summaries(id<domain::RunId>("foreign")));
  CHECK(f.store.history == before);
  CHECK(f.backend.count() == 1);
}
