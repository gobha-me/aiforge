#include <aiforge/adapters/admin_dialog.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {
using namespace aiforge;
using namespace aiforge::surfaces;
template <class T> auto id(std::string value) -> T {
  auto result = T::from(std::move(value));
  REQUIRE(result);
  return std::move(*result);
}
class Controls final : public AdminControls {
 public:
  AdminState state;
  std::vector<AdminAction> actions;
  bool refuse{};
  Controls() {
    state.targets.push_back({id<domain::OpsTargetId>("local"), "Local Linux",
                             domain::OpsTargetKind::linux_local});
    state.session = id<domain::SessionId>("session");
    state.session_epoch = 1;
    state.phase = AdminPhase::idle;
  }
  auto execute(const AdminAction& value)
      -> std::expected<void, ManualOpsFailure> override {
    actions.push_back(value);
    if (refuse)
      return std::unexpected(ManualOpsFailure{ManualOpsErrorCode::busy});
    return {};
  }
  auto inspect() const noexcept -> const AdminState& override { return state; }
};
auto key(char32_t character) -> termforge::Event {
  return termforge::KeyEvent{termforge::Key::Char, character};
}
auto render(adapters::AdminDialog& dialog, int cols, int rows) -> std::string {
  termforge::Screen screen{cols, rows};
  dialog.draw(screen);
  std::string result;
  for (int row = 0; row < rows; ++row) {
    for (int col = 0; col < cols; ++col)
      result += screen.text_at(col, row);
    result += '\n';
  }
  return result;
}
auto click(adapters::AdminDialog& dialog, std::string_view label) -> bool {
  termforge::Screen screen{120, 32};
  dialog.draw(screen);
  for (int row = 0; row < 32; ++row)
    for (int col = 0; col < 120; ++col) {
      std::string text;
      for (int i = 0; i < static_cast<int>(label.size()) && col + i < 120; ++i)
        text += screen.text_at(col + i, row);
      if (text == label)
        return dialog.on_event(termforge::MouseEvent{col, row, 0, true});
    }
  return false;
}
auto committed(std::string event = "observed") -> CommittedOpsObservation {
  domain::OpsObservationRequest request{
      id<domain::OpsOwnerId>("owner"),
      id<domain::SessionId>("session"),
      id<domain::OpsRequestId>("request"),
      {id<domain::OpsTargetId>("local"),
       id<domain::OpsConfigurationRevision>("revision"),
       domain::LinuxOpsIdentity{domain::LinuxExecutionScope::container,
                                "12345678-1234-1234-1234-123456789abc", 42,
                                43}},
      7,
      domain::OpsObservationOperation::linux_services,
      {},
      1,
      {}};
  domain::OpsObservation observation{
      request,
      domain::EventTimestamp{std::chrono::milliseconds{1000}},
      domain::EventTimestamp{std::chrono::milliseconds{1001}},
      domain::OpsObservationCompleteness::complete,
      0,
      0,
      {},
      domain::LinuxServicesObservation{
          {{{"example.service", {}},
            domain::OpsServiceState::failed,
            domain::OpsObservationReason::failed_exit,
            7,
            3}}}};
  REQUIRE(domain::validate_recorded_ops_observation(observation));
  return {{id<domain::RunId>("run"), id<domain::InvocationId>("invocation")},
          id<domain::EventId>(std::move(event)),
          id<domain::EventId>("result"),
          std::move(observation)};
}
auto kube_committed(std::string event = "kube-observed")
    -> CommittedOpsObservation {
  domain::OpsObservationRequest request{
      id<domain::OpsOwnerId>("owner"),
      id<domain::SessionId>("session"),
      id<domain::OpsRequestId>("request"),
      {id<domain::OpsTargetId>("cluster"),
       id<domain::OpsConfigurationRevision>("revision"),
       domain::KubernetesOpsIdentity{
           "home", "apps", {"10.0.0.2", 6443}, "sha256:fixture"}},
      9,
      domain::OpsObservationOperation::kubernetes_workloads,
      {},
      1,
      {}};
  domain::OpsObservation observation{
      request,
      domain::EventTimestamp{std::chrono::milliseconds{1000}},
      domain::EventTimestamp{std::chrono::milliseconds{2000}},
      domain::OpsObservationCompleteness::complete,
      0,
      0,
      "rv-1",
      domain::KubernetesWorkloadsObservation{
          {{{domain::OpsWorkloadKind::pod, "apps", "broken-pod",
             id<domain::OpsResourceUid>("pod-uid")},
            domain::OpsHealthState::unhealthy,
            1,
            0,
            1},
           {{domain::OpsWorkloadKind::deployment, "apps", "web",
             id<domain::OpsResourceUid>("deployment-uid")},
            domain::OpsHealthState::degraded,
            2,
            1,
            2}}}};
  REQUIRE(domain::validate_recorded_ops_observation(observation));
  return {{id<domain::RunId>("run"), id<domain::InvocationId>("invocation")},
          id<domain::EventId>(std::move(event)),
          id<domain::EventId>("result"),
          std::move(observation)};
}
auto service_details_committed() -> CommittedOpsObservation {
  auto result = committed("service-details");
  result.observation.request.operation =
      domain::OpsObservationOperation::linux_service_health;
  result.observation.request.resource =
      domain::LinuxServiceIdentity{"example.service", {}};
  result.observation.payload =
      domain::LinuxServiceObservation{{"example.service", {}},
                                      domain::OpsServiceState::failed,
                                      domain::OpsObservationReason::failed_exit,
                                      7,
                                      3};
  REQUIRE(domain::validate_recorded_ops_observation(result.observation));
  return result;
}
auto kube_pod_committed() -> CommittedOpsObservation {
  auto result = kube_committed("pod-details");
  const domain::KubernetesPodIdentity pod{
      "apps", "broken-pod", id<domain::OpsResourceUid>("pod-uid"), {}};
  result.observation.request.operation =
      domain::OpsObservationOperation::kubernetes_pod_health;
  result.observation.request.resource = pod;
  result.observation.payload =
      domain::KubernetesPodObservation{pod, domain::OpsPodPhase::failed, {}};
  REQUIRE(domain::validate_recorded_ops_observation(result.observation));
  return result;
}
auto kube_events_committed(bool exact_pod) -> CommittedOpsObservation {
  auto result = kube_committed(exact_pod ? "pod-events" : "namespace-events");
  result.observation.request.operation =
      domain::OpsObservationOperation::kubernetes_events;
  if (exact_pod)
    result.observation.request.resource = domain::KubernetesPodIdentity{
        "apps", "broken-pod", id<domain::OpsResourceUid>("pod-uid"), {}};
  result.observation.payload = domain::KubernetesEventsObservation{{}};
  REQUIRE(domain::validate_recorded_ops_observation(result.observation));
  return result;
}
} // namespace
TEST_CASE("Admin rendering and view navigation send no operation",
          "[admin][dialog]") {
  Controls controls;
  adapters::AdminDialog dialog{controls};
  for (const auto& [cols, rows] :
       {std::pair{120, 32}, {80, 24}, {40, 12}, {20, 5}, {2, 2}, {1, 1}}) {
    for (const bool toolbar : {true, false}) {
      dialog.set_toolbar_visible(toolbar);
      for (const char32_t view : {U't', U'h', U's', U'd'}) {
        REQUIRE(dialog.on_event(key(view)));
        REQUIRE(dialog.refresh());
        CHECK_FALSE(render(dialog, cols, rows).empty());
      }
    }
  }
  CHECK(controls.actions.empty());
  REQUIRE(dialog.on_event(termforge::KeyEvent{termforge::Key::Escape}));
  REQUIRE(controls.actions.size() == 1);
  CHECK(std::holds_alternative<AdminCloseView>(controls.actions.front()));
}
TEST_CASE("Admin explicit buttons retain typed intent and failed status",
          "[admin][dialog]") {
  Controls controls;
  adapters::AdminDialog dialog{controls};
  REQUIRE(click(dialog, "[ Use target ]"));
  REQUIRE(controls.actions.size() == 1);
  CHECK(std::get<AdminSelectTarget>(controls.actions.back()).target.value() ==
        "local");
  REQUIRE(dialog.on_event(key(U'h')));
  REQUIRE(click(dialog, "[ Read ]"));
  REQUIRE(controls.actions.size() == 2);
  CHECK(std::holds_alternative<AdminReadHealth>(controls.actions.back()));
  REQUIRE(dialog.on_event(key(U's')));
  REQUIRE(click(dialog, "[ Read ]"));
  CHECK(std::holds_alternative<AdminReadServices>(controls.actions.back()));
  controls.refuse = true;
  CHECK_FALSE(dialog.execute(AdminReadHealth{}));
  CHECK(dialog.status() == "Session or source worker is busy");
  REQUIRE(click(dialog, "[ Cancel ]"));
  CHECK(std::holds_alternative<AdminCancel>(controls.actions.back()));
}
TEST_CASE("Admin service actions keep the displayed snapshot identity",
          "[admin][dialog]") {
  Controls controls;
  controls.state.snapshots[1] = committed();
  adapters::AdminDialog dialog{controls};
  REQUIRE(dialog.on_event(key(U's')));
  CHECK(dialog.display_text().find("observed") != std::string::npos);
  CHECK(dialog.display_text().find("example.service") != std::string::npos);
  // Replace the controller cache without refreshing the displayed row.
  controls.state.snapshots[1] = committed("replacement");
  REQUIRE(click(dialog, "example.service"));
  REQUIRE_FALSE(controls.actions.empty());
  const auto& action =
      std::get<AdminReadCachedService>(controls.actions.back());
  CHECK(action.session.value() == "session");
  CHECK(action.inventory_event.value() == "observed");
  CHECK(action.selection_generation == 7);
  CHECK(action.row == 0);
}

