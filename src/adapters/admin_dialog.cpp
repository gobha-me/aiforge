#include <aiforge/adapters/admin_dialog.hpp>
#include <aiforge/detail/admin_input.hpp>
#include <aiforge/detail/utf8_text.hpp>
#include <aiforge/runtime/ops_observation_history.hpp>
#include <algorithm>
#include <utility>

namespace aiforge::adapters {
namespace {
using namespace surfaces;
auto failure() -> std::unexpected<ManualOpsFailure> {
  return std::unexpected(
      ManualOpsFailure{ManualOpsErrorCode::internal_failure});
}
auto phase_text(AdminPhase phase) -> std::string_view {
  switch (phase) {
    case AdminPhase::detached: return "No session attached";
    case AdminPhase::idle: return "Choose and use a target";
    case AdminPhase::preparing: return "Preparing selected target";
    case AdminPhase::retiring: return "Waiting for source preparation cleanup";
    case AdminPhase::ready: return "Ready for an explicit read";
    case AdminPhase::observing: return "Reading";
    case AdminPhase::awaiting_approval: return "Waiting for approval";
    case AdminPhase::failed: return "Admin operation failed";
  }
  return "Unavailable";
}
auto compact_phase_text(AdminPhase phase) -> std::string_view {
  switch (phase) {
    case AdminPhase::detached: return "status detached";
    case AdminPhase::idle: return "status choose target";
    case AdminPhase::preparing: return "status preparing";
    case AdminPhase::retiring: return "status retiring";
    case AdminPhase::ready: return "status ready";
    case AdminPhase::observing: return "status reading";
    case AdminPhase::awaiting_approval: return "status approval";
    case AdminPhase::failed: return "error failed";
  }
  return "status unavailable";
}
auto error_text(ManualOpsErrorCode code) -> std::string_view {
  switch (code) {
    case ManualOpsErrorCode::busy: return "Session or source worker is busy";
    case ManualOpsErrorCode::invalid_input:
      return "Admin action is invalid or stale";
    case ManualOpsErrorCode::unavailable:
      return "Admin operation is unavailable";
    case ManualOpsErrorCode::closed: return "Admin session is closed";
    case ManualOpsErrorCode::wrong_operation:
      return "Action does not match the current operation";
    case ManualOpsErrorCode::cancelled: return "Admin operation cancelled";
    case ManualOpsErrorCode::storage_failure: return "Admin storage failed";
    case ManualOpsErrorCode::invalid_history:
      return "Admin history could not be validated";
    case ManualOpsErrorCode::operation_failed:
      return "Admin observation failed";
    case ManualOpsErrorCode::resource_exhausted:
      return "Admin resource limit reached";
    case ManualOpsErrorCode::internal_failure:
      return "Admin operation failed internally";
  }
  return "Admin operation failed";
}
auto compact_error_text(ManualOpsErrorCode code) -> std::string_view {
  switch (code) {
    case ManualOpsErrorCode::busy: return "error busy";
    case ManualOpsErrorCode::invalid_input: return "error stale action";
    case ManualOpsErrorCode::unavailable: return "error unavailable";
    case ManualOpsErrorCode::closed: return "error closed";
    case ManualOpsErrorCode::wrong_operation: return "error wrong action";
    case ManualOpsErrorCode::cancelled: return "error cancelled";
    case ManualOpsErrorCode::storage_failure: return "error storage";
    case ManualOpsErrorCode::invalid_history: return "error history";
    case ManualOpsErrorCode::operation_failed: return "error observation";
    case ManualOpsErrorCode::resource_exhausted: return "error limit";
    case ManualOpsErrorCode::internal_failure: return "error internal";
  }
  return "error failed";
}
auto safe_text(std::string_view text, std::size_t maximum,
               std::string_view fallback) -> std::string {
  if (text.empty() || text.size() > maximum ||
      !detail::is_safe_utf8_text(text) ||
      text.find_first_of("\r\n\t") != std::string_view::npos)
    return std::string{fallback};
  return std::string{text};
}
auto safe_label(std::string_view text) -> std::string {
  return safe_text(text, 128, "Unavailable label");
}
auto index_valid(int index, std::size_t count) -> bool {
  return index >= 0 && std::cmp_less(index, count);
}
auto action_at(const std::vector<std::optional<AdminAction>>& actions,
               int index) -> const AdminAction* {
  if (!index_valid(index, actions.size())) return nullptr;
  const auto& action = actions[static_cast<std::size_t>(index)];
  return action ? &action.value() : nullptr;
}
auto action_value(const std::optional<AdminAction>& action)
    -> const AdminAction* {
  return action ? &action.value() : nullptr;
}
auto freshness_text(AdminEvidenceFreshness value) -> std::string_view {
  switch (value) {
    case AdminEvidenceFreshness::unavailable: return "unavailable";
    case AdminEvidenceFreshness::last_success: return "last success";
    case AdminEvidenceFreshness::refreshing: return "refreshing";
    case AdminEvidenceFreshness::refresh_failed: return "refresh failed";
    case AdminEvidenceFreshness::disconnected: return "disconnected";
  }
  return "unavailable";
}
auto kind_text(domain::OpsWorkloadKind kind) -> std::string_view {
  switch (kind) {
    case domain::OpsWorkloadKind::pod: return "Pod";
    case domain::OpsWorkloadKind::deployment: return "Deployment";
    case domain::OpsWorkloadKind::stateful_set: return "StatefulSet";
    case domain::OpsWorkloadKind::daemon_set: return "DaemonSet";
    case domain::OpsWorkloadKind::job: return "Job";
    case domain::OpsWorkloadKind::cron_job: return "CronJob";
  }
  return "Resource";
}
struct PreparedSnapshot {
  std::optional<domain::EventId> event;
  std::optional<AdminAction> refresh_action;
  std::string text{"No committed observation for this view."};
  std::vector<std::string> labels;
  std::vector<std::optional<AdminAction>> actions;
};
auto supports_refresh(std::size_t slot,
                      const domain::OpsObservationRequest& request) -> bool {
  constexpr std::array operations{
      domain::OpsObservationOperation::linux_health,
      domain::OpsObservationOperation::linux_services,
      domain::OpsObservationOperation::linux_service_health,
      domain::OpsObservationOperation::kubernetes_workloads,
      domain::OpsObservationOperation::kubernetes_pod_health,
      domain::OpsObservationOperation::kubernetes_events};
  if (slot >= operations.size() || request.operation != operations[slot])
    return false;
  if (slot == 2)
    return std::holds_alternative<domain::LinuxServiceIdentity>(
        request.resource);
  if (slot == 4)
    return std::holds_alternative<domain::KubernetesPodIdentity>(
        request.resource);
  if (slot == 5)
    return std::holds_alternative<std::monostate>(request.resource) ||
           std::holds_alternative<domain::KubernetesPodIdentity>(
               request.resource);
  return std::holds_alternative<std::monostate>(request.resource);
}
auto prepare_inventory_actions(const CommittedOpsObservation& snapshot,
                               const domain::EventId& event, std::size_t slot,
                               PreparedSnapshot& result) -> void {
  const auto& request = snapshot.observation.request;
  if (slot == 1) {
    const auto* services = std::get_if<domain::LinuxServicesObservation>(
        &snapshot.observation.payload);
    if (services == nullptr) return;
    for (const auto& service : services->services) {
      result.actions.emplace_back(AdminReadCachedService{
          request.session_id, event, request.selection_generation,
          result.labels.size()});
      result.labels.push_back(service.identity.unit_name);
    }
    return;
  }
  if (slot != 3) return;
  const auto* workloads = std::get_if<domain::KubernetesWorkloadsObservation>(
      &snapshot.observation.payload);
  if (workloads == nullptr) return;
  for (const auto& workload : workloads->workloads) {
    const auto row = result.labels.size();
    result.labels.push_back(std::string{kind_text(workload.identity.kind)} +
                            " " + workload.identity.name);
    if (workload.identity.kind == domain::OpsWorkloadKind::pod)
      result.actions.emplace_back(AdminReadCachedPod{
          request.session_id, event, request.selection_generation, row});
    else
      result.actions.emplace_back(std::nullopt);
  }
}
auto prepare_snapshot(const std::optional<CommittedOpsObservation>& snapshot,
                      AdminEvidenceFreshness freshness,
                      domain::EventTimestamp now, std::size_t slot)
    -> PreparedSnapshot {
  PreparedSnapshot result;
  if (!snapshot) {
    result.text = "No committed observation for this view. Freshness: " +
                  std::string{freshness_text(freshness)} + ".";
    return result;
  }
  const auto& committed = snapshot.value();
  const auto content =
      runtime::format_ops_observation_content(committed.observation);
  if (!content || content->size() != 1 ||
      !std::holds_alternative<domain::TextBlock>(content->front())) {
    result.text = "Committed observation could not be rendered.";
    return result;
  }
  const auto event = committed.observation_event_id;
  result.event = event;
  const auto captured = committed.observation.completed_at;
  const auto age =
      now > captured
          ? std::chrono::duration_cast<std::chrono::seconds>(now - captured)
          : std::chrono::seconds::zero();
  result.text = "Committed evidence | event " + safe_label(event.value()) +
                " | captured_ms " +
                std::to_string(captured.time_since_epoch().count()) +
                " | age " + std::to_string(age.count()) + "s | freshness " +
                std::string{freshness_text(freshness)} + "\n" +
                std::get<domain::TextBlock>(content->front()).text;
  const auto& request = committed.observation.request;
  const auto displayed = [&] {
    return AdminRefreshDisplayed{request.session_id,
                                 request.target,
                                 request.selection_generation,
                                 event,
                                 request.operation,
                                 request.resource};
  };
  if (supports_refresh(slot, request)) result.refresh_action = displayed();
  prepare_inventory_actions(committed, event, slot, result);
  return result;
}

enum class PresentedView {
  unchanged,
  health,
  services,
  details,
  workloads,
  pod,
  events
};
template <class... Callables> struct Overloaded : Callables... {
  using Callables::operator()...;
};
template <class... Callables>
Overloaded(Callables...) -> Overloaded<Callables...>;
auto presented_view(const AdminAction& action) -> PresentedView {
  return std::visit(
      Overloaded{
          [](const AdminReadHealth&) { return PresentedView::health; },
          [](const AdminReadServices&) { return PresentedView::services; },
          [](const AdminReadNamedService&) { return PresentedView::details; },
          [](const AdminReadCachedService&) { return PresentedView::details; },
          [](const AdminReadWorkloads&) { return PresentedView::workloads; },
          [](const AdminReadNamedPod&) { return PresentedView::pod; },
          [](const AdminReadCachedPod&) { return PresentedView::pod; },
          [](const AdminReadEvents&) { return PresentedView::events; },
          [](const AdminReadNamedPodEvents&) { return PresentedView::events; },
          [](const AdminReadCachedPodEvents&) { return PresentedView::events; },
          [](const AdminRefreshDisplayed& displayed) {
            switch (displayed.operation) {
              case domain::OpsObservationOperation::linux_services:
                return PresentedView::services;
              case domain::OpsObservationOperation::linux_service_health:
                return PresentedView::details;
              case domain::OpsObservationOperation::kubernetes_workloads:
                return PresentedView::workloads;
              case domain::OpsObservationOperation::kubernetes_pod_health:
                return PresentedView::pod;
              case domain::OpsObservationOperation::kubernetes_events:
                return PresentedView::events;
              case domain::OpsObservationOperation::linux_health:
                return PresentedView::health;
            }
            return PresentedView::unchanged;
          },
          [](const auto&) { return PresentedView::unchanged; }},
      action);
}
auto target_identity(const std::optional<domain::OpsTargetBinding>& binding)
    -> std::string {
  if (!binding) return "none";
  auto result = safe_label(binding->target_id.value());
  if (const auto* linux =
          std::get_if<domain::LinuxOpsIdentity>(&binding->identity)) {
    const auto scope =
        linux->scope == domain::LinuxExecutionScope::host        ? "host"
        : linux->scope == domain::LinuxExecutionScope::container ? "container"
                                                                 : "unknown";
    return result + " | Linux scope " + scope;
  }
  if (const auto* kube =
          std::get_if<domain::KubernetesOpsIdentity>(&binding->identity))
    return result + " | context " +
           safe_text(kube->context_name, 256, "unavailable") + " | namespace " +
           safe_text(kube->namespace_name, 63, "unavailable") + " | https://" +
           safe_text(kube->endpoint.host, 253, "unavailable") + ':' +
           std::to_string(kube->endpoint.port);
  return result + " | unavailable target kind";
}
struct CompactTargetIdentity {
  std::string title{"target none"};
  std::string body;
};
auto compact_target_identity(
    const std::optional<domain::OpsTargetBinding>& binding)
    -> CompactTargetIdentity {
  if (!binding) return {};
  CompactTargetIdentity result{
      "target " + safe_label(binding->target_id.value()), {}};
  if (const auto* linux =
          std::get_if<domain::LinuxOpsIdentity>(&binding->identity)) {
    const auto scope =
        linux->scope == domain::LinuxExecutionScope::host        ? "host"
        : linux->scope == domain::LinuxExecutionScope::container ? "container"
                                                                 : "unknown";
    result.body = "scope " + std::string{scope};
    return result;
  }
  if (const auto* kube =
          std::get_if<domain::KubernetesOpsIdentity>(&binding->identity)) {
    result.body = "ctx " + safe_text(kube->context_name, 256, "unavailable") +
                  " ns " + safe_text(kube->namespace_name, 63, "unavailable");
    return result;
  }
  result.body = "unavailable kind";
  return result;
}
} // namespace

AdminDialog::AdminDialog(AdminControls& controls,
                         runtime::TimestampSource timestamp_source)
    : Dialog("Admin"), m_controls(controls),
      m_timestamp_source(timestamp_source ? std::move(timestamp_source) : [] {
        return std::chrono::time_point_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now());
      }) {
  set_max_width(118);
  m_menu.set_menus(
      {{"Target",
        {{"Choose target", [this] { show(View::targets); }},
         {"Use highlighted target", [this] { use_target(); }}}},
       {"Read",
        {{"Health",
          [this] {
            perform(AdminReadHealth{});
            show(View::health);
          }},
         {"Loaded services",
          [this] {
            perform(AdminReadServices{});
            show(View::services);
          }},
         {"Selected service", [this] { read_service(m_services.selected()); }},
         {"Workloads",
          [this] {
            perform(AdminReadWorkloads{});
            show(View::workloads);
          }},
         {"Selected Pod", [this] { read_pod(m_services.selected()); }},
         {"Namespace events",
          [this] {
            perform(AdminReadEvents{});
            show(View::events);
          }},
         {"Selected Pod events",
          [this] { read_resource_events(m_services.selected()); }},
         {"Cancel", [this] { perform(AdminCancel{}); }}}},
       {"View",
        {{"Targets", [this] { show(View::targets); }},
         {"Health", [this] { show(View::health); }},
         {"Services", [this] { show(View::services); }},
         {"Details", [this] { show(View::details); }},
         {"Workloads", [this] { show(View::workloads); }},
         {"Pod", [this] { show(View::pod); }},
         {"Events", [this] { show(View::events); }},
         {"Hide toolbar", [this] { set_toolbar_visible(false); }}}}});
  m_targets.on_select([this](int, const std::string&) {
    report("Use target explicitly to prepare the highlighted target",
           "select use target");
  });
  m_services.on_select(
      [this](int row, const std::string&) { read_resource(row); });
  m_use.on_activate([this] { use_target(); });
  m_read.on_activate([this] { read_view(); });
  m_cancel.on_activate([this] { perform(AdminCancel{}); });
  m_close.on_activate([this] { on_escape(); });
  children();
  if (!refresh())
    report("Admin presentation is unavailable", "error unavailable");
}
auto AdminDialog::report(std::string message, std::string compact) -> void {
  m_status = std::move(message);
  m_compact_status = std::move(compact);
  m_dialog_text =
      m_status + "\n" + m_summary +
      "\nT targets | H health | S services | D details | W workloads | P "
      "pod | E events"
      "\nU use | R read | C cancel | F10 toolbar | Esc close";
  if (!m_compact) set_text(m_dialog_text);
}
auto AdminDialog::execute(const AdminAction& action)
    -> std::expected<void, ManualOpsFailure> {
  try {
    auto result = m_controls.execute(action);
    const auto presentation = refresh();
    if (!result) {
      report(std::string{error_text(result.error().code)},
             std::string{compact_error_text(result.error().code)});
      return result;
    }
    if (!presentation) return presentation;
    switch (presented_view(action)) {
      case PresentedView::health: show(View::health); break;
      case PresentedView::services: show(View::services); break;
      case PresentedView::details: show(View::details); break;
      case PresentedView::workloads: show(View::workloads); break;
      case PresentedView::pod: show(View::pod); break;
      case PresentedView::events: show(View::events); break;
      case PresentedView::unchanged: break;
    }
    return {};
  } catch (...) {
    return failure();
  }
}
auto AdminDialog::perform(AdminAction action) -> void {
  if (const auto result = execute(action); !result)
    report(std::string{error_text(result.error().code)},
           std::string{compact_error_text(result.error().code)});
}
auto AdminDialog::read_view() -> void {
  constexpr std::array slots{0U, 0U, 1U, 2U, 3U, 4U, 5U};
  const auto slot = slots[static_cast<std::size_t>(m_view)];
  if (const auto* action = action_value(m_snapshot_actions[slot])) {
    perform(*action);
    return;
  }
  if (m_snapshot_present[slot]) {
    if (m_view != View::targets)
      report("Read evidence for this view before refreshing it",
             "status no evidence");
    return;
  }
  switch (m_view) {
    case View::details: read_service(m_services.selected()); break;
    case View::pod: read_pod(m_services.selected()); break;
    case View::events: perform(AdminReadEvents{}); break;
    case View::services: perform(AdminReadServices{}); break;
    case View::workloads: perform(AdminReadWorkloads{}); break;
    case View::health: perform(AdminReadHealth{}); break;
    case View::targets: break;
  }
}
auto AdminDialog::use_target() -> void {
  const auto row = m_targets.selected();
  if (!index_valid(row, m_target_ids.size())) {
    report("Highlight a configured target first", "select target");
    return;
  }
  perform(AdminSelectTarget{m_target_ids[static_cast<std::size_t>(row)]});
}
auto AdminDialog::read_resource(int row) -> void {
  const auto* selected = action_at(m_resource_actions, row);
  if (selected == nullptr) {
    report("Read an inventory and choose an inspectable service or Pod first",
           "select resource");
    return;
  }
  // Copy the displayed identity before execute can refresh the widget rows.
  auto action = *selected;
  const bool pod = std::holds_alternative<AdminReadCachedPod>(action);
  perform(std::move(action));
  show(pod ? View::pod : View::details);
}
auto AdminDialog::read_service(int row) -> void {
  const auto* selected = action_at(m_resource_actions, row);
  const auto* service = selected == nullptr
                            ? nullptr
                            : std::get_if<AdminReadCachedService>(selected);
  if (service == nullptr) {
    report("Read loaded services and choose a service first", "select service");
    return;
  }
  perform(*service);
  show(View::details);
}
auto AdminDialog::read_pod(int row) -> void {
  const auto* selected = action_at(m_resource_actions, row);
  const auto* pod =
      selected == nullptr ? nullptr : std::get_if<AdminReadCachedPod>(selected);
  if (pod == nullptr) {
    report("Read workloads and choose a Pod first", "select Pod");
    return;
  }
  perform(*pod);
  show(View::pod);
}
auto AdminDialog::read_resource_events(int row) -> void {
  const auto* selected = action_at(m_resource_actions, row);
  const auto* pod =
      selected == nullptr ? nullptr : std::get_if<AdminReadCachedPod>(selected);
  if (pod == nullptr) {
    report("Read workloads and choose a Pod first", "select Pod");
    return;
  }
  perform(AdminReadCachedPodEvents{pod->session, pod->inventory_event,
                                   pod->selection_generation, pod->row});
  show(View::events);
}
auto AdminDialog::refresh_targets(const AdminState& state) -> void {
  std::vector<domain::OpsTargetId> ids;
  std::vector<std::string> labels;
  if (state.targets.size() <= 33)
    for (const auto& target : state.targets) {
      if (!detail::valid_admin_target(target.id.value())) continue;
      ids.push_back(target.id);
      labels.push_back(std::string{target.id.value()} + " | " +
                       safe_label(target.display_name) +
                       (target.kind == domain::OpsTargetKind::ceph
#if !defined(__linux__)
                                ||
                                target.kind == domain::OpsTargetKind::kubernetes
#endif
                            ? " (collection unavailable)"
                            : ""));
    }
  if (ids == m_target_ids && labels == m_target_labels) return;
  int next{};
  if (index_valid(m_targets.selected(), m_target_ids.size())) {
    const auto found = std::ranges::find(
        ids, m_target_ids[static_cast<std::size_t>(m_targets.selected())]);
    if (found != ids.end()) next = static_cast<int>(found - ids.begin());
  }
  m_target_ids = std::move(ids);
  m_target_labels = std::move(labels);
  m_targets.set_items(m_target_labels);
  m_targets.set_selected(next);
}
auto AdminDialog::refresh_snapshots(const AdminState& state) -> void {
  bool service_inventory_changed{};
  bool workload_inventory_changed{};
  for (std::size_t slot = 0; slot < state.snapshots.size(); ++slot) {
    const auto& snapshot = state.snapshots[slot];
    if (!snapshot && !m_events[slot] &&
        m_freshness[slot] == state.freshness[slot])
      continue;
    auto prepared = prepare_snapshot(snapshot, state.freshness[slot],
                                     m_timestamp_source(), slot);
    const bool evidence_changed = m_events[slot] != prepared.event;
    if (slot == 1 || slot == 3) {
      if (slot == 1 && evidence_changed) {
        m_service_labels = std::move(prepared.labels);
        m_service_actions = std::move(prepared.actions);
        service_inventory_changed = true;
      } else if (slot == 3 && evidence_changed) {
        m_workload_labels = std::move(prepared.labels);
        m_workload_actions = std::move(prepared.actions);
        workload_inventory_changed = true;
      }
    }
    m_snapshots[slot] = std::move(prepared.text);
    m_snapshot_actions[slot] = std::move(prepared.refresh_action);
    m_snapshot_present[slot] = snapshot.has_value();
    m_events[slot] = std::move(prepared.event);
    m_freshness[slot] = state.freshness[slot];
  }
  if (m_view == View::services && service_inventory_changed) {
    m_services.set_items(m_service_labels);
    m_resource_actions = m_service_actions;
  } else if (m_view == View::workloads && workload_inventory_changed) {
    m_services.set_items(m_workload_labels);
    m_resource_actions = m_workload_actions;
  }
}
auto AdminDialog::refresh_body() -> void {
  constexpr std::array slots{0U, 0U, 1U, 2U, 3U, 4U, 5U};
  const auto slot = slots[static_cast<std::size_t>(m_view)];
  const auto& snapshot = m_snapshots[slot];
  const std::string_view text = snapshot.empty()
                                    ? "No committed observation for this view."
                                    : std::string_view{snapshot};
  const auto next = m_compact
                        ? m_compact_identity + '\n' +
                              std::string{freshness_text(m_freshness[slot])} +
                              '\n' + std::string{text}
                        : std::string{text};
  if (next == m_body) return;
  m_body = next;
  m_evidence.clear();
  m_evidence.append(m_body);
  m_evidence.scroll(-1000000);
}
auto AdminDialog::refresh() -> std::expected<void, ManualOpsFailure> {
  try {
    const auto& state = m_controls.inspect();
    if (m_epoch != state.session_epoch) {
      m_epoch = state.session_epoch;
      m_events = {};
      m_freshness = {};
      m_snapshots = {};
      m_snapshot_actions = {};
      m_snapshot_present = {};
      m_resource_actions.clear();
      m_service_actions.clear();
      m_workload_actions.clear();
      m_service_labels.clear();
      m_workload_labels.clear();
      m_services.set_items({});
    }
    refresh_targets(state);
    refresh_snapshots(state);
    m_summary = "Active target: " + target_identity(state.active_target);
    auto compact_identity = compact_target_identity(state.active_target);
    m_compact_title = std::move(compact_identity.title);
    m_compact_identity = std::move(compact_identity.body);
    if (state.pending_target)
      m_summary += " | Pending: " + safe_label(state.pending_target->value());
    m_summary += "\nEvidence retains its original target and observation time.";
    const auto message = state.problem ? error_text(state.problem->code)
                         : state.source_problem ? "Source preparation failed"
                                                : phase_text(state.phase);
    const auto compact = state.problem ? compact_error_text(state.problem->code)
                         : state.source_problem
                             ? "error source"
                             : compact_phase_text(state.phase);
    report(std::string{message}, std::string{compact});
    refresh_body();
    mark_dirty();
    return {};
  } catch (...) {
    return failure();
  }
}
auto AdminDialog::show(View view) -> void {
  m_menu.close_dropdown();
  m_view = view;
  if (view == View::services) {
    m_services.set_items(m_service_labels);
    m_resource_actions = m_service_actions;
  } else if (view == View::workloads) {
    m_services.set_items(m_workload_labels);
    m_resource_actions = m_workload_actions;
  }
  refresh_body();
  children();
  mark_dirty();
}
auto AdminDialog::set_toolbar_visible(bool visible) -> void {
  m_menu.close_dropdown();
  m_toolbar_visible = visible;
  children();
  mark_dirty();
}
auto AdminDialog::children() -> void {
  auto* previous = ring().current();
  clear_children();
  const auto visible = [this](termforge::Widget* widget) {
    if (widget->rect().w > 0 && widget->rect().h > 0) add_child(widget);
  };
  if (m_toolbar_visible) visible(&m_menu);
  if (m_wide || m_view == View::targets) {
    visible(&m_targets);
    visible(&m_use);
  }
  if (m_view == View::services || m_view == View::workloads)
    visible(&m_services);
  if (m_wide || m_view != View::targets) visible(&m_evidence);
  if (m_view != View::targets) visible(&m_read);
  visible(&m_cancel);
  visible(&m_close);
  static_cast<void>(ring().focus(previous));
}
auto AdminDialog::content_rows() const -> int {
  return 24;
}
auto AdminDialog::content_cols() const -> int {
  return 112;
}
auto AdminDialog::layout_content(termforge::Rect area) -> void {
  area.w = std::max(0, area.w);
  area.h = std::max(0, area.h);
  if (area.x != m_area.x || area.y != m_area.y || area.w != m_area.w ||
      area.h != m_area.h)
    m_menu.close_dropdown();
  m_area = area;
  const bool wide = area.w >= 72;
  if (wide != m_wide) {
    m_wide = wide;
    children();
  }
  const auto menu_rows = !m_compact && m_toolbar_visible && area.h > 0 ? 1 : 0;
  m_menu.set_geometry({area.x, area.y, area.w, menu_rows});
  area.y += menu_rows;
  area.h -= menu_rows;
  const auto buttons = !m_compact && area.h > 0 ? 1 : 0;
  const auto body_rows = area.h - buttons;
  m_targets.set_geometry({});
  m_services.set_geometry({});
  m_evidence.set_geometry({});
  const auto left = m_wide ? area.w / 3 : 0;
  if (!m_compact && (m_wide || m_view == View::targets))
    m_targets.set_geometry({area.x, area.y, m_wide ? left : area.w, body_rows});
  termforge::Rect body{area.x + left, area.y, area.w - left, body_rows};
  if (!m_compact && (m_view == View::services || m_view == View::workloads)) {
    const auto rows = body.h / 2;
    m_services.set_geometry({body.x, body.y, body.w, rows});
    body.y += rows;
    body.h -= rows;
  }
  if (m_wide || m_view != View::targets) m_evidence.set_geometry(body);
  const auto width = area.w / 4;
  const auto y = area.y + body_rows;
  m_use.set_geometry({area.x, y, width, buttons});
  m_read.set_geometry({area.x + width, y, width, buttons});
  m_cancel.set_geometry({area.x + (2 * width), y, width, buttons});
  m_close.set_geometry(
      {area.x + (3 * width), y, area.w - (3 * width), buttons});
  children();
}
auto AdminDialog::draw(termforge::Screen& screen) -> void {
  const bool compact = screen.cols() <= 22 || screen.rows() <= 7;
  if (compact != m_compact) {
    m_compact = compact;
    refresh_body();
    children();
  }
  if (m_compact) {
    constexpr std::array slots{0U, 0U, 1U, 2U, 3U, 4U, 5U};
    const auto slot = slots[static_cast<std::size_t>(m_view)];
    set_title(m_compact_title);
    auto text = m_compact_identity;
    if (!text.empty()) text += '\n';
    text += freshness_text(m_freshness[slot]);
    text += '\n';
    text += m_compact_status;
    set_text(std::move(text));
  } else {
    set_title("Admin");
    set_text(m_dialog_text);
  }
  Dialog::draw(screen);
}
auto AdminDialog::draw_content(termforge::Screen& screen) -> void {
  m_targets.draw(screen);
  m_services.draw(screen);
  m_evidence.draw(screen);
  if (m_wide || m_view == View::targets) m_use.draw(screen);
  if (m_view != View::targets) m_read.draw(screen);
  m_cancel.draw(screen);
  m_close.draw(screen);
  if (m_toolbar_visible) m_menu.draw(screen);
}
auto AdminDialog::on_event(const termforge::Event& event) -> bool {
  if (const auto* key = std::get_if<termforge::KeyEvent>(&event);
      key != nullptr && key->action != termforge::KeyAction::Release) {
    if (key->key == termforge::Key::F10) {
      set_toolbar_visible(true);
      layout_content(m_area);
      static_cast<void>(ring().focus(&m_menu));
      return true;
    }
    if (key->key == termforge::Key::Char && !key->ctrl && !key->alt &&
        !m_menu.dropdown_open()) {
      switch (key->ch) {
        case U't': show(View::targets); return true;
        case U'h': show(View::health); return true;
        case U's': show(View::services); return true;
        case U'd': show(View::details); return true;
        case U'w': show(View::workloads); return true;
        case U'p': show(View::pod); return true;
        case U'e': show(View::events); return true;
        case U'u': use_target(); return true;
        case U'r': read_view(); return true;
        case U'c': perform(AdminCancel{}); return true;
        default: break;
      }
    }
  }
  return Dialog::on_event(event);
}
auto AdminDialog::on_escape() -> void {
  if (!begin_result()) return;
  perform(AdminCloseView{});
  close();
}
auto AdminDialog::status() const noexcept -> const std::string& {
  return m_status;
}
auto AdminDialog::display_text() const noexcept -> const std::string& {
  return m_body;
}
} // namespace aiforge::adapters
