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

}  // namespace

auto AskUserDialogController::present(runtime::PendingQuestionInput input,
                                      runtime::RunKernel& kernel)
    -> std::expected<void, AskUserDialogError> {
  try {
    const auto current = kernel.pending_question_input();
    if (!current || *current != input || input.questions.empty()) {
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
      for (std::size_t option_index = 0;
           option_index < question.options.size(); ++option_index) {
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
    m_kernel = &kernel;
    m_input = std::move(input);
    m_last_error.reset();
    m_resolved = false;
    m_dialog.on_result(
        [this](std::optional<termforge::ChoiceWizardResult> result) {
          resolve(std::move(result));
        });
    return {};
  } catch (...) {
    return invalid("question dialog setup failed internally");
  }
}

auto AskUserDialogController::resolve(
    std::optional<termforge::ChoiceWizardResult> result) -> void {
  if (m_resolved || m_kernel == nullptr || !m_input) return;
  m_resolved = true;
  std::expected<void, runtime::RunKernelError> resolved;
  if (!result) {
    resolved = m_kernel->cancel_questions(m_input->run_id,
                                          m_input->invocation_id,
                                          "dialog cancelled");
  } else if (result->pages.size() != m_input->questions.size()) {
    m_last_error = {AskUserDialogErrorCode::invalid_request,
                    "question dialog returned the wrong page count"};
    return;
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
          return;
        }
        selected_ids.push_back(question.options[option_index].option_id);
      }
      answers.push_back({question.question_id, std::move(selected_ids),
                         page.other});
    }
    resolved = m_kernel->answer_questions(
        m_input->run_id, m_input->invocation_id, std::move(answers));
  }
  if (!resolved) {
    m_last_error = {AskUserDialogErrorCode::runtime_failure,
                    resolved.error().message};
  }
}

}  // namespace aiforge::adapters
