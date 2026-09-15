#include "fixture.hpp"
#include <limits>

using namespace admin_controller_test;

TEST_CASE(
    "Admin controller refuses invalid construction metadata before factory IO",
    "[admin][controller]") {
  Fixture f;
  auto owner = id<OpsOwnerId>("owner");
  std::shared_ptr<AdminSourceCatalog> catalog = f.catalog;
  auto worker = f.worker;
  SECTION("missing catalog") {
    catalog.reset();
  }
  SECTION("borrowed catalog") {
    f.catalog->owned = false;
  }
  SECTION("missing worker") {
    worker.reset();
  }
  SECTION("unsafe owner") {
    auto invalid = OpsOwnerId::from(std::string(1, static_cast<char>(0xff)));
    REQUIRE(invalid);
    owner = *invalid;
  }
  SECTION("empty catalog") {
    f.catalog->choices.clear();
  }
  SECTION("duplicate target") {
    f.catalog->choices.push_back(f.catalog->choices.front());
  }
  SECTION("unsafe target") {
    f.catalog->choices.front().id = id<OpsTargetId>("../target");
  }
  SECTION("unsafe label") {
    f.catalog->choices.front().display_name = "label\nsecond";
  }
  SECTION("long label") {
    f.catalog->choices.front().display_name.assign(129, 'x');
  }
  SECTION("unknown kind") {
    f.catalog->choices.front().kind = static_cast<OpsTargetKind>(99);
  }
  SECTION("too many targets") {
    for (unsigned i{2}; i < 34; ++i)
      f.catalog->choices.push_back(
          {id<OpsTargetId>("target-" + std::to_string(i)), "Target",
           OpsTargetKind::linux_local});
  }
  REQUIRE_FALSE(AdminController::create(owner, catalog, worker));
  CHECK(f.catalog->calls == 0);
  CHECK(f.catalog->state->prepared == 0);
}

TEST_CASE(
    "Admin catalog metadata is copied once within its exact count boundary",
    "[admin][controller]") {
  Fixture f;
  for (unsigned i{2}; i < 33; ++i)
    f.catalog->choices.push_back(
        {id<OpsTargetId>("target-" + std::to_string(i)), std::string(128, 'x'),
         OpsTargetKind::linux_local});
  f.open();
  REQUIRE(f.controller->inspect().targets.size() == 33);
  CHECK(f.catalog->calls == 0);
  REQUIRE(f.controller->execute(AdminInspect{}));
  CHECK(f.controller->inspect().visible);
  REQUIRE(f.controller->execute(AdminCloseView{}));
  CHECK_FALSE(f.controller->inspect().visible);
  CHECK(f.manual.pumps == 0);
  CHECK(f.catalog->state->observed == 0);
}

TEST_CASE("First unbound native selection is allowed while busy and closed "
          "sessions refuse",
          "[admin][controller]") {
  Fixture f;
  f.open();
  CHECK_FALSE(f.manual.inspection.available);
  SECTION("busy") {
    f.manual.inspection.busy = true;
    auto result =
        f.controller->execute(AdminSelectTarget{id<OpsTargetId>("alpha")});
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ManualOpsErrorCode::busy);
    CHECK(f.catalog->calls == 0);
  }
  SECTION("closed") {
    f.manual.inspection.closed = true;
    REQUIRE_FALSE(
        f.controller->execute(AdminSelectTarget{id<OpsTargetId>("alpha")}));
    CHECK(f.catalog->calls == 0);
  }
  SECTION("first selection") {
    f.select();
    f.ready();
    REQUIRE(f.controller->inspect().active_target);
    CHECK(f.controller->inspect().active_target->target_id ==
          id<OpsTargetId>("alpha"));
    REQUIRE(f.manual.authority);
    CHECK_FALSE(f.manual.authority->logs.enabled);
    CHECK(f.manual.authority->logs.permitted_sources.empty());
    CHECK(f.manual.authority->operations.size() == 3);
    CHECK(f.catalog->state->observed == 0);
  }
}

