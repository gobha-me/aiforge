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
    "Kubernetes selection grants only bounded namespace observation operations",
    "[admin][controller][kubernetes]") {
  Fixture f;
  f.catalog->enable_kubernetes();
  f.open();
  f.select("kube");
  f.ready();
  REQUIRE(f.manual.authority);
  CHECK(f.manual.authority->operations ==
        std::vector{OpsObservationOperation::kubernetes_workloads,
                    OpsObservationOperation::kubernetes_pod_health,
                    OpsObservationOperation::kubernetes_events});
  const auto* identity = std::get_if<KubernetesOpsIdentity>(
      &f.controller->inspect().active_target->identity);
  REQUIRE(identity);
  CHECK(identity->context_name == "fixture-context");
  CHECK(identity->namespace_name == "fixture-namespace");
  REQUIRE_FALSE(f.controller->execute(AdminReadHealth{}));
  CHECK(f.manual.intents.empty());
  f.capture(AdminReadWorkloads{});
  CHECK(f.manual.intents.back().operation ==
        OpsObservationOperation::kubernetes_workloads);
  CHECK(f.controller->inspect().freshness[3] ==
        AdminEvidenceFreshness::last_success);
}

TEST_CASE("Replayed target attaches as historical and requires reselection",
          "[admin][controller][replay]") {
  Fixture f;
  f.catalog->enable_kubernetes();
  const OpsTargetBinding historical{id<OpsTargetId>("kube"),
                                    id<OpsConfigurationRevision>("old-config"),
                                    KubernetesOpsIdentity{"old-context",
                                                          "old-namespace",
                                                          {"10.0.0.2", 6443},
                                                          "sha256:fixture"}};
  f.manual.inspection.selection = historical;
  f.manual.inspection.selection_generation = 7;
  f.manual.inspection.projection.historical_selection =
      runtime::RecordedOpsTargetSelection{id<RunId>("selection-7"),
                                          id<EventId>("selection-event-7"),
                                          historical, 7};
  f.manual.inspection.projection.maximum_selection_generation = 7;
  f.manual.inspection.source_connection =
      ManualOpsSourceConnection::historical_unverified;
  f.open();
  const auto& replayed = f.controller->inspect();
  CHECK_FALSE(replayed.active_target);
  REQUIRE(replayed.historical_target);
  CHECK(*replayed.historical_target == historical);
  CHECK(replayed.selection_generation == 7);
  CHECK(replayed.phase == AdminPhase::idle);
  CHECK_FALSE(f.controller->execute(AdminReadWorkloads{}));
  CHECK(f.manual.intents.empty());

  f.select("kube");
  f.ready();
  REQUIRE(f.controller->inspect().active_target);
  REQUIRE(f.controller->inspect().historical_target);
  CHECK(*f.controller->inspect().historical_target == historical);
  CHECK(f.controller->inspect().selection_generation == 8);
}

