#include "fixture.hpp"
#include <stdexcept>

using namespace ops_session_test;

TEST_CASE("Standalone open rejects invalid preparation before durable create",
          "[ops-session]") {
  ops_session_test::Fixture f;
  auto request = f.request();
  auto dependencies = f.dependencies();
  SECTION("identity with invalid UTF-8") {
    // Domain IDs admit non-control bytes; open additionally requires safe
    // UTF-8.
    auto malformed = SurfaceId::from(std::string(1, static_cast<char>(0xFF)));
    REQUIRE(malformed);
    request.surface_id = *malformed;
  }
  SECTION("missing identity generator") {
    dependencies.identity_suffix_source = {};
  }
  SECTION("missing broker") {
    dependencies.broker.reset();
  }
  SECTION("invalid kernel limits") {
    dependencies.run_limits.pending_updates = 0;
  }
  auto opened = OpsSession::open(request, f.store, std::move(dependencies));
  REQUIRE_FALSE(opened);
  REQUIRE(f.store.creates == 0);
  REQUIRE(f.store.opens == 0);
  REQUIRE(f.store.replays == 0);
  REQUIRE(f.source->calls == 0);
}
TEST_CASE(
    "Create-only duplicate cannot replay or disturb an existing selection",
    "[ops-session]") {
  ops_session_test::Fixture f;
  f.open();
  f.bind();
  auto duplicate = OpsSession::open(f.request(), f.store, f.dependencies());
  REQUIRE_FALSE(duplicate);
  REQUIRE(f.store.opens == 0);
  REQUIRE(f.store.replays == 0);
  REQUIRE(f.source->calls == 0);
  REQUIRE(f.session->submit_observation(f.intent()));
  f.idle();
  REQUIRE(f.session->inspect_observations().projection.latest_success);
}
TEST_CASE(
    "Manual admission rejects unavailable stale busy and reused identities",
    "[ops-session]") {
  ops_session_test::Fixture f;
  f.open();
  SECTION("unbound") {
    REQUIRE_FALSE(f.session->submit_observation(f.intent()));
    REQUIRE(f.store.delegate.history.empty());
  }
  SECTION("stale intent") {
    f.bind();
    auto intent = f.intent();
    ++intent.selection_generation;
    REQUIRE_FALSE(f.session->submit_observation(intent));
    REQUIRE(f.store.delegate.history.empty());
  }
  SECTION("active") {
    f.bind();
    f.source->blocked = true;
    REQUIRE(f.session->submit_observation(f.intent()));
    const auto count = f.store.delegate.history.size();
    auto again = f.session->submit_observation(f.intent());
    REQUIRE_FALSE(again);
    REQUIRE(again.error().code == ManualOpsErrorCode::busy);
    REQUIRE(f.store.delegate.history.size() == count);
  }
  SECTION("reused suffix") {
    f.bind();
    REQUIRE(f.session->submit_observation(f.intent()));
    f.idle();
    f.suffix = 0;
    const auto count = f.store.delegate.history.size();
    REQUIRE_FALSE(f.session->submit_observation(f.intent()));
    REQUIRE(f.store.delegate.history.size() == count);
  }
}
TEST_CASE("Prompt decisions use exact current manual identity and scopes",
          "[ops-session]") {
  ops_session_test::Fixture f;
  f.open(runtime::ApprovalMode::prompt);
  f.bind();
  auto submitted = f.session->submit_observation(f.intent());
  REQUIRE(submitted);
  const auto pending = f.session->inspect_observations().approval;
  REQUIRE(pending);
  REQUIRE(f.source->calls == 0);
  REQUIRE_FALSE(f.session->decide_observation_approval(
      submitted->run_id, id<InvocationId>("foreign"), {}));
  runtime::ToolApprovalResolution decision;
  SECTION("approve") {
    decision.decision = ApprovalDecision::approved;
    decision.granted_scopes = pending->scopes;
  }
  SECTION("deny") {
    decision.decision = ApprovalDecision::denied;
  }
  REQUIRE(f.session->decide_observation_approval(
      submitted->run_id, submitted->invocation_id, decision));
  f.idle();
  const auto& view = f.session->inspect_observations();
  REQUIRE(view.projection.current);
  REQUIRE(view.projection.current->submission == *submitted);
  if (decision.decision == ApprovalDecision::approved) {
    REQUIRE(view.projection.latest_success);
    REQUIRE(view.projection.current->status == RunStatus::completed);
  } else {
    REQUIRE_FALSE(view.projection.latest_success);
    REQUIRE(view.projection.current->status == RunStatus::failed);
    REQUIRE(f.source->calls == 0);
  }
  REQUIRE(count<InferenceStarted>(f.store.delegate.history) == 0);
}
TEST_CASE("Failed later observations preserve prior committed original-target "
          "evidence",
          "[ops-session]") {
  ops_session_test::Fixture f;
  f.open();
  f.bind();
  REQUIRE(f.session->submit_observation(f.intent()));
  f.idle();
  const auto previous =
      f.session->inspect_observations().projection.latest_success;
  REQUIRE(previous);
  f.source->fail = true;
  REQUIRE(f.session->submit_observation(f.intent()));
  f.idle();
  const auto& view = f.session->inspect_observations();
  REQUIRE(view.projection.current->status == RunStatus::failed);
  REQUIRE(view.projection.latest_success == previous);
}
TEST_CASE("Storage refusal cannot expose uncommitted evidence",
          "[ops-session]") {
  ops_session_test::Fixture f;
  f.open();
  f.bind();
  SECTION("admission") {
    f.store.delegate.reject = [](auto) { return true; };
    REQUIRE_FALSE(f.session->submit_observation(f.intent()));
  }
  SECTION("result") {
    f.store.delegate.reject = [](auto events) {
      return count<OpsObservationRecorded>(events) != 0;
    };
    REQUIRE(f.session->submit_observation(f.intent()));
    f.until(
        [&] { return f.session->inspect_observations().problem.has_value(); },
        true);
  }
  REQUIRE_FALSE(f.session->inspect_observations().projection.latest_success);
  REQUIRE_FALSE(f.session->inspect_observations().available);
  REQUIRE(count<OpsObservationRecorded>(f.store.delegate.history) == 0);
}
TEST_CASE(
    "Explicit close records cancellation and never closes a replacement issuer",
    "[ops-session]") {
  ops_session_test::Fixture f;
  f.open(runtime::ApprovalMode::prompt);
  f.bind();
  auto submitted = f.session->submit_observation(f.intent());
  REQUIRE(submitted);
  REQUIRE(f.session->close());
  REQUIRE(count<RunCancelled>(f.store.delegate.history) == 1);
  REQUIRE(f.session->inspect_observations().closed);
  REQUIRE_FALSE(f.session->submit_observation(f.intent()));
  auto replacement = f.broker->activate_session(f.specification.session_id);
  REQUIRE(replacement);
  f.session.reset();
  auto next = OpsObservationAuthority::create(f.specification).value();
  REQUIRE(f.broker->preflight_selection(**replacement, next, f.source));
}
TEST_CASE("Cancellation append refusal is reported by explicit close",
          "[ops-session]") {
  ops_session_test::Fixture f;
  f.open(runtime::ApprovalMode::prompt);
  f.bind();
  REQUIRE(f.session->submit_observation(f.intent()));
  f.store.delegate.reject = [](auto) { return true; };
  auto closed = f.session->close();
  REQUIRE_FALSE(closed);
  REQUIRE(closed.error().code == ManualOpsErrorCode::storage_failure);
  REQUIRE_FALSE(f.session->inspect_observations().projection.latest_success);
}
TEST_CASE("Inspection and repeated permitted reads never perform inference",
          "[ops-session]") {
  ops_session_test::Fixture f;
  f.open();
  f.bind();
  for (unsigned index = 0; index < 2; ++index) {
    auto submitted = f.session->submit_observation(f.intent());
    REQUIRE(submitted);
    f.idle();
    const auto& view = f.session->inspect_observations();
    REQUIRE(view.projection.current->submission == *submitted);
    REQUIRE(view.projection.current->observation_event_id);
    REQUIRE(view.projection.latest_success->submission == *submitted);
  }
  const auto calls = f.source->calls.load();
  const auto writes = f.store.delegate.attempts;
  for (unsigned index = 0; index < 10; ++index)
    REQUIRE(f.session->inspect_observations().projection.latest_success);
  REQUIRE(f.source->calls == calls);
  REQUIRE(f.store.delegate.attempts == writes);
  REQUIRE(count<InferenceStarted>(f.store.delegate.history) == 0);
  REQUIRE(f.store.opens == 0);
  REQUIRE(f.store.replays == 0);
}
TEST_CASE("Automatic Observe preserves its match ceiling and reports immediate "
          "denial",
          "[ops-session]") {
  ops_session_test::Fixture f;
  f.endpoint = f.broker->activate_session(f.specification.session_id).value();
  runtime::ToolRegistry registry;
  REQUIRE(runtime::register_ops_observation_tool(
      registry, OpsObservationAuthority::create(f.specification).value(),
      f.endpoint));
  auto tools = registry.snapshot().value();
  const auto* native = dynamic_cast<const runtime::OpsObservationTool*>(
      tools.find("observe_target")->executor.get());
  REQUIRE(native);
  auto prepared = native->prepare(id<InvocationId>("for-rule"), f.intent());
  REQUIRE(prepared);
  auto canonical =
      runtime::canonicalize_validated_tool_arguments(prepared->value);
  REQUIRE(canonical);
  auto matcher = runtime::compile_automatic_approval_matcher(
      {runtime::ExactToolArgumentsApprovalRule{
          "observe_target",
          *canonical,
          {{runtime::RestrictionLevel::high}, 1, {}, 0}}});
  REQUIRE(matcher);
  auto dependencies = f.dependencies();
  runtime::ApplicationLaunchContextConfiguration config;
  config.approval_mode = runtime::ApprovalMode::automatic;
  config.matcher_policy_identity = std::string{(*matcher)->identity()};
  dependencies.launch_policy.launch_context =
      runtime::make_application_launch_context(config).value();
  dependencies.launch_policy.automatic_matcher = *matcher;
  auto opened = OpsSession::open(f.request(), f.store, std::move(dependencies));
  REQUIRE(opened);
  f.session = std::move(*opened);
  REQUIRE(f.session->bind_observation(
      OpsObservationAuthority::create(f.specification).value(), f.source,
      f.endpoint));
  REQUIRE(f.session->submit_observation(f.intent()));
  f.idle();
  REQUIRE(f.session->inspect_observations().projection.latest_success);
  const auto first =
      f.session->inspect_observations().projection.latest_success;
  auto denied = f.session->submit_observation(f.intent());
  REQUIRE(denied);
  const auto& view = f.session->inspect_observations();
  REQUIRE(view.projection.current->submission == *denied);
  REQUIRE(view.projection.current->status == RunStatus::failed);
  REQUIRE_FALSE(view.projection.current->observation_event_id);
  REQUIRE(view.projection.latest_success == first);
  REQUIRE(f.source->calls == 1);
  REQUIRE(count<InferenceStarted>(f.store.delegate.history) == 0);
}

