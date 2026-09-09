#include "../131summarykernel/fixture.hpp"
#include <aiforge/runtime/tool_launch_policy.hpp>
#include <aiforge/testing/application_launch_context.hpp>
#include <atomic>

namespace {
using namespace aiforge;
using summary_kernel_test::id;
using namespace std::chrono_literals;

struct ToolState {
  std::atomic<unsigned> starts{};
  std::atomic<bool> release{};
  std::atomic<bool> stopped{};
};
class ToolStream final : public runtime::ToolExecutionStream {
 public:
  explicit ToolStream(std::shared_ptr<ToolState> state)
      : m_state(std::move(state)) {}
  auto next(std::stop_token stop)
      -> std::expected<std::optional<runtime::ToolExecutionEvent>,
                       runtime::ToolExecutionError> override {
    if (m_step++ == 0)
      return runtime::ToolProgress{{domain::TextBlock{"working"}}};
    if (m_step > 2) return std::nullopt;
    while (!m_state->release && !stop.stop_requested())
      std::this_thread::sleep_for(1ms);
    if (stop.stop_requested()) {
      m_state->stopped = true;
      return std::nullopt;
    }
    return runtime::ToolResult{{domain::TextBlock{"finished"}}};
  }

 private:
  std::shared_ptr<ToolState> m_state;
  unsigned m_step{};
};
class Executor final : public runtime::ToolExecutor {
 public:
  std::shared_ptr<ToolState> state{std::make_shared<ToolState>()};
  auto validate(const domain::StructuredDataBlock& arguments) const
      -> std::expected<runtime::ValidatedToolArguments,
                       runtime::ToolExecutionError> override {
    return runtime::ValidatedToolArguments{arguments};
  }
  auto start(runtime::ToolInvocation, std::stop_token)
      -> std::expected<std::unique_ptr<runtime::ToolExecutionStream>,
                       runtime::ToolExecutionError> override {
    ++state->starts;
    return std::make_unique<ToolStream>(state);
  }
};
class BackendStream final : public backend::BackendStream {
 public:
  auto next(std::stop_token)
      -> std::expected<std::optional<backend::BackendEvent>,
                       backend::BackendError> override {
    switch (m_step++) {
      case 0: return backend::ResponseStarted{"response"};
      case 1:
        return backend::ToolCallDelta{id<domain::InvocationId>("first"), "read",
                                      "{}"};
      case 2:
        return backend::ToolCallDelta{id<domain::InvocationId>("second"),
                                      "read", "{}"};
      case 3: return backend::UsageObserved{{7, 3, 10}};
      case 4: return backend::ResponseFinished{domain::FinishReason::tool_call};
      default: return std::nullopt;
    }
  }