TEST_CASE("Historical catalog keeps A evidence beside selected B and replaces "
          "absent slots on session change",
          "[admin][controller][replay][catalog]") {
  Fixture f;
  f.open();
  f.select();
  f.ready();
  f.capture(AdminReadHealth{});
  const auto evidence = *f.manual.inspection.projection.catalog[0];
  auto selected = evidence.observation.request.target;
  selected.target_id = id<OpsTargetId>("beta");
  selected.configuration_revision =
      id<OpsConfigurationRevision>("beta-revision");
  f.manual.inspection.selection = selected;
  f.manual.inspection.selection_generation = 2;
  f.manual.inspection.projection.historical_selection =
      runtime::RecordedOpsTargetSelection{id<RunId>("selection-2"),
                                          id<EventId>("selection-event-2"),
                                          selected, 2};
  f.manual.inspection.projection.maximum_selection_generation = 2;
  f.manual.inspection.source_connection =
      ManualOpsSourceConnection::historical_unverified;
  f.manual.inspection.available = false;
  REQUIRE(f.controller->detach());
  REQUIRE(f.controller->attach({f.session, f.manual, f.binding, f.endpoint}));

  const auto& replayed = f.controller->inspect();
  REQUIRE(replayed.snapshots[0]);
  CHECK(replayed.snapshots[0]->observation.request.target != selected);
  CHECK(replayed.snapshots[0]->observation_event_id ==
        evidence.observation_event_id);
  CHECK(replayed.freshness[0] == AdminEvidenceFreshness::historical_unverified);
  REQUIRE(replayed.historical_target);
  CHECK(*replayed.historical_target == selected);
  CHECK_FALSE(replayed.active_target);
  f.manual.inspection.source_connection =
      ManualOpsSourceConnection::disconnected;
  REQUIRE(f.controller->pump());
  CHECK(f.controller->inspect().freshness[0] ==
        AdminEvidenceFreshness::historical_unverified);
  f.select("beta");
  f.ready();
  REQUIRE(f.controller->inspect().active_target);
  REQUIRE(f.controller->inspect().historical_target);
  CHECK(*f.controller->inspect().historical_target == selected);
  CHECK(f.controller->inspect().snapshots[0] == evidence);
  f.manual.inspection.source_connection =
      ManualOpsSourceConnection::disconnected;
  f.manual.inspection.available = false;
  REQUIRE(f.controller->pump());
  CHECK(f.controller->inspect().freshness[0] ==
        AdminEvidenceFreshness::historical_unverified);
  f.manual.inspection.source_connection = ManualOpsSourceConnection::connected;
  f.manual.inspection.available = true;
  f.capture(AdminReadHealth{});
  REQUIRE(f.controller->inspect().snapshots[0]);
  CHECK(f.controller->inspect().snapshots[0]->observation.request.target ==
        *f.controller->inspect().active_target);
  CHECK(f.controller->inspect().snapshots[0]->observation_event_id !=
        evidence.observation_event_id);
  CHECK(f.controller->inspect().freshness[0] ==
        AdminEvidenceFreshness::last_success);

  Manual replacement;
  Binding replacement_binding{f.broker, replacement};
  const auto next_session = id<SessionId>("replacement-session");
  auto next_endpoint = f.broker->activate_session(next_session);
  REQUIRE(next_endpoint);
  REQUIRE(f.controller->attach(
      {next_session, replacement, replacement_binding, *next_endpoint}));
  const auto& replaced = f.controller->inspect();
  CHECK(replaced.session == next_session);
  CHECK_FALSE(replaced.historical_target);
  CHECK(replaced.selection_generation == 0);
  for (std::size_t slot{}; slot < replaced.snapshots.size(); ++slot) {
    CHECK_FALSE(replaced.snapshots[slot]);
    CHECK(replaced.freshness[slot] == AdminEvidenceFreshness::unavailable);
  }
  REQUIRE(f.controller->detach());
}

TEST_CASE("Legacy observation generation seeds the next durable selection",
          "[admin][controller][replay][migration]") {
  Fixture f;
  f.manual.inspection.projection.maximum_selection_generation = 1;
  f.manual.inspection.selection_generation = 1;
  f.open();
  CHECK_FALSE(f.controller->inspect().historical_target);
  CHECK_FALSE(f.controller->inspect().active_target);
  CHECK(f.controller->inspect().selection_generation == 1);
  f.select();
  f.ready();
  REQUIRE(f.manual.authority);
  CHECK(f.manual.authority->selection_generation == 2);
}

TEST_CASE("Exhausted replay generation refuses source preparation",
          "[admin][controller][replay][bounds]") {
  Fixture f;
  const OpsTargetBinding historical{
      id<OpsTargetId>("alpha"), id<OpsConfigurationRevision>("old-config"),
      LinuxOpsIdentity{LinuxExecutionScope::container,
                       "12345678-1234-1234-1234-123456789abc", 42, 43}};
  f.manual.inspection.selection = historical;
  f.manual.inspection.projection.maximum_selection_generation =
      std::numeric_limits<std::uint64_t>::max();
  f.manual.inspection.selection_generation =
      std::numeric_limits<std::uint64_t>::max();
  f.manual.inspection.projection.historical_selection =
      runtime::RecordedOpsTargetSelection{
          id<RunId>("selection-max"), id<EventId>("selection-event-max"),
          historical, std::numeric_limits<std::uint64_t>::max()};
  f.manual.inspection.source_connection =
      ManualOpsSourceConnection::historical_unverified;
  f.open();
  const auto selected =
      f.controller->execute(AdminSelectTarget{id<OpsTargetId>("alpha")});
  REQUIRE_FALSE(selected);
  CHECK(selected.error().code == ManualOpsErrorCode::resource_exhausted);
  CHECK(f.catalog->calls == 0);
  CHECK(f.catalog->state->prepared == 0);
}

