#include "../159foldergrant/fixture.hpp"
#include "../181opsobservationkernel/fixture.hpp"
#include <aiforge/runtime/tool_launch_policy.hpp>
#include <aiforge/surfaces/chat_session.hpp>
#include <set>

namespace {
using namespace observation_kernel_test;
class Models final : public backend::ModelContextProvider {
 public:
  unsigned calls{};
  bool unavailable{};
  bool supports_tools{true};
  auto lookup(const ModelId& model, std::stop_token)
      -> std::expected<backend::ModelContextInfo,
                       backend::BackendError> override {
    ++calls;
    if (unavailable)
      return std::unexpected(
          backend::BackendError{backend::BackendErrorKind::unavailable,
                                "model fixture unavailable",
                                false,
                                {}});
    return backend::ModelContextInfo{
        model, 32768, 256, {}, {{"tools", supports_tools}}};
  }
};
struct StreamState {
  std::shared_ptr<folder_grant_test::Gate> gate{
      std::make_shared<folder_grant_test::Gate>()};
  std::atomic<unsigned> calls{};
};
class HeldStream final : public backend::BackendStream {
 public:
  explicit HeldStream(std::shared_ptr<StreamState> state)
      : m_state(std::move(state)) {}
  auto next(std::stop_token)
      -> std::expected<std::optional<backend::BackendEvent>,
                       backend::BackendError> override {
    const auto call = ++m_state->calls;
    if (call == 1)
      return backend::BackendEvent{backend::ResponseStarted{"fixture"}};
    if (call == 2) {
      m_state->gate->wait();
      return backend::BackendEvent{
          backend::ResponseFinished{FinishReason::stop}};
    }
    return std::nullopt;
  }

 private:
  std::shared_ptr<StreamState> m_state;
};
class ChatBackend final : public backend::Backend {
 public:
  std::atomic<unsigned> calls{};
  bool unavailable{true};
  bool tool{};
  std::shared_ptr<StreamState> held;
  auto start(backend::BackendRequest request, std::stop_token)
      -> std::expected<std::unique_ptr<backend::BackendStream>,
                       backend::BackendError> override {
    ++calls;
    if (unavailable)
      return std::unexpected(
          backend::BackendError{backend::BackendErrorKind::unavailable,
                                "backend fixture unavailable",
                                false,
                                {}});
    if (held) return std::make_unique<HeldStream>(held);
    return std::make_unique<Stream>(request.assistant_message_id, tool);
  }
};
struct ChatFixture {
  OpsObservationAuthoritySpec specification{spec()};
  std::shared_ptr<runtime::LocalSourceWorker> worker{
      runtime::LocalSourceWorker::create(1).value()};
  std::shared_ptr<runtime::OpsObservationBroker> broker{
      runtime::OpsObservationBroker::create(worker).value()};
  std::shared_ptr<runtime::OpsObservationEndpoint> endpoint{
      broker->activate_session(specification.session_id).value()};
  std::shared_ptr<Source> source{std::make_shared<Source>()};
  Store store;
  Models models;
  ChatBackend backend;
  surfaces::ChatSessionDependencies dependencies;
  std::uint64_t identity{1000};
  std::unique_ptr<surfaces::ChatSession> chat;
  explicit ChatFixture(
      runtime::ApprovalMode mode = runtime::ApprovalMode::allow_all) {
    runtime::ApplicationLaunchContextConfiguration configuration;
    configuration.approval_mode = mode;
    auto context = runtime::make_application_launch_context(configuration);
    REQUIRE(context);
    runtime::ToolRegistry registry;
    dependencies.tools = registry.snapshot().value();
    auto policy = runtime::make_tool_launch_policy(
        dependencies.tools, {id<PermissionProfileId>("observe"), *context, {}});
    REQUIRE(policy);
    dependencies.tool_policy = *policy;
    dependencies.permission_profile_id = id<PermissionProfileId>("observe");
    dependencies.observation_broker = broker;
    dependencies.observation_context = surfaces::ChatObservationContext{
        id<SurfaceId>("chat"), id<WorkspaceId>("ops")};
    dependencies.identity_suffix_source = [this] { return ++identity; };
  }
  ~ChatFixture() {
    chat.reset();
    static_cast<void>(broker->close());
  }
  auto open() -> void {
    models.unavailable = false;
    auto opened = surfaces::ChatSession::open(
        {id<ModelId>("model"), surfaces::ChatSessionOpen::Mode::resume,
         specification.session_id,
         RunProvenance{
             "test", "fake", {}, id<ModelId>("model"), {}, {}, {}, {}}},
        backend, models, &store, nullptr, {}, {1024U * 1024U, 256},
        dependencies);
    INFO((opened ? "opened" : opened.error().message));
    REQUIRE(opened);
    chat = std::move(*opened);
    models.unavailable = true;
  }
  auto bind() -> void {
    REQUIRE(chat->bind_observation(
        OpsObservationAuthority::create(specification).value(), source,
        endpoint));
  }
  auto intent() const -> runtime::OpsObservationIntent {
    return {specification.target.target_id,
            specification.selection_generation,
            OpsObservationOperation::linux_health,
            {},
            specification.limits};
  }
  auto submit() -> surfaces::ObservationSubmission {
    auto submitted = chat->submit_observation(intent());
    REQUIRE(submitted);
    return *submitted;
  }
  auto until(const std::function<bool()>& finished) -> void {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!finished() && std::chrono::steady_clock::now() < deadline) {
      REQUIRE(chat->pump_observations());
      std::this_thread::yield();
    }
    REQUIRE(finished());
  }
  auto idle() -> void {
    until([&] { return !chat->active(); });
  }
  auto no_provider(unsigned lookups) const -> void {
    CHECK(models.calls == lookups);
    CHECK(backend.calls.load() == 0);
  }
};
} // namespace

