#pragma once
#include <aiforge/surfaces/chat_session.hpp>
#include <aiforge/surfaces/conversation_commands.hpp>
#include <functional>
#include <termforge/widgets/button.hpp>
#include <termforge/widgets/choice_wizard_dialog.hpp>
#include <termforge/widgets/composer.hpp>
#include <termforge/widgets/dialog.hpp>
#include <termforge/widgets/menu_bar.hpp>
#include <termforge/widgets/text_box.hpp>

namespace aiforge::adapters {
// Owns a single explicit review; neither opening nor closing submits the chat
// composer, dispatches an inference, or applies a summary.
class ConversationContextDialog final : public termforge::Dialog {
 public:
  explicit ConversationContextDialog(surfaces::ChatSession& session,
                                     std::function<std::string()> draft);
  [[nodiscard]] auto execute(const surfaces::ConversationCommand& command)
      -> std::expected<void, surfaces::ChatSessionError>;
  auto on_toolbar(std::function<void(bool)> callback) -> void;
  auto on_repository(std::function<void()> callback) -> void;
  auto on_committed(std::function<void(std::vector<domain::RunEvent>)> callback)
      -> void;
  auto invalidate_review() -> void;
  [[nodiscard]] auto status() const noexcept -> const std::string&;
  [[nodiscard]] auto review_available() const noexcept -> bool;
  [[nodiscard]] auto editing_text() const noexcept -> const std::string&;
  [[nodiscard]] auto display_text() const noexcept -> const std::string&;
  [[nodiscard]] auto hit_test_tree(int x, int y) const -> bool override;
  auto draw(termforge::Screen& screen) -> void override;
  auto on_event(const termforge::Event& event) -> bool override;

 protected:
  [[nodiscard]] auto content_rows() const -> int override;
  [[nodiscard]] auto content_cols() const -> int override;
  auto layout_content(termforge::Rect area) -> void override;
  auto draw_content(termforge::Screen& screen) -> void override;
  auto on_escape() -> void override;

 private:
  [[nodiscard]] auto dispatch(const surfaces::ConversationCommand& command)
      -> std::expected<void, surfaces::ChatSessionError>;
  [[nodiscard]] auto inspect()
      -> std::expected<void, surfaces::ChatSessionError>;
  [[nodiscard]] auto summaries()
      -> std::expected<void, surfaces::ChatSessionError>;
  [[nodiscard]] auto candidate(const domain::ConversationSummaryId& id,
                               bool publish)
      -> std::expected<domain::ConversationSummaryCandidate,
                       surfaces::ChatSessionError>;
  [[nodiscard]] auto set_mode(const surfaces::SetConversationMode& command)
      -> std::expected<void, surfaces::ChatSessionError>;
  [[nodiscard]] auto set_pin(const surfaces::PinConversationRun& command)
      -> std::expected<void, surfaces::ChatSessionError>;
  [[nodiscard]] auto generate(
      const surfaces::GenerateConversationSummary& command)
      -> std::expected<void, surfaces::ChatSessionError>;
  [[nodiscard]] auto review(const domain::ConversationSummaryId& id, bool edit)
      -> std::expected<void, surfaces::ChatSessionError>;
  [[nodiscard]] auto preview(
      const surfaces::PreviewConversationSummary& command)
      -> std::expected<void, surfaces::ChatSessionError>;
  [[nodiscard]] auto apply() -> std::expected<void, surfaces::ChatSessionError>;
  [[nodiscard]] auto disable(const domain::ConversationSummaryId& id)
      -> std::expected<void, surfaces::ChatSessionError>;
  auto choose_sources(bool pin) -> void;
  auto choose_summary(unsigned action) -> void;
  auto choose_replacements(domain::ConversationSummaryId id) -> void;
  auto choose(termforge::ChoiceWizardPage page,
              std::function<void(std::vector<std::size_t>)> callback) -> void;
  auto save_edit() -> void;
  auto body(std::string text) -> void;
  auto standard_children() -> void;
  auto perform(surfaces::ConversationCommand command) -> void;
  surfaces::ChatSession& m_session;
  std::function<std::string()> m_draft;
  std::function<void(bool)> m_toolbar;
  std::function<void()> m_repository;
  std::function<void(std::vector<domain::RunEvent>)> m_committed;
  termforge::MenuBar m_menu;
  termforge::TextBox m_text;
  termforge::Button m_primary{"[ Refresh ]"};
  termforge::Button m_secondary{"[ Summaries ]"};
  termforge::Button m_close{"[ Close ]"};
  termforge::Composer m_editor;
  termforge::ChoiceWizardDialog m_picker;
  bool m_picking{};
  bool m_editing{};
  std::optional<domain::ConversationSummaryCandidate> m_edit_parent;
  std::uint64_t m_edit_sequence{};
  std::optional<surfaces::ChatSummaryPreview> m_review;
  std::string m_review_draft;
  domain::ModelId m_review_model;
  std::uint64_t m_review_sequence{};
  std::string m_status;
  std::string m_body;
};
} // namespace aiforge::adapters