TEST_CASE("Malformed replacement catalog leaves current attachment untouched",
          "[admin][controller][replay][catalog]") {
  Fixture f;
  f.open();
  f.select();
  f.ready();
  f.capture(AdminReadHealth{});
  const auto before = f.controller->inspect();
  Manual replacement;
  SECTION("wrong operation slot") {
    replacement.inspection.projection.catalog[1] =
        f.manual.inspection.projection.catalog[0];
    replacement.inspection.projection.latest_success =
        replacement.inspection.projection.catalog[1];
  }
  SECTION("foreign owner") {
    auto evidence = *f.manual.inspection.projection.catalog[0];
    evidence.observation.request.owner_id = id<OpsOwnerId>("foreign-owner");
    replacement.inspection.projection.catalog[0] = evidence;
    replacement.inspection.projection.latest_success = std::move(evidence);
    replacement.inspection.projection.maximum_selection_generation = 1;
    replacement.inspection.selection_generation = 1;
  }
  SECTION("connected selection missing durable projection") {
    replacement.inspection.selection = f.controller->inspect().active_target;
    replacement.inspection.selection_generation = 1;
    replacement.inspection.projection.maximum_selection_generation = 1;
    replacement.inspection.source_connection =
        ManualOpsSourceConnection::connected;
  }
  Binding replacement_binding{f.broker, replacement};
  const auto attached = f.controller->attach(
      {f.session, replacement, replacement_binding, f.endpoint});
  REQUIRE_FALSE(attached);
  CHECK(attached.error().code == ManualOpsErrorCode::invalid_history);
  CHECK(f.controller->inspect().session == before.session);
  CHECK(f.controller->inspect().active_target == before.active_target);
  CHECK(f.controller->inspect().snapshots == before.snapshots);
}

TEST_CASE("Cached Kubernetes Pod reads retain inventory identity and reject "
          "stale rows",
          "[admin][controller][kubernetes]") {
  Fixture f;
  f.catalog->enable_kubernetes();
  f.open();
  f.select("kube");
  f.ready();
  f.capture(AdminReadWorkloads{});
  const auto inventory = *f.controller->inspect().snapshots[3];
  const AdminReadCachedPod pod{f.session, inventory.observation_event_id, 1, 0};
  f.capture(pod);
  REQUIRE(std::holds_alternative<KubernetesPodIdentity>(
      f.manual.intents.back().resource));
  const auto identity =
      std::get<KubernetesPodIdentity>(f.manual.intents.back().resource);
  CHECK(identity.namespace_name == "fixture-namespace");
  CHECK(identity.name == "failed-pod");
  CHECK(identity.uid == id<OpsResourceUid>("pod-uid"));
  f.capture(AdminReadCachedPodEvents{f.session, inventory.observation_event_id,
                                     1, 0});
  CHECK(f.manual.intents.back().operation ==
        OpsObservationOperation::kubernetes_events);
  CHECK(f.manual.intents.back().resource == OpsResourceIdentity{identity});

  const auto before = f.manual.intents.size();
  REQUIRE_FALSE(f.controller->execute(
      AdminReadCachedPod{f.session, inventory.observation_event_id, 1, 1}));
  CHECK(f.manual.intents.size() == before);
  REQUIRE_FALSE(f.controller->execute(
      AdminReadCachedPod{f.session, inventory.observation_event_id, 2, 0}));
  CHECK(f.manual.intents.size() == before);
}

TEST_CASE("Kubernetes namespace and named Pod events preserve exact scope",
          "[admin][controller][kubernetes]") {
  Fixture f;
  f.catalog->enable_kubernetes();
  f.open();
  f.select("kube");
  f.ready();
  f.capture(AdminReadEvents{});
  CHECK(
      std::holds_alternative<std::monostate>(f.manual.intents.back().resource));
  f.capture(
      AdminReadNamedPodEvents{"failed-pod", id<OpsResourceUid>("pod-uid")});
  const auto& pod =
      std::get<KubernetesPodIdentity>(f.manual.intents.back().resource);
  CHECK(pod.namespace_name == "fixture-namespace");
  CHECK(pod.name == "failed-pod");
  CHECK(pod.uid == id<OpsResourceUid>("pod-uid"));
}

