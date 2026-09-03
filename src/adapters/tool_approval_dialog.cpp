#include <aiforge/adapters/tool_approval_dialog.hpp>

#include <algorithm>
#include <limits>
#include <span>
#include <string_view>
#include <utility>

#include <aiforge/detail/utf8_text.hpp>

namespace aiforge::adapters {
namespace {

constexpr std::size_t kDenyChoice = 0;
constexpr std::size_t kAllowOnceChoice = 1;

[[nodiscard]] auto failure(const ToolApprovalDialogErrorCode code,
                           std::string message)
    -> std::unexpected<ToolApprovalDialogError> {
  return std::unexpected(ToolApprovalDialogError{code, std::move(message)});
}

[[nodiscard]] auto is_single_line_safe(const std::string_view value) -> bool {
  return detail::is_safe_utf8_text(value) &&
         value.find_first_of("\r\n\t") == std::string_view::npos;
}

[[nodiscard]] auto valid_effect(const domain::Effect effect) -> bool {
  switch (effect) {
    case domain::Effect::read:
    case domain::Effect::write:
    case domain::Effect::remove:
    case domain::Effect::execute:
    case domain::Effect::network:
    case domain::Effect::communicate:
    case domain::Effect::spend:
    case domain::Effect::change_infrastructure:
    case domain::Effect::change_privileges: return true;
  }
  return false;
}

[[nodiscard]] auto effect_text(const domain::Effect effect)
    -> std::string_view {
  switch (effect) {
    case domain::Effect::read: return "read";
    case domain::Effect::write: return "write";
    case domain::Effect::remove: return "remove";
    case domain::Effect::execute: return "execute";
    case domain::Effect::network: return "network";
    case domain::Effect::communicate: return "communicate externally";
    case domain::Effect::spend: return "spend";
    case domain::Effect::change_infrastructure: return "change infrastructure";
    case domain::Effect::change_privileges: return "change privileges";
  }
  return "unknown";
}

[[nodiscard]] auto limits_are_valid(const ToolApprovalDialogLimits& limits)
    -> bool {
  return limits.maximum_tool_name_bytes != 0 && limits.maximum_effects != 0 &&
         limits.maximum_scopes != 0 && limits.maximum_scope_kind_bytes != 0 &&
         limits.maximum_scope_value_bytes != 0 &&
         limits.maximum_total_text_bytes != 0;
}

[[nodiscard]] auto checked_add(std::size_t& total, const std::size_t amount)
    -> bool {
  if (amount > std::numeric_limits<std::size_t>::max() - total) return false;
  total += amount;
  return true;
}

[[nodiscard]] auto validate_effects(
    const std::span<const domain::Effect> effects,
    const std::size_t maximum_effects)
    -> std::expected<void, ToolApprovalDialogError> {
  if (effects.empty() || effects.size() > maximum_effects) {
    return failure(ToolApprovalDialogErrorCode::invalid_request,
                   "tool approval effects are empty or too numerous");
  }
  for (std::size_t index{}; index < effects.size(); ++index) {
    const auto previous = effects.first(index);
    if (!valid_effect(effects[index]) ||
        std::ranges::find(previous, effects[index]) != previous.end()) {
      return failure(ToolApprovalDialogErrorCode::invalid_request,
                     "tool approval effects are invalid or duplicated");
    }
  }
  return {};
}

[[nodiscard]] auto validate_scope_fields(
    const domain::CapabilityScope& scope,
    const std::span<const domain::Effect> effects,
    const ToolApprovalDialogLimits& limits)
    -> std::expected<void, ToolApprovalDialogError> {
  if (!valid_effect(scope.effect) ||
      std::ranges::find(effects, scope.effect) == effects.end()) {
    return failure(ToolApprovalDialogErrorCode::invalid_request,
                   "tool approval scope has an undeclared effect");
  }
  if (!is_single_line_safe(scope.kind) ||
      scope.kind.size() > limits.maximum_scope_kind_bytes ||
      !is_single_line_safe(scope.value) ||
      scope.value.size() > limits.maximum_scope_value_bytes) {
    return failure(ToolApprovalDialogErrorCode::invalid_request,
                   "tool approval scope is empty, unsafe, or too large");
  }
  return {};
}

[[nodiscard]] auto validate_scopes(
    const std::span<const domain::CapabilityScope> scopes,
    const std::span<const domain::Effect> effects,
    const ToolApprovalDialogLimits& limits, std::size_t& total)
    -> std::expected<void, ToolApprovalDialogError> {
  if (scopes.size() > limits.maximum_scopes) {
    return failure(ToolApprovalDialogErrorCode::invalid_request,
                   "tool approval scopes are too numerous");
  }
  for (std::size_t index{}; index < scopes.size(); ++index) {
    const auto& scope = scopes[index];
    if (auto valid = validate_scope_fields(scope, effects, limits); !valid) {
      return valid;
    }
    const auto previous = scopes.first(index);
    if (std::ranges::find(previous, scope) != previous.end()) {
      return failure(ToolApprovalDialogErrorCode::invalid_request,
                     "tool approval scope is duplicated");
    }
    if (!checked_add(total, scope.kind.size()) ||
        !checked_add(total, scope.value.size())) {
      return failure(ToolApprovalDialogErrorCode::invalid_request,
                     "tool approval text size overflowed");
    }
  }
  return {};
}

[[nodiscard]] auto validate(const PendingToolApprovalView& input,
                            const ToolApprovalDialogLimits& limits)
    -> std::expected<void, ToolApprovalDialogError> {
  if (!limits_are_valid(limits)) {
    return failure(ToolApprovalDialogErrorCode::invalid_limits,
                   "tool approval dialog limits must be positive");
  }
  if (!is_single_line_safe(input.tool_name) ||
      input.tool_name.size() > limits.maximum_tool_name_bytes) {
    return failure(ToolApprovalDialogErrorCode::invalid_request,
                   "tool approval name is empty, unsafe, or too large");
  }
  if (auto valid = validate_effects(input.effects, limits.maximum_effects);
      !valid) {
    return valid;
  }
  std::size_t total = input.tool_name.size();
  if (auto valid = validate_scopes(input.scopes, input.effects, limits, total);
      !valid) {
    return valid;
  }
  if (total > limits.maximum_total_text_bytes) {
    return failure(ToolApprovalDialogErrorCode::invalid_request,
                   "tool approval text is too large");
  }
  return {};
}

[[nodiscard]] auto summary(const PendingToolApprovalView& input)
    -> std::string {
  std::string text = "Tool: " + input.tool_name + "\nEffects: ";
  for (std::size_t index{}; index < input.effects.size(); ++index) {
    if (index != 0) text += ", ";
    text += effect_text(input.effects[index]);
  }
  text += "\nRequested authority:";
  if (input.scopes.empty()) {
    text += " none";
  } else {
    for (const auto& scope : input.scopes) {
      text += "\n- ";
      text += effect_text(scope.effect);
      text += " / ";
      text += scope.kind;
      text += ": ";
      text += scope.value;
    }
  }
  return text;
}

[[nodiscard]] auto no_grant(const domain::ApprovalDecision decision)
    -> runtime::ToolApprovalResolution {
  return {decision, {}, domain::ApprovalGrantLifetime::invocation};
}

} // namespace

ToolApprovalDialogController::~ToolApprovalDialogController() {
  m_dialog.on_result({});
}

auto ToolApprovalDialogController::present(
    PendingToolApprovalView input, ToolApprovalDialogCallback on_resolved)
    -> std::expected<void, ToolApprovalDialogError> {
  if (m_active) {
    return failure(ToolApprovalDialogErrorCode::invalid_request,
                   "a tool approval dialog is already active");
  }
  if (!on_resolved) {
    return failure(ToolApprovalDialogErrorCode::invalid_request,
                   "tool approval dialog requires a resolution callback");
  }
  if (auto valid = validate(input, m_limits); !valid) return valid;

  try {
    termforge::ChoiceWizardPage page;
    page.title = "Approve tool action";
    page.text = summary(input);
    page.mode = termforge::ChoiceMode::Single;
    page.choices = {
        {"Deny", "Do not run this tool."},
        {"Allow once", "Run once with exactly the requested authority."}};
    page.selected_indices = {kDenyChoice};
    page.minimum_selected = 1;
    page.maximum_selected = 1;
    if (!m_dialog.set_pages({std::move(page)})) {
      return failure(ToolApprovalDialogErrorCode::invalid_request,
                     "tool approval dialog rejected its page");
    }
    m_dialog.set_labels("Back", "Next", "Confirm", "Cancel");
    m_input = std::move(input);
    m_on_resolved = std::move(on_resolved);
    m_last_error.reset();
    m_cancelled = false;
    m_active = true;
    m_dialog.on_result(
        [this](std::optional<termforge::ChoiceWizardResult> result) {
          resolve(std::move(result));
        });
    return {};
  } catch (...) {
    return failure(ToolApprovalDialogErrorCode::invalid_request,
                   "tool approval dialog setup failed internally");
  }
}

auto ToolApprovalDialogController::resolve(
    std::optional<termforge::ChoiceWizardResult> result) -> void {
  if (!m_active || !m_input) return;
  if (!result) {
    m_cancelled = true;
    finish(no_grant(domain::ApprovalDecision::cancelled));
    return;
  }
  if (result->pages.size() != 1 ||
      result->pages.front().selected_indices.size() != 1) {
    m_last_error = {ToolApprovalDialogErrorCode::invalid_request,
                    "tool approval dialog returned an invalid result"};
    finish(no_grant(domain::ApprovalDecision::denied));
    return;
  }

  const auto choice = result->pages.front().selected_indices.front();
  if (choice == kDenyChoice) {
    finish(no_grant(domain::ApprovalDecision::denied));
    return;
  }
  if (choice == kAllowOnceChoice) {
    finish({domain::ApprovalDecision::approved, m_input->scopes,
            domain::ApprovalGrantLifetime::invocation});
    return;
  }
  m_last_error = {ToolApprovalDialogErrorCode::invalid_request,
                  "tool approval dialog returned an unknown choice"};
  finish(no_grant(domain::ApprovalDecision::denied));
}

auto ToolApprovalDialogController::finish(
    runtime::ToolApprovalResolution resolution) -> void {
  auto callback = std::move(m_on_resolved);
  m_active = false;
  m_input.reset();
  try {
    callback(std::move(resolution));
  } catch (...) {
    m_last_error = {ToolApprovalDialogErrorCode::callback_failure,
                    "tool approval resolution callback failed"};
  }
}

} // namespace aiforge::adapters