TEST_CASE("Fatal cancellation survives broker cleanup and repeated close",
          "[ops-session]") {
  ops_session_test::Fixture f;
  f.open(runtime::ApprovalMode::prompt);
  f.bind();
  auto submitted = f.session->submit_observation(f.intent());
  REQUIRE(submitted);
  f.store.delegate.reject = [](auto) { return true; };
  auto cancelled = f.session->cancel_observation(submitted->run_id);
  REQUIRE_FALSE(cancelled);
  REQUIRE(cancelled.error().code == ManualOpsErrorCode::storage_failure);
  auto approved = f.session->decide_observation_approval(
      submitted->run_id, submitted->invocation_id, {});
  REQUIRE_FALSE(approved);
  REQUIRE(approved.error().code == ManualOpsErrorCode::unavailable);
  SECTION("close immediately after cancellation refusal") {
  }
  SECTION("close after broker cleanup retires the failed run") {
    REQUIRE(f.broker->close());
    auto pumped = f.session->pump_observations();
    REQUIRE_FALSE(pumped);
    REQUIRE(pumped.error() == cancelled.error());
    REQUIRE_FALSE(f.session->inspect_observations().busy);
  }
  auto closed = f.session->close();
  REQUIRE_FALSE(closed);
  REQUIRE(closed.error() == cancelled.error());
  const auto writes = f.store.delegate.attempts;
  auto repeated = f.session->close();
  REQUIRE_FALSE(repeated);
  REQUIRE(repeated.error() == cancelled.error());
  REQUIRE(f.session->inspect_observations().problem == cancelled.error());
  REQUIRE(f.session->inspect_observations().closed);
  REQUIRE_FALSE(f.session->inspect_observations().available);
  REQUIRE(f.store.delegate.attempts == writes);
  REQUIRE(count<RunCancelled>(f.store.delegate.history) == 0);
  REQUIRE(f.source->calls == 0);
}
TEST_CASE(
    "Identity generator failure cannot append or launch a manual operation",
    "[ops-session]") {
  ops_session_test::Fixture f;
  auto dependencies = f.dependencies();
  dependencies.identity_suffix_source = []() -> std::uint64_t {
    throw std::runtime_error("private generator error");
  };
  auto opened = OpsSession::open(f.request(), f.store, std::move(dependencies));
  REQUIRE(opened);
  f.session = std::move(*opened);
  f.bind();
  auto submitted = f.session->submit_observation(f.intent());
  REQUIRE_FALSE(submitted);
  REQUIRE(submitted.error().code == ManualOpsErrorCode::internal_failure);
  REQUIRE(f.store.delegate.history.empty());
  REQUIRE(f.source->calls == 0);
  REQUIRE_FALSE(f.session->inspect_observations().available);
}
TEST_CASE("Failed owner binding preserves visible selection and old authority",
          "[ops-session]") {
  ops_session_test::Fixture f;
  f.open();
  f.bind();
  const auto previous = f.session->inspect_observations().selection;
  auto next = f.specification;
  ++next.selection_generation;
  next.target.target_id = id<OpsTargetId>("second");
  auto source = std::make_shared<Source>();
  source->binding = next.target;
  auto foreign = runtime::OpsObservationBroker::create(f.worker).value();
  auto endpoint = foreign->activate_session(next.session_id).value();
  auto rejected = f.session->bind_observation(
      OpsObservationAuthority::create(next).value(), source, endpoint);
  REQUIRE_FALSE(rejected);
  REQUIRE(f.session->inspect_observations().selection == previous);
  REQUIRE(f.source->calls == 0);
  REQUIRE(source->calls == 0);
  REQUIRE(f.session->submit_observation(f.intent()));
  f.idle();
  REQUIRE(f.session->inspect_observations().projection.latest_success);
}
TEST_CASE("Cancellation after dispatch is durable without allowing another "
          "active read",
          "[ops-session]") {
  ops_session_test::Fixture f;
  f.open();
  f.bind();
  f.source->blocked = true;
  auto submitted = f.session->submit_observation(f.intent());
  REQUIRE(submitted);
  f.until([&] { return f.source->calls.load() == 1; });
  REQUIRE_FALSE(f.session->cancel_observation(id<RunId>("foreign")));
  REQUIRE(f.session->cancel_observation(submitted->run_id));
  f.idle();
  REQUIRE(f.session->inspect_observations().projection.current->status ==
          RunStatus::cancelled);
  REQUIRE_FALSE(f.session->inspect_observations().projection.latest_success);
  REQUIRE(count<RunCancelled>(f.store.delegate.history) == 1);
  REQUIRE(count<InferenceStarted>(f.store.delegate.history) == 0);
}
