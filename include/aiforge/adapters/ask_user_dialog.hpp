#pragma once

#include <expected>
#include <functional>
#include <optional>
#include <string>

#include <aiforge/runtime/run_kernel.hpp>
#include <aiforge/surfaces/chat_session.hpp>
#include <termforge/widgets/choice_wizard_dialog.hpp>

namespace aiforge::adapters {

enum class AskUserDialogErrorCode {
  invalid_request,
  runtime_failure,
};

struct AskUserDialogError {
  AskUserDialogErrorCode code{AskUserDialogErrorCode::invalid_request};
  std::string message;
  auto operator==(const AskUserDialogError&) const -> bool = default;
};

// Maps stable application identities to TermForge presentation indices. The
// controller and kernel must outlive the showing configured by present().
class AskUserDialogController final {
 public:
  explicit AskUserDialogController(termforge::ChoiceWizardDialog& dialog)
      : m_dialog(dialog) {}

  [[nodiscard]] auto present(runtime::PendingQuestionInput input,
                             runtime::RunKernel& kernel,
                             std::function<void()> on_resolved = {})
      -> std::expected<void, AskUserDialogError>;
  [[nodiscard]] auto present(runtime::PendingQuestionInput input,
                             surfaces::ChatSession& session,
                             std::function<void()> on_resolved = {})
      -> std::expected<void, AskUserDialogError>;

  [[nodiscard]] auto last_error() const noexcept
      -> const std::optional<AskUserDialogError>& {
    return m_last_error;
  }
  [[nodiscard]] auto was_cancelled() const noexcept -> bool {
    return m_cancelled;
  }

 private:
  [[nodiscard]] auto prepare(runtime::PendingQuestionInput input,
                             std::function<void()> on_resolved)
      -> std::expected<void, AskUserDialogError>;
  auto resolve(std::optional<termforge::ChoiceWizardResult> result) -> void;
  auto finish() -> void;

  termforge::ChoiceWizardDialog& m_dialog;
  runtime::RunKernel* m_kernel{};
  surfaces::ChatSession* m_session{};
  std::optional<runtime::PendingQuestionInput> m_input;
  std::optional<AskUserDialogError> m_last_error;
  std::function<void()> m_on_resolved;
  bool m_resolved{};
  bool m_cancelled{};
};

} // namespace aiforge::adapters
