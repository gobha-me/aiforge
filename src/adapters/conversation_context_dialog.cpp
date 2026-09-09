#include <aiforge/adapters/conversation_context_dialog.hpp>
#include <algorithm>
#include <concepts>
#include <limits>
#include <map>
#include <ranges>
#include <utility>

namespace aiforge::adapters {
namespace {
using Error = surfaces::ChatSessionError;
auto failure(std::string message) -> std::unexpected<Error> {
  return std::unexpected(Error{surfaces::ChatSessionErrorCode::context_failed,
                               std::move(message)});
}
auto version(const domain::ConversationSummaryCandidate& value)
    -> std::expected<domain::ConversationSummaryVersion, Error> {
  if (!value.candidate_digest)
    return failure("Summary candidate has no sealed digest");
  return domain::ConversationSummaryVersion{value.summary_id, value.revision,
                                            *value.candidate_digest};
}

auto mode(domain::ConversationMode value) -> std::string_view {
  return value == domain::ConversationMode::rolling ? "rolling" : "full";
}
auto decision(
    const std::optional<runtime::ConversationSelectionDecision>& value)
    -> std::string_view {
  if (!value) return "not admitted: preparation unavailable";
  switch (*value) {
    case runtime::ConversationSelectionDecision::admitted_full:
      return "included (full)";
    case runtime::ConversationSelectionDecision::admitted_pin:
      return "included (pinned)";
    case runtime::ConversationSelectionDecision::admitted_recent:
      return "included (recent)";
    case runtime::ConversationSelectionDecision::omitted_summary:
      return "covered by summary";
    case runtime::ConversationSelectionDecision::omitted_capacity:
      return "omitted: capacity";
    case runtime::ConversationSelectionDecision::omitted_older:
      return "omitted: older";
  }
  return "unavailable";
}
auto latest(const surfaces::ChatSummaryCatalog& catalog)
    -> std::vector<domain::ConversationSummaryId> {
  std::vector<domain::ConversationSummaryId> result;
  result.reserve(catalog.snapshot.intents.size());
  for (const auto& intent : catalog.snapshot.intents)
    result.push_back(intent.summary_id);
  return result;
}
auto local_evidence_text(
    const std::optional<domain::LocalContextAdmission>& admission)
    -> std::string {
  if (!admission) return {};
  std::string text = "\n\nLocal file evidence (exact selection):";
  for (const auto& evidence : admission->evidence) {
    std::string_view decision = "included";
    if (evidence.decision == domain::LocalContextDecision::omitted_budget)
      decision = "omitted: total capacity";
    if (evidence.decision == domain::LocalContextDecision::omitted_class_budget)
      decision = "omitted: evidence allowance";
    text += "\n" + evidence.source.root.binding.substr(0, 8) + ":" +
            evidence.source.relative_path + " | " +
            std::to_string(evidence.estimated_tokens) + " tokens | " +
            std::string{decision};
  }
  return text;
}
auto context_text(const surfaces::ChatConversationContextInspection& view)
    -> std::string {
  std::string text =
      "Next turn: " + std::string{mode(view.policy.policy.mode)} +
      " | policy revision " + std::to_string(view.policy.policy.revision) +
      "\nModel: " + std::string{view.model_id.value()} + "\nCapacity: " +
      std::to_string(view.mandatory.capacity.context_window_tokens) +
      " | output reserved: " +
      std::to_string(view.mandatory.capacity.reserved_output_tokens) +
      " | tools reserved: " +
      std::to_string(view.mandatory.capacity.reserved_input_tokens);
  if (view.active_admission)
    text += "\nActive run remains " +
            std::string{mode(view.active_admission->mode)} +
            " at policy revision " +
            std::to_string(view.active_admission->policy_revision);
  text += "\nRequired instructions:";
  for (const auto& input : view.mandatory.instructions)
    text += "\n  " + std::string{input.entry_id.value()} + ": " +
            std::to_string(input.estimated_tokens);
  for (const auto& input : view.mandatory.content)
    text += "\nCurrent input: " + std::to_string(input.estimated_tokens) +
            " estimated tokens (unsubmitted)";
  if (view.next_context)
    text += "\nNext input estimate: " +
            std::to_string(view.next_context->estimated_input_tokens);
  if (view.preparation_error)
    text += "\nCannot prepare: " + view.preparation_error->message;
  text += "\n\nWhole original runs (oldest first):";
  for (const auto& group : view.groups)
    text += "\n" + std::string{group.run_id.value()} + " | " +
            std::to_string(group.estimated_tokens) + " tokens | " +
            std::to_string(group.entry_count) + " messages | " +
            std::string{decision(group.decision)} +
            (group.pinned ? " | pin" : "");
  text += "\n\nReviewed summaries: " + std::to_string(view.summaries.size());
  for (const auto& summary : view.summaries)
    text += "\n" + std::string{summary.candidate.summary_id.value()} +
            " revision " + std::to_string(summary.candidate.revision) + " | " +
            std::to_string(summary.estimated_tokens) + " tokens";
  if (view.policy.policy.mode == domain::ConversationMode::full)
    text += "\nSummary activations are dormant in full mode.";
  text += local_evidence_text(view.local_admission);
  text += "\n\nCtrl+G opens Context even with toolbar hidden. Escape closes; "
          "the chat draft stays intact.";
  return text;
}
} // namespace

ConversationContextDialog::ConversationContextDialog(
    surfaces::ChatSession& session, std::function<std::string()> draft,
    bool async_preparation)
    : Dialog("Context - Conversation"), m_async_preparation(async_preparation),
      m_session(session), m_draft(std::move(draft)),
      m_review_model(session.model_id()) {
  set_max_width(110);
  set_text("Context controls affect the next turn. Summary generation is an "
           "explicit paid request.");
  m_editor.set_max_height(16);
  m_editor.set_enter_mode(termforge::ComposerEnterMode::Newline);
  m_menu.set_menus(
      {{"Conversation",
        {{"Inspect", [this] { perform(surfaces::InspectConversation{}); }},
         {"Full history",
          [this] {
            perform(
                surfaces::SetConversationMode{domain::ConversationMode::full});
          }},
         {"Rolling history",
          [this] {
            perform(surfaces::SetConversationMode{
                domain::ConversationMode::rolling});
          }},
         {"Pin / unpin run", [this] { choose_sources(true); }}}},
       {"Summary",
        {{"Generate from selected runs", [this] { choose_sources(false); }},
         {"Review / publish draft", [this] { choose_summary(0); }},
         {"Edit reviewed text", [this] { choose_summary(1); }},
         {"Preview application", [this] { choose_summary(2); }},
         {"Apply reviewed preview",
          [this] { perform(surfaces::ApplyConversationSummary{}); }},
         {"Disable active summary", [this] { choose_summary(3); }}}},
       {"View",
        {{"Local files",
          [this] {
            if (m_files) m_files();
          }},
         {"Repository evidence",
          [this] {
            if (m_repository) m_repository();
          }},
         {"Hide toolbar",
          [this] { perform(surfaces::SetContextToolbar{false}); }},
         {"Show toolbar",
          [this] { perform(surfaces::SetContextToolbar{true}); }},
         {"Discard review",
          [this] { perform(surfaces::DiscardConversationReview{}); }}}}});
  m_primary.on_activate([this] {
    if (m_editing)
      save_edit();
    else if (m_review)
      perform(surfaces::ApplyConversationSummary{});
    else
      perform(surfaces::InspectConversation{});
  });
  m_secondary.on_activate([this] {
    if (m_editing) {
      m_editing = false;
      standard_children();
    } else
      perform(surfaces::InspectConversationSummaries{});
  });
  m_close.on_activate([this] { on_escape(); });
  standard_children();
}
auto ConversationContextDialog::pending_evidence_work() const
    -> std::optional<surfaces::ChatEvidenceWorkToken> {
  if (m_pending_evidence) return m_pending_evidence->token;
  return std::nullopt;
}
auto ConversationContextDialog::begin_evidence_work(std::string draft)
    -> std::expected<void, Error> {
  auto token = m_session.pending_evidence_work();
  if (!token)
    return failure("Context preparation did not provide an operation identity");
  m_pending_evidence = PendingEvidence{*token, std::move(draft), {}, {}};
  m_status =
      "Preparing exact context sources; prior view and chat draft retained";
  m_primary.set_label("[ Preparing ]");
  return {};
}
auto ConversationContextDialog::cancel_pending_evidence() -> void {
  if (!m_pending_evidence) return;
  if (m_session.pending_evidence_work() == m_pending_evidence->token)
    m_session.cancel_evidence_work();
  m_pending_evidence.reset();
  if (!m_editing) m_primary.set_label(m_review ? "[ Apply ]" : "[ Refresh ]");
}
auto ConversationContextDialog::fail_evidence_work(
    const surfaces::ChatEvidenceWorkToken& token, const Error& error) -> bool {
  if (!m_pending_evidence || m_pending_evidence->token != token) return false;
  m_pending_evidence.reset();
  m_status = "Context preparation failed: " + error.message;
  set_text(m_status);
  if (!m_editing) m_primary.set_label(m_review ? "[ Apply ]" : "[ Refresh ]");
  return true;
}
auto ConversationContextDialog::complete_evidence_work(
    surfaces::ChatEvidenceOutcome outcome) -> std::expected<bool, Error> {
  if (!m_pending_evidence || m_pending_evidence->token != outcome.token)
    return false;
  const auto rejected = [this](std::string message) -> std::unexpected<Error> {
    m_pending_evidence.reset();
    m_status = std::move(message);
    set_text(m_status);
    if (!m_editing) m_primary.set_label(m_review ? "[ Apply ]" : "[ Refresh ]");
    return failure(m_status);
  };
  try {
    if (m_pending_evidence->draft != m_draft() ||
        outcome.token.model_id != m_session.model_id()) {
      const auto error = failure("Context changed; prepare again").error();
      static_cast<void>(fail_evidence_work(outcome.token, error));
      return std::unexpected(error);
    }
    auto pending = std::move(*m_pending_evidence);
    m_pending_evidence.reset();
    const auto finished =
        finish_evidence_work(std::move(outcome), std::move(pending));
    if (!finished) return rejected(finished.error().message);
    return true;
  } catch (...) {
    return rejected("Context completion failed internally");
  }
}
auto ConversationContextDialog::finish_evidence_work(
    surfaces::ChatEvidenceOutcome outcome, PendingEvidence pending)
    -> std::expected<void, Error> {
  if (outcome.token.purpose == surfaces::ChatEvidenceWorkPurpose::inspection) {
    const auto* inspection =
        std::get_if<surfaces::ChatConversationContextInspection>(
            &outcome.result);
    if (inspection == nullptr)
      return failure("Context inspection result type does not match");
    if (pending.choose_sources)
      choose_source_groups(inspection->groups, *pending.choose_sources);
    else
      show_inspection(*inspection);
  } else if (outcome.token.purpose ==
             surfaces::ChatEvidenceWorkPurpose::summary_preview) {
    auto* preview = std::get_if<surfaces::ChatSummaryPreview>(&outcome.result);
    if (preview == nullptr || !pending.candidate)
      return failure("Summary preview result type does not match");
    show_preview(std::move(*preview), *pending.candidate,
                 std::move(pending.draft));
  } else if (outcome.token.purpose ==
                 surfaces::ChatEvidenceWorkPurpose::summary_apply &&
             std::holds_alternative<domain::ConversationSummaryActivation>(
                 outcome.result)) {
    invalidate_review();
    if (auto inspected = inspect(); !inspected) {
      m_status = "Summary applied; context inspection unavailable";
      set_text(m_status);
    }
  } else
    return failure("Context preparation result type does not match");
  return {};
}
auto ConversationContextDialog::on_toolbar(std::function<void(bool)> callback)
    -> void {
  m_toolbar = std::move(callback);
}
auto ConversationContextDialog::on_files(std::function<void()> callback)
    -> void {
  m_files = std::move(callback);
}
auto ConversationContextDialog::on_repository(std::function<void()> callback)
    -> void {
  m_repository = std::move(callback);
}
auto ConversationContextDialog::on_committed(
    std::function<void(std::vector<domain::RunEvent>)> callback) -> void {
  m_committed = std::move(callback);
}
auto ConversationContextDialog::status() const noexcept -> const std::string& {
  return m_status;
}
auto ConversationContextDialog::review_available() const noexcept -> bool {
  return m_review.has_value() && !m_pending_evidence;
}
auto ConversationContextDialog::editing_text() const noexcept
    -> const std::string& {
  return m_editor.text();
}
auto ConversationContextDialog::body(std::string text) -> void {
  m_body = std::move(text);
  m_text.clear();
  m_text.append(m_body);
}
auto ConversationContextDialog::invalidate_review() -> void {
  cancel_pending_evidence();
  m_review.reset();
  m_review_draft.clear();
  if (!m_editing) m_primary.set_label("[ Refresh ]");
}
auto ConversationContextDialog::standard_children() -> void {
  clear_children();
  add_child(&m_menu);
  add_child(&m_text);
  add_child(&m_primary);
  add_child(&m_secondary);
  add_child(&m_close);
  m_primary.set_label(m_review ? "[ Apply ]" : "[ Refresh ]");
  m_secondary.set_label("[ Summaries ]");
}
auto ConversationContextDialog::inspect() -> std::expected<void, Error> {
  if (m_async_preparation) {
    auto draft = m_draft();
    const auto requested = m_session.request_context_inspection(draft);
    if (!requested) return std::unexpected(requested.error());
    return begin_evidence_work(std::move(draft));
  }
  auto inspected = m_session.inspect_conversation_context(m_draft());
  if (!inspected) return std::unexpected(inspected.error());
  show_inspection(*inspected);
  return {};
}
auto ConversationContextDialog::show_inspection(
    const surfaces::ChatConversationContextInspection& inspected) -> void {
  m_editing = false;
  standard_children();
  body(context_text(inspected));
  m_status = inspected.preparation_error
                 ? inspected.preparation_error->message
                 : "Next-turn context inspected; no inference dispatched";
}
auto ConversationContextDialog::summaries() -> std::expected<void, Error> {
  auto catalog = m_session.summary_catalog();
  if (!catalog) return std::unexpected(catalog.error());
  std::string text =
      "Summary generation uses the current model and normal spending controls. "
      "Review/edit, then Preview and Apply are separate actions.\n";
  for (const auto& intent : catalog->snapshot.intents)
    text += "\n" + std::string{intent.summary_id.value()} + " | model " +
            std::string{intent.model_id.value()} + " | source groups " +
            std::to_string(intent.sources.groups.size()) +
            " | reserved output " +
            std::to_string(intent.capacity.reserved_output_tokens);
  for (const auto& candidate : catalog->snapshot.candidates)
    text += "\nReviewed " + std::string{candidate.summary_id.value()} +
            " revision " + std::to_string(candidate.revision);
  for (const auto& draft : catalog->unpublished)
    text +=
        "\nReady to review: " + std::string{draft.intent.summary_id.value()};
  for (const auto& rejected : catalog->unpublishable)
    text += "\nUnavailable " + std::string{rejected.summary_id.value()} + ": " +
            rejected.message;
  for (const auto& active : catalog->snapshot.active)
    text += "\nActive: " + std::string{active.candidate.summary_id.value()} +
            " revision " + std::to_string(active.candidate.revision);
  m_editing = false;
  standard_children();
  body(std::move(text));
  m_status = "Summary catalog; nothing applied";
  return {};
}
auto ConversationContextDialog::candidate(
    const domain::ConversationSummaryId& id, bool publish)
    -> std::expected<domain::ConversationSummaryCandidate, Error> {
  auto catalog = m_session.summary_catalog();
  if (!catalog) return std::unexpected(catalog.error());
  for (const auto& value : std::views::reverse(catalog->snapshot.candidates))
    if (value.summary_id == id) return value;
  if (publish) return m_session.publish_conversation_summary(id);
  return failure(
      "Review the completed summary draft before previewing or editing it");
}
auto ConversationContextDialog::set_mode(
    const surfaces::SetConversationMode& command)
    -> std::expected<void, Error> {
  invalidate_review();
  auto policy = m_session.conversation_policy();
  if (!policy) return std::unexpected(policy.error());
  auto changed = m_session.set_conversation_policy(
      policy->policy.revision, command.mode, policy->policy.pinned_run_ids);
  if (!changed) return std::unexpected(changed.error());
  invalidate_review();
  return inspect();
}
auto ConversationContextDialog::set_pin(
    const surfaces::PinConversationRun& command) -> std::expected<void, Error> {
  invalidate_review();
  auto policy = m_session.conversation_policy();
  if (!policy) return std::unexpected(policy.error());
  auto& pins = policy->policy.pinned_run_ids;
  std::erase(pins, command.run_id);
  if (command.pinned) pins.push_back(command.run_id);
  auto changed = m_session.set_conversation_policy(
      policy->policy.revision, policy->policy.mode, std::move(pins));
  if (!changed) return std::unexpected(changed.error());
  invalidate_review();
  return inspect();
}
auto ConversationContextDialog::generate(
    const surfaces::GenerateConversationSummary& command)
    -> std::expected<void, Error> {
  invalidate_review();
  auto generated = m_session.generate_conversation_summary(
      {m_session.event_log().last_sequence(), command.run_ids});
  if (!generated) return std::unexpected(generated.error());
  body("Generating summary " + std::string{generated->summary_id.value()} +
       "\nOne tool-free request using " +
       std::string{m_session.model_id().value()} +
       ". Usage and cost appear in the session header.\nWhen complete, choose "
       "Summary > Review. It will not apply automatically.");
  m_status = "Summary generation started; paid request, review required";
  return {};
}
auto ConversationContextDialog::review(const domain::ConversationSummaryId& id,
                                       bool edit)
    -> std::expected<void, Error> {
  invalidate_review();
  auto value = candidate(id, !edit);
  if (!value) return std::unexpected(value.error());
  invalidate_review();
  m_editing = edit;
  if (edit) {
    m_edit_parent = *value;
    m_edit_sequence = m_session.event_log().last_sequence();
    m_editor.set_text(value->text);
    clear_children();
    add_child(&m_editor);
    add_child(&m_primary);
    add_child(&m_secondary);
    add_child(&m_close);
    m_primary.set_label("[ Save edit ]");
    m_secondary.set_label("[ Back ]");
  } else {
    standard_children();
    body("Reviewed summary " + std::string{id.value()} + " revision " +
         std::to_string(value->revision) +
         "\nDerived untrusted evidence. Review accuracy before Preview and "
         "Apply.\n\n" +
         value->text);
  }
  m_status = edit ? "Editing summary only; the chat draft is unchanged"
                  : "Summary published for review; not applied";
  return {};
}
auto ConversationContextDialog::preview(
    const surfaces::PreviewConversationSummary& command)
    -> std::expected<void, Error> {
  invalidate_review();
  auto value = candidate(command.summary_id, false);
  if (!value) return std::unexpected(value.error());
  auto catalog = m_session.summary_catalog();
  if (!catalog) return std::unexpected(catalog.error());
  std::vector<domain::ConversationSummaryVersion> replacements;
  for (const auto& id : command.replacements) {
    const auto found =
        std::ranges::find_if(catalog->snapshot.active, [&](const auto& active) {
          return active.candidate.summary_id == id;
        });
    if (found == catalog->snapshot.active.end())
      return failure("Replacement summary is no longer active");
    replacements.push_back(found->candidate);
  }
  auto parent = version(*value);
  if (!parent) return std::unexpected(parent.error());
  auto draft = m_draft();
  if (m_async_preparation) {
    const auto requested = m_session.request_summary_preview(
        *parent, std::move(replacements), draft);
    if (!requested) return std::unexpected(requested.error());
    if (auto started = begin_evidence_work(std::move(draft)); !started)
      return started;
    if (!m_pending_evidence)
      return failure("Summary preview preparation identity disappeared");
    m_pending_evidence->candidate = std::move(*value);
    return {};
  }
  auto prepared = m_session.preview_conversation_summary(
      *parent, std::move(replacements), draft);
  if (!prepared) return std::unexpected(prepared.error());
  show_preview(std::move(*prepared), *value, std::move(draft));
  return {};
}
auto ConversationContextDialog::show_preview(
    surfaces::ChatSummaryPreview prepared,
    const domain::ConversationSummaryCandidate& candidate, std::string draft)
    -> void {
  const auto tokens = prepared.context().estimated_input_tokens;
  const auto covered = prepared.activation().covered_run_ids.size();
  const auto local_text = local_evidence_text(prepared.local_admission());
  m_review = std::move(prepared);
  m_review_draft = std::move(draft);
  m_review_model = m_session.model_id();
  m_review_sequence = m_session.event_log().last_sequence();
  m_editing = false;
  standard_children();
  body("Apply reviewed summary " + std::string{candidate.summary_id.value()} +
       "\nNext request estimate: " + std::to_string(tokens) +
       "\nCovered original runs: " + std::to_string(covered) +
       "\nModel: " + std::string{m_review_model.value()} +
       "\nChat draft preserved and bound to this preview. Apply rechecks "
       "current sources and budget.\n\n" +
       candidate.text + local_text);
  m_status = "Preview ready; Apply is explicit";
}

auto ConversationContextDialog::apply() -> std::expected<void, Error> {
  if (!m_review) return failure("Preview a reviewed summary before Apply");
  if (m_async_preparation) {
    auto draft = m_draft();
    const auto requested = m_session.request_summary_apply(*m_review, draft);
    if (!requested) return std::unexpected(requested.error());
    return begin_evidence_work(std::move(draft));
  }
  auto result = m_session.apply_conversation_summary(*m_review, m_draft());
  if (!result) return std::unexpected(result.error());
  invalidate_review();
  return inspect();
}
auto ConversationContextDialog::disable(const domain::ConversationSummaryId& id)
    -> std::expected<void, Error> {
  invalidate_review();
  auto catalog = m_session.summary_catalog();
  if (!catalog) return std::unexpected(catalog.error());
  const auto found =
      std::ranges::find_if(catalog->snapshot.active, [&](const auto& item) {
        return item.candidate.summary_id == id;
      });
  if (found == catalog->snapshot.active.end())
    return failure("Summary is not active");
  auto result = m_session.disable_conversation_summary(
      catalog->snapshot.policy_revision, found->candidate,
      found->activation_event_id);
  if (!result) return std::unexpected(result.error());
  invalidate_review();
  return inspect();
}
auto ConversationContextDialog::dispatch(
    const surfaces::ConversationCommand& command)
    -> std::expected<void, Error> {
  return std::visit(
      [this](const auto& value) -> std::expected<void, Error> {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::same_as<T, surfaces::InspectConversation>)
          return inspect();
        else if constexpr (std::same_as<T, surfaces::SetConversationMode>)
          return set_mode(value);
        else if constexpr (std::same_as<T, surfaces::PinConversationRun>)
          return set_pin(value);
        else if constexpr (std::same_as<T,
                                        surfaces::InspectConversationSummaries>)
          return summaries();
        else if constexpr (std::same_as<T,
                                        surfaces::GenerateConversationSummary>)
          return generate(value);
        else if constexpr (std::same_as<T, surfaces::ReviewConversationSummary>)
          return review(value.summary_id, false);
        else if constexpr (std::same_as<T, surfaces::EditConversationSummary>)
          return review(value.summary_id, true);
        else if constexpr (std::same_as<T,
                                        surfaces::PreviewConversationSummary>)
          return preview(value);
        else if constexpr (std::same_as<T, surfaces::ApplyConversationSummary>)
          return apply();
        else if constexpr (std::same_as<T,
                                        surfaces::DisableConversationSummary>)
          return disable(value.summary_id);
        else if constexpr (std::same_as<T, surfaces::SetContextToolbar>) {
          if (m_toolbar) m_toolbar(value.visible);
          m_status =
              value.visible
                  ? "Toolbar shown"
                  : "Toolbar hidden; Ctrl+G and /context remain available";
          return {};
        } else {
          invalidate_review();
          m_status =
              "Summary review discarded; draft and durable history retained";
          return {};
        }
      },
      command);
}
auto ConversationContextDialog::execute(
    const surfaces::ConversationCommand& command)
    -> std::expected<void, Error> {
  const auto before = m_session.event_log().last_sequence();
  try {
    if (!std::holds_alternative<surfaces::SetContextToolbar>(command))
      cancel_pending_evidence();
    auto result = dispatch(command);
    if (m_committed && m_session.event_log().last_sequence() != before) {
      std::vector<domain::RunEvent> events;
      for (const auto& event : m_session.event_log().events())
        if (event.metadata.sequence > before) events.push_back(event);
      m_committed(std::move(events));
    }
    if (!result) {
      m_status = result.error().message;
      set_text("Context action failed: " + m_status);
    } else
      set_text("Next-turn context. Summary generation is a paid request; "
               "review and Apply are separate.");
    return result;
  } catch (...) {
    m_status = "Context action failed internally";
    return std::unexpected(
        Error{surfaces::ChatSessionErrorCode::internal_failure, m_status, false,
              m_session.event_log().last_sequence() != before});
  }
}
auto ConversationContextDialog::perform(surfaces::ConversationCommand command)
    -> void {
  const auto result = execute(command);
  if (!result) m_status = result.error().message;
}
auto ConversationContextDialog::choose(
    termforge::ChoiceWizardPage page,
    std::function<void(std::vector<std::size_t>)> callback) -> void {
  if (!m_picker.set_pages({std::move(page)})) {
    m_status = "Context choices are unavailable";
    return;
  }
  m_picker.on_result([this, callback = std::move(callback)](
                         std::optional<termforge::ChoiceWizardResult> result) {
    m_picking = false;
    if (!result || result->pages.size() != 1) {
      m_status = "Selection cancelled; chat draft retained";
      return;
    }
    callback(std::move(result->pages.front().selected_indices));
  });
  m_picking = true;
}
auto ConversationContextDialog::choose_sources(bool pin) -> void {
  if (m_async_preparation) {
    cancel_pending_evidence();
    const auto result = inspect();
    if (!result) {
      m_status = result.error().message;
      return;
    }
    if (m_pending_evidence) m_pending_evidence->choose_sources = pin;
    return;
  }
  auto inspected = m_session.inspect_conversation_context(m_draft());
  if (!inspected) {
    m_status = inspected.error().message;
    return;
  }
  choose_source_groups(std::move(inspected->groups), pin);
}
auto ConversationContextDialog::choose_source_groups(
    std::vector<surfaces::ChatConversationGroupInspection> groups, bool pin)
    -> void {
  termforge::ChoiceWizardPage page;
  page.title = pin ? "Pin or unpin one whole run"
                   : "Generate one summary from selected whole runs";
  page.text =
      pin ? "Pins are mandatory in rolling mode; policy applies next turn."
          : "Explicit paid request using the current model. Select up to 128 "
            "original runs. The result requires review and Apply.";
  page.mode =
      pin ? termforge::ChoiceMode::Single : termforge::ChoiceMode::Multiple;
  page.minimum_selected = 1;
  page.maximum_selected = pin ? 1 : domain::summary_maximum_groups;
  for (const auto& group : groups)
    page.choices.push_back({std::string{group.run_id.value()},
                            std::to_string(group.estimated_tokens) + " tokens" +
                                (group.pinned ? "; pinned" : "")});
  choose(std::move(page), [this, groups = std::move(groups),
                           pin](std::vector<std::size_t> indices) {
    std::vector<domain::RunId> ids;
    for (const auto index : indices) {
      if (index >= groups.size()) return;
      ids.push_back(groups[index].run_id);
    }
    if (ids.empty()) return;
    if (pin)
      perform(surfaces::PinConversationRun{ids.front(),
                                           !groups[indices.front()].pinned});
    else
      perform(surfaces::GenerateConversationSummary{std::move(ids)});
  });
}
auto ConversationContextDialog::choose_summary(unsigned action) -> void {
  auto catalog = m_session.summary_catalog();
  if (!catalog) {
    m_status = catalog.error().message;
    return;
  }
  auto ids = latest(*catalog);
  termforge::ChoiceWizardPage page;
  page.title = "Choose exact summary";
  page.minimum_selected = 1;
  page.maximum_selected = 1;
  for (const auto& id : ids)
    page.choices.push_back(
        {std::string{id.value()},
         "Inspect the latest durable version of this summary."});
  choose(std::move(page), [this, ids = std::move(ids),
                           action](std::vector<std::size_t> selected) {
    if (selected.size() != 1 || selected.front() >= ids.size()) return;
    auto id = ids[selected.front()];
    if (action == 0)
      perform(surfaces::ReviewConversationSummary{std::move(id)});
    else if (action == 1)
      perform(surfaces::EditConversationSummary{std::move(id)});
    else if (action == 2)
      choose_replacements(std::move(id));
    else
      perform(surfaces::DisableConversationSummary{std::move(id)});
  });
}
auto ConversationContextDialog::choose_replacements(
    domain::ConversationSummaryId id) -> void {
  auto catalog = m_session.summary_catalog();
  if (!catalog) {
    m_status = catalog.error().message;
    return;
  }
  if (catalog->snapshot.active.empty()) {
    perform(surfaces::PreviewConversationSummary{std::move(id), {}});
    return;
  }
  auto active = std::move(catalog->snapshot.active);
  termforge::ChoiceWizardPage page;
  page.title = "Explicitly replace active summaries";
  page.text = "Choose every overlapping active summary to replace. Leave empty "
              "to retain all.";
  page.mode = termforge::ChoiceMode::Multiple;
  page.maximum_selected = domain::summary_maximum_active;
  for (const auto& value : active)
    page.choices.push_back(
        {std::string{value.candidate.summary_id.value()},
         "revision " + std::to_string(value.candidate.revision)});
  choose(std::move(page),
         [this, id = std::move(id), active = std::move(active)](
             std::vector<std::size_t> selected) mutable {
           std::vector<domain::ConversationSummaryId> replacements;
           for (const auto index : selected) {
             if (index >= active.size()) return;
             replacements.push_back(active[index].candidate.summary_id);
           }
           perform(surfaces::PreviewConversationSummary{
               std::move(id), std::move(replacements)});
         });
}
auto ConversationContextDialog::save_edit() -> void {
  if (!m_edit_parent) return;
  const auto before = m_session.event_log().last_sequence();
  auto parent = version(*m_edit_parent);
  if (!parent) {
    m_status = parent.error().message;
    return;
  }
  auto result = m_session.edit_conversation_summary(m_edit_sequence, *parent,
                                                    m_editor.text());
  if (!result) {
    m_status = result.error().message;
    set_text("Edit not saved: " + m_status);
    return;
  }
  if (m_committed) {
    std::vector<domain::RunEvent> events;
    for (const auto& event : m_session.event_log().events())
      if (event.metadata.sequence > before) events.push_back(event);
    m_committed(std::move(events));
  }
  m_editing = false;
  standard_children();
  body("Edited summary saved; review before Preview and Apply.\n\n" +
       result->text);
  m_status = "Summary edit saved; not applied";
}
auto ConversationContextDialog::content_rows() const -> int {
  return 22;
}
auto ConversationContextDialog::content_cols() const -> int {
  return 100;
}
auto ConversationContextDialog::layout_content(termforge::Rect area) -> void {
  const auto menu_rows = area.h > 1 ? 1 : 0;
  const auto buttons = area.h > 0 ? 1 : 0;
  const auto height = std::max(0, area.h - menu_rows - buttons);
  m_menu.set_geometry({area.x, area.y, area.w, menu_rows});
  m_text.set_geometry({area.x, area.y + menu_rows, area.w, height});
  m_editor.set_geometry({area.x, area.y + menu_rows, area.w, height});
  const auto width = area.w / 3;
  const auto y = area.y + std::max(0, area.h - 1);
  m_primary.set_geometry({area.x, y, width, buttons});
  m_secondary.set_geometry({area.x + width, y, width, buttons});
  m_close.set_geometry(
      {area.x + (2 * width), y, area.w - (2 * width), buttons});
}
auto ConversationContextDialog::draw_content(termforge::Screen& screen)
    -> void {
  if (m_editing)
    m_editor.draw(screen);
  else
    m_text.draw(screen);
  m_primary.draw(screen);
  m_secondary.draw(screen);
  m_close.draw(screen);
  if (!m_editing) m_menu.draw(screen);
}
auto ConversationContextDialog::display_text() const noexcept
    -> const std::string& {
  return m_body;
}
auto ConversationContextDialog::hit_test_tree(int x, int y) const -> bool {
  return m_picking ? m_picker.hit_test_tree(x, y) : Dialog::hit_test_tree(x, y);
}
auto ConversationContextDialog::draw(termforge::Screen& screen) -> void {
  if (m_pending_evidence &&
      (m_pending_evidence->draft != m_draft() ||
       m_pending_evidence->token.model_id != m_session.model_id())) {
    cancel_pending_evidence();
    m_status = "Context changed; prepare again";
    set_text(m_status);
  }
  if (m_review &&
      (m_review_draft != m_draft() || m_review_model != m_session.model_id() ||
       m_review_sequence != m_session.event_log().last_sequence())) {
    invalidate_review();
    m_status = "Context changed; preview again before Apply";
    set_text(m_status);
  }
  if (m_picking)
    m_picker.draw(screen);
  else
    Dialog::draw(screen);
}
auto ConversationContextDialog::on_event(const termforge::Event& event)
    -> bool {
  if (m_picking) return m_picker.on_event(event);
  if (m_editing) {
    if (const auto* paste = std::get_if<termforge::PasteEvent>(&event);
        paste != nullptr &&
        paste->text.size() > domain::summary_maximum_text_bytes -
                                 std::min(m_editor.text().size(),
                                          domain::summary_maximum_text_bytes)) {
      m_status = "Summary edit exceeds 64 KiB";
      set_text(m_status);
      return true;
    }
    if (const auto* key = std::get_if<termforge::KeyEvent>(&event);
        key != nullptr && key->key == termforge::Key::Char && !key->ctrl &&
        m_editor.text().size() > domain::summary_maximum_text_bytes - 4) {
      m_status = "Summary edit exceeds 64 KiB";
      return true;
    }
  }
  return Dialog::on_event(event);
}
auto ConversationContextDialog::on_escape() -> void {
  cancel_pending_evidence();
  invalidate_review();
  m_picking = false;
  m_editing = false;
  m_status = "Context closed; chat draft retained";
  if (begin_result()) close();
}
} // namespace aiforge::adapters
