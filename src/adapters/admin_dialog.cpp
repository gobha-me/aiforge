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
auto safe_label(std::string_view text) -> std::string {
  if (text.empty() || text.size() > 128 || !detail::is_safe_utf8_text(text) ||
      text.find_first_of("\r\n\t") != std::string_view::npos)
    return "Unavailable label";
  return std::string{text};
}
auto index_valid(int index, std::size_t count) -> bool {
  return index >= 0 && std::cmp_less(index, count);
}
struct PreparedSnapshot {
  std::optional<domain::EventId> event;
  std::string text{"No committed observation for this view."};
  std::vector<std::string> labels;
  std::vector<AdminReadCachedService> actions;
};
auto prepare_snapshot(const std::optional<CommittedOpsObservation>& snapshot,
                      bool include_services) -> PreparedSnapshot {
  PreparedSnapshot result;
  if (!snapshot) return result;
  const auto content =
      runtime::format_ops_observation_content(snapshot->observation);
  if (!content || content->size() != 1 ||
      !std::holds_alternative<domain::TextBlock>(content->front())) {
    result.text = "Committed observation could not be rendered.";
    return result;
  }
  result.event = snapshot->observation_event_id;
  result.text = "Committed evidence | event " +
                safe_label(result.event->value()) + "\n" +
                std::get<domain::TextBlock>(content->front()).text;
  if (!include_services) return result;
  const auto* services = std::get_if<domain::LinuxServicesObservation>(
      &snapshot->observation.payload);
  if (services == nullptr) return result;
  const auto& request = snapshot->observation.request;
  for (const auto& service : services->services) {
    result.actions.push_back(AdminReadCachedService{
        request.session_id, *result.event, request.selection_generation,
        result.labels.size()});
    result.labels.push_back(service.identity.unit_name);
  }
  return result;
}
} // namespace

AdminDialog::AdminDialog(AdminControls& controls)
    : Dialog("Admin"), m_controls(controls) {
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
         {"Cancel", [this] { perform(AdminCancel{}); }}}},
       {"View",
        {{"Targets", [this] { show(View::targets); }},
         {"Health", [this] { show(View::health); }},
         {"Services", [this] { show(View::services); }},
         {"Details", [this] { show(View::details); }},
         {"Hide toolbar", [this] { set_toolbar_visible(false); }}}}});
  m_targets.on_select([this](int, const std::string&) {
    report("Use target explicitly to prepare the highlighted target");
  });
  m_services.on_select(
      [this](int row, const std::string&) { read_service(row); });
  m_use.on_activate([this] { use_target(); });
  m_read.on_activate([this] { read_view(); });
  m_cancel.on_activate([this] { perform(AdminCancel{}); });
  m_close.on_activate([this] { on_escape(); });
  children();
  if (!refresh()) report("Admin presentation is unavailable");
}
auto AdminDialog::report(std::string message) -> void {
  m_status = std::move(message);
  set_text(m_status + "\n" + m_summary +
           "\nT targets | H health | S services | D details"
           "\nU use | R read | C cancel | F10 toolbar | Esc close");
}
auto AdminDialog::execute(const AdminAction& action)
    -> std::expected<void, ManualOpsFailure> {
  try {
    auto result = m_controls.execute(action);
    const auto presentation = refresh();
    if (!result) {
      report(std::string{error_text(result.error().code)});
      return result;
    }
    if (!presentation) return presentation;
    return {};
  } catch (...) {
    return failure();
  }
}
auto AdminDialog::perform(AdminAction action) -> void {
  if (const auto result = execute(action); !result)
    report(std::string{error_text(result.error().code)});
}
auto AdminDialog::read_view() -> void {
  if (m_view == View::services)
    perform(AdminReadServices{});
  else if (m_view == View::details)
    read_service(m_services.selected());
  else
    perform(AdminReadHealth{});
}
auto AdminDialog::use_target() -> void {
  const auto row = m_targets.selected();
  if (!index_valid(row, m_target_ids.size())) {
    report("Highlight a configured target first");
    return;
  }
  perform(AdminSelectTarget{m_target_ids[static_cast<std::size_t>(row)]});
}
auto AdminDialog::read_service(int row) -> void {
  if (!index_valid(row, m_service_actions.size())) {
    report("Read loaded services and choose a service first");
    return;
  }
  // Copy the displayed identity before execute can refresh the widget rows.
  perform(m_service_actions[static_cast<std::size_t>(row)]);
  show(View::details);
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
                       (target.kind == domain::OpsTargetKind::linux_local
                            ? ""
                            : " (collection unavailable)"));
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
  for (std::size_t slot = 0; slot < state.snapshots.size(); ++slot) {
    const auto& snapshot = state.snapshots[slot];
    if (snapshot && m_events[slot] == snapshot->observation_event_id) continue;
    if (!snapshot && !m_events[slot]) continue;
    auto prepared = prepare_snapshot(snapshot, slot == 1);
    if (slot == 1) {
      m_services.set_items(std::move(prepared.labels));
      m_service_actions = std::move(prepared.actions);
    }
    m_snapshots[slot] = std::move(prepared.text);
    m_events[slot] = std::move(prepared.event);
  }
}
auto AdminDialog::refresh_body() -> void {
  const auto slot = m_view == View::services  ? 1U
                    : m_view == View::details ? 2U
                                              : 0U;
  const auto& text = m_snapshots[slot];
  const std::string_view next = text.empty()
                                    ? "No committed observation for this view."
                                    : std::string_view{text};
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
      m_snapshots = {};
      m_service_actions.clear();
      m_services.set_items({});
    }
    refresh_targets(state);
    refresh_snapshots(state);
    m_summary = "Active target: " +
                (state.active_target
                     ? safe_label(state.active_target->target_id.value())
                     : "none");
    if (state.pending_target)
      m_summary += " | Pending: " + safe_label(state.pending_target->value());
    m_summary += "\nEvidence retains its original target and observation time.";
    const auto message = state.problem ? error_text(state.problem->code)
                         : state.source_problem ? "Source preparation failed"
                                                : phase_text(state.phase);
    report(std::string{message});
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
  if (m_view == View::services) visible(&m_services);
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
  const auto menu_rows = m_toolbar_visible && area.h > 0 ? 1 : 0;
  m_menu.set_geometry({area.x, area.y, area.w, menu_rows});
  area.y += menu_rows;
  area.h -= menu_rows;
  const auto buttons = area.h > 0 ? 1 : 0;
  const auto body_rows = area.h - buttons;
  m_targets.set_geometry({});
  m_services.set_geometry({});
  m_evidence.set_geometry({});
  const auto left = m_wide ? area.w / 3 : 0;
  if (m_wide || m_view == View::targets)
    m_targets.set_geometry({area.x, area.y, m_wide ? left : area.w, body_rows});
  termforge::Rect body{area.x + left, area.y, area.w - left, body_rows};
  if (m_view == View::services) {
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