TEST_CASE("Buffered manual events transfer once without an inference drain",
          "[chat][ops][ui-state]") {
  ChatFixture f;
  f.open();
  f.bind();
  const auto lookups = f.models.calls;
  CHECK(f.chat->take_buffered_surface_events().empty());
  f.submit();
  f.idle();
  const auto before = f.store.history.size();
  const auto calls = f.source->calls.load();
  auto events = f.chat->take_buffered_surface_events();
  REQUIRE(events.size() == before);
  CHECK(count<OpsObservationRecorded>(events) == 1);
  CHECK(count<RunCompleted>(events) == 1);
  for (std::size_t i{}; i < events.size(); ++i)
    CHECK(events[i].metadata.event_id == f.store.history[i].metadata.event_id);
  CHECK(f.chat->take_buffered_surface_events().empty());
  CHECK(f.store.history.size() == before);
  CHECK(f.source->calls.load() == calls);
  f.no_provider(lookups);
  REQUIRE(f.chat->pump_observations());
  CHECK(f.chat->take_buffered_surface_events().empty());
  auto ordinary = f.chat->drain();
  REQUIRE(ordinary);
  CHECK(ordinary->empty());
}

TEST_CASE("Buffered events survive manual pump failure without clearing it",
          "[chat][ops][ui-state]") {
  ChatFixture f{runtime::ApprovalMode::prompt};
  f.open();
  f.bind();
  const auto lookups = f.models.calls;
  f.submit();
  REQUIRE(f.chat->inspect_observations().approval);
  REQUIRE(f.broker->close());
  REQUIRE_FALSE(f.chat->pump_observations());
  const auto before = f.store.history.size();
  REQUIRE(before > 0);
  const auto inspection = f.chat->inspect_observations();
  REQUIRE(inspection.problem);
  auto events = f.chat->take_buffered_surface_events();
  REQUIRE(events.size() == before);
  CHECK(count<HumanObservationRequested>(events) == 1);
  CHECK(count<RunCancelled>(events) == 1);
  CHECK(f.chat->take_buffered_surface_events().empty());
  REQUIRE(f.chat->inspect_observations().problem);
  CHECK(f.chat->inspect_observations().problem->code ==
        inspection.problem->code);
  CHECK(f.chat->inspect_observations().closed == inspection.closed);
  CHECK(f.store.history.size() == before);
  CHECK(f.source->calls.load() == 0);
  f.no_provider(lookups);
}

