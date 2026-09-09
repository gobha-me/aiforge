#include "../181opsobservationkernel/fixture.hpp"

#include <aiforge/runtime/tool_launch_policy.hpp>

namespace {
using namespace observation_kernel_test;
using namespace std::chrono_literals;

struct BindingFixture : Fixture {
  std::shared_ptr<runtime::ToolPolicy> launch_policy;
  auto open_binding(
      runtime::ApprovalMode mode = runtime::ApprovalMode::allow_all,
      bool with_broker = true, std::shared_ptr<runtime::ToolPolicy> custom = {},
      std::shared_ptr<runtime::ChildRunner> children = {}) -> void {
    runtime::ApplicationLaunchContextConfiguration configuration;
    configuration.approval_mode = mode;
    auto context = runtime::make_application_launch_context(configuration);
    REQUIRE(context);
    auto created = runtime::make_tool_launch_policy(
        tools, {id<PermissionProfileId>("observe"), *context, {}});
    REQUIRE(created);
    launch_policy = custom ? std::move(custom) : *created;
    auto opened = runtime::RunKernel::open_durable(
        {specification.session_id, runtime::DurableSessionMode::create, {}},
        store, backend, nullptr, {}, {}, tools, launch_policy,
        std::move(children), with_broker ? broker : nullptr);
    INFO((opened ? "opened" : opened.error().message));
    REQUIRE(opened);
    kernel = std::move(*opened);
  }
  auto next_spec() const -> OpsObservationAuthoritySpec {
    auto next = specification;
    next.target.target_id = id<OpsTargetId>("second-target");
    next.target.configuration_revision =
        id<OpsConfigurationRevision>("second-revision");
    ++next.selection_generation;
    return next;
  }
  auto old_request() const -> OpsObservationRequest {
    const auto* registration = tools.find("observe_target");
    REQUIRE(registration);
    const auto* native = dynamic_cast<const runtime::OpsObservationTool*>(
        registration->executor.get());
    REQUIRE(native);
    const auto prepared =
        native->prepare(control().invocation_id, control().intent);
    REQUIRE(prepared);
    REQUIRE(prepared->observation_request);
    return *prepared->observation_request;
  }
  auto bind(const OpsObservationAuthoritySpec& next,
            std::shared_ptr<runtime::OpsObservationSource> next_source,
            std::shared_ptr<runtime::OpsObservationEndpoint> next_endpoint)
      -> std::expected<runtime::OpsObservationBinding,
                       runtime::RunKernelError> {
    auto authority = OpsObservationAuthority::create(next);
    REQUIRE(authority);
    return kernel->bind_ops_observation(std::move(*authority),
                                        std::move(next_source),
                                        std::move(next_endpoint));
  }
};

auto source_for(const OpsObservationAuthoritySpec& next)
    -> std::shared_ptr<Source> {
  auto source = std::make_shared<Source>();
  source->binding = next.target;
  return source;
}

class WaitingChildStream final : public runtime::ChildRunResultStream {
 public:
  auto next(std::stop_token stop)
      -> std::expected<std::optional<runtime::ChildRunResult>,
                       runtime::ChildRunError> override {
    std::mutex mutex;
    std::unique_lock lock{mutex};
    std::condition_variable_any changed;
    changed.wait(lock, stop, [] { return false; });
    return std::nullopt;
  }
};
class WaitingChildren final : public runtime::ChildRunner {
 public:
  auto start(runtime::ChildRunInvocation, std::stop_token)
      -> std::expected<std::unique_ptr<runtime::ChildRunResultStream>,
                       runtime::ChildRunError> override {
    return std::make_unique<WaitingChildStream>();
  }
};

auto start_child(runtime::RunKernel& kernel) -> void {
  const RepositorySnapshotIdentity snapshot{id<RepositoryId>("repository"),
                                            {"sha256", "aaaaaaaaaaaaaaaa", 64}};
  const PlanTask task{id<PlanTaskId>("task"),
                      {},
                      {},
                      "Bounded child",
                      {"Result can be replayed"},
                      {Effect::read},
                      {{Effect::read, "repository_path", "src"}}};
  const PlanRevision revision{
      id<PlanId>("plan"),
      id<PlanRevisionId>("revision"),
      {},
      "Approved work",
      snapshot,
      {task},
      {{id<EvidenceId>("approval"), {"sha256", "bbbbbbbbbbbbbbbb", 16}}}};
  const RunStarted attributes{id<SurfaceId>("test"),
                              id<WorkspaceId>("code"),
                              id<PermissionProfileId>("observe"),
                              {}};
  const auto parent = id<RunId>("planning-run");
  REQUIRE(kernel.start_plan({parent, attributes, revision}));
  REQUIRE(kernel.decide_plan(parent,
                             {revision.plan_id,
                              revision.revision_id,
                              PlanDecision::approved,
                              PlanDecisionSource::user,
                              {}},
                             {snapshot, revision.evidence}) ==
          runtime::PlanDecisionOutcome::recorded);
  ContextParcel parcel{id<ContextParcelId>("parcel"),
                       "Execute the accepted task",
                       TaskPhase::editing,
                       snapshot,
                       {{id<EvidenceId>("context"),
                         ExactSourceEvidence{RepositorySourceIdentity{
                             snapshot,
                             "src/main.cpp",
                             {"sha256", "cccccccccccccccc", 13},
                             SourceByteRange{0, 13}}},
                         EvidenceFreshness::current,
                         {EvidenceDerivation::observed,
                          "filesystem",
                          "1",
                          EventTimestamp{100ms},
                          snapshot,
                          {},
                          {},
                          {}},
                         {TextBlock{"int main() {}"}},
                         13,
                         4}}};
  const CapabilityScope scope{Effect::read, "filesystem.root",
                              "/fixture/repository"};
  REQUIRE(kernel.dispatch_child({id<RunId>("child-run"),
                                 parent,
                                 attributes,
                                 revision.plan_id,
                                 revision.revision_id,
                                 task.task_id,
                                 std::move(parcel),
                                 {2, 3, 100, 50, 5s},
                                 {Effect::read},
                                 {scope},
                                 {Effect::read},
                                 {scope},
                                 1,
                                 {}}));
}
} // namespace

