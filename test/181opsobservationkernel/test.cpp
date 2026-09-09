// Failure matrix before implementation:
// Foreign broker/endpoint issuer, replaced same-session mailbox, stale target/
// generation/log revision, missing current grant, wrong kernel session or
// invocation, widening scopes/limits, generic executor carrying native proof or
// receipt: fail before policy, executor start or source IO. Missing versioned
// native/policy provenance and busy primary/child work reject admission.
// Atomic manual start+human intent+typed proposal refuses partial persistence;
// admission refusal performs zero policy/source/backend work. Native result
// publication rejects stale/wrong receipts, cancelled/deadline results and
// arbitrary content, progress, questions, artifacts or spend. Store refusal
// never publishes fresh evidence/success. Policy denial, approval-port failure,
// startup failure, malformed EOF, protocol failure, deadline and cancellation
// produce complete tool/run terminal pairs. Next manual/ordinary start remains
// possible after idle terminal cleanup; worker-owned terminal state drains to
// ToolEnded before reuse. Old-kernel destruction cannot close a replacement
// broker session or cancel a reused invocation ID. Manual success makes one
// source call and zero backend calls, with explicit owner broker pumping.
// Completed history reopens without IO; unfinished manual work never retries;
// unfinished model proof remains unrecoverable until fresh binding is defined.

#include "fixture.hpp"
using namespace observation_kernel_test;

TEST_CASE("manual observation admission rejects foreign and stale authority "
          "before policy or IO",
          "[ops][kernel]") {
  Fixture f;
  auto control = f.control();
  SECTION("foreign broker with identical session and target") {
    auto foreign = runtime::OpsObservationBroker::create(f.worker).value();
    REQUIRE(foreign->activate_session(spec().session_id));
    REQUIRE(foreign->select(OpsObservationAuthority::create(spec()).value(),
                            f.source));
    f.broker = std::move(foreign);
  }
  SECTION("same-session replacement invalidates captured endpoint") {
    REQUIRE(f.broker->activate_session(spec().session_id));
    f.select();
  }
  SECTION("selected target changed") {
    ++f.specification.selection_generation;
    f.select();
  }
  SECTION("requested generation is foreign") {
    ++control.intent.selection_generation;
  }
  SECTION("unknown target") {
    control.intent.target_id = id<OpsTargetId>("other");
  }
  SECTION("caller cannot set manual contract") {
    control.attributes.manual_observation_required = true;
  }
  SECTION("kernel session differs") {
    f.kernel = std::make_unique<runtime::RunKernel>(
        id<SessionId>("other"), f.backend, nullptr, runtime::TimestampSource{},
        runtime::RunKernelLimits{}, f.tools, f.policy,
        std::shared_ptr<runtime::ChildRunner>{}, f.broker);
  }
  if (!f.kernel) f.open();
  REQUIRE_FALSE(f.kernel->start_observation_control(control));
  REQUIRE(f.kernel->event_log().events().empty());
  REQUIRE(f.policy->evaluations == 0);
  REQUIRE(f.policy->approvals == 0);
  REQUIRE(f.source->calls == 0);
  REQUIRE(f.backend.calls == 0);
}

TEST_CASE("missing native registration and policy provenance do not admit "
          "manual work",
          "[ops][kernel]") {
  Fixture f;
  SECTION("missing registration") {
    f.tools = {};
  }
  SECTION("missing launch provenance") {
    f.policy->missing_provenance = true;
  }
  SECTION("wrong permission profile") {
    f.policy->retained.permission_profile_id = id<PermissionProfileId>("other");
  }
  f.open();
  REQUIRE_FALSE(f.kernel->start_observation_control(f.control()));
  REQUIRE(f.store.history.empty());
  REQUIRE(f.policy->evaluations == 0);
  REQUIRE(f.source->calls == 0);
}

TEST_CASE("admission persistence precedes policy and source work",
          "[ops][kernel]") {
  Fixture f;
  f.open();
  f.store.reject = [](auto events) {
    return count<HumanObservationRequested>(events) != 0;
  };
  REQUIRE_FALSE(f.kernel->start_observation_control(f.control()));
  REQUIRE(f.store.history.empty());
  REQUIRE(f.store.batches.empty());
  REQUIRE(f.policy->evaluations == 0);
  REQUIRE(f.source->calls == 0);
  REQUIRE(f.backend.calls == 0);
}