TEST_CASE(
    "Taking buffered events preserves a manual approval without executing it",
    "[chat][ops][ui-state]") {
  ChatFixture f{runtime::ApprovalMode::prompt};
  f.open();
  f.bind();
  const auto lookups = f.models.calls;
  f.submit();
  const auto before = f.store.history.size();
  const auto approval = f.chat->inspect_observations().approval;
  REQUIRE(approval);
  auto events = f.chat->take_buffered_surface_events();
  CHECK(events.size() == before);
  REQUIRE(f.chat->inspect_observations().approval);
  CHECK(f.chat->inspect_observations().approval->invocation_id ==
        approval->invocation_id);
  CHECK(f.chat->take_buffered_surface_events().empty());
  CHECK(f.store.history.size() == before);
  CHECK(f.source->calls.load() == 0);
  f.no_provider(lookups);
}

TEST_CASE("Manual and ordinary cancellation buffers preserve exact event order",
          "[chat][ops][ui-state]") {
  ChatFixture f;
  f.open();
  f.bind();
  f.submit();
  f.idle();
  const auto manual_count = f.store.history.size();
  f.models.unavailable = false;
  f.backend.unavailable = false;
  auto ordinary =
      f.chat->submit("An ordinary response after manual observation");
  REQUIRE(ordinary);
  REQUIRE(f.chat->cancel_active());
  const auto before = f.store.history.size();
  const auto source_calls = f.source->calls.load();
  const auto backend_calls = f.backend.calls.load();
  const auto model_calls = f.models.calls;
  auto events = f.chat->take_buffered_surface_events();
  REQUIRE(events.size() >= manual_count);
  CHECK(count<OpsObservationRecorded>(events) == 1);
  CHECK(count<RunCompleted>(events) == 1);
  CHECK(count<RunCancelled>(events) == 1);
  std::set<std::uint64_t> sequences;
  std::uint64_t last{};
  for (const auto& event : events) {
    CHECK(sequences.insert(event.metadata.sequence).second);
    CHECK(event.metadata.sequence > last);
    last = event.metadata.sequence;
    CHECK(event.metadata.event_id ==
          f.store.history[event.metadata.sequence - 1].metadata.event_id);
  }
  CHECK(f.chat->take_buffered_surface_events().empty());
  CHECK(f.store.history.size() == before);
  CHECK(f.source->calls.load() == source_calls);
  CHECK(f.backend.calls.load() == backend_calls);
  CHECK(f.models.calls == model_calls);
}

TEST_CASE("Taking a manual buffer while a model runs never drains queued "
          "provider events",
          "[chat][ops][ui-state]") {
  ChatFixture f;
  f.open();
  f.bind();
  f.submit();
  f.idle();
  const auto manual_count = f.store.history.size();
  f.models.unavailable = false;
  f.backend.unavailable = false;
  f.backend.held = std::make_shared<StreamState>();
  const folder_grant_test::Release release{f.backend.held->gate};
  REQUIRE(f.chat->submit("An ordinary run with buffered provider events"));
  REQUIRE(f.backend.held->gate->await());
  REQUIRE(f.chat->active());
  const auto before = f.store.history.size();
  const auto source_calls = f.source->calls.load();
  const auto model_calls = f.models.calls;
  const auto backend_calls = f.backend.calls.load();
  // The producer has queued ResponseStarted and is physically held in next().
  // A model drain would consume and persist it even though this run is active.
  REQUIRE(f.backend.held->calls == 2);
  auto events = f.chat->take_buffered_surface_events();
  CHECK(events.size() == manual_count);
  CHECK(count<OpsObservationRecorded>(events) == 1);
  CHECK(f.chat->take_buffered_surface_events().empty());
  CHECK(f.store.history.size() == before);
  CHECK(f.backend.held->calls == 2);
  CHECK(f.source->calls.load() == source_calls);
  CHECK(f.models.calls == model_calls);
  CHECK(f.backend.calls.load() == backend_calls);
  CHECK(f.chat->active());
  f.backend.held->gate->release();
  REQUIRE(f.chat->cancel_active());
}
