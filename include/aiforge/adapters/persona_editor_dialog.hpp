#pragma once

#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <variant>

#include <aiforge/persona/editor.hpp>

#include <termforge/widgets/button.hpp>
#include <termforge/widgets/composer.hpp>
#include <termforge/widgets/dialog.hpp>

namespace aiforge::adapters {

using PersonaEditorSubmission =
    std::variant<persona::PersonaCreate, persona::PersonaReplace>;
using SavePersona = std::function<
    std::expected<persona::PersonaWriteReceipt, persona::PersonaEditorError>(
        PersonaEditorSubmission)>;

struct PersonaEditorDialogResult {
  std::optional<persona::PersonaWriteReceipt> receipt;
  bool effect_may_have_applied{};
};

enum class PersonaEditorDialogStage {
  editing,
  preview,
};

class PersonaEditorDialog final : public termforge::Dialog {
 public:
  PersonaEditorDialog();

  auto set_submission(PersonaEditorSubmission submission, bool selected)
      -> void;
  auto on_save(SavePersona callback) -> void;
  auto on_result(std::function<void(PersonaEditorDialogResult)> callback)
      -> void;

  [[nodiscard]] auto stage() const noexcept -> PersonaEditorDialogStage;
  [[nodiscard]] auto draft_text() const noexcept -> const std::string&;
  [[nodiscard]] auto preview() const noexcept
      -> const std::optional<domain::PersonaDocument>&;
  [[nodiscard]] auto error_message() const noexcept -> const std::string&;

  auto on_event(const termforge::Event& event) -> bool override;

 protected:
  [[nodiscard]] auto content_rows() const -> int override;
  [[nodiscard]] auto content_cols() const -> int override { return 92; }
  auto layout_content(termforge::Rect area) -> void override;
  auto draw_content(termforge::Screen& screen) -> void override;
  auto on_escape() -> void override;

 private:
  auto show_editing() -> void;
  auto show_preview() -> void;
  auto review() -> void;
  auto save() -> void;
  auto cancel() -> void;
  auto update_body() -> void;
  [[nodiscard]] auto submission_with_text(std::string text) const
      -> PersonaEditorSubmission;
  [[nodiscard]] auto prepare(const PersonaEditorSubmission& submission) const
      -> std::expected<domain::PersonaDocument, persona::PersonaEditorError>;
  [[nodiscard]] auto accepts_insertion(std::string insertion) -> bool;
  [[nodiscard]] auto accepts_key(const termforge::KeyEvent& key) -> bool;

  PersonaEditorSubmission m_submission{persona::PersonaCreate{}};
  PersonaEditorSubmission m_reviewed_submission{persona::PersonaCreate{}};
  termforge::Composer m_composer;
  termforge::Button m_review{"[ Review ]"};
  termforge::Button m_save{"[ Save ]"};
  termforge::Button m_back{"[ Back ]"};
  termforge::Button m_cancel{"[ Cancel ]"};
  PersonaEditorDialogStage m_stage{PersonaEditorDialogStage::editing};
  bool m_selected{};
  std::optional<domain::PersonaDocument> m_preview;
  std::string m_error;
  bool m_save_blocked{};
  SavePersona m_on_save;
  std::function<void(PersonaEditorDialogResult)> m_on_result;
};

} // namespace aiforge::adapters
