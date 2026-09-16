#pragma once

#include <aiforge/surfaces/admin_controller.hpp>
#include <termforge/widgets/button.hpp>
#include <termforge/widgets/dialog.hpp>
#include <termforge/widgets/list_widget.hpp>
#include <termforge/widgets/menu_bar.hpp>
#include <termforge/widgets/text_box.hpp>

namespace aiforge::adapters {
// Borrows cached controls only. The application owns pumping, binding, policy,
// approval overlays and lifetime; this view never collects or polls evidence.
class AdminDialog final : public termforge::Dialog {
 public:
  explicit AdminDialog(surfaces::AdminControls& controls,
                       runtime::TimestampSource timestamp_source = {});
  [[nodiscard]] auto execute(const surfaces::AdminAction& action)
      -> std::expected<void, surfaces::ManualOpsFailure>;
  [[nodiscard]] auto refresh()
      -> std::expected<void, surfaces::ManualOpsFailure>;
  auto set_toolbar_visible(bool visible) -> void;
  [[nodiscard]] auto status() const noexcept -> const std::string&;
  [[nodiscard]] auto display_text() const noexcept -> const std::string&;
  auto draw(termforge::Screen& screen) -> void override;
  auto on_event(const termforge::Event& event) -> bool override;

 protected:
  [[nodiscard]] auto content_rows() const -> int override;
  [[nodiscard]] auto content_cols() const -> int override;
  auto layout_content(termforge::Rect area) -> void override;
  auto draw_content(termforge::Screen& screen) -> void override;
  auto on_escape() -> void override;

 private:
  enum class View {
    targets,
    health,
    services,
    details,
    workloads,
    pod,
    events
  };
  auto perform(surfaces::AdminAction action) -> void;
  auto show(View view) -> void;
  auto children() -> void;
  auto use_target() -> void;
  auto read_view() -> void;
  auto read_resource(int row) -> void;
  auto read_service(int row) -> void;
  auto read_pod(int row) -> void;
  auto read_resource_events(int row) -> void;
  auto report(std::string message, std::string compact) -> void;
  auto refresh_targets(const surfaces::AdminState& state) -> void;
  auto refresh_snapshots(const surfaces::AdminState& state) -> void;
  auto refresh_body() -> void;
  surfaces::AdminControls& m_controls;
  runtime::TimestampSource m_timestamp_source;
  termforge::MenuBar m_menu;
  termforge::ListWidget m_targets;
  termforge::ListWidget m_services;
  termforge::TextBox m_evidence;
  termforge::Button m_use{"[ Use target ]"};
  termforge::Button m_read{"[ Read ]"};
  termforge::Button m_cancel{"[ Cancel ]"};
  termforge::Button m_close{"[ Close ]"};
  View m_view{View::targets};
  bool m_toolbar_visible{true};
  bool m_wide{};
  bool m_compact{};
  termforge::Rect m_area{};
  std::vector<domain::OpsTargetId> m_target_ids;
  std::vector<std::string> m_target_labels;
  std::vector<std::optional<surfaces::AdminAction>> m_resource_actions;
  std::vector<std::optional<surfaces::AdminAction>> m_service_actions;
  std::vector<std::optional<surfaces::AdminAction>> m_workload_actions;
  std::vector<std::string> m_service_labels;
  std::vector<std::string> m_workload_labels;
  std::array<std::optional<domain::EventId>, surfaces::admin_snapshot_count>
      m_events;
  std::array<surfaces::AdminEvidenceFreshness, surfaces::admin_snapshot_count>
      m_freshness{};
  std::array<std::optional<surfaces::AdminAction>,
             surfaces::admin_snapshot_count>
      m_snapshot_actions;
  std::array<bool, surfaces::admin_snapshot_count> m_snapshot_present{};
  std::array<std::string, surfaces::admin_snapshot_count> m_snapshots;
  std::uint64_t m_epoch{};
  std::string m_status;
  std::string m_summary;
  std::string m_dialog_text;
  std::string m_compact_title{"target none"};
  std::string m_compact_identity;
  std::string m_compact_status{"status unavailable"};
  std::string m_body;
};
} // namespace aiforge::adapters
