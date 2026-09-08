#include "../131summarykernel/fixture.hpp"
#include "../135summarycontext/fixture.hpp"
#include <aiforge/runtime/context_builder.hpp>
#include <aiforge/runtime/conversation_policy.hpp>
#include <aiforge/runtime/tool_launch_policy.hpp>
#include <aiforge/runtime/tool_registry.hpp>
#include <aiforge/testing/application_launch_context.hpp>
#include <atomic>

namespace {
using namespace summary_context_test;
class ToolStream final : public runtime::ToolExecutionStream {
 public:
  auto next(std::stop_token)
      -> std::expected<std::optional<runtime::ToolExecutionEvent>,
                       runtime::ToolExecutionError> override {
    if (finished) return std::nullopt;
    finished = true;
    return runtime::ToolExecutionEvent{
        runtime::ToolResult{{domain::TextBlock{"fake read complete"}}}};
  }
  bool finished{};
};
class Executor final : public runtime::ToolExecutor {
 public:
  std::atomic<unsigned> starts{};
  auto validate(const domain::StructuredDataBlock& arguments) const
      -> std::expected<runtime::ValidatedToolArguments,
                       runtime::ToolExecutionError> override {
    return runtime::ValidatedToolArguments{arguments};
  }
  auto start(runtime::ToolInvocation, std::stop_token)
      -> std::expected<std::unique_ptr<runtime::ToolExecutionStream>,
                       runtime::ToolExecutionError> override {
    ++starts;
    return std::make_unique<ToolStream>();
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
class Store final : public storage::SessionStore {
 public:
  std::vector<domain::RunEvent> history;
  std::vector<std::vector<domain::RunEvent>> batches;
  bool fail_append{};
  bool fail_tool_started{};
  std::size_t append_attempts{};
  std::mutex mutex;
  auto create_session(storage::SessionCreate, std::stop_token)
      -> std::expected<void, storage::SessionStoreError> override {
    return {};
  }
  auto open_session(const domain::SessionId& session, std::stop_token)
      -> std::expected<storage::SessionInfo,
                       storage::SessionStoreError> override {
    std::lock_guard lock{mutex};
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
  auto append_events(const domain::SessionId&,
                     std::span<const domain::RunEvent> events, std::stop_token)
      -> std::expected<void, storage::SessionStoreError> override {
    std::lock_guard lock{mutex};
    ++append_attempts;
    if (fail_tool_started && std::ranges::any_of(events, [](const auto& event) {
          return std::holds_alternative<domain::ToolStarted>(event.payload);
        })) {
      fail_tool_started = false;
      return std::unexpected(
          storage::SessionStoreError{storage::SessionStoreErrorCode::io_failure,
                                     "checkpoint before tool start", false});
    }
    if (fail_append)
      return std::unexpected(storage::SessionStoreError{
          storage::SessionStoreErrorCode::io_failure, "append refused", false});
    batches.emplace_back(events.begin(), events.end());
    history.insert(history.end(), events.begin(), events.end());
    return {};
  }
  auto replay_events(const domain::SessionId&, std::stop_token)
      -> std::expected<std::vector<domain::RunEvent>,
                       storage::SessionStoreError> override {
    std::lock_guard lock{mutex};
    return history;
  }
};
struct KernelFixture {
  Fixture source;
  Store store;
  std::shared_ptr<Executor> executor{std::make_shared<Executor>()};
  domain::PermissionProfileId profile{
      id<domain::PermissionProfileId>("tools-none-implicit-v2")};
  Backend backend;
  runtime::ToolRegistrySnapshot tools;
  std::unique_ptr<runtime::RunKernel> kernel;
  std::optional<domain::ConversationSummaryActivation> active;
  std::optional<runtime::RunStart> started;
  KernelFixture() {
    runtime::ToolRegistry registry;
    REQUIRE(registry.register_tool(
        {"unavailable-tool",
         "Allowed recovery fixture",
         {"application/schema+json", R"({"type":"object"})"},
         {domain::Effect::read},
         {{domain::Effect::read, "fixture", "value"}}},
        executor, {},
        runtime::ToolExecutorContract{"test.summary-recovery", "1"}));
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
    auto policy = runtime::make_tool_launch_policy(
        tools,
        {profile,
         testing::available_application_launch_context(
             runtime::RestrictionLevel::none, runtime::ApprovalMode::allow_all),
         {}});
    REQUIRE(policy);
    auto result = runtime::RunKernel::open_durable(
        {source.log.session_id(), runtime::DurableSessionMode::resume, {}},
        store, backend, nullptr, {}, {}, tools, *policy);
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
    attributes.permission_profile_id = profile;
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
  auto checkpoint() -> void {
    started = request();
    started->provenance = domain::RunProvenance{
        "test", "fake", {}, id<domain::ModelId>("model"), {}, {}, {}, {}};
    store.fail_tool_started = true;
    const auto launched = kernel->start(*started);
    INFO((launched ? "started" : launched.error().message));
    REQUIRE(launched);
    std::optional<runtime::RunKernelError> interrupted;
    for (unsigned i = 0; i < 1000 && !interrupted; ++i) {
      auto result = kernel->drain();
      if (!result)
        interrupted = result.error();
      else
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    REQUIRE(interrupted);
    INFO(interrupted->message);
    REQUIRE(interrupted->code == runtime::RunKernelErrorCode::storage_failure);
    CHECK(executor->starts == 0);
    REQUIRE(count<domain::ToolPolicyDecided>() == 1);
    REQUIRE(count<domain::ToolStarted>() == 0);
    REQUIRE(count<domain::ToolSpendReserved>() == 0);
    reopen();
    REQUIRE(kernel->active_run_id() == started->run_id);
    REQUIRE_FALSE(kernel->pending_tool_approval());
    REQUIRE(count<domain::ToolStarted>() == 0);
  }
  template <typename T> auto count() const -> std::size_t {
    return static_cast<std::size_t>(
        std::ranges::count_if(store.history, [](const auto& event) {
          return std::holds_alternative<T>(event.payload);
        }));
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
};
} // namespace

TEST_CASE(
    "unresolved recovered summary blocks a queued allowed tool before launch",
    "[summarytoolrecovery][failure]") {
  KernelFixture f;
  f.checkpoint();
  f.disable();
  const auto before = f.store.history;
  const auto result = f.kernel->drain();
  REQUIRE_FALSE(result);
  CHECK(result.error().code ==
        runtime::RunKernelErrorCode::continuation_not_ready);
  CHECK(f.store.history == before);
  CHECK(f.kernel->event_log().events() == before);
  CHECK(f.executor->starts == 0);
  CHECK(f.count<domain::ToolStarted>() == 0);
  CHECK(f.count<domain::ToolSpendReserved>() == 0);
  CHECK(f.backend.count() == 1);
  CHECK_FALSE(f.kernel->pin_conversation_summaries(f.started->run_id));
  REQUIRE(
      f.kernel->cancel_run(f.started->run_id, "cancel unresolved recovery"));
  CHECK(f.count<domain::RunCancelled>() == 1);
  CHECK(f.executor->starts == 0);
}

TEST_CASE(
    "explicitly pinned recovered summary allows its queued tool after disable",
    "[summarytoolrecovery][success]") {
  KernelFixture f;
  f.checkpoint();
  REQUIRE(f.kernel->pin_conversation_summaries(f.started->run_id));
  f.disable();
  const auto launched = f.kernel->drain();
  INFO((launched ? "launched" : launched.error().message));
  REQUIRE(launched);
  REQUIRE(f.count<domain::ToolStarted>() == 1);
  for (unsigned i = 0; i < 1000 && f.count<domain::ToolResultRecorded>() == 0;
       ++i) {
    REQUIRE(f.kernel->drain());
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  CHECK(f.executor->starts == 1);
  CHECK(f.count<domain::ToolResultRecorded>() == 1);
  CHECK(f.count<domain::ToolSpendReserved>() == 0);
  CHECK(f.backend.count() == 1);
  REQUIRE(f.kernel->cancel_run(f.started->run_id, "fixture complete"));
}

TEST_CASE("pinned recovered queued tools can be cancelled before launch",
          "[summarytoolrecovery][cancellation]") {
  KernelFixture f;
  f.checkpoint();
  REQUIRE(f.kernel->pin_conversation_summaries(f.started->run_id));
  f.disable();
  REQUIRE(f.kernel->cancel_run(f.started->run_id, "cancel before launch"));
  REQUIRE(f.kernel->drain());
  CHECK(f.executor->starts == 0);
  CHECK(f.count<domain::ToolStarted>() == 0);
  CHECK(f.count<domain::ToolSpendReserved>() == 0);
  CHECK(f.count<domain::RunCancelled>() == 1);
  CHECK(f.backend.count() == 1);
}
