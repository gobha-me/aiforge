#include "../181opsobservationkernel/fixture.hpp"

#include <aiforge/runtime/repository_context_controller.hpp>
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
class ChatBackend final : public backend::Backend {
 public:
  std::atomic<unsigned> calls{};
  bool unavailable{true};
  bool tool{};
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
    return std::make_unique<Stream>(request.assistant_message_id, tool);
  }
};
class UnavailableRepository final : public runtime::RepositoryContextSource {
 public:
  unsigned calls{};
  RepositoryRootIdentity root{id<RepositoryId>("repository"), "/fixture"};
  auto identity() const noexcept -> std::string_view override {
    return "chat-ops-repository";
  }
  auto guarantees_pinned_read_only_sources() const noexcept -> bool override {
    return true;
  }
  auto observe(repository::RepositorySnapshotLimits, std::stop_token)
      -> std::expected<RepositorySnapshot,
                       repository::RepositorySnapshotError> override {
    ++calls;
    return std::unexpected(repository::RepositorySnapshotError{
        repository::RepositorySnapshotErrorCode::not_found, "unavailable"});
  }
  auto discover(repository::ProjectInstructionRequest, std::stop_token)
      -> std::expected<ProjectInstructionDiscovery,
                       repository::ProjectInstructionError> override {
    ++calls;
    return std::unexpected(repository::ProjectInstructionError{});
  }
  auto read(repository::ExactSourceReadRequest, std::stop_token)
      -> std::expected<repository::ExactSourceReadResult,
                       repository::ExactSourceEditError> override {
    ++calls;
    return std::unexpected(repository::ExactSourceEditError{});
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

TEST_CASE("Chat manual Ops requires explicit application dependencies",
          "[chat][ops]") {
  ChatFixture f;
  SECTION("missing broker") {
    f.dependencies.observation_broker.reset();
  }
  SECTION("missing frozen context") {
    f.dependencies.observation_context.reset();
  }
  SECTION("missing permission identity") {
    f.dependencies.permission_profile_id.reset();
  }
  f.open();
  const auto lookups = f.models.calls;
  REQUIRE_FALSE(f.chat->bind_observation(
      OpsObservationAuthority::create(f.specification).value(), f.source,
      f.endpoint));
  REQUIRE_FALSE(f.chat->submit_observation(f.intent()));
  CHECK(f.store.history.empty());
  CHECK(f.source->calls.load() == 0);
  f.no_provider(lookups);
}

TEST_CASE("Chat manual Ops refuses foreign binding and preserves selection",
          "[chat][ops]") {
  ChatFixture f;
  f.open();
  f.bind();
  const auto original = f.chat->inspect_observations().selection;
  const auto lookups = f.models.calls;
  auto next = f.specification;
  ++next.selection_generation;
  auto source = std::make_shared<Source>();
  auto endpoint = f.endpoint;
  std::shared_ptr<runtime::OpsObservationBroker> foreign;
  SECTION("wrong session") {
    next.session_id = id<SessionId>("other");
  }
  SECTION("wrong owner") {
    next.owner_id = id<OpsOwnerId>("other");
  }
  SECTION("wrong source") {
    source->binding.target_id = id<OpsTargetId>("other");
  }
  SECTION("stale generation") {
    --next.selection_generation;
  }
  SECTION("foreign broker issuer") {
    foreign = runtime::OpsObservationBroker::create(f.worker).value();
    endpoint = foreign->activate_session(next.session_id).value();
  }
  REQUIRE_FALSE(f.chat->bind_observation(
      OpsObservationAuthority::create(next).value(), source, endpoint));
  CHECK(f.chat->inspect_observations().selection == original);
  CHECK(f.store.history.empty());
  CHECK(source->calls.load() == 0);
  CHECK(f.source->calls.load() == 0);
  f.no_provider(lookups);
  f.submit();
  f.idle();
  REQUIRE(f.chat->inspect_observations().projection.latest_success);
}

TEST_CASE("Chat manual prompt uses exact current approval without model work",
          "[chat][ops]") {
  ChatFixture f{runtime::ApprovalMode::prompt};
  f.open();
  f.bind();
  const auto lookups = f.models.calls;
  const auto submitted = f.submit();
  f.until([&] { return f.chat->pending_tool_approval().has_value(); });
  REQUIRE(f.chat->inspect_observations().approval);
  const auto before = f.store.history.size();
  const runtime::ToolApprovalResolution allow{
      ApprovalDecision::approved, {{Effect::read, "ops.target", "target"}}};
  REQUIRE_FALSE(f.chat->decide_observation_approval(
      id<RunId>("wrong"), submitted.invocation_id, allow));
  REQUIRE_FALSE(f.chat->decide_observation_approval(
      submitted.run_id, id<InvocationId>("wrong"), allow));
  REQUIRE_FALSE(f.chat->cancel_observation(id<RunId>("wrong")));
  CHECK(f.store.history.size() == before);
  CHECK(f.source->calls.load() == 0);
  SECTION("borrowed manual approval port") {
    REQUIRE(f.chat->decide_observation_approval(
        submitted.run_id, submitted.invocation_id, allow));
  }
  SECTION("ordinary TUI approval dialog routes the same manual operation") {
    REQUIRE(f.chat->decide_tool_approval(submitted.run_id,
                                         submitted.invocation_id, allow));
  }
  f.idle();
  CHECK(count<OpsObservationRecorded>(f.store.history) == 1);
  CHECK(count<RunCompleted>(f.store.history) == 1);
  REQUIRE_FALSE(f.chat->decide_observation_approval(
      submitted.run_id, submitted.invocation_id, allow));
  f.no_provider(lookups);
}

TEST_CASE("Chat manual denial and cancellation are durable terminal outcomes",
          "[chat][ops]") {
  ChatFixture f{runtime::ApprovalMode::prompt};
  f.open();
  f.bind();
  const auto lookups = f.models.calls;
  const auto submitted = f.submit();
  f.until([&] { return f.chat->pending_tool_approval().has_value(); });
  SECTION("denied") {
    REQUIRE(f.chat->decide_observation_approval(
        submitted.run_id, submitted.invocation_id,
        {ApprovalDecision::denied, {}}));
  }
  SECTION("cancelled") {
    REQUIRE(f.chat->cancel_observation(submitted.run_id));
  }
  f.idle();
  CHECK(f.source->calls.load() == 0);
  CHECK(count<OpsObservationRecorded>(f.store.history) == 0);
  CHECK(count<RunCompleted>(f.store.history) == 0);
  CHECK(count<RunFailed>(f.store.history) +
            count<RunCancelled>(f.store.history) ==
        1);
  REQUIRE(runtime::recorded_ops_observations(f.chat->event_log()));
  REQUIRE(f.chat->inspect_observations().projection.current);
  CHECK_FALSE(f.chat->inspect_observations().projection.latest_success);
  f.no_provider(lookups);
}

TEST_CASE("Chat manual binding and start refuse busy operations without IO",
          "[chat][ops]") {
  ChatFixture f{runtime::ApprovalMode::prompt};
  f.open();
  f.bind();
  const auto submitted = f.submit();
  f.until([&] { return f.chat->pending_tool_approval().has_value(); });
  const auto before = f.store.history.size();
  auto next = f.specification;
  ++next.selection_generation;
  REQUIRE_FALSE(f.chat->bind_observation(
      OpsObservationAuthority::create(next).value(), f.source, f.endpoint));
  REQUIRE_FALSE(f.chat->submit_observation(f.intent()));
  CHECK(f.store.history.size() == before);
  CHECK(f.source->calls.load() == 0);
  REQUIRE(f.chat->pending_tool_approval());
  CHECK(f.chat->pending_tool_approval()->run_id == submitted.run_id);
  REQUIRE(f.chat->cancel_observation(submitted.run_id));
  f.idle();
}

TEST_CASE("Chat manual source failure preserves earlier evidence and identity",
          "[chat][ops]") {
  ChatFixture f;
  f.open();
  f.bind();
  f.submit();
  f.idle();
  const auto previous =
      f.chat->inspect_observations().projection.latest_success;
  REQUIRE(previous);
  ++f.specification.selection_generation;
  f.specification.target.target_id = id<OpsTargetId>("second-target");
  f.specification.target.configuration_revision =
      id<OpsConfigurationRevision>("second-revision");
  f.source = std::make_shared<Source>();
  f.source->binding = f.specification.target;
  f.source->fail = true;
  f.bind();
  f.submit();
  f.idle();
  const auto& inspection = f.chat->inspect_observations();
  CHECK(inspection.projection.latest_success == previous);
  REQUIRE(inspection.selection);
  CHECK(inspection.selection->target_id == f.specification.target.target_id);
  CHECK(count<OpsObservationRecorded>(f.store.history) == 1);
  CHECK(count<RunFailed>(f.store.history) == 1);
  CHECK(f.backend.calls.load() == 0);
}

TEST_CASE("Chat manual persistence refusal never returns successful evidence",
          "[chat][ops]") {
  ChatFixture f;
  f.open();
  f.bind();
  f.store.reject = [](std::span<const RunEvent>) { return true; };
  REQUIRE_FALSE(f.chat->submit_observation(f.intent()));
  CHECK(f.store.history.empty());
  CHECK(f.source->calls.load() == 0);
  CHECK_FALSE(f.chat->inspect_observations().projection.latest_success);
  CHECK(f.backend.calls.load() == 0);
  REQUIRE(f.chat->inspect_observations().problem);
  CHECK(f.chat->inspect_observations().problem->code ==
        surfaces::ManualOpsErrorCode::storage_failure);
  REQUIRE(f.broker->close());
  REQUIRE_FALSE(f.chat->pump_observations());
  REQUIRE(f.chat->inspect_observations().problem);
  CHECK(f.chat->inspect_observations().problem->code ==
        surfaces::ManualOpsErrorCode::storage_failure);
}

TEST_CASE(
    "Chat manual approval persistence failure closes admission immediately",
    "[chat][ops]") {
  ChatFixture f{runtime::ApprovalMode::prompt};
  f.open();
  f.bind();
  const auto submitted = f.submit();
  REQUIRE(f.chat->inspect_observations().approval);
  runtime::ToolApprovalResolution decision{ApprovalDecision::denied, {}};
  SECTION("deny") {
  }
  SECTION("approve") {
    decision = {ApprovalDecision::approved,
                {{Effect::read, "ops.target", "target"}}};
  }
  f.store.reject = [](std::span<const RunEvent>) { return true; };
  REQUIRE_FALSE(f.chat->decide_observation_approval(
      submitted.run_id, submitted.invocation_id, decision));
  const auto& inspection = f.chat->inspect_observations();
  REQUIRE(inspection.problem);
  CHECK(inspection.problem->code ==
        surfaces::ManualOpsErrorCode::storage_failure);
  CHECK(inspection.closed);
  CHECK_FALSE(inspection.available);
  CHECK_FALSE(inspection.approval);
  CHECK_FALSE(inspection.projection.latest_success);
  CHECK(f.source->calls.load() == 0);
  REQUIRE_FALSE(f.chat->decide_observation_approval(
      submitted.run_id, submitted.invocation_id, decision));
}

TEST_CASE(
    "Chat manual binding does not retain usable state after broker closure",
    "[chat][ops]") {
  ChatFixture f;
  f.open();
  f.bind();
  REQUIRE(f.broker->close());
  ++f.specification.selection_generation;
  REQUIRE_FALSE(f.chat->bind_observation(
      OpsObservationAuthority::create(f.specification).value(), f.source,
      f.endpoint));
  CHECK(f.chat->inspect_observations().closed);
  CHECK_FALSE(f.chat->inspect_observations().available);
  CHECK(f.store.history.empty());
  CHECK(f.source->calls.load() == 0);
}

TEST_CASE(
    "Chat manual reads bypass unavailable repository but ordinary turns do not",
    "[chat][ops]") {
  UnavailableRepository source;
  runtime::RepositoryContextController controller{source, source.root};
  ChatFixture f{runtime::ApprovalMode::prompt};
  f.dependencies.repository_context_controller = &controller;
  f.dependencies.repository_context_selection =
      runtime::RepositoryContextRequest{"", 1, {}};
  f.open();
  f.bind();
  const auto lookups = f.models.calls;
  const auto repository_calls = source.calls;
  const auto submitted = f.submit();
  f.until([&] { return f.chat->pending_tool_approval().has_value(); });
  REQUIRE(f.chat->decide_tool_approval(
      submitted.run_id, submitted.invocation_id,
      {ApprovalDecision::approved, {{Effect::read, "ops.target", "target"}}}));
  f.idle();
  REQUIRE(f.chat->inspect_observations().projection.latest_success);
  CHECK(source.calls == repository_calls);
  f.no_provider(lookups);
  const auto before = f.store.history.size();
  REQUIRE_FALSE(f.chat->submit("inspect the repository"));
  CHECK(source.calls > repository_calls);
  CHECK(f.store.history.size() == before);
  CHECK(f.backend.calls.load() == 0);
}

TEST_CASE("Chat manual pending repository work preserves busy admission",
          "[chat][ops]") {
  UnavailableRepository source;
  runtime::RepositoryContextController controller{source, source.root};
  ChatFixture f;
  f.dependencies.repository_context_controller = &controller;
  f.dependencies.repository_context_selection =
      runtime::RepositoryContextRequest{"", 1, {}};
  f.dependencies.async_repository_preparation = true;
  f.open();
  f.bind();
  REQUIRE(f.chat->request_repository_submit("inspect the repository"));
  REQUIRE(f.chat->pending_repository_work());
  const auto token = f.chat->pending_repository_work()->token;
  const auto before = f.store.history.size();
  REQUIRE_FALSE(f.chat->submit_observation(f.intent()));
  ++f.specification.selection_generation;
  REQUIRE_FALSE(f.chat->bind_observation(
      OpsObservationAuthority::create(f.specification).value(), f.source,
      f.endpoint));
  REQUIRE(f.chat->pending_repository_work());
  CHECK(f.chat->pending_repository_work()->token == token);
  CHECK(f.store.history.size() == before);
  CHECK(f.source->calls.load() == 0);
  f.chat->cancel_repository_work();
}

TEST_CASE(
    "Chat manual use preserves model tool narrowing and emits events once",
    "[chat][ops]") {
  ChatFixture f;
  std::string profile{"off"};
  SECTION("off profile") {
  }
  SECTION("model cannot call tools") {
    f.models.supports_tools = false;
    profile = "admin";
  }
  SECTION("model maximum is off") {
    profile = "admin";
    f.dependencies.model_tool_profile_maximums.emplace(
        id<ModelId>("model"), id<ToolProfileId>("off"));
  }
  f.open();
  REQUIRE(f.chat->select_tool_profile(id<ToolProfileId>(profile)));
  const auto original = f.chat->tool_profile_state();
  REQUIRE(original);
  f.bind();
  const auto lookups = f.models.calls;
  const auto submitted = f.submit();
  // Only the manual port is pumped; no ordinary inference drain is needed.
  f.idle();
  REQUIRE(f.chat->inspect_observations().projection.latest_success);
  CHECK(f.chat->inspect_observations().projection.latest_success->submission ==
        submitted);
  const auto after = f.chat->tool_profile_state();
  REQUIRE(after);
  CHECK(after->selection == original->selection);
  CHECK(after->effective_tools.declarations().empty());
  const auto source_calls = f.source->calls.load();
  const auto sequence = f.store.history.back().metadata.sequence;
  for (unsigned i{}; i < 3; ++i) {
    CHECK(f.chat->inspect_observations().projection.last_sequence == sequence);
  }
  CHECK(f.source->calls.load() == source_calls);
  auto events = f.chat->drain();
  REQUIRE(events);
  CHECK(events->size() == f.store.history.size());
  std::set<std::uint64_t> delivered;
  for (const auto& event : *events)
    CHECK(delivered.insert(event.metadata.sequence).second);
  CHECK(count<OpsObservationRecorded>(*events) == 1);
  CHECK(count<RunCompleted>(*events) == 1);
  REQUIRE(f.chat->pump_observations());
  auto again = f.chat->drain();
  REQUIRE(again);
  CHECK(again->empty());
  CHECK(f.source->calls.load() == source_calls);
  f.no_provider(lookups);
}

TEST_CASE("Chat manual port cannot approve or advance an ordinary model call",
          "[chat][ops]") {
  ChatFixture f{runtime::ApprovalMode::prompt};
  f.open();
  f.bind();
  REQUIRE(f.chat->select_tool_profile(id<ToolProfileId>("admin")));
  f.backend.unavailable = false;
  f.backend.tool = true;
  auto submitted = f.chat->submit("Inspect the configured target");
  REQUIRE(submitted);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{5};
  while (!f.chat->pending_tool_approval() &&
         std::chrono::steady_clock::now() < deadline) {
    auto events = f.chat->drain();
    REQUIRE(events);
    std::this_thread::yield();
  }
  const auto approval = f.chat->pending_tool_approval();
  REQUIRE(approval);
  const auto before = f.store.history.size();
  REQUIRE_FALSE(f.chat->decide_observation_approval(
      approval->run_id, approval->invocation_id,
      {ApprovalDecision::approved, {{Effect::read, "ops.target", "target"}}}));
  REQUIRE_FALSE(f.chat->cancel_observation(approval->run_id));
  REQUIRE_FALSE(f.chat->submit_observation(f.intent()));
  REQUIRE(f.chat->pump_observations());
  CHECK(f.store.history.size() == before);
  CHECK(f.source->calls.load() == 0);
  REQUIRE(f.chat->pending_tool_approval());
  CHECK(f.chat->pending_tool_approval()->invocation_id ==
        approval->invocation_id);
  REQUIRE(f.chat->cancel_active());
}
