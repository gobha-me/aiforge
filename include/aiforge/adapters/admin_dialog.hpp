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
  explicit AdminDialog(surfaces::AdminControls& controls);
  [[nodiscard]] auto execute(const surfaces::AdminAction& action)
      -> std::expected<void, surfaces::ManualOpsFailure>;
  [[nodiscard]] auto refresh()
      -> std::expected<void, surfaces::ManualOpsFailure>;
  auto set_toolbar_visible(bool visible) -> void;
  [[nodiscard]] auto status() const noexcept -> const std::string&;
  [[nodiscard]] auto display_text() const noexcept -> const std::string&;
  auto on_event(const termforge::Event& event) -> bool override;

 protected:
  [[nodiscard]] auto content_rows() const -> int override;
  [[nodiscard]] auto content_cols() const -> int override;
  auto layout_content(termforge::Rect area) -> void override;
  auto draw_content(termforge::Screen& screen) -> void override;
  auto on_escape() -> void override;

 private:
  enum class View { targets, health, services, details };
  auto perform(surfaces::AdminAction action) -> void;
  auto show(View view) -> void;
  auto children() -> void;
  auto use_target() -> void;
  auto read_view() -> void;
  auto read_service(int row) -> void;
  auto report(std::string message) -> void;
  auto refresh_targets(const surfaces::AdminState& state) -> void;
  auto refresh_snapshots(const surfaces::AdminState& state) -> void;
  auto refresh_body() -> void;
  surfaces::AdminControls& m_controls;
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
  termforge::Rect m_area{};
  std::vector<domain::OpsTargetId> m_target_ids;
  std::vector<std::string> m_target_labels;
  std::vector<surfaces::AdminReadCachedService> m_service_actions;
  std::array<std::optional<domain::EventId>, 3> m_events;
  std::array<std::string, 3> m_snapshots;
  std::uint64_t m_epoch{};
  std::string m_status;
  std::string m_summary;
  std::string m_body;
};
} // namespace aiforge::adapters