TEST_CASE("Displayed resource refresh proof rejects every target change",
          "[admin][controller][authority]") {
  Fixture f;
  std::size_t slot{};
  f.catalog->enable_kubernetes();
  f.open();
  SECTION("service details") {
    f.select();
    f.ready();
    f.capture(AdminReadNamedService{"fixture.service"});
    slot = 2;
  }
  SECTION("Pod details") {
    f.select("kube");
    f.ready();
    f.capture(AdminReadNamedPod{"failed-pod", id<OpsResourceUid>("pod-uid")});
    slot = 4;
  }
  SECTION("namespace events") {
    f.select("kube");
    f.ready();
    f.capture(AdminReadEvents{});
    slot = 5;
  }
  SECTION("exact Pod events") {
    f.select("kube");
    f.ready();
    f.capture(
        AdminReadNamedPodEvents{"failed-pod", id<OpsResourceUid>("pod-uid")});
    slot = 5;
  }
  const auto& snapshot = *f.controller->inspect().snapshots[slot];
  const auto& request = snapshot.observation.request;
  AdminRefreshDisplayed displayed{request.session_id,
                                  request.target,
                                  request.selection_generation,
                                  snapshot.observation_event_id,
                                  request.operation,
                                  request.resource};
  const auto original_target = displayed.target;
  const auto original_resource = displayed.resource;
  f.capture(displayed);
  CHECK(f.manual.intents.back().operation == displayed.operation);
  CHECK(f.manual.intents.back().resource == original_resource);
  const auto replacement =
      original_target.target_id == id<OpsTargetId>("beta") ? "alpha" : "beta";
  f.select(replacement);
  f.ready();
  const auto before = f.manual.intents.size();
  auto refreshed = f.controller->execute(displayed);
  REQUIRE_FALSE(refreshed);
  CHECK(refreshed.error().code == ManualOpsErrorCode::wrong_operation);
  CHECK(f.manual.intents.size() == before);
  CHECK(displayed.target == original_target);
  CHECK(displayed.resource == original_resource);
}

TEST_CASE("Detach preserves truthful freshness without inventing a disconnect",
          "[admin][controller][kubernetes]") {
  Fixture f;
  f.catalog->enable_kubernetes();
  f.open();
  f.select("kube");
  f.ready();
  f.capture(AdminReadWorkloads{});
  const auto snapshot = f.controller->inspect().snapshots[3];
  f.manual.hold = true;
  REQUIRE(f.controller->execute(AdminReadWorkloads{}));
  CHECK(f.controller->inspect().freshness[3] ==
        AdminEvidenceFreshness::refreshing);
  CHECK(f.controller->inspect().snapshots[3] == snapshot);
  f.manual.hold = false;
  f.manual.fail_completion = true;
  REQUIRE(f.controller->pump());
  CHECK(f.controller->inspect().freshness[3] ==
        AdminEvidenceFreshness::refresh_failed);
  CHECK(f.controller->inspect().snapshots[3] == snapshot);
  REQUIRE(f.controller->detach());
  CHECK(f.controller->inspect().freshness[3] ==
        AdminEvidenceFreshness::refresh_failed);
  CHECK(f.controller->inspect().snapshots[3] == snapshot);
  CHECK(f.controller->inspect().phase == AdminPhase::detached);
}

TEST_CASE("Detach preserves last-success freshness and retained evidence",
          "[admin][controller][kubernetes]") {
  Fixture f;
  f.catalog->enable_kubernetes();
  f.open();
  f.select("kube");
  f.ready();
  f.capture(AdminReadWorkloads{});
  const auto snapshot = f.controller->inspect().snapshots[3];
  REQUIRE(f.controller->inspect().freshness[3] ==
          AdminEvidenceFreshness::last_success);
  f.manual.hold = true;
  REQUIRE(f.controller->execute(AdminReadWorkloads{}));
  REQUIRE(f.controller->inspect().freshness[3] ==
          AdminEvidenceFreshness::refreshing);
  REQUIRE(f.controller->inspect().snapshots[3] == snapshot);
  REQUIRE(f.controller->detach());
  CHECK(f.manual.cancels == 1);
  CHECK(f.controller->inspect().freshness[3] ==
        AdminEvidenceFreshness::last_success);
  CHECK(f.controller->inspect().snapshots[3] == snapshot);
  CHECK(f.controller->inspect().phase == AdminPhase::detached);
}