TEST_CASE("Admin selected-resource menus reject the opposite inventory kind",
          "[admin][dialog]") {
  SECTION("Pod menu rejects a service row") {
    Controls controls;
    controls.state.snapshots[1] = committed();
    adapters::AdminDialog dialog{controls};
    REQUIRE(dialog.on_event(key(U's')));
    REQUIRE(click(dialog, "Read"));
    REQUIRE(click(dialog, "Selected Pod"));
    CHECK(controls.actions.empty());
    CHECK(dialog.status() == "Read workloads and choose a Pod first");
  }
  SECTION("service menu rejects a Pod row") {
    Controls controls;
    controls.state.snapshots[3] = kube_committed();
    adapters::AdminDialog dialog{controls};
    REQUIRE(dialog.on_event(key(U'w')));
    REQUIRE(click(dialog, "Read"));
    REQUIRE(click(dialog, "Selected service"));
    CHECK(controls.actions.empty());
    CHECK(dialog.status() == "Read loaded services and choose a service first");
  }
}
TEST_CASE("Admin preserves evidence identity and hides malformed labels",
          "[admin][dialog]") {
  Controls controls;
  controls.state.targets.front().display_name = "bad\033[31m";
  controls.state.snapshots[1] = committed();
  controls.state.pending_target = id<domain::OpsTargetId>("other");
  adapters::AdminDialog dialog{controls};
  REQUIRE(dialog.on_event(key(U's')));
  const auto before = dialog.display_text();
  controls.refuse = true;
  CHECK_FALSE(dialog.execute(AdminReadServices{}));
  CHECK(dialog.display_text() == before);
  CHECK(render(dialog, 120, 32).find('\033') == std::string::npos);
  ++controls.state.session_epoch;
  controls.state.snapshots = {};
  REQUIRE(dialog.refresh());
  CHECK(dialog.display_text().find("example.service") == std::string::npos);
}