TEST_CASE("manual terminal failures retain complete history and permit "
          "subsequent starts",
          "[ops][kernel]") {
  Fixture f;
  SECTION("policy denied") {
    f.policy->decision = PolicyDecision::deny;
  }
  SECTION("policy evaluation failed") {
    f.policy->fail_evaluation = true;
  }
  SECTION("source failed") {
    f.source->fail = true;
  }
  SECTION("source returned mismatched identity") {
    f.source->malformed = true;
  }
  f.open();
  REQUIRE(f.kernel->start_observation_control(f.control()));
  f.idle();
  REQUIRE(count<ToolErrored>(f.store.history) == 1);
  REQUIRE(count<RunFailed>(f.store.history) == 1);
  REQUIRE(count<OpsObservationRecorded>(f.store.history) == 0);
  REQUIRE(runtime::recorded_ops_observations(f.kernel->event_log()));
  REQUIRE(f.backend.calls == 0);
  f.next_starts();
}

TEST_CASE("approval denial and port failure finish the manual run atomically",
          "[ops][kernel]") {
  Fixture f;
  f.policy->decision = PolicyDecision::require_approval;
  f.open();
  const auto control = f.control();
  REQUIRE(f.kernel->start_observation_control(control));
  REQUIRE(f.kernel->pending_tool_approval());
  runtime::ToolApprovalResolution decision{ApprovalDecision::denied, {}};
  SECTION("denied") {
  }
  SECTION("cancelled") {
    decision.decision = ApprovalDecision::cancelled;
  }
  SECTION("approval port failed") {
    f.policy->fail_approval = true;
    decision.decision = ApprovalDecision::approved;
    decision.granted_scopes = {{Effect::read, "ops.target", "target"}};
  }
  const auto approved = f.kernel->decide_approval(
      control.run_id, control.invocation_id, decision);
  REQUIRE(approved.has_value() != f.policy->fail_approval);
  REQUIRE_FALSE(f.kernel->active_run_id());
  REQUIRE(count<ToolErrored>(f.store.history) == 1);
  REQUIRE(count<RunFailed>(f.store.history) == 1);
  REQUIRE(runtime::recorded_ops_observations(f.kernel->event_log()));
  REQUIRE(f.source->calls == 0);
  f.next_starts();
}

TEST_CASE("manual cancellation retains worker cleanup and permits next start",
          "[ops][kernel]") {
  Fixture f;
  bool running{};
  SECTION("before approval") {
    f.policy->decision = PolicyDecision::require_approval;
  }
  SECTION("while source is running") {
    f.source->blocked = true;
    running = true;
  }
  f.open();
  const auto control = f.control();
  REQUIRE(f.kernel->start_observation_control(control));
  if (running) f.pump_until([&] { return f.source->calls.load() != 0; });
  REQUIRE(f.kernel->cancel_run(control.run_id));
  f.idle();
  REQUIRE(count<ToolErrored>(f.store.history) == 1);
  REQUIRE(count<RunCancelled>(f.store.history) == 1);
  REQUIRE(count<RunFailed>(f.store.history) == 0);
  REQUIRE(runtime::recorded_ops_observations(f.kernel->event_log()));
  f.next_starts();
}

TEST_CASE("stale approval cannot launch against a newer selection",
          "[ops][kernel]") {
  Fixture f;
  f.policy->decision = PolicyDecision::require_approval;
  f.open();
  const auto control = f.control();
  REQUIRE(f.kernel->start_observation_control(control));
  ++f.specification.selection_generation;
  f.select();
  REQUIRE(f.kernel->decide_approval(
      control.run_id, control.invocation_id,
      {ApprovalDecision::approved, {{Effect::read, "ops.target", "target"}}}));
  REQUIRE_FALSE(f.kernel->active_run_id());
  REQUIRE(f.source->calls == 0);
  REQUIRE(count<ToolStarted>(f.store.history) == 0);
  REQUIRE(count<RunFailed>(f.store.history) == 1);
  REQUIRE(runtime::recorded_ops_observations(f.kernel->event_log()));
  f.registry();
  REQUIRE(f.kernel->replace_available_tools(f.tools));
  f.next_starts();
}

TEST_CASE("live grammar rejection closes the kernel without claiming success",
          "[ops][kernel]") {
  Fixture f;
  f.policy->invalid_scopes = true;
  f.open();
  auto started = f.kernel->start_observation_control(f.control());
  REQUIRE_FALSE(started);
  REQUIRE(started.error().code ==
          runtime::RunKernelErrorCode::event_log_rejected);
  REQUIRE_FALSE(f.kernel->active_run_id());
  REQUIRE(count<HumanObservationRequested>(f.store.history) == 1);
  REQUIRE(count<ToolPolicyDecided>(f.store.history) == 0);
  REQUIRE(f.source->calls == 0);
  REQUIRE_FALSE(f.kernel->start_observation_control(f.control("next")));
  f.policy->invalid_scopes = false;
  f.open(runtime::DurableSessionMode::resume);
  REQUIRE(count<RunFailed>(f.store.history) == 1);
  REQUIRE(f.source->calls == 0);
}