TEST_CASE("Source disconnection preserves evidence but marks it disconnected",
          "[admin][controller][kubernetes][failure]") {
  Fixture f;
  f.catalog->enable_kubernetes();
  f.open();
  f.select("kube");
  f.ready();
  f.capture(AdminReadWorkloads{});
  const auto snapshot = f.controller->inspect().snapshots[3];
  REQUIRE(f.broker->close());
  f.manual.inspection.available = false;
  f.manual.inspection.source_connection =
      ManualOpsSourceConnection::disconnected;
  f.manual.pump_failure = ManualOpsFailure{ManualOpsErrorCode::unavailable};
  REQUIRE_FALSE(f.controller->pump());
  CHECK(f.controller->inspect().freshness[3] ==
        AdminEvidenceFreshness::disconnected);
  CHECK(f.controller->inspect().snapshots[3] == snapshot);
  REQUIRE(f.controller->detach());
  CHECK(f.controller->inspect().freshness[3] ==
        AdminEvidenceFreshness::disconnected);
  const auto reads = f.manual.intents.size();
  REQUIRE_FALSE(f.controller->execute(AdminReadWorkloads{}));
  CHECK(f.manual.intents.size() == reads);
}

TEST_CASE("Non-source session failures never relabel retained evidence as "
          "disconnected",
          "[admin][controller][kubernetes][failure]") {
  using Code = ManualOpsErrorCode;
  for (const auto code : {Code::storage_failure, Code::invalid_history,
                          Code::internal_failure, Code::closed}) {
    Fixture f;
    f.catalog->enable_kubernetes();
    f.open();
    f.select("kube");
    f.ready();
    f.capture(AdminReadWorkloads{});
    f.manual.inspection.available = false;
    f.manual.inspection.problem = ManualOpsFailure{code};
    f.manual.pump_failure = ManualOpsFailure{code};
    REQUIRE_FALSE(f.controller->pump());
    CHECK(f.controller->inspect().freshness[3] ==
          AdminEvidenceFreshness::last_success);
  }
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

TEST_CASE("Successful target selection preserves retained evidence freshness",
          "[admin][controller]") {
  Fixture f;
  f.open();
  f.select();
  f.ready();
  f.capture(AdminReadHealth{});
  const auto snapshot = f.controller->inspect().snapshots[0];
  REQUIRE(f.controller->inspect().freshness[0] ==
          AdminEvidenceFreshness::last_success);

  std::string expected_target;
  SECTION("same target reselected") {
    expected_target = "alpha";
    f.select();
  }
  SECTION("different target selected") {
    expected_target = "beta";
    f.select("beta");
  }
  f.ready();

  REQUIRE(f.controller->inspect().active_target);
  CHECK(f.controller->inspect().active_target->target_id ==
        id<OpsTargetId>(expected_target));
  CHECK(f.controller->inspect().selection_generation == 2);
  CHECK(f.controller->inspect().snapshots[0] == snapshot);
  CHECK(f.controller->inspect().freshness[0] ==
        AdminEvidenceFreshness::last_success);
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
  for (std::size_t slot{}; slot < 3; ++slot)
    REQUIRE(snapshots[slot]);
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
  const auto freshness = f.controller->inspect().freshness[0];
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
  CHECK(f.controller->inspect().freshness[0] == freshness);
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
  REQUIRE_FALSE(f.controller->inspect().snapshots[0]);
  REQUIRE(f.controller->inspect().freshness[0] ==
          AdminEvidenceFreshness::refreshing);
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
  CHECK_FALSE(f.controller->inspect().snapshots[0]);
  CHECK(f.controller->inspect().freshness[0] ==
        AdminEvidenceFreshness::unavailable);
  const auto calls = f.manual.pumps;
  const auto cancels = f.manual.cancels;
  REQUIRE(f.controller->pump());
  REQUIRE(f.controller->detach());
  CHECK(f.manual.pumps == calls);
  CHECK(f.manual.cancels == cancels);
  CHECK(f.controller->inspect().fatal);
}

TEST_CASE("Detach failure preserves retained evidence after cancellation "
          "refusal or exception",
          "[admin][controller][failure]") {
  Fixture f;
  f.open();
  f.select();
  f.ready();
  f.capture(AdminReadHealth{});
  const auto snapshot = f.controller->inspect().snapshots[0];
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
  CHECK_FALSE(f.controller->inspect().active_target);
  CHECK(f.controller->inspect().snapshots[0] == snapshot);
  CHECK(f.controller->inspect().freshness[0] ==
        AdminEvidenceFreshness::last_success);
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