TEST_CASE("Admin close is dispatched once per showing", "[admin][dialog]") {
  Controls controls;
  adapters::AdminDialog dialog{controls};
  unsigned closed{};
  dialog.on_close([&closed] { ++closed; });
  static_cast<void>(render(dialog, 120, 32));
  REQUIRE(dialog.on_event(termforge::KeyEvent{termforge::Key::Escape}));
  REQUIRE(dialog.on_event(termforge::KeyEvent{termforge::Key::Escape}));
  CHECK(closed == 1);
  REQUIRE(controls.actions.size() == 1);
  CHECK(std::holds_alternative<AdminCloseView>(controls.actions.front()));
  static_cast<void>(render(dialog, 120, 32));
  REQUIRE(dialog.on_event(termforge::KeyEvent{termforge::Key::Escape}));
  CHECK(closed == 2);
  CHECK(controls.actions.size() == 2);
}

TEST_CASE("Admin F10 restores and focuses the hidden menu", "[admin][dialog]") {
  Controls controls;
  adapters::AdminDialog dialog{controls};
  dialog.set_toolbar_visible(false);
  static_cast<void>(render(dialog, 120, 32));
  REQUIRE(dialog.on_event(termforge::KeyEvent{termforge::Key::F10}));
  static_cast<void>(render(dialog, 120, 32));
  REQUIRE(dialog.on_event(termforge::KeyEvent{termforge::Key::Enter}));
  CHECK(render(dialog, 120, 32).find("Use highlighted target") !=
        std::string::npos);
  CHECK(controls.actions.empty());
  REQUIRE(dialog.on_event(termforge::KeyEvent{termforge::Key::Escape}));
  CHECK(controls.actions.empty());
}