TEST_CASE(
    "publication persistence refusal never exposes fresh observation success",
    "[ops][kernel]") {
  Fixture f;
  f.open();
  f.store.reject = [](auto events) {
    return count<OpsObservationRecorded>(events) != 0;
  };
  REQUIRE(f.kernel->start_observation_control(f.control()));
  bool failed{};
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{5};
  while (!failed && std::chrono::steady_clock::now() < deadline) {
    REQUIRE(f.broker->service());
    failed = !f.kernel->drain();
    std::this_thread::yield();
  }
  REQUIRE(failed);
  REQUIRE(f.source->calls == 1);
  REQUIRE_FALSE(f.kernel->active_run_id());
  REQUIRE(count<OpsObservationRecorded>(f.store.history) == 0);
  REQUIRE(count<ToolResultRecorded>(f.store.history) == 0);
  REQUIRE(count<RunCompleted>(f.store.history) == 0);
  f.store.reject = {};
  f.open(runtime::DurableSessionMode::resume);
  REQUIRE(count<RunFailed>(f.store.history) == 1);
  REQUIRE(f.source->calls == 1);
}

TEST_CASE("manual success records complete evidence without inference and "
          "replays without IO",
          "[ops][kernel]") {
  Fixture f;
  f.open();
  const auto control = f.control();
  REQUIRE(f.kernel->start_observation_control(control));
  REQUIRE(f.store.batches.front().size() == 3);
  REQUIRE(f.store.batches.front().front().metadata.schema_version == 6);
  REQUIRE(f.store.batches.front().back().metadata.schema_version == 3);
  REQUIRE(count<HumanObservationRequested>(f.store.batches.front()) == 1);
  REQUIRE_FALSE(f.kernel->start_observation_control(f.control("busy")));
  f.idle();
  REQUIRE(f.source->calls == 1);
  REQUIRE(f.backend.calls == 0);
  REQUIRE(count<OpsObservationRecorded>(f.store.history) == 1);
  REQUIRE(count<ToolResultRecorded>(f.store.history) == 1);
  REQUIRE(count<RunCompleted>(f.store.history) == 1);
  REQUIRE(count<InferenceStarted>(f.store.history) == 0);
  REQUIRE(runtime::recorded_ops_observations(f.kernel->event_log()));
  const auto stored = f.store.history;
  f.open(runtime::DurableSessionMode::resume);
  REQUIRE(f.store.history == stored);
  REQUIRE(f.source->calls == 1);
  REQUIRE_FALSE(f.kernel->start_observation_control(control));
  f.next_starts();
}

TEST_CASE("native model calls need real recorded tool and policy provenance",
          "[ops][kernel]") {
  Fixture f;
  f.backend.tool = true;
  bool with_provenance{true};
  SECTION("recorded native provenance") {
    f.open();
  }
  SECTION("ephemeral missing provenance is refused") {
    with_provenance = false;
    f.kernel = std::make_unique<runtime::RunKernel>(
        spec().session_id, f.backend, nullptr, runtime::TimestampSource{},
        runtime::RunKernelLimits{}, f.tools, f.policy,
        std::shared_ptr<runtime::ChildRunner>{}, f.broker);
  }
  REQUIRE(f.kernel->start(f.ordinary("model", with_provenance)));
  f.pump_until([&] {
    const auto& events = f.kernel->event_log().events();
    return count<ToolResultRecorded>(events) != 0 ||
           count<ToolErrored>(events) != 0;
  });
  const auto& events = f.kernel->event_log().events();
  REQUIRE(count<HumanObservationRequested>(events) == 0);
  REQUIRE(f.backend.calls == 1);
  if (with_provenance) {
    REQUIRE(f.source->calls == 1);
    REQUIRE(count<OpsObservationRecorded>(events) == 1);
    const auto proposal = std::ranges::find_if(events, [](const auto& event) {
      return std::holds_alternative<ToolProposed>(event.payload);
    });
    REQUIRE(proposal != events.end());
    REQUIRE(proposal->metadata.schema_version == 3);
    REQUIRE(std::get<ToolProposed>(proposal->payload).validated_arguments);
    REQUIRE(std::get<ToolProposed>(proposal->payload).observation_request);
  } else {
    REQUIRE(f.source->calls == 0);
    REQUIRE(count<ToolErrored>(events) == 1);
  }
  REQUIRE(runtime::recorded_ops_observations(f.kernel->event_log()));
}