TEST_CASE("A superseding choice retires the exact old preparation without "
          "queued retry",
          "[admin][controller]") {
  Fixture f;
  f.open();
  auto old = f.catalog->state;
  old->gate = std::make_shared<Gate>();
  const Release release{old->gate};
  f.select();
  REQUIRE(old->gate->await());
  auto result =
      f.controller->execute(AdminSelectTarget{id<OpsTargetId>("beta")});
  REQUIRE_FALSE(result);
  CHECK(result.error().code == ManualOpsErrorCode::busy);
  CHECK(f.controller->inspect().phase == AdminPhase::retiring);
  CHECK_FALSE(f.controller->inspect().pending_target);
  CHECK(f.catalog->calls == 1);
  old->gate->release();
  REQUIRE(until([&] {
    REQUIRE(f.controller->pump());
    return f.worker->occupied_slots() == 0;
  }));
  CHECK(f.catalog->calls == 1);
  CHECK(f.binding.calls == 0);
  f.catalog->state = std::make_shared<PreparationState>();
  f.select("beta");
  f.ready();
  CHECK(f.controller->inspect().active_target->target_id ==
        id<OpsTargetId>("beta"));
}

TEST_CASE("Ready preparation stays producer-owned while the session is busy",
          "[admin][controller]") {
  Fixture f;
  f.open();
  f.select();
  REQUIRE(until([&] { return f.catalog->state->prepared != 0; }));
  const auto token = f.catalog->state->request().token;
  REQUIRE(until([&] {
    auto state = f.worker->preparation_state(token);
    return state && state->ready;
  }));
  f.manual.inspection.busy = true;
  for (unsigned i{}; i < 3; ++i)
    REQUIRE(f.controller->pump());
  auto state = f.worker->preparation_state(token);
  REQUIRE(state);
  CHECK(state->ready);
  CHECK(state->physically_outstanding);
  CHECK(f.binding.calls == 0);
  f.manual.inspection.busy = false;
  REQUIRE(f.controller->pump());
  CHECK(f.controller->inspect().phase == AdminPhase::retiring);
  CHECK(f.binding.calls == 0);
  f.ready();
  CHECK(f.binding.calls == 1);
}

TEST_CASE(
    "Busy ready preparation retains its original deadline and never binds late",
    "[admin][controller]") {
  Fixture f;
  f.open();
  f.select();
  REQUIRE(until([&] { return f.catalog->state->prepared != 0; }));
  const auto request = f.catalog->state->request();
  REQUIRE(until([&] {
    auto state = f.worker->preparation_state(request.token);
    return state && state->ready;
  }));
  f.manual.inspection.busy = true;
  REQUIRE(f.controller->pump());
  std::this_thread::sleep_until(request.deadline);
  auto result = f.controller->pump();
  REQUIRE_FALSE(result);
  CHECK(f.controller->inspect().source_problem ==
        runtime::OpsObservationSourceError::timed_out);
  f.manual.inspection.busy = false;
  REQUIRE(until([&] {
    static_cast<void>(f.controller->pump());
    return f.worker->occupied_slots() == 0;
  }));
  CHECK(f.binding.calls == 0);
  CHECK_FALSE(f.controller->inspect().active_target);
  CHECK(f.catalog->calls == 1);
}

TEST_CASE("Failed B preparation or binding preserves active A and its last "
          "good evidence",
          "[admin][controller]") {
  Fixture f;
  f.open();
  f.select();
  f.ready();
  f.capture(AdminReadHealth{});
  const auto target = f.controller->inspect().active_target;
  const auto snapshot = f.controller->inspect().snapshots[0];
  f.catalog->state = std::make_shared<PreparationState>();
  bool fatal{};
  SECTION("source fails") {
    f.catalog->state->failure =
        runtime::OpsObservationSourceError::permission_denied;
  }
  SECTION("source internal failure") {
    fatal = true;
    f.catalog->state->failure =
        runtime::OpsObservationSourceError::internal_failure;
  }
  SECTION("ordinary bind refusal") {
    f.binding.failure = ManualOpsFailure{ManualOpsErrorCode::busy};
  }
  SECTION("fatal bind refusal") {
    fatal = true;
    f.binding.failure = ManualOpsFailure{ManualOpsErrorCode::storage_failure};
  }
  f.select("beta");
  CHECK(f.controller->inspect().active_target == target);
  REQUIRE(until([&] {
    static_cast<void>(f.controller->pump());
    return f.controller->inspect().problem.has_value();
  }));
  CHECK(f.controller->inspect().active_target == target);
  CHECK(f.controller->inspect().snapshots[0] == snapshot);
  CHECK(f.controller->inspect().selection_generation == 1);
  CHECK(f.controller->inspect().fatal == fatal);
  if (fatal) {
    const auto calls = f.catalog->calls;
    REQUIRE_FALSE(
        f.controller->execute(AdminSelectTarget{id<OpsTargetId>("alpha")}));
    CHECK(f.catalog->calls == calls);
  } else {
    REQUIRE(until([&] {
      static_cast<void>(f.controller->pump());
      return f.worker->occupied_slots() == 0 &&
             f.controller->inspect().phase != AdminPhase::retiring;
    }));
    f.capture(AdminReadHealth{});
    CHECK(f.manual.intents.back().target_id == target->target_id);
  }
}

