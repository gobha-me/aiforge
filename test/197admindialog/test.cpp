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
  bool advance_log_policy{};
  bool require_current_log_proof{};
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
    if (require_current_log_proof) {
      const auto* enabled = std::get_if<AdminEnableDisplayedLogs>(&value);
      if (enabled != nullptr &&
          (!enabled->displayed || !state.session || !state.active_target ||
           enabled->displayed->session != *state.session ||
           enabled->displayed->target != *state.active_target ||
           enabled->displayed->selection_generation !=
               state.selection_generation ||
           enabled->displayed->log_policy_revision !=
               state.log_policy_revision))
        return std::unexpected(
            ManualOpsFailure{ManualOpsErrorCode::wrong_operation});
    }
    if (advance_log_policy) {
      if (const auto* enabled = std::get_if<AdminEnableDisplayedLogs>(&value);
          enabled != nullptr && enabled->displayed) {
        ++state.selection_generation;
        ++state.log_policy_revision;
        state.log_consent = AdminLogConsentState::enabled;
        state.log_source = enabled->displayed->source;
        state.log_evidence_event = enabled->displayed->observation_event;
      }
    }
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
  const domain::LinuxServiceIdentity source{
      "example.service", id<domain::OpsResourceUid>("invocation")};
  result.observation.request.operation =
      domain::OpsObservationOperation::linux_service_health;
  result.observation.request.resource = source;
  result.observation.payload = domain::LinuxServiceObservation{
      source, domain::OpsServiceState::failed,
      domain::OpsObservationReason::failed_exit, 7, 3};
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
  result.observation.payload = domain::KubernetesPodObservation{
      pod,
      domain::OpsPodPhase::failed,
      {{"app", std::optional<std::string>{"containerd://app"},
        domain::OpsContainerState::terminated, domain::OpsReadiness::not_ready,
        domain::OpsObservationReason::failed_exit, 2, 7}}};
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

