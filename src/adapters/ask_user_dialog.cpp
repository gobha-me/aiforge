#include <aiforge/adapters/ask_user_dialog.hpp>

#include <string>
#include <utility>

namespace aiforge::adapters {
namespace {

[[nodiscard]] auto invalid(std::string message)
    -> std::unexpected<AskUserDialogError> {
  return std::unexpected(AskUserDialogError{
      AskUserDialogErrorCode::invalid_request, std::move(message)});
}

} // namespace

auto AskUserDialogController::present(runtime::PendingQuestionInput input,
                                      runtime::RunKernel& kernel,
                                      std::function<void()> on_resolved)
    -> std::expected<void, AskUserDialogError> {
  const auto current = kernel.pending_question_input();
  if (!current || *current != input) {
    return invalid("question dialog request is stale or unavailable");
  }
  m_kernel = &kernel;
  m_session = nullptr;
  return prepare(std::move(input), std::move(on_resolved));
}

auto AskUserDialogController::present(runtime::PendingQuestionInput input,
                                      surfaces::ChatSession& session,
                                      std::function<void()> on_resolved)
    -> std::expected<void, AskUserDialogError> {
  const auto current = session.pending_question_input();
  if (!current || *current != input) {
    return invalid("question dialog request is stale or unavailable");
  }
  m_kernel = nullptr;
  m_session = &session;
  return prepare(std::move(input), std::move(on_resolved));
}

auto AskUserDialogController::prepare(runtime::PendingQuestionInput input,
                                      std::function<void()> on_resolved)
    -> std::expected<void, AskUserDialogError> {
  try {
    if (input.questions.empty()) {
      return invalid("question dialog request is stale or unavailable");
    }

    std::vector<termforge::ChoiceWizardPage> pages;
    pages.reserve(input.questions.size());
    for (std::size_t index = 0; index < input.questions.size(); ++index) {
      const auto& question = input.questions[index];
      termforge::ChoiceWizardPage page;
      page.title = "Question " + std::to_string(index + 1) + " of " +
                   std::to_string(input.questions.size());
      page.text = question.prompt;
      page.mode = question.selection == domain::QuestionSelection::one &&
                          question.required
                      ? termforge::ChoiceMode::Single
                      : termforge::ChoiceMode::Multiple;
      page.minimum_selected = question.minimum_selections;
      page.maximum_selected = question.maximum_selections;
      for (std::size_t option_index = 0; option_index < question.options.size();
           ++option_index) {
        const auto& option = question.options[option_index];
        page.choices.push_back(
            {option.label, option.description.value_or(std::string{})});
        if (option.recommended) {
          page.selected_indices.push_back(option_index);
        }
      }
      if (question.other) {
        page.other_enabled = true;
        page.other_label = question.other->label;
        page.other_placeholder =
            question.other->placeholder.value_or(std::string{});
      }
      pages.push_back(std::move(page));
    }
    if (!m_dialog.set_pages(std::move(pages))) {
      return invalid("question dialog rejected its pages");
    }
    m_input = std::move(input);
    m_last_error.reset();
    m_on_resolved = std::move(on_resolved);
    m_resolved = false;
    m_cancelled = false;
    m_dialog.on_result(
        [this](std::optional<termforge::ChoiceWizardResult> result) {
          resolve(std::move(result));
        });
    return {};
  } catch (...) {
    return invalid("question dialog setup failed internally");
  }
}

// clang-format off
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Explicitly validates and routes every dialog resolution state.
auto AskUserDialogController::resolve(
    std::optional<termforge::ChoiceWizardResult> result) -> void {
  // clang-format on
  if (m_resolved || (m_kernel == nullptr && m_session == nullptr) || !m_input)
    return;
  m_resolved = true;
  if (!result) {
    m_cancelled = true;
    if (m_kernel != nullptr) {
      auto resolved = m_kernel->cancel_questions(
          m_input->run_id, m_input->invocation_id, "dialog cancelled");
      if (!resolved) {
        m_last_error = {AskUserDialogErrorCode::runtime_failure,
                        resolved.error().message};
      }
    } else {
      auto resolved = m_session->cancel_questions(
          m_input->run_id, m_input->invocation_id, "dialog cancelled");
      if (!resolved) {
        m_last_error = {AskUserDialogErrorCode::runtime_failure,
                        resolved.error().message};
      }
    }
  } else if (result->pages.size() != m_input->questions.size()) {
    m_last_error = {AskUserDialogErrorCode::invalid_request,
                    "question dialog returned the wrong page count"};
  } else {
    std::vector<domain::QuestionAnswer> answers;
    answers.reserve(result->pages.size());
    for (std::size_t page_index = 0; page_index < result->pages.size();
         ++page_index) {
      const auto& question = m_input->questions[page_index];
      const auto& page = result->pages[page_index];
      std::vector<std::string> selected_ids;
      selected_ids.reserve(page.selected_indices.size());
      for (const auto option_index : page.selected_indices) {
        if (option_index >= question.options.size()) {
          m_last_error = {AskUserDialogErrorCode::invalid_request,
                          "question dialog returned an invalid option"};
          finish();
          return;
        }
        selected_ids.push_back(question.options[option_index].option_id);
      }
      answers.push_back(
          {question.question_id, std::move(selected_ids), page.other});
    }
    if (m_kernel != nullptr) {
      auto resolved = m_kernel->answer_questions(
          m_input->run_id, m_input->invocation_id, std::move(answers));
      if (!resolved) {
        m_last_error = {AskUserDialogErrorCode::runtime_failure,
                        resolved.error().message};
      }
    } else {
      auto resolved = m_session->answer_questions(
          m_input->run_id, m_input->invocation_id, std::move(answers));
      if (!resolved) {
        m_last_error = {AskUserDialogErrorCode::runtime_failure,
                        resolved.error().message};
      }
    }
  }
  finish();
}

auto AskUserDialogController::finish() -> void {
  if (m_on_resolved) m_on_resolved();
}

} // namespace aiforge::adapters