TEST_CASE("native model approval port failure clears the pending run",
          "[ops][kernel]") {
  Fixture f;
  f.backend.tool = true;
  f.policy->decision = PolicyDecision::require_approval;
  f.open();
  const auto start = f.ordinary("model");
  REQUIRE(f.kernel->start(start));
  f.pump_until([&] { return f.kernel->pending_tool_approval().has_value(); });
  f.policy->fail_approval = true;
  REQUIRE_FALSE(f.kernel->decide_approval(
      start.run_id, id<InvocationId>("model-invocation"),
      {ApprovalDecision::approved, {{Effect::read, "ops.target", "target"}}}));
  f.idle();
  REQUIRE_FALSE(f.kernel->pending_tool_approval());
  REQUIRE(f.kernel->projection(start.run_id)->status() == RunStatus::failed);
  REQUIRE(count<ToolPolicyFailed>(f.store.history) == 1);
  REQUIRE(count<ToolErrored>(f.store.history) == 1);
  REQUIRE(count<RunFailed>(f.store.history) == 1);
  REQUIRE(count<OpsObservationRecorded>(f.store.history) == 0);
  REQUIRE(f.source->calls == 0);
  REQUIRE(runtime::recorded_ops_observations(f.kernel->event_log()));
  f.next_starts();
}

TEST_CASE("manual deadline stops work and leaves a reusable kernel",
          "[ops][kernel]") {
  Fixture f;
  f.specification.limits.timeout = std::chrono::milliseconds{100};
  f.source->blocked = true;
  f.select();
  f.registry();
  f.open();
  REQUIRE(f.kernel->start_observation_control(f.control()));
  f.idle();
  REQUIRE(count<ToolErrored>(f.store.history) == 1);
  REQUIRE(count<RunFailed>(f.store.history) == 1);
  REQUIRE(count<OpsObservationRecorded>(f.store.history) == 0);
  REQUIRE(runtime::recorded_ops_observations(f.kernel->event_log()));
  f.specification.limits.timeout = OpsObservationLimits{}.timeout;
  f.select();
  f.registry();
  REQUIRE(f.kernel->replace_available_tools(f.tools));
  f.next_starts();
}

TEST_CASE("destroying an old kernel cannot cancel a replacement session's "
          "reused invocation",
          "[ops][kernel]") {
  Fixture old;
  old.source->blocked = true;
  old.open();
  REQUIRE(old.kernel->start_observation_control(old.control()));
  old.pump_until([&] { return old.source->calls.load() == 1; });
  Fixture replacement;
  replacement.broker = old.broker;
  replacement.endpoint =
      replacement.broker->activate_session(spec().session_id).value();
  replacement.select();
  replacement.registry();
  replacement.open();
  REQUIRE(replacement.kernel->start_observation_control(replacement.control()));
  old.kernel.reset();
  replacement.idle();
  REQUIRE(replacement.source->calls == 1);
  REQUIRE(count<OpsObservationRecorded>(replacement.store.history) == 1);
  REQUIRE(count<RunCompleted>(replacement.store.history) == 1);
  REQUIRE(count<RunCancelled>(replacement.store.history) == 0);
  REQUIRE(runtime::recorded_ops_observations(replacement.kernel->event_log()));
}

TEST_CASE("ordinary executor cannot dispatch a copied native observation proof",
          "[ops][kernel]") {
  class CopiedProof final : public runtime::ToolExecutor {
   public:
    explicit CopiedProof(std::shared_ptr<runtime::ToolExecutor> native)
        : native(std::move(native)) {}
    std::shared_ptr<runtime::ToolExecutor> native;
    unsigned starts{};
    auto validate(const StructuredDataBlock& value) const
        -> std::expected<runtime::ValidatedToolArguments,
                         runtime::ToolExecutionError> override {
      return native->validate(value);
    }
    auto prepare(const InvocationId& invocation,
                 const StructuredDataBlock& value) const
        -> std::expected<runtime::ValidatedToolArguments,
                         runtime::ToolExecutionError> override {
      return native->prepare(invocation, value);
    }
    auto start(runtime::ToolInvocation, std::stop_token)
        -> std::expected<std::unique_ptr<runtime::ToolExecutionStream>,
                         runtime::ToolExecutionError> override {
      ++starts;
      return std::unexpected(runtime::ToolExecutionError{
          runtime::ToolExecutionErrorCode::unavailable,
          "fixture must not start", false});
    }
  };
  Fixture f;
  auto registration = *f.tools.find("observe_target");
  auto ordinary = std::make_shared<CopiedProof>(registration.executor);
  registration.executor = ordinary;
  f.tools = f.tools.replace(std::move(registration)).value();
  f.backend.tool = true;
  f.open();
  REQUIRE(f.kernel->start(f.ordinary("model")));
  f.pump_until([&] { return count<ToolErrored>(f.store.history) != 0; });
  REQUIRE(count<ToolStarted>(f.store.history) == 0);
  REQUIRE(count<OpsObservationRecorded>(f.store.history) == 0);
  REQUIRE(f.policy->evaluations == 0);
  REQUIRE(ordinary->starts == 0);
  REQUIRE(f.source->calls == 0);
  REQUIRE(runtime::recorded_ops_observations(f.kernel->event_log()));
}