TEST_CASE("Cached service callbacks require exact session event generation "
          "target and row",
          "[admin][controller]") {
  Fixture f;
  f.open();
  f.select();
  f.ready();
  f.capture(AdminReadServices{});
  const auto& snapshot = *f.controller->inspect().snapshots[1];
  AdminReadCachedService action{f.session, snapshot.observation_event_id, 1, 0};
  SECTION("session") {
    action.session = id<SessionId>("foreign");
  }
  SECTION("event") {
    action.inventory_event = id<EventId>("foreign");
  }
  SECTION("generation") {
    ++action.selection_generation;
  }
  SECTION("row") {
    action.row = 1;
  }
  SECTION("target changed") {
    f.select("beta");
    f.ready();
  }
  SECTION("same target reselected") {
    f.select();
    f.ready();
  }
  const auto before = f.manual.intents.size();
  REQUIRE_FALSE(f.controller->execute(action));
  CHECK(f.manual.intents.size() == before);
}

TEST_CASE("Cached identity preserves invocation while explicit service input "
          "stays name-only",
          "[admin][controller]") {
  Fixture f;
  f.open();
  f.select();
  f.ready();
  f.capture(AdminReadServices{});
  const auto event = f.controller->inspect().snapshots[1]->observation_event_id;
  f.capture(AdminReadCachedService{f.session, event, 1, 0});
  CHECK(std::get<LinuxServiceIdentity>(f.manual.intents.back().resource)
            .invocation_id == id<OpsResourceUid>("invocation"));
  f.capture(AdminReadNamedService{"fixture.service"});
  CHECK_FALSE(std::get<LinuxServiceIdentity>(f.manual.intents.back().resource)
                  .invocation_id);
  const auto before = f.manual.intents.size();
  for (const auto* value : {"", "*.service", "../fixture.service",
                            "--all.service", "fixture\n.service"}) {
    REQUIRE_FALSE(f.controller->execute(AdminReadNamedService{value}));
    CHECK(f.manual.intents.size() == before);
  }
}

TEST_CASE(
    "Three bounded snapshots only change on exact committed current success",
    "[admin][controller]") {
  Fixture f;
  f.open();
  f.select();
  f.ready();
  f.capture(AdminReadHealth{});
  f.capture(AdminReadServices{});
  f.capture(AdminReadNamedService{"fixture.service"});
  const auto snapshots = f.controller->inspect().snapshots;
  for (const auto& value : snapshots)
    REQUIRE(value);
  REQUIRE(f.controller->pump());
  CHECK(f.controller->inspect().snapshots == snapshots);
  f.manual.fail_completion = true;
  REQUIRE(f.controller->execute(AdminReadHealth{}));
  REQUIRE(f.controller->pump());
  CHECK(f.controller->inspect().current_status == RunStatus::failed);
  CHECK(f.controller->inspect().snapshots == snapshots);
}

TEST_CASE(
    "Forged committed evidence fails before a snapshot becomes renderable",
    "[admin][controller]") {
  Fixture f;
  f.open();
  f.select();
  f.ready();
  f.manual.malformed = true;
  REQUIRE(f.controller->execute(AdminReadHealth{}));
  REQUIRE_FALSE(f.controller->pump());
  CHECK(f.controller->inspect().fatal);
  CHECK(f.controller->inspect().problem->code ==
        ManualOpsErrorCode::invalid_history);
  CHECK_FALSE(f.controller->inspect().snapshots[0]);
}

TEST_CASE("Closing view cancels its own approval and retains active target and "
          "evidence",
          "[admin][controller]") {
  Fixture f;
  f.open();
  f.select();
  f.ready();
  f.capture(AdminReadHealth{});
  const auto target = f.controller->inspect().active_target;
  const auto snapshot = f.controller->inspect().snapshots[0];
  f.manual.approval = true;
  REQUIRE(f.controller->execute(AdminInspect{}));
  REQUIRE(f.controller->execute(AdminReadServices{}));
  CHECK(f.controller->inspect().phase == AdminPhase::awaiting_approval);
  REQUIRE(f.controller->execute(AdminCloseView{}));
  CHECK_FALSE(f.controller->inspect().visible);
  CHECK(f.manual.cancels == 1);
  CHECK(f.controller->inspect().current_status == RunStatus::cancelled);
  CHECK(f.controller->inspect().active_target == target);
  CHECK(f.controller->inspect().snapshots[0] == snapshot);
  REQUIRE(f.controller->execute(AdminCloseView{}));
  CHECK(f.manual.cancels == 1);
}

