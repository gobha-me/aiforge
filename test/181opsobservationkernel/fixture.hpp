#pragma once
#include <aiforge/runtime/local_source_worker.hpp>
#include <aiforge/runtime/ops_observation_history.hpp>
#include <aiforge/runtime/run_kernel.hpp>
#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace observation_kernel_test {
using namespace aiforge;
using namespace aiforge::domain;
template <class T> auto id(std::string value) -> T {
  return T::from(std::move(value)).value();
}
auto spec() -> OpsObservationAuthoritySpec {
  return {id<OpsOwnerId>("owner"),
          id<SessionId>("session"),
          {id<OpsTargetId>("target"), id<OpsConfigurationRevision>("revision"),
           LinuxOpsIdentity{LinuxExecutionScope::container,
                            "12345678-1234-1234-1234-123456789abc", 42, 43}},
          1,
          {OpsObservationOperation::linux_health},
          {},
          {}};
}
class Source final : public runtime::OpsObservationSource {
 public:
  OpsTargetBinding binding{spec().target};
  std::atomic<unsigned> calls{};
  bool fail{};
  bool malformed{};
  bool blocked{};
  auto guarantees_bound_read_only_observations() const noexcept
      -> bool override {
    return true;
  }
  auto target_binding() const noexcept -> const OpsTargetBinding& override {
    return binding;
  }
  auto observe(const OpsObservationRequest& request, std::stop_token stop)
      -> std::expected<OpsObservation,
                       runtime::OpsObservationSourceError> override {
    ++calls;
    if (blocked) {
      std::mutex mutex;
      std::unique_lock lock{mutex};
      std::condition_variable_any changed;
      changed.wait(lock, stop, [] { return false; });
    }
    if (fail || stop.stop_requested())
      return std::unexpected(runtime::OpsObservationSourceError::unavailable);
    auto captured = request;
    if (malformed) captured.selection_generation += 1;
    return OpsObservation{
        captured,
        EventTimestamp{std::chrono::milliseconds{1000}},
        EventTimestamp{std::chrono::milliseconds{1001}},
        OpsObservationCompleteness::complete,
        0,
        0,
        {},
        LinuxHealthObservation{OpsHealthState::healthy, 12, {}, 1, 0}};
  }
};
class Store final : public storage::SessionStore {
 public:
  std::vector<RunEvent> history;
  std::vector<std::vector<RunEvent>> batches;
  std::function<bool(std::span<const RunEvent>)> reject;
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
        0};
  }
  auto list_sessions(std::size_t, std::stop_token)
      -> std::expected<std::vector<storage::SessionInfo>,
                       storage::SessionStoreError> override {
    return std::vector<storage::SessionInfo>{};
  }
  auto replay_events(const SessionId&, std::stop_token)
      -> std::expected<std::vector<RunEvent>,
                       storage::SessionStoreError> override {
    return history;
  }
  auto append_events(const SessionId&, std::span<const RunEvent> events,
                     std::stop_token)
      -> std::expected<void, storage::SessionStoreError> override {
    ++attempts;
    if (reject && reject(events))
      return std::unexpected(
          storage::SessionStoreError{storage::SessionStoreErrorCode::io_failure,
                                     "fixture refused append", false});
    batches.emplace_back(events.begin(), events.end());
    history.insert(history.end(), events.begin(), events.end());
    return {};
  }
};
class Policy final : public runtime::ToolPolicy {
 public:
  mutable unsigned inspections{};
  unsigned evaluations{};
  unsigned approvals{};
  PolicyDecision decision{PolicyDecision::allow};
  bool fail_evaluation{};
  bool fail_approval{};
  bool invalid_scopes{};
  bool missing_provenance{};
  ToolPolicyProvenance retained{"aiforge.tool-launch-policy.v1",
                                id<PermissionProfileId>("observe"),
                                ToolRestrictionLevel::medium,
                                ToolApprovalMode::prompt,
                                {Effect::read},
                                {{Effect::read, "ops.target", "target"}},
                                {}};
  auto provenance() const noexcept -> const ToolPolicyProvenance* override {
    ++inspections;
    return missing_provenance ? nullptr : &retained;
  }
  auto selected_restriction() const noexcept
      -> std::optional<runtime::RestrictionLevel> override {
    return runtime::RestrictionLevel::medium;
  }
  auto evaluate(const runtime::ToolPolicyRequest& request)
      -> std::expected<runtime::ToolPolicyResolution,
                       runtime::ToolPolicyError> override {
    ++evaluations;
    if (fail_evaluation)
      return std::unexpected(runtime::ToolPolicyError{
          runtime::ToolPolicyErrorCode::internal_failure,
          "fixture evaluation failed", false});
    return runtime::ToolPolicyResolution{decision,
                                         decision == PolicyDecision::deny ||
                                                 invalid_scopes
                                             ? std::vector<CapabilityScope>{}
                                             : request.scopes,
                                         {}};
  }
  auto approve(const runtime::ToolPolicyRequest& request,
               runtime::ToolPolicyApproval)
      -> std::expected<runtime::ToolPolicyResolution,
                       runtime::ToolPolicyError> override {
    ++approvals;
    if (fail_approval)
      return std::unexpected(runtime::ToolPolicyError{
          runtime::ToolPolicyErrorCode::internal_failure,
          "fixture approval failed", false});
    return runtime::ToolPolicyResolution{
        PolicyDecision::allow, request.scopes, {}};
  }
};
class Stream final : public backend::BackendStream {
 public:
  explicit Stream(MessageId assistant, bool tool) {
    events.emplace_back(backend::ResponseStarted{"fixture"});
    if (tool) {
      events.emplace_back(backend::ToolCallDelta{
          id<InvocationId>("model-invocation"), "observe_target",
          R"({"target_id":"target","selection_generation":1,"operation":"linux_health","resource":{"kind":"none"}})"});
      events.emplace_back(backend::ResponseFinished{FinishReason::tool_call});
    } else {
      events.emplace_back(
          backend::ContentDelta{assistant, TextBlock{"ordinary response"}});
      events.emplace_back(backend::ResponseFinished{FinishReason::stop});
    }
  }
  auto next(std::stop_token)
      -> std::expected<std::optional<backend::BackendEvent>,
                       backend::BackendError> override {
    if (position == events.size()) return std::nullopt;
    return events[position++];
  }
  std::vector<backend::BackendEvent> events;
  std::size_t position{};
};
class Backend final : public backend::Backend {
 public:
  std::atomic<unsigned> calls{};
  bool tool{};
  auto start(backend::BackendRequest request, std::stop_token)
      -> std::expected<std::unique_ptr<backend::BackendStream>,
                       backend::BackendError> override {
    ++calls;
    return std::make_unique<Stream>(request.assistant_message_id, tool);
  }
};
template <class T> auto count(std::span<const RunEvent> events) -> std::size_t {
  return std::ranges::count_if(events, [](const auto& event) {
    return std::holds_alternative<T>(event.payload);
  });
}
struct Fixture {
  OpsObservationAuthoritySpec specification{spec()};
  std::shared_ptr<runtime::LocalSourceWorker> worker{
      runtime::LocalSourceWorker::create(1).value()};
  std::shared_ptr<runtime::OpsObservationBroker> broker{
      runtime::OpsObservationBroker::create(worker).value()};
  std::shared_ptr<runtime::OpsObservationEndpoint> endpoint{
      broker->activate_session(spec().session_id).value()};
  std::shared_ptr<Source> source{std::make_shared<Source>()};
  std::shared_ptr<Policy> policy{std::make_shared<Policy>()};
  runtime::ToolRegistrySnapshot tools;
  Store store;
  Backend backend;
  std::unique_ptr<runtime::RunKernel> kernel;
  Fixture() {
    select();
    registry();
  }
  ~Fixture() {
    kernel.reset();
    static_cast<void>(broker->close());
  }
  auto select() -> void {
    REQUIRE(broker->select(
        OpsObservationAuthority::create(specification).value(), source));
  }
  auto registry() -> void {
    runtime::ToolRegistry registrations;
    REQUIRE(runtime::register_ops_observation_tool(
        registrations, OpsObservationAuthority::create(specification).value(),
        endpoint));
    tools = registrations.snapshot().value();
  }
  auto open(runtime::DurableSessionMode mode =
                runtime::DurableSessionMode::create) -> void {
    kernel.reset();
    auto result = runtime::RunKernel::open_durable(
        {spec().session_id, mode, {}}, store, backend, nullptr, {}, {}, tools,
        policy, {}, broker);
    INFO((result ? "opened" : result.error().message));
    REQUIRE(result);
    kernel = std::move(*result);
  }
  auto control(std::string suffix = "1") const
      -> runtime::ObservationControlStart {
    return {id<RunId>("manual-" + suffix),
            {id<SurfaceId>("test"),
             id<WorkspaceId>("ops"),
             id<PermissionProfileId>("observe"),
             {}},
            id<InvocationId>("manual-invocation-" + suffix),
            {specification.target.target_id,
             specification.selection_generation,
             OpsObservationOperation::linux_health,
             {},
             specification.limits}};
  }
  auto pump_until(const std::function<bool()>& finished, bool service = true)
      -> void {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!finished() && std::chrono::steady_clock::now() < deadline) {
      if (service) REQUIRE(broker->service());
      auto drained = kernel->drain();
      INFO((drained ? "drained" : drained.error().message));
      REQUIRE(drained);
      std::this_thread::yield();
    }
    REQUIRE(finished());
  }
  auto idle() -> void {
    pump_until([&] { return !kernel->active_run_id(); });
  }
  auto ordinary(std::string suffix = "ordinary", bool provenance = true) const
      -> runtime::RunStart {
    ConstructedContext context{
        {ContextEntry{id<ContextEntryId>("context"),
                      ContextEntryKind::instruction,
                      InstructionLayer::application_runtime,
                      Message{id<MessageId>("runtime"),
                              Role::system,
                              {TextBlock{"runtime contract"}},
                              {}},
                      {id<ContextSourceId>("source"), {}, {}},
                      0,
                      1,
                      2}},
        {},
        {4096, 512, 0},
        2};
    runtime::RunStart result{id<RunId>(suffix),
                             {id<SurfaceId>("test"),
                              id<WorkspaceId>("chat"),
                              id<PermissionProfileId>("observe"),
                              {}},
                             {id<MessageId>("user-" + suffix),
                              Role::user,
                              {TextBlock{"inspect"}},
                              {}},
                             {id<InferenceId>("inference-" + suffix),
                              id<MessageId>("assistant-" + suffix),
                              id<ModelId>("fake"),
                              std::move(context),
                              tools.declarations(),
                              {}}};
    if (provenance)
      result.provenance = RunProvenance{"test", "fake", {}, id<ModelId>("fake"),
                                        {},     {},     {}, {}};
    return result;
  }
  auto next_starts() -> void {
    policy->decision = PolicyDecision::allow;
    policy->fail_evaluation = false;
    policy->fail_approval = false;
    source->fail = false;
    source->malformed = false;
    source->blocked = false;
    REQUIRE(kernel->start_observation_control(control("next")));
    idle();
    backend.tool = false;
    REQUIRE(kernel->start(ordinary()));
    idle();
  }
};
} // namespace observation_kernel_test