TEST_CASE("Malformed Admin evidence cannot create a service action",
          "[admin][dialog]") {
  Controls controls;
  auto invalid = committed();
  auto& services =
      std::get<domain::LinuxServicesObservation>(invalid.observation.payload);
  services.services.front().identity.unit_name = "bad\033.service";
  controls.state.snapshots[1] = std::move(invalid);
  adapters::AdminDialog dialog{controls};
  REQUIRE(dialog.on_event(key(U's')));
  CHECK(dialog.display_text() ==
        "Committed observation could not be rendered.");
  CHECK(render(dialog, 120, 32).find('\033') == std::string::npos);
  REQUIRE(dialog.on_event(key(U'd')));
  REQUIRE(click(dialog, "[ Read ]"));
  CHECK(controls.actions.empty());
  CHECK(dialog.status() == "Read loaded services and choose a service first");
}

TEST_CASE("Admin Kubernetes views preserve source identity freshness and "
          "cached Pod rows",
          "[admin][dialog][kubernetes]") {
  Controls controls;
  controls.state.targets.push_back({id<domain::OpsTargetId>("cluster"),
                                    "Private cluster",
                                    domain::OpsTargetKind::kubernetes});
  auto snapshot = kube_committed();
  controls.state.active_target = snapshot.observation.request.target;
  controls.state.snapshots[3] = snapshot;
  controls.state.freshness[3] = AdminEvidenceFreshness::refresh_failed;
  auto now = std::chrono::milliseconds{7000};
  adapters::AdminDialog dialog{controls,
                               [&now] { return domain::EventTimestamp{now}; }};
  REQUIRE(dialog.on_event(key(U'w')));
  CHECK(dialog.display_text().find("captured_ms 2000") != std::string::npos);
  CHECK(dialog.display_text().find("age 5s") != std::string::npos);
  CHECK(dialog.display_text().find("freshness refresh failed") !=
        std::string::npos);
  const auto tiny = render(dialog, 20, 5);
  CHECK(tiny.find("cluster") != std::string::npos);
  CHECK(tiny.find("home") != std::string::npos);
  CHECK(tiny.find("apps") != std::string::npos);
  CHECK(tiny.find("refresh failed") != std::string::npos);
  const auto frame = render(dialog, 120, 32);
  CHECK(frame.find("context home") != std::string::npos);
  CHECK(frame.find("namespace apps") != std::string::npos);
  CHECK(frame.find("https://10.0.0.2:6443") != std::string::npos);
  now = std::chrono::milliseconds{9000};
  REQUIRE(dialog.refresh());
  CHECK(dialog.display_text().find("age 7s") != std::string::npos);
  REQUIRE(click(dialog, "Pod broken-pod"));
  REQUIRE(std::holds_alternative<AdminReadCachedPod>(controls.actions.back()));
  const auto pod = std::get<AdminReadCachedPod>(controls.actions.back());
  CHECK(pod.session.value() == "session");
  CHECK(pod.inventory_event.value() == "kube-observed");
  CHECK(pod.selection_generation == 9);
  CHECK(pod.row == 0);

  REQUIRE(dialog.on_event(key(U'w')));
  const auto before = controls.actions.size();
  REQUIRE(click(dialog, "Deployment web"));
  CHECK(controls.actions.size() == before);
  CHECK(dialog.status().find("inspectable service or Pod") !=
        std::string::npos);
  now = std::chrono::milliseconds{11000};
  REQUIRE(dialog.refresh());
  REQUIRE(dialog.on_event(termforge::KeyEvent{termforge::Key::Enter}));
  CHECK(controls.actions.size() == before);
}