TEST_CASE("Detach clears borrowed ports even if cancellation refuses or throws",
          "[admin][controller]") {
  Fixture f;
  f.open();
  f.select();
  f.ready();
  f.manual.hold = true;
  REQUIRE(f.controller->execute(AdminReadHealth{}));
  SECTION("refuses") {
    f.manual.cancel_failure =
        ManualOpsFailure{ManualOpsErrorCode::storage_failure};
  }
  SECTION("throws") {
    f.manual.throws_cancel = true;
  }
  REQUIRE_FALSE(f.controller->detach());
  CHECK_FALSE(f.controller->inspect().session);
  CHECK_FALSE(f.controller->inspect().current);
  CHECK_FALSE(f.controller->inspect().active_target);
  const auto calls = f.manual.pumps;
  const auto cancels = f.manual.cancels;
  REQUIRE(f.controller->pump());
  REQUIRE(f.controller->detach());
  CHECK(f.manual.pumps == calls);
  CHECK(f.manual.cancels == cancels);
  CHECK(f.controller->inspect().fatal);
}

TEST_CASE("Detached old preparation retires without touching replacement "
          "session ports",
          "[admin][controller]") {
  Fixture f;
  f.open();
  f.catalog->state->gate = std::make_shared<Gate>();
  const Release release{f.catalog->state->gate};
  f.select();
  REQUIRE(f.catalog->state->gate->await());
  REQUIRE(f.controller->detach());
  const auto old_calls = f.manual.pumps;
  Manual replacement;
  Binding binding{f.broker, replacement};
  const DetachOnExit detach{*f.controller};
  const auto next = id<SessionId>("replacement");
  auto endpoint = f.broker->activate_session(next);
  REQUIRE(endpoint);
  REQUIRE(f.controller->attach({next, replacement, binding, *endpoint}));
  f.catalog->state->gate->release();
  REQUIRE(until([&] {
    REQUIRE(f.controller->pump());
    return f.worker->occupied_slots() == 0;
  }));
  CHECK(f.manual.pumps == old_calls);
  CHECK(binding.calls == 0);
  CHECK(f.controller->inspect().session == next);
  REQUIRE(f.controller->detach());
}

TEST_CASE("Invalid attachment metadata leaves the current controller untouched",
          "[admin][controller]") {
  Fixture f;
  f.open();
  f.select();
  f.ready();
  f.capture(AdminReadHealth{});
  const auto target = f.controller->inspect().active_target;
  const auto snapshot = f.controller->inspect().snapshots[0];
  const auto epoch = f.controller->inspect().session_epoch;
  auto session = f.session;
  auto endpoint = f.endpoint;
  SECTION("unsafe session") {
    auto invalid = SessionId::from(std::string(1, static_cast<char>(0xff)));
    REQUIRE(invalid);
    session = *invalid;
  }
  SECTION("missing endpoint") {
    endpoint.reset();
  }
  REQUIRE_FALSE(f.controller->attach({session, f.manual, f.binding, endpoint}));
  CHECK(f.controller->inspect().session == f.session);
  CHECK(f.controller->inspect().session_epoch == epoch);
  CHECK(f.controller->inspect().active_target == target);
  CHECK(f.controller->inspect().snapshots[0] == snapshot);
  CHECK(f.manual.cancels == 0);
}

TEST_CASE("Detached view actions cannot create source work or call old ports",
          "[admin][controller]") {
  Fixture f;
  f.open();
  REQUIRE(f.controller->detach());
  REQUIRE(f.controller->execute(AdminInspect{}));
  CHECK(f.controller->inspect().visible);
  REQUIRE_FALSE(
      f.controller->execute(AdminSelectTarget{id<OpsTargetId>("alpha")}));
  REQUIRE_FALSE(f.controller->execute(AdminReadHealth{}));
  REQUIRE(f.controller->execute(AdminCloseView{}));
  REQUIRE(f.controller->pump());
  CHECK_FALSE(f.controller->inspect().visible);
  CHECK(f.manual.pumps == 0);
  CHECK(f.manual.cancels == 0);
  CHECK(f.catalog->calls == 0);
  CHECK(f.catalog->state->prepared == 0);
}

TEST_CASE(
    "Overlong Admin owner identity is refused by the typed input boundary",
    "[admin][controller]") {
  auto owner = OpsOwnerId::from(std::string(OpsOwnerId::max_size + 1, 'x'));
  REQUIRE_FALSE(owner);
  CHECK(owner.error() == IdError::too_long);
}