TEST_CASE(
    "Kernel Ops binding rejects unavailable broker and copied policy authority",
    "[ops][binding]") {
  BindingFixture f;
  SECTION("missing broker") {
    f.open_binding(runtime::ApprovalMode::allow_all, false);
  }
  SECTION("custom policy has genuine copied provenance") {
    auto context = runtime::make_application_launch_context({});
    REQUIRE(context);
    auto genuine = runtime::make_tool_launch_policy(
        f.tools, {id<PermissionProfileId>("observe"), *context, {}});
    REQUIRE(genuine);
    f.policy->retained = *(*genuine)->provenance();
    f.open_binding(runtime::ApprovalMode::allow_all, true, f.policy);
  }
  auto next = f.next_spec();
  auto source = source_for(next);
  const auto old = f.old_request();
  const auto before = f.store.attempts;
  const auto result = f.bind(next, source, f.endpoint);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == runtime::RunKernelErrorCode::invalid_tool_state);
  CHECK(f.store.attempts == before);
  CHECK(f.store.history.empty());
  CHECK(f.kernel->event_log().events().empty());
  CHECK(f.broker->preflight(*f.endpoint, old));
  CHECK(f.policy->evaluations == 0);
  CHECK(f.policy->approvals == 0);
  CHECK(f.source->calls == 0);
  CHECK(source->calls == 0);
  CHECK(f.backend.calls == 0);
}