 private:
  unsigned m_step{};
};
class Backend final : public backend::Backend {
 public:
  std::atomic<unsigned> starts{};
  auto start(backend::BackendRequest, std::stop_token)
      -> std::expected<std::unique_ptr<backend::BackendStream>,
                       backend::BackendError> override {
    ++starts;
    return std::make_unique<BackendStream>();
  }
};
struct Fixture {
  summary_kernel_test::Store store;
  Backend backend;
  std::shared_ptr<Executor> executor{std::make_shared<Executor>()};
  runtime::ToolRegistrySnapshot tools;
  std::shared_ptr<runtime::ToolPolicy> policy;
  std::unique_ptr<runtime::RunKernel> kernel;
  domain::RunId run{id<domain::RunId>("run")};
  Fixture() {
    store.history.clear();
    runtime::ToolRegistry registry;
    REQUIRE(registry.register_tool(
        {"read",
         "Read fixture",
         {"application/schema+json", R"({"type":"object"})"},
         {domain::Effect::read},
         {{domain::Effect::read, "filesystem.root", "/fixture"}}},
        executor, {},
        runtime::ToolExecutorContract{"test.observe-drain", "1"}));
    tools = registry.snapshot().value();
    auto made = runtime::make_tool_launch_policy(
        tools,
        {id<domain::PermissionProfileId>("tools-none-implicit-v2"),
         testing::available_application_launch_context(
             runtime::RestrictionLevel::none, runtime::ApprovalMode::allow_all),
         {}});
    REQUIRE(made);
    policy = *made;
    reopen();
  }
  auto reopen() -> void {
    kernel.reset();
    auto opened = runtime::RunKernel::open_durable(
        {id<domain::SessionId>("session"),
         runtime::DurableSessionMode::resume,
         {}},
        store, backend, nullptr, {}, {}, tools, policy);
    INFO((opened ? "opened" : opened.error().message));
    REQUIRE(opened);
    kernel = std::move(*opened);
  }
  auto start() -> void {
    domain::ConstructedContext context{
        {{id<domain::ContextEntryId>("runtime"),
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
    runtime::RunStart request{
        run,
        {id<domain::SurfaceId>("test"),
         id<domain::WorkspaceId>("test"),
         id<domain::PermissionProfileId>("tools-none-implicit-v2"),
         {}},
        {id<domain::MessageId>("user"),
         domain::Role::user,
         {domain::TextBlock{"read both"}},
         {}},
        {id<domain::InferenceId>("inference"),
         id<domain::MessageId>("assistant"),
         id<domain::ModelId>("model"),
         std::move(context),
         tools.declarations(),
         {}}};
    request.provenance = domain::RunProvenance{
        "test", "fake", {}, request.request.model_id, {}, {}, {}, {}};
    auto result = kernel->start(std::move(request));
    INFO((result ? "started" : result.error().message));
    REQUIRE(result);
  }
  template <class T> auto count() const -> std::size_t {
    return static_cast<std::size_t>(std::ranges::count_if(
        kernel->event_log().events(), [](const auto& event) {
          return std::holds_alternative<T>(event.payload);
        }));
  }
  template <class Predicate> auto observe_until(Predicate done) -> void {
    for (unsigned i = 0; i < 2000 && !done(); ++i) {
      auto observed = kernel->drain(runtime::RunDrainMode::observe_only);
      INFO((observed ? "observed" : observed.error().message));
      REQUIRE(observed);
      std::this_thread::sleep_for(1ms);
    }
    REQUIRE(done());
  }
  auto queued() -> void {
    start();
    observe_until([&] { return kernel->pending_tool_dispatch(); });
    REQUIRE(count<domain::ToolPolicyDecided>() == 2);
    REQUIRE(count<domain::ToolStarted>() == 0);
    REQUIRE(executor->state->starts == 0);
  }
};
} // namespace

TEST_CASE(
    "observe-only accounts backend updates without dispatching queued tools",
    "[observedrain][failure]") {
  Fixture f;
  CHECK_FALSE(f.kernel->pending_tool_dispatch());
  f.queued();
  REQUIRE(f.count<domain::UsageRecorded>() == 1);
  const auto saved = f.kernel->event_log().events();
  for (unsigned i = 0; i < 5; ++i)
    REQUIRE(f.kernel->drain(runtime::RunDrainMode::observe_only));
  CHECK(f.kernel->event_log().events() == saved);
  CHECK(f.count<domain::ToolSpendReserved>() == 0);
  CHECK(f.backend.starts == 1);
  REQUIRE(f.kernel->cancel_run(f.run, "cancel queued"));
  CHECK_FALSE(f.kernel->pending_tool_dispatch());
  REQUIRE(f.kernel->drain());
  CHECK(f.executor->state->starts == 0);
}

TEST_CASE("observe-only preserves the recovered queued dispatch gate",
          "[observedrain][recovery]") {
  Fixture f;
  f.queued();
  f.reopen();
  REQUIRE(f.kernel->pending_tool_dispatch());
  const auto saved = f.kernel->event_log().events();
  REQUIRE(f.kernel->drain(runtime::RunDrainMode::observe_only));
  CHECK(f.kernel->event_log().events() == saved);
  CHECK(f.executor->state->starts == 0);
  REQUIRE(f.kernel->drain());
  CHECK_FALSE(f.kernel->pending_tool_dispatch());
  f.observe_until([&] { return f.count<domain::ToolProgressed>() == 1; });
  CHECK(f.executor->state->starts == 1);
  REQUIRE(f.kernel->cancel_run(f.run, "cancel running"));
  f.observe_until([&] { return f.executor->state->stopped.load(); });
  REQUIRE(f.kernel->drain());
  CHECK(f.count<domain::ToolStarted>() == 1);
  CHECK(f.backend.starts == 1);
}

TEST_CASE("one normal drain resumes deferred tools once while observed tool "
          "results remain visible",
          "[observedrain][success]") {
  Fixture f;
  f.queued();
  REQUIRE(f.kernel->drain());
  f.observe_until([&] { return f.count<domain::ToolProgressed>() == 1; });
  CHECK_FALSE(f.kernel->pending_tool_dispatch());
  REQUIRE(f.kernel->drain());
  CHECK(f.count<domain::ToolStarted>() == 1);
  f.executor->state->release = true;
  f.observe_until([&] { return f.kernel->pending_tool_dispatch(); });
  CHECK(f.count<domain::ToolResultRecorded>() == 1);
  CHECK(f.count<domain::ToolStarted>() == 1);
  CHECK(f.executor->state->starts == 1);
  REQUIRE(f.kernel->drain());
  f.observe_until([&] { return f.count<domain::ToolResultRecorded>() == 2; });
  REQUIRE(f.kernel->drain());
  CHECK_FALSE(f.kernel->pending_tool_dispatch());
  CHECK(f.count<domain::ToolStarted>() == 2);
  CHECK(f.executor->state->starts == 2);
  CHECK(f.backend.starts == 1);
  REQUIRE(f.kernel->cancel_run(f.run, "fixture complete"));
}

TEST_CASE("default drain retains automatic ordered tool dispatch",
          "[observedrain][compatibility]") {
  Fixture f;
  f.executor->state->release = true;
  f.start();
  for (unsigned i = 0; i < 2000 && f.count<domain::ToolResultRecorded>() != 2;
       ++i) {
    REQUIRE(f.kernel->drain());
    std::this_thread::sleep_for(1ms);
  }
  CHECK(f.count<domain::ToolResultRecorded>() == 2);
  CHECK(f.count<domain::ToolStarted>() == 2);
  CHECK(f.executor->state->starts == 2);
  CHECK(f.backend.starts == 1);
  CHECK_FALSE(f.kernel->pending_tool_dispatch());
  REQUIRE(f.kernel->cancel_run(f.run, "fixture complete"));
}
