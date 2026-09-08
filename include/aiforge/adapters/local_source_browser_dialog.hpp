#pragma once

#include <aiforge/surfaces/local_browser_commands.hpp>
#include <termforge/widgets/button.hpp>
#include <termforge/widgets/dialog.hpp>
#include <termforge/widgets/list_widget.hpp>
#include <termforge/widgets/menu_bar.hpp>
#include <termforge/widgets/text_box.hpp>
#include <termforge/widgets/text_input.hpp>

namespace aiforge::adapters {
// Presentation only. The application owns/polls the browser across dialog and
// session lifetimes. Closing cancels browsing, retaining the evidence tray and
// any independently prepared context. No composer or inference dependency.
class LocalSourceBrowserDialog final : public termforge::Dialog {
 public:
  explicit LocalSourceBrowserDialog(surfaces::LocalSourceBrowser& browser);
  [[nodiscard]] auto execute(const surfaces::LocalBrowserAction& action)
      -> std::expected<void, domain::LocalSourceError>;
  // Reads bounded in-memory state only. Call after application-owned poll().
  auto refresh() -> void;
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
  enum class View { files, preview, tray };
  auto perform(surfaces::LocalBrowserAction action) -> void;
  auto show(View view) -> void;
  auto children() -> void;
  auto open_folder(int index) -> void;
  auto open_entry(int index, bool add) -> void;
  auto remove_selected() -> void;
  auto remove_folder() -> void;
  auto navigate(bool parent) -> void;
  auto report(std::string message) -> void;
  auto refresh_folders() -> void;
  auto refresh_listing() -> void;
  auto refresh_tray() -> void;
  auto refresh_preview() -> void;
  auto layout_wide(termforge::Rect area) -> void;
  auto layout_narrow(termforge::Rect area) -> void;
  [[nodiscard]] auto input_event(const termforge::Event& event) -> bool;
  surfaces::LocalSourceBrowser& m_browser;
  termforge::MenuBar m_menu;
  termforge::TextInput m_path;
  termforge::TextInput m_filter;
  termforge::ListWidget m_folders;
  termforge::ListWidget m_files;
  termforge::ListWidget m_tray;
  termforge::TextBox m_preview;
  termforge::Button m_grant{"[ Add folder ]"};
  termforge::Button m_apply_filter{"[ Filter ]"};
  termforge::Button m_add{"[ Add evidence ]"};
  termforge::Button m_remove{"[ Remove ]"};
  termforge::Button m_close{"[ Close ]"};
  View m_view{View::files};
  bool m_wide{};
  std::uint64_t m_session_epoch{};
  std::vector<domain::LocalRootIdentity> m_folder_roots;
  std::vector<std::string> m_folder_labels;
  std::optional<runtime::LocalListResult> m_listing;
  std::vector<domain::LocalSourceIdentity> m_selection;
  std::vector<std::string> m_tray_labels;
  std::optional<runtime::LocalPreviewResult> m_preview_value;
  std::string m_status;
  std::string m_body;
  std::string m_browser_message;
};
} // namespace aiforge::adapters