TEST_CASE("Kernel Ops binding rejects mismatched selection before mutation",
          "[ops][binding]") {
  BindingFixture f;
  f.open_binding();
  auto next = f.next_spec();
  auto source = source_for(next);
  auto endpoint = f.endpoint;
  std::shared_ptr<runtime::OpsObservationBroker> foreign;
  SECTION("null issuer") {
    endpoint.reset();
  }
  SECTION("foreign issuer with same session") {
    foreign = runtime::OpsObservationBroker::create(f.worker).value();
    endpoint = foreign->activate_session(f.specification.session_id).value();
  }
  SECTION("expired issuer after same-session activation") {
    f.endpoint = f.broker->activate_session(f.specification.session_id).value();
    f.select();
    f.registry();
  }
  SECTION("foreign session") {
    next.session_id = id<SessionId>("other-session");
  }
  SECTION("foreign owner") {
    next.owner_id = id<OpsOwnerId>("other-owner");
  }
  SECTION("stale generation") {
    next.selection_generation = f.specification.selection_generation;
  }
  SECTION("null source") {
    source.reset();
  }
  SECTION("source belongs to another target") {
    source->binding = f.specification.target;
  }
  SECTION("log consent changes without advancing its revision") {
    next.target = f.specification.target;
    source->binding = next.target;
    next.logs.enabled = true;
    next.logs.permitted_sources.push_back(LinuxServiceIdentity{
        "application.service", id<OpsResourceUid>("invocation")});
  }
  const auto old = f.old_request();
  const auto before = f.store.attempts;
  const auto result = f.bind(next, source, endpoint);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == runtime::RunKernelErrorCode::invalid_tool_state);
  CHECK(f.store.attempts == before);
  CHECK(f.store.history.empty());
  CHECK(f.kernel->event_log().events().empty());
  CHECK(f.broker->preflight(*f.endpoint, old));
  CHECK(f.source->calls == 0);
  if (source) CHECK(source->calls == 0);
  CHECK(f.backend.calls == 0);
}

TEST_CASE("Kernel Ops binding preserves active work and pending approval",
          "[ops][binding]") {
  BindingFixture f;
  bool model{};
  SECTION("manual approval pending") {
    f.open_binding(runtime::ApprovalMode::prompt);
    REQUIRE(f.kernel->start_observation_control(f.control()));
    REQUIRE(f.kernel->pending_tool_approval());
  }
  SECTION("manual source queued") {
    f.open_binding();
    REQUIRE(f.kernel->start_observation_control(f.control()));
  }
  SECTION("model response active") {
    model = true;
    f.open_binding();
    REQUIRE(f.kernel->start(f.ordinary()));
  }
  SECTION("child active with no primary run") {
    f.open_binding(runtime::ApprovalMode::allow_all, true, {},
                   std::make_shared<WaitingChildren>());
    start_child(*f.kernel);
    REQUIRE_FALSE(f.kernel->active_run_id());
    REQUIRE_FALSE(f.kernel->active_session_tasks().empty());
  }
  const auto tasks = f.kernel->active_session_tasks();
  const auto active = f.kernel->active_run_id();
  const auto pending = f.kernel->pending_tool_approval();
  const auto history = f.store.history;
  const auto attempts = f.store.attempts;
  auto next = f.next_spec();
  auto source = source_for(next);
  const auto result = f.bind(next, source, f.endpoint);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == runtime::RunKernelErrorCode::run_already_active);
  CHECK(f.kernel->active_session_tasks() == tasks);
  CHECK(f.kernel->active_run_id() == active);
  CHECK(f.kernel->pending_tool_approval() == pending);
  CHECK(f.store.history == history);
  CHECK(f.store.attempts == attempts);
  CHECK(f.broker->preflight(*f.endpoint, f.old_request()));
  CHECK(f.source->calls == 0);
  CHECK(source->calls == 0);
  if (!model) CHECK(f.backend.calls == 0);
}

TEST_CASE("Kernel Ops binding refuses an unusable durable session",
          "[ops][binding]") {
  BindingFixture f;
  f.open_binding();
  f.store.reject = [](std::span<const RunEvent>) { return true; };
  REQUIRE_FALSE(f.kernel->start_observation_control(f.control()));
  const auto attempts = f.store.attempts;
  auto next = f.next_spec();
  auto source = source_for(next);
  const auto result = f.bind(next, source, f.endpoint);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == runtime::RunKernelErrorCode::storage_failure);
  CHECK(f.store.attempts == attempts);
  CHECK(f.store.history.empty());
  CHECK(f.broker->preflight(*f.endpoint, f.old_request()));
  CHECK(f.source->calls == 0);
  CHECK(source->calls == 0);
  CHECK(f.backend.calls == 0);
}