TEST_CASE("Admin detach and source disconnect remain visibly distinct",
          "[admin][dialog][kubernetes][failure]") {
  Controls controls;
  auto snapshot = kube_committed();
  controls.state.snapshots[3] = std::move(snapshot);
  controls.state.phase = AdminPhase::detached;
  controls.state.freshness[3] = AdminEvidenceFreshness::last_success;
  adapters::AdminDialog dialog{controls};
  REQUIRE(dialog.on_event(key(U'w')));
  auto tiny = render(dialog, 20, 5);
  CHECK(tiny.find("last success") != std::string::npos);
  CHECK(tiny.find("status detached") != std::string::npos);
  CHECK(tiny.find("disconnected") == std::string::npos);

  controls.state.freshness[3] = AdminEvidenceFreshness::disconnected;
  REQUIRE(dialog.refresh());
  tiny = render(dialog, 20, 5);
  CHECK(tiny.find("disconnected") != std::string::npos);
  CHECK(tiny.find("status detached") != std::string::npos);
}

TEST_CASE("Admin compact view keeps active target distinct from historical "
          "evidence freshness",
          "[admin][dialog][history]") {
  Controls controls;
  auto historical = committed();
  historical.observation.request.target.target_id =
      id<domain::OpsTargetId>("alpha");
  controls.state.snapshots[1] = historical;
  controls.state.freshness[1] = AdminEvidenceFreshness::last_success;
  auto active = historical.observation.request.target;
  active.target_id = id<domain::OpsTargetId>("beta");
  active.configuration_revision =
      id<domain::OpsConfigurationRevision>("beta-revision");
  controls.state.active_target = std::move(active);
  controls.state.selection_generation = 2;
  controls.state.phase = AdminPhase::ready;
  adapters::AdminDialog dialog{controls};
  REQUIRE(dialog.on_event(key(U's')));
  CHECK(dialog.display_text().find("target.target_id=\"alpha\"") !=
        std::string::npos);
  const auto tiny = render(dialog, 20, 5);
  CHECK(tiny.find("target beta") != std::string::npos);
  CHECK(tiny.find("last success") != std::string::npos);
  CHECK(tiny.find("disconnected") == std::string::npos);
}

TEST_CASE("Admin Kubernetes keyboard reads remain available without toolbar",
          "[admin][dialog][kubernetes]") {
  Controls controls;
  adapters::AdminDialog dialog{controls};
  dialog.set_toolbar_visible(false);
  static_cast<void>(render(dialog, 1, 1));
  REQUIRE(dialog.on_event(key(U'w')));
  REQUIRE(dialog.on_event(key(U'r')));
  CHECK(std::holds_alternative<AdminReadWorkloads>(controls.actions.back()));
  REQUIRE(dialog.on_event(key(U'e')));
  REQUIRE(dialog.on_event(key(U'r')));
  CHECK(std::holds_alternative<AdminReadEvents>(controls.actions.back()));
}

