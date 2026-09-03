#pragma once

#include <cstddef>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <aiforge/domain/content.hpp>
#include <aiforge/runtime/run_kernel.hpp>
#include <termforge/widgets/choice_wizard_dialog.hpp>

namespace aiforge::adapters {

struct PendingToolApprovalView {
  std::string tool_name;
  std::vector<domain::Effect> effects;
  std::vector<domain::CapabilityScope> scopes;
  auto operator==(const PendingToolApprovalView&) const -> bool = default;
};

struct ToolApprovalDialogLimits {
  std::size_t maximum_tool_name_bytes{256};
  std::size_t maximum_effects{16};
  std::size_t maximum_scopes{64};
  std::size_t maximum_scope_kind_bytes{256};
  std::size_t maximum_scope_value_bytes{4096};
  std::size_t maximum_total_text_bytes{64U * 1024U};
  auto operator==(const ToolApprovalDialogLimits&) const -> bool = default;
};

enum class ToolApprovalDialogErrorCode {
  invalid_limits,
  invalid_request,
  callback_failure,
};

struct ToolApprovalDialogError {
  ToolApprovalDialogErrorCode code{
      ToolApprovalDialogErrorCode::invalid_request};
  std::string message;
  auto operator==(const ToolApprovalDialogError&) const -> bool = default;
};

using ToolApprovalDialogCallback =
    std::function<void(runtime::ToolApprovalResolution)>;

// Presents one bounded, runtime-owned approval decision. The dialog and this
// controller must both outlive the active showing. Every emitted grant has
// invocation lifetime; cancellation and malformed dialog results grant no
// scopes.
class ToolApprovalDialogController final {
 public:
  explicit ToolApprovalDialogController(termforge::ChoiceWizardDialog& dialog,
                                        ToolApprovalDialogLimits limits = {})
      : m_dialog(dialog), m_limits(limits) {}
  ~ToolApprovalDialogController();

  ToolApprovalDialogController(const ToolApprovalDialogController&) = delete;
  auto operator=(const ToolApprovalDialogController&)
      -> ToolApprovalDialogController& = delete;
  ToolApprovalDialogController(ToolApprovalDialogController&&) = delete;
  auto operator=(ToolApprovalDialogController&&)
      -> ToolApprovalDialogController& = delete;

  [[nodiscard]] auto present(PendingToolApprovalView input,
                             ToolApprovalDialogCallback on_resolved)
      -> std::expected<void, ToolApprovalDialogError>;

  [[nodiscard]] auto last_error() const noexcept
      -> const std::optional<ToolApprovalDialogError>& {
    return m_last_error;
  }
  [[nodiscard]] auto active() const noexcept -> bool { return m_active; }
  [[nodiscard]] auto was_cancelled() const noexcept -> bool {
    return m_cancelled;
  }

 private:
  auto resolve(std::optional<termforge::ChoiceWizardResult> result) -> void;
  auto finish(runtime::ToolApprovalResolution resolution) -> void;

  termforge::ChoiceWizardDialog& m_dialog;
  ToolApprovalDialogLimits m_limits;
  std::optional<PendingToolApprovalView> m_input;
  std::optional<ToolApprovalDialogError> m_last_error;
  ToolApprovalDialogCallback m_on_resolved;
  bool m_active{};
  bool m_cancelled{};
};

} // namespace aiforge::adapters