TEST_CASE("Admin compact view labels historical A evidence under active B",
          "[admin][dialog][history][replay]") {
  Controls controls;
  auto historical = committed();
  historical.observation.request.target.target_id =
      id<domain::OpsTargetId>("alpha");
  controls.state.snapshots[1] = historical;
  controls.state.freshness[1] = AdminEvidenceFreshness::historical_unverified;
  auto active = historical.observation.request.target;
  active.target_id = id<domain::OpsTargetId>("beta");
  active.configuration_revision =
      id<domain::OpsConfigurationRevision>("beta-revision");
  controls.state.active_target = std::move(active);
  controls.state.phase = AdminPhase::ready;
  adapters::AdminDialog dialog{controls};
  REQUIRE(dialog.on_event(key(U's')));
  CHECK(dialog.display_text().find("target.target_id=\"alpha\"") !=
        std::string::npos);
  const auto tiny = render(dialog, 20, 5);
  CHECK(tiny.find("target beta") != std::string::npos);
  CHECK(tiny.find("unverified history") != std::string::npos);
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

TEST_CASE("Admin replay renders exact Kubernetes target as unverified",
          "[admin][dialog][kubernetes][replay]") {
  Controls controls;
  controls.state.historical_target = domain::OpsTargetBinding{
      id<domain::OpsTargetId>("cluster"),
      id<domain::OpsConfigurationRevision>("revision"),
      domain::KubernetesOpsIdentity{
          "home", "apps", {"10.0.0.2", 6443}, "sha256:fixture"}};
  controls.state.snapshots[3] = kube_committed();
  controls.state.freshness[3] = AdminEvidenceFreshness::historical_unverified;
  adapters::AdminDialog dialog{controls};
  const auto presentation = render(dialog, 120, 32);
  CHECK(presentation.find("Historical target: cluster") != std::string::npos);
  CHECK(presentation.find("context home") != std::string::npos);
  CHECK(presentation.find("namespace apps") != std::string::npos);
  CHECK(presentation.find("unverified") != std::string::npos);
  REQUIRE(dialog.on_event(key(U'w')));
  CHECK(dialog.display_text().find("freshness unverified history") !=
        std::string::npos);
  const auto tiny = render(dialog, 20, 5);
  CHECK(tiny.find("cluster") != std::string::npos);
  CHECK(tiny.find("home") != std::string::npos);
  CHECK(tiny.find("apps") != std::string::npos);
  CHECK(tiny.find("history") != std::string::npos);
  CHECK(tiny.find("unverified") != std::string::npos);
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

TEST_CASE("Admin log controls hydrate exact displayed proof across hidden UI",
          "[admin][dialog][logs]") {
  Controls controls;
  controls.state.snapshots[2] = service_details_committed();
  controls.state.active_target =
      controls.state.snapshots[2]->observation.request.target;
  controls.state.selection_generation = 7;
  controls.state.log_consent = AdminLogConsentState::disabled;
  adapters::AdminDialog dialog{controls};
  dialog.set_toolbar_visible(false);
  REQUIRE(dialog.on_event(key(U'd')));
  REQUIRE(dialog.on_event(key(U'l')));
  const auto& enabled =
      std::get<AdminEnableDisplayedLogs>(controls.actions.back());
  REQUIRE(enabled.displayed);
  CHECK(enabled.displayed->observation_event.value() == "service-details");
  const auto& source =
      std::get<domain::LinuxServiceIdentity>(enabled.displayed->source);
  CHECK(source.unit_name == "example.service");
  REQUIRE(source.invocation_id);

  REQUIRE(dialog.on_event(key(U'g')));
  CHECK(
      std::holds_alternative<AdminReadDisplayedLogs>(controls.actions.back()));
  REQUIRE(dialog.on_event(key(U'o')));
  CHECK(std::holds_alternative<AdminDisableDisplayedLogs>(
      controls.actions.back()));
}

TEST_CASE("Admin reads newly enabled retained consent from the detail view",
          "[admin][dialog][logs][revision]") {
  Controls controls;
  controls.advance_log_policy = true;
  controls.state.snapshots[2] = service_details_committed();
  controls.state.active_target =
      controls.state.snapshots[2]->observation.request.target;
  controls.state.selection_generation = 7;
  controls.state.log_policy_revision = 1;
  controls.state.log_consent = AdminLogConsentState::disabled;
  adapters::AdminDialog dialog{controls};
  dialog.set_toolbar_visible(false);
  REQUIRE(dialog.on_event(key(U'd')));
  REQUIRE(dialog.on_event(key(U'l')));
  CHECK(controls.state.selection_generation == 8);
  CHECK(controls.state.log_policy_revision == 2);
  REQUIRE(dialog.on_event(key(U'g')));
  const auto& read = std::get<AdminReadDisplayedLogs>(controls.actions.back());
  REQUIRE(read.displayed);
  CHECK(read.displayed->selection_generation == 8);
  CHECK(read.displayed->log_policy_revision == 2);
  CHECK(read.displayed->observation_event.value() == "service-details");
}

TEST_CASE("Admin log buttons and menus dispatch the same exact typed proof",
          "[admin][dialog][logs][toolbar]") {
  for (const auto choice : {0, 1, 2}) {
    Controls controls;
    controls.state.snapshots[2] = service_details_committed();
    controls.state.active_target =
        controls.state.snapshots[2]->observation.request.target;
    controls.state.selection_generation = 7;
    controls.state.log_policy_revision = 1;
    if (choice == 1) {
      const auto& snapshot = *controls.state.snapshots[2];
      controls.state.log_consent = AdminLogConsentState::enabled;
      controls.state.log_source = std::get<domain::LinuxServiceObservation>(
                                      snapshot.observation.payload)
                                      .identity;
      controls.state.log_evidence_event = snapshot.observation_event_id;
    }
    adapters::AdminDialog dialog{controls};
    REQUIRE(dialog.on_event(key(U'd')));
    const std::string_view button = choice == 0   ? "[ Allow logs ]"
                                    : choice == 1 ? "[ Revoke logs ]"
                                                  : "[ Read logs ]";
    REQUIRE(click(dialog, button));
    REQUIRE_FALSE(controls.actions.empty());
    const auto& action = controls.actions.back();
    const AdminDisplayedLogSource* proof{};
    if (choice == 0) {
      const auto& typed = std::get<AdminEnableDisplayedLogs>(action);
      REQUIRE(typed.displayed);
      proof = &*typed.displayed;
    } else if (choice == 1) {
      const auto& typed = std::get<AdminDisableDisplayedLogs>(action);
      REQUIRE(typed.displayed);
      proof = &*typed.displayed;
    } else {
      const auto& typed = std::get<AdminReadDisplayedLogs>(action);
      REQUIRE(typed.displayed);
      proof = &*typed.displayed;
    }
    REQUIRE(proof);
    CHECK(proof->observation_event.value() == "service-details");
  }

  for (const auto choice : {0, 1, 2}) {
    Controls controls;
    controls.state.snapshots[2] = service_details_committed();
    controls.state.active_target =
        controls.state.snapshots[2]->observation.request.target;
    controls.state.selection_generation = 7;
    controls.state.log_policy_revision = 1;
    adapters::AdminDialog dialog{controls};
    REQUIRE(dialog.on_event(key(U'd')));
    REQUIRE(click(dialog, "Read"));
    const std::string_view item = choice == 0   ? "Enable displayed logs"
                                  : choice == 1 ? "Disable displayed logs"
                                                : "Read displayed logs";
    REQUIRE(click(dialog, item));
    REQUIRE_FALSE(controls.actions.empty());
    const bool typed =
        choice == 0 ? std::holds_alternative<AdminEnableDisplayedLogs>(
                          controls.actions.back())
        : choice == 1 ? std::holds_alternative<AdminDisableDisplayedLogs>(
                            controls.actions.back())
                      : std::holds_alternative<AdminReadDisplayedLogs>(
                            controls.actions.back());
    CHECK(typed);
  }
}

TEST_CASE("Admin Pod log controls bind the displayed container runtime proof",
          "[admin][dialog][kubernetes][logs]") {
  Controls controls;
  controls.state.snapshots[4] = kube_pod_committed();
  controls.state.active_target =
      controls.state.snapshots[4]->observation.request.target;
  controls.state.selection_generation = 9;
  controls.state.log_policy_revision = 1;
  adapters::AdminDialog dialog{controls};
  REQUIRE(dialog.on_event(key(U'p')));
  REQUIRE(dialog.on_event(key(U'l')));
  const auto& action =
      std::get<AdminEnableDisplayedLogs>(controls.actions.back());
  REQUIRE(action.displayed);
  const auto& source =
      std::get<domain::KubernetesPodIdentity>(action.displayed->source);
  CHECK(source.name == "broken-pod");
  CHECK(source.uid == id<domain::OpsResourceUid>("pod-uid"));
  REQUIRE(source.container);
  CHECK(source.container->name == "app");
  CHECK(source.container->runtime_identity == "containerd://app");
}

TEST_CASE(
    "Admin requires an explicit container choice for multi-container Pods",
    "[admin][dialog][kubernetes][logs]") {
  Controls controls;
  auto pod = kube_pod_committed();
  std::get<domain::KubernetesPodObservation>(pod.observation.payload)
      .containers.push_back(
          {"sidecar", std::optional<std::string>{"containerd://sidecar"},
           domain::OpsContainerState::running, domain::OpsReadiness::ready,
           domain::OpsObservationReason::none, 0, 0});
  REQUIRE(domain::validate_recorded_ops_observation(pod.observation));
  controls.state.snapshots[4] = std::move(pod);
  controls.state.active_target =
      controls.state.snapshots[4]->observation.request.target;
  controls.state.selection_generation = 9;
  controls.state.log_policy_revision = 1;
  adapters::AdminDialog dialog{controls};
  REQUIRE(dialog.on_event(key(U'p')));
  REQUIRE(click(dialog, "Container sidecar"));
  REQUIRE(dialog.on_event(key(U'l')));
  const auto& action =
      std::get<AdminEnableDisplayedLogs>(controls.actions.back());
  REQUIRE(action.displayed);
  const auto& source =
      std::get<domain::KubernetesPodIdentity>(action.displayed->source);
  REQUIRE(source.container);
  CHECK(source.container->name == "sidecar");
  CHECK(source.container->runtime_identity == "containerd://sidecar");
}

TEST_CASE("Admin container command resolves only exact displayed Pod proof",
          "[admin][dialog][kubernetes][logs][commands]") {
  Controls controls;
  auto pod = kube_pod_committed();
  std::get<domain::KubernetesPodObservation>(pod.observation.payload)
      .containers.push_back(
          {"sidecar", std::optional<std::string>{"containerd://sidecar"},
           domain::OpsContainerState::running, domain::OpsReadiness::ready,
           domain::OpsObservationReason::none, 0, 0});
  REQUIRE(domain::validate_recorded_ops_observation(pod.observation));
  controls.state.snapshots[4] = std::move(pod);
  controls.state.active_target =
      controls.state.snapshots[4]->observation.request.target;
  controls.state.selection_generation = 9;
  controls.state.log_policy_revision = 1;
  controls.require_current_log_proof = true;
  adapters::AdminDialog dialog{controls};
  REQUIRE(dialog.on_event(key(U'p')));

  REQUIRE(dialog.enable_displayed_container_logs("sidecar"));
  const auto& enabled =
      std::get<AdminEnableDisplayedLogs>(controls.actions.back());
  REQUIRE(enabled.displayed);
  const auto& source =
      std::get<domain::KubernetesPodIdentity>(enabled.displayed->source);
  REQUIRE(source.container);
  CHECK(source.container->name == "sidecar");
  CHECK(source.container->runtime_identity == "containerd://sidecar");

  const auto calls = controls.actions.size();
  REQUIRE_FALSE(dialog.enable_displayed_container_logs("unknown"));
  CHECK(controls.actions.size() == calls);

  ++controls.state.selection_generation;
  REQUIRE_FALSE(dialog.enable_displayed_container_logs("sidecar"));
  CHECK(controls.actions.size() == calls + 1);
}

TEST_CASE("Admin container command rejects missing or ambiguous runtime proof",
          "[admin][dialog][kubernetes][logs][commands][failure]") {
  for (const bool duplicate : {false, true}) {
    Controls controls;
    auto pod = kube_pod_committed();
    auto& containers =
        std::get<domain::KubernetesPodObservation>(pod.observation.payload)
            .containers;
    if (duplicate) {
      containers.push_back(containers.front());
    } else {
      containers.front().runtime_identity.reset();
    }
    controls.state.snapshots[4] = std::move(pod);
    controls.state.active_target =
        controls.state.snapshots[4]->observation.request.target;
    controls.state.selection_generation = 9;
    controls.state.log_policy_revision = 1;
    adapters::AdminDialog dialog{controls};
    REQUIRE(dialog.on_event(key(U'p')));
    REQUIRE_FALSE(dialog.enable_displayed_container_logs("app"));
    CHECK(controls.actions.empty());
  }
}

TEST_CASE(
    "Admin separates displayed log candidate from retained consent source",
    "[admin][dialog][logs]") {
  Controls controls;
  auto candidate = service_details_committed();
  auto& request = candidate.observation.request;
  request.resource = domain::LinuxServiceIdentity{"beta.service", {}};
  std::get<domain::LinuxServiceObservation>(candidate.observation.payload)
      .identity = {"beta.service",
                   id<domain::OpsResourceUid>("beta-invocation")};
  REQUIRE(domain::validate_recorded_ops_observation(candidate.observation));
  const auto target = request.target;
  controls.state.snapshots[2] = std::move(candidate);
  controls.state.active_target = target;
  controls.state.selection_generation = 7;
  controls.state.log_policy_revision = 1;
  controls.state.log_consent = AdminLogConsentState::enabled;
  controls.state.log_source = domain::LinuxServiceIdentity{
      "alpha.service", id<domain::OpsResourceUid>("alpha-invocation")};
  controls.state.log_evidence_event = id<domain::EventId>("alpha-details");
  adapters::AdminDialog dialog{controls};
  REQUIRE(dialog.on_event(key(U'd')));

  REQUIRE(dialog.on_event(key(U'o')));
  const auto& disabled =
      std::get<AdminDisableDisplayedLogs>(controls.actions.back());
  REQUIRE(disabled.displayed);
  CHECK(std::get<domain::LinuxServiceIdentity>(disabled.displayed->source)
            .unit_name == "alpha.service");

  REQUIRE(dialog.on_event(key(U'l')));
  const auto& enabled =
      std::get<AdminEnableDisplayedLogs>(controls.actions.back());
  REQUIRE(enabled.displayed);
  CHECK(std::get<domain::LinuxServiceIdentity>(enabled.displayed->source)
            .unit_name == "beta.service");
}

TEST_CASE("Admin repeats service and Pod log reads from retained exact consent",
          "[admin][dialog][logs]") {
  for (const bool kubernetes : {false, true}) {
    Controls controls;
    controls.state.log_consent = AdminLogConsentState::enabled;
    controls.state.selection_generation = kubernetes ? 10 : 8;
    controls.state.log_policy_revision = 2;
    controls.state.log_evidence_event =
        id<domain::EventId>(kubernetes ? "pod-health" : "service-health");
    controls.state.active_target =
        kubernetes ? kube_pod_committed().observation.request.target
                   : service_details_committed().observation.request.target;
    controls.state.log_source =
        kubernetes
            ? domain::OpsResourceIdentity{domain::KubernetesPodIdentity{
                  "apps", "broken-pod", id<domain::OpsResourceUid>("pod-uid"),
                  domain::KubernetesContainerIdentity{"app",
                                                      "containerd://app"}}}
            : domain::OpsResourceIdentity{domain::LinuxServiceIdentity{
                  "example.service", id<domain::OpsResourceUid>("invocation")}};
    AdminDisplayedLogSource active{*controls.state.session,
                                   *controls.state.active_target,
                                   controls.state.selection_generation,
                                   controls.state.log_policy_revision,
                                   *controls.state.log_evidence_event,
                                   *controls.state.log_source};
    adapters::AdminDialog dialog{controls};
    REQUIRE(dialog.execute(AdminReadDisplayedLogs{active}));
    REQUIRE(dialog.on_event(key(U'g')));
    const auto& repeated =
        std::get<AdminReadDisplayedLogs>(controls.actions.back());
    REQUIRE(repeated.displayed);
    CHECK(repeated.displayed->source == active.source);
    CHECK(repeated.displayed->observation_event == active.observation_event);
  }
}

TEST_CASE("Admin compact view exposes log consent and exact source",
          "[admin][dialog][logs][compact]") {
  Controls controls;
  controls.state.snapshots[2] = service_details_committed();
  controls.state.active_target =
      controls.state.snapshots[2]->observation.request.target;
  controls.state.selection_generation = 8;
  controls.state.log_consent = AdminLogConsentState::enabled;
  controls.state.log_source = domain::LinuxServiceIdentity{
      "example.service", id<domain::OpsResourceUid>("invocation")};
  controls.state.log_evidence_event = id<domain::EventId>("service-details");
  adapters::AdminDialog dialog{controls};
  REQUIRE(dialog.on_event(key(U'd')));
  const auto tiny = render(dialog, 20, 5);
  CHECK(tiny.find("log on") != std::string::npos);
  CHECK(tiny.find("exampl") != std::string::npos);
}