TEST_CASE("Admin detail reads remain bound to the displayed resource across "
          "navigation",
          "[admin][dialog][resource]") {
  Controls controls;
  controls.state.snapshots[1] = committed();
  controls.state.snapshots[2] = service_details_committed();
  controls.state.snapshots[3] = kube_committed();
  controls.state.snapshots[4] = kube_pod_committed();
  adapters::AdminDialog dialog{controls};

  REQUIRE(dialog.on_event(key(U'w')));
  REQUIRE(dialog.on_event(key(U'd')));
  REQUIRE(click(dialog, "[ Read ]"));
  const auto& service =
      std::get<AdminRefreshDisplayed>(controls.actions.back());
  CHECK(service.target.target_id.value() == "local");
  CHECK(service.selection_generation == 7);
  CHECK(service.operation ==
        domain::OpsObservationOperation::linux_service_health);
  CHECK(std::get<domain::LinuxServiceIdentity>(service.resource).unit_name ==
        "example.service");

  REQUIRE(dialog.on_event(key(U's')));
  REQUIRE(dialog.on_event(key(U'p')));
  REQUIRE(dialog.on_event(key(U'r')));
  const auto& pod = std::get<AdminRefreshDisplayed>(controls.actions.back());
  CHECK(pod.target.target_id.value() == "cluster");
  CHECK(pod.selection_generation == 9);
  CHECK(pod.operation ==
        domain::OpsObservationOperation::kubernetes_pod_health);
  const auto& pod_identity =
      std::get<domain::KubernetesPodIdentity>(pod.resource);
  CHECK(pod_identity.name == "broken-pod");
  CHECK(pod_identity.uid == id<domain::OpsResourceUid>("pod-uid"));
}

TEST_CASE("Admin empty detail views never borrow the opposite inventory kind",
          "[admin][dialog][resource]") {
  Controls controls;
  controls.state.snapshots[1] = committed();
  controls.state.snapshots[3] = kube_committed();
  adapters::AdminDialog dialog{controls};
  REQUIRE(dialog.on_event(key(U'w')));
  REQUIRE(dialog.on_event(key(U'd')));
  REQUIRE(dialog.on_event(key(U'r')));
  CHECK(controls.actions.empty());
  REQUIRE(dialog.on_event(key(U's')));
  REQUIRE(dialog.on_event(key(U'p')));
  REQUIRE(click(dialog, "[ Read ]"));
  CHECK(controls.actions.empty());
}

TEST_CASE("Admin event refresh preserves the displayed namespace or exact Pod "
          "scope",
          "[admin][dialog][kubernetes][events]") {
  Controls controls;
  controls.state.snapshots[1] = committed();
  controls.state.snapshots[3] = kube_committed();
  controls.state.snapshots[5] = kube_events_committed(true);
  adapters::AdminDialog dialog{controls};
  REQUIRE(dialog.on_event(key(U'w')));
  REQUIRE(click(dialog, "Read"));
  REQUIRE(click(dialog, "Selected Pod events"));
  REQUIRE(std::holds_alternative<AdminReadCachedPodEvents>(
      controls.actions.back()));
  REQUIRE(dialog.on_event(key(U's')));
  REQUIRE(dialog.on_event(key(U'e')));
  REQUIRE(click(dialog, "[ Read ]"));
  const auto& exact = std::get<AdminRefreshDisplayed>(controls.actions.back());
  CHECK(exact.target.target_id.value() == "cluster");
  CHECK(exact.operation == domain::OpsObservationOperation::kubernetes_events);
  const auto& exact_pod =
      std::get<domain::KubernetesPodIdentity>(exact.resource);
  CHECK(exact_pod.name == "broken-pod");
  CHECK(exact_pod.uid == id<domain::OpsResourceUid>("pod-uid"));

  controls.state.snapshots[5] = kube_events_committed(false);
  REQUIRE(dialog.refresh());
  REQUIRE(dialog.on_event(key(U'w')));
  REQUIRE(dialog.on_event(key(U'e')));
  REQUIRE(dialog.on_event(key(U'r')));
  const auto& namespaced =
      std::get<AdminRefreshDisplayed>(controls.actions.back());
  CHECK(namespaced.target.target_id.value() == "cluster");
  CHECK(namespaced.operation ==
        domain::OpsObservationOperation::kubernetes_events);
  CHECK(std::holds_alternative<std::monostate>(namespaced.resource));
}