TEST_CASE("Successful idle binding drives the next durable manual observation",
          "[ops][binding]") {
  BindingFixture f;
  f.open_binding();
  auto next = f.next_spec();
  auto source = source_for(next);
  const auto old = f.old_request();
  const auto attempts = f.store.attempts;
  auto result = f.bind(next, source, f.endpoint);
  REQUIRE(result);
  CHECK(f.store.attempts == attempts);
  CHECK(f.store.history.empty());
  CHECK(f.kernel->event_log().events().empty());
  CHECK(f.source->calls == 0);
  CHECK(source->calls == 0);
  CHECK(f.backend.calls == 0);
  CHECK_FALSE(f.broker->preflight(*f.endpoint, old));
  REQUIRE(result->available_tools.find("observe_target"));
  REQUIRE(result->policy);
  f.specification = next;
  // Updating the owner's copies must not be required to update the kernel.
  REQUIRE(f.kernel->start_observation_control(f.control("new")));
  f.idle();
  CHECK(f.source->calls == 0);
  CHECK(source->calls == 1);
  CHECK(f.backend.calls == 0);
  REQUIRE(count<OpsObservationRecorded>(f.store.history) == 1);
  CHECK(count<RunCompleted>(f.store.history) == 1);
  CHECK(count<ToolResultRecorded>(f.store.history) == 1);
  for (const auto& event : f.store.history) {
    if (const auto* recorded =
            std::get_if<OpsObservationRecorded>(&event.payload)) {
      CHECK(recorded->observation.request.target == next.target);
      CHECK(recorded->observation.request.selection_generation ==
            next.selection_generation);
      CHECK(recorded->observation.request.session_id == next.session_id);
    }
  }
  CHECK(runtime::recorded_ops_observations(f.kernel->event_log()));
}

TEST_CASE(
    "An old approval cannot authorize the target selected after it finishes",
    "[ops][binding]") {
  BindingFixture f;
  f.open_binding(runtime::ApprovalMode::prompt);
  const auto old = f.control("old");
  REQUIRE(f.kernel->start_observation_control(old));
  REQUIRE(f.kernel->pending_tool_approval());
  auto next = f.next_spec();
  auto source = source_for(next);
  REQUIRE_FALSE(f.bind(next, source, f.endpoint));
  unsigned old_observations{};
  SECTION("denied request is retained") {
    REQUIRE(f.kernel->decide_approval(old.run_id, old.invocation_id,
                                      {ApprovalDecision::denied, {}}));
  }
  SECTION("completed invocation grant is retained") {
    REQUIRE(
        f.kernel->decide_approval(old.run_id, old.invocation_id,
                                  {ApprovalDecision::approved,
                                   {{Effect::read, "ops.target", "target"}}}));
    f.idle();
    old_observations = 1;
  }
  REQUIRE_FALSE(f.kernel->active_run_id());
  REQUIRE(f.bind(next, source, f.endpoint));
  const auto history = f.store.history;
  const auto attempts = f.store.attempts;
  const runtime::ToolApprovalResolution old_grant{
      ApprovalDecision::approved, {{Effect::read, "ops.target", "target"}}};
  CHECK_FALSE(
      f.kernel->decide_approval(old.run_id, old.invocation_id, old_grant));
  CHECK(f.store.history == history);
  CHECK(f.store.attempts == attempts);
  f.specification = next;
  const auto current = f.control("new");
  REQUIRE(f.kernel->start_observation_control(current));
  REQUIRE(f.kernel->pending_tool_approval());
  CHECK(f.source->calls == old_observations);
  CHECK(source->calls == 0);
  CHECK_FALSE(
      f.kernel->decide_approval(old.run_id, old.invocation_id, old_grant));
  REQUIRE(f.kernel->pending_tool_approval());
  REQUIRE(f.kernel->decide_approval(
      current.run_id, current.invocation_id,
      {ApprovalDecision::approved,
       {{Effect::read, "ops.target", "second-target"}}}));
  f.idle();
  CHECK(f.source->calls == old_observations);
  CHECK(source->calls == 1);
  CHECK(f.backend.calls == 0);
  CHECK(count<OpsObservationRecorded>(f.store.history) == old_observations + 1);
  CHECK(runtime::recorded_ops_observations(f.kernel->event_log()));
}