TEST_CASE("Admin displayed refresh never adopts a newly active target",
          "[admin][dialog][authority]") {
  Controls controls;
  controls.state.targets.push_back({id<domain::OpsTargetId>("cluster"),
                                    "Private cluster",
                                    domain::OpsTargetKind::kubernetes});
  std::size_t slot{};
  char32_t view{};
  SECTION("service details") {
    controls.state.snapshots[2] = service_details_committed();
    controls.state.active_target =
        controls.state.snapshots[2]->observation.request.target;
    controls.state.selection_generation = 7;
    slot = 2;
    view = U'd';
  }
  SECTION("Pod details") {
    controls.state.snapshots[4] = kube_pod_committed();
    controls.state.active_target =
        controls.state.snapshots[4]->observation.request.target;
    controls.state.selection_generation = 9;
    slot = 4;
    view = U'p';
  }
  SECTION("namespace events") {
    controls.state.snapshots[5] = kube_events_committed(false);
    controls.state.active_target =
        controls.state.snapshots[5]->observation.request.target;
    controls.state.selection_generation = 9;
    slot = 5;
    view = U'e';
  }
  SECTION("exact Pod events") {
    controls.state.snapshots[5] = kube_events_committed(true);
    controls.state.active_target =
        controls.state.snapshots[5]->observation.request.target;
    controls.state.selection_generation = 9;
    slot = 5;
    view = U'e';
  }
  adapters::AdminDialog dialog{controls};
  const auto displayed_target =
      controls.state.snapshots[slot]->observation.request.target;
  const auto displayed_resource =
      controls.state.snapshots[slot]->observation.request.resource;
  controls.state.active_target = committed().observation.request.target;
  controls.state.selection_generation = 10;
  REQUIRE(dialog.refresh());
  REQUIRE(dialog.on_event(key(view)));
  REQUIRE(dialog.on_event(key(U'r')));
  const auto& action = std::get<AdminRefreshDisplayed>(controls.actions.back());
  CHECK(action.target == displayed_target);
  CHECK(action.resource == displayed_resource);
  CHECK(action.selection_generation != controls.state.selection_generation);
}

TEST_CASE("Admin tiny Kubernetes layout retains feedback with identity and "
          "freshness",
          "[admin][dialog][kubernetes][compact]") {
  Controls controls;
  auto snapshot = kube_committed();
  controls.state.active_target = snapshot.observation.request.target;
  controls.state.snapshots[3] = snapshot;
  controls.state.freshness[3] = AdminEvidenceFreshness::refresh_failed;
  controls.refuse = true;
  adapters::AdminDialog dialog{controls};
  REQUIRE(dialog.on_event(key(U'w')));
  REQUIRE(dialog.on_event(key(U'r')));
  const auto tiny = render(dialog, 20, 5);
  CHECK(tiny.find("cluster") != std::string::npos);
  CHECK(tiny.find("home") != std::string::npos);
  CHECK(tiny.find("apps") != std::string::npos);
  CHECK(tiny.find("refresh failed") != std::string::npos);
  CHECK(tiny.find("error busy") != std::string::npos);
}

TEST_CASE("Admin explicit actions remain reachable with no visible buttons",
          "[admin][dialog]") {
  Controls controls;
  adapters::AdminDialog dialog{controls};
  dialog.set_toolbar_visible(false);
  static_cast<void>(render(dialog, 1, 1));
  REQUIRE(dialog.on_event(key(U'u')));
  REQUIRE(controls.actions.size() == 1);
  CHECK(std::get<AdminSelectTarget>(controls.actions.back()).target.value() ==
        "local");
  REQUIRE(dialog.on_event(key(U'h')));
  REQUIRE(dialog.on_event(key(U'r')));
  CHECK(std::holds_alternative<AdminReadHealth>(controls.actions.back()));
  REQUIRE(dialog.on_event(key(U'c')));
  CHECK(std::holds_alternative<AdminCancel>(controls.actions.back()));
}
