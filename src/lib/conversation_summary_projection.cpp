#include <aiforge/runtime/conversation_summary_projection.hpp>

#include <aiforge/detail/utf8_text.hpp>
#include <aiforge/runtime/conversation_policy.hpp>
#include <aiforge/runtime/conversation_summary_sources.hpp>
#include <algorithm>
#include <limits>
#include <set>
#include <span>

namespace aiforge::runtime {
namespace {
using namespace domain;

auto invalid(const char* message,
             ConversationSummaryErrorCode code =
                 ConversationSummaryErrorCode::invalid_candidate)
    -> std::unexpected<ConversationSummaryError> {
  return std::unexpected(ConversationSummaryError{code, message});
}

auto prefix(const SessionEventLog& log, std::optional<std::uint64_t> snapshot)
    -> std::expected<std::span<const RunEvent>, ConversationSummaryError> {
  const auto sequence = snapshot.value_or(log.last_sequence());
  const auto end = std::ranges::upper_bound(
      log.events(), sequence, {},
      [](const auto& event) { return event.metadata.sequence; });
  const auto count = static_cast<std::size_t>(end - log.events().begin());
  if (sequence > log.last_sequence() ||
      (sequence != 0 &&
       (count == 0 || log.events()[count - 1].metadata.sequence != sequence)))
    return invalid("summary snapshot is unavailable");
  if (count > 65536)
    return invalid("summary event limit exceeded",
                   ConversationSummaryErrorCode::resource_exhausted);
  return std::span<const RunEvent>{log.events().data(), count};
}

auto control_transaction(std::span<const RunEvent> events, std::size_t index)
    -> bool {
  if (index == 0 || index + 1 >= events.size()) return false;
  const auto& before = events[index - 1];
  const auto& current = events[index];
  const auto& after = events[index + 1];
  const auto* started = std::get_if<RunStarted>(&before.payload);
  return started != nullptr && started->purpose == RunPurpose::control &&
         !started->memory_selection && !started->conversation_admission &&
         before.metadata.schema_version == 3 &&
         current.metadata.schema_version == 1 &&
         after.metadata.schema_version == 1 && !before.metadata.parent_run_id &&
         !current.metadata.parent_run_id && !after.metadata.parent_run_id &&
         before.metadata.run_id == current.metadata.run_id &&
         after.metadata.run_id == current.metadata.run_id &&
         std::holds_alternative<RunCompleted>(after.payload);
}

auto find_candidate(const ConversationSummarySnapshot& state,
                    const ConversationSummaryVersion& version)
    -> const ConversationSummaryCandidate* {
  const auto found =
      std::ranges::find_if(state.candidates, [&](const auto& candidate) {
        if (!candidate.candidate_digest) return false;
        return candidate.summary_id == version.summary_id &&
               candidate.revision == version.revision &&
               *candidate.candidate_digest == version.candidate_digest;
      });
  return found == state.candidates.end() ? nullptr : &*found;
}

auto intent_provenance(std::span<const RunEvent> events,
                       std::size_t start_index, std::size_t index) -> bool {
  bool provenance_seen{};
  bool persona_seen{};
  for (auto cursor = start_index + 1; cursor < index; ++cursor) {
    const auto& event = events[cursor];
    if (event.metadata.schema_version != 1 || event.metadata.parent_run_id)
      return false;
    if (std::holds_alternative<RunProvenanceRecorded>(event.payload) &&
        !provenance_seen && !persona_seen)
      provenance_seen = true;
    else if (std::holds_alternative<PersonaSelectionRecorded>(event.payload) &&
             !persona_seen)
      persona_seen = true;
    else
      return false;
  }
  return true;
}

auto intent_transaction(std::span<const RunEvent> events, std::size_t index,
                        const ConversationSummaryIntent& intent) -> bool {
  if (index == 0 || events[index].metadata.schema_version != 1 ||
      events[index].metadata.parent_run_id ||
      events[index].metadata.run_id != intent.producing_run_id)
    return false;
  auto start_index = index;
  while (start_index > 0 &&
         !std::holds_alternative<RunStarted>(events[start_index].payload)) {
    --start_index;
    if (events[start_index].metadata.run_id != intent.producing_run_id)
      return false;
  }
  const auto& start = events[start_index];
  const auto* attributes = std::get_if<RunStarted>(&start.payload);
  const auto snapshot =
      start_index == 0 ? 0 : events[start_index - 1].metadata.sequence;
  if (attributes == nullptr || attributes->purpose != RunPurpose::summary ||
      attributes->memory_selection || attributes->conversation_admission ||
      start.metadata.schema_version != 3 || start.metadata.parent_run_id ||
      snapshot != intent.sources.snapshot_sequence)
    return false;
  if (!intent_provenance(events, start_index, index)) return false;
  if (events.size() - index < 4) return false;
  for (auto cursor = index + 1; cursor <= index + 3; ++cursor) {
    const auto& event = events[cursor];
    if (event.metadata.run_id != intent.producing_run_id ||
        event.metadata.parent_run_id || event.metadata.schema_version != 1)
      return false;
  }
  const auto* user = std::get_if<UserContentAdded>(&events[index + 1].payload);
  const auto* inference =
      std::get_if<InferenceStarted>(&events[index + 3].payload);
  return user != nullptr && user->message.role == Role::user &&
         user->message.tool_calls.empty() &&
         std::holds_alternative<RunCompletionRequested>(
             events[index + 2].payload) &&
         inference != nullptr &&
         inference->inference_id == intent.producing_inference_id &&
         inference->model_id == intent.model_id;
}

auto recorded_intent_matches(std::span<const RunEvent> events,
                             const ConversationSummaryIntent& intent) -> bool {
  bool found{};
  for (std::size_t index = 0; index < events.size(); ++index) {
    const auto* recorded =
        std::get_if<ConversationSummaryGenerationIntentRecorded>(
            &events[index].payload);
    if (recorded == nullptr || recorded->intent.summary_id != intent.summary_id)
      continue;
    if (found || recorded->intent != intent ||
        !intent_transaction(events, index, intent))
      return false;
    found = true;
  }
  return found;
}

struct CatalogBudget {
  std::size_t manifest_bytes{};
  std::size_t source_entries{};
  std::size_t resolution_events{};
  std::uint64_t source_tokens{};

  auto reserve(const ConversationSummaryIntent& intent, std::size_t events)
      -> bool {
    constexpr std::size_t maximum_manifest = std::size_t{16} * 1024U * 1024U;
    constexpr std::size_t maximum_work = std::size_t{8} * 1024U * 1024U;
    if (events > maximum_work - resolution_events) return false;
    resolution_events += events;
    auto add = [&](std::size_t bytes) {
      if (bytes > maximum_manifest - manifest_bytes) return false;
      manifest_bytes += bytes;
      return true;
    };
    if (!add(2048 + intent.runtime_version.size())) return false;
    for (const auto& group : intent.sources.groups) {
      if (!add(256 + group.run_id.value().size() +
               group.terminal_event_id.value().size()))
        return false;
      for (const auto& entry : group.entries) {
        if (++source_entries > 16384) return false;
        if (!add(512 + entry.completed_event_id.value().size() +
                 entry.entry_id.value().size() +
                 entry.message_id.value().size() +
                 entry.provenance.source_id.value().size()) ||
            (entry.provenance.source_location &&
             !add(entry.provenance.source_location->size())) ||
            (entry.provenance.digest && !add(entry.provenance.digest->size())))
          return false;
      }
    }
    return true;
  }
};

auto add_intent(ConversationSummarySnapshot& state, const SessionEventLog& log,
                std::span<const RunEvent> events, std::size_t index,
                const ConversationSummaryIntent& intent, CatalogBudget& budget)
    -> ConversationSummaryStatus {
  if (state.intents.size() >= summary_maximum_intents)
    return invalid("summary intent limit exceeded",
                   ConversationSummaryErrorCode::resource_exhausted);
  if (!validate_conversation_summary_intent(intent) ||
      intent.sources.session_id != log.session_id() ||
      !intent_transaction(events, index, intent) ||
      std::ranges::any_of(state.intents, [&](const auto& prior) {
        return prior.summary_id == intent.summary_id ||
               prior.producing_run_id == intent.producing_run_id ||
               prior.producing_inference_id == intent.producing_inference_id;
      }))
    return invalid("summary generation intent is inconsistent",
                   ConversationSummaryErrorCode::invalid_intent);
  if (!budget.reserve(intent, events.size()))
    return invalid("summary catalog metadata or work limit exceeded",
                   ConversationSummaryErrorCode::resource_exhausted);
  auto resolved = resolve_conversation_summary_sources(log, intent.sources);
  if (!resolved)
    return invalid("summary generation sources are unavailable",
                   ConversationSummaryErrorCode::invalid_source);
  constexpr std::uint64_t maximum_source_tokens =
      std::size_t{16} * 1024U * 1024U;
  if (resolved->estimated_tokens > maximum_source_tokens - budget.source_tokens)
    return invalid("summary catalog source limit exceeded",
                   ConversationSummaryErrorCode::resource_exhausted);
  budget.source_tokens += resolved->estimated_tokens;
  state.intents.push_back(intent);
  return {};
}

auto add_candidate(ConversationSummarySnapshot& state,
                   const SessionEventLog& log, const RunEvent& event,
                   const ConversationSummaryCandidate& candidate,
                   std::size_t& text_bytes) -> ConversationSummaryStatus {
  if (state.candidates.size() >= summary_maximum_candidates ||
      candidate.text.size() > summary_maximum_retained_text_bytes - text_bytes)
    return invalid("retained summary limit exceeded",
                   ConversationSummaryErrorCode::resource_exhausted);
  const auto intent = std::ranges::find(state.intents, candidate.summary_id,
                                        &ConversationSummaryIntent::summary_id);
  if (intent == state.intents.end() ||
      candidate.created_event_id != event.metadata.event_id ||
      candidate.created_sequence != event.metadata.sequence)
    return invalid("summary publication has no matching intent or envelope");
  const ConversationSummaryCandidate* parent = nullptr;
  for (const auto& prior : state.candidates) {
    if (prior.summary_id != candidate.summary_id) continue;
    if (prior.revision >= candidate.revision)
      return invalid("summary candidate revision is reused or stale");
    if (parent == nullptr || prior.revision > parent->revision) parent = &prior;
  }
  if (!validate_conversation_summary_candidate(candidate, *intent, parent))
    return invalid("summary candidate seal or parent is invalid");
  if (candidate.author == ConversationSummaryAuthor::model) {
    auto draft = recover_conversation_summary_draft(log, *intent,
                                                    event.metadata.sequence);
    if (!draft || draft->output_event_id != candidate.output_event_id ||
        draft->output_sequence != candidate.output_sequence ||
        draft->text != candidate.text)
      return invalid("summary candidate differs from its durable output");
  }
  text_bytes += candidate.text.size();
  state.candidates.push_back(candidate);
  return {};
}

auto activate(ConversationSummarySnapshot& state,
              const ConversationSummaryActivated& action, const RunEvent& event)
    -> ConversationSummaryStatus {
  const auto* candidate = find_candidate(state, action.activation.candidate);
  if (candidate == nullptr ||
      action.activation.activation_event_id != event.metadata.event_id ||
      action.activation.activation_sequence != event.metadata.sequence)
    return invalid("summary activation has no matching candidate or envelope",
                   ConversationSummaryErrorCode::invalid_activation);
  if (std::ranges::any_of(state.candidates, [&](const auto& later) {
        return later.summary_id == candidate->summary_id &&
               later.revision > candidate->revision;
      }))
    return invalid("summary activation targets a stale candidate",
                   ConversationSummaryErrorCode::stale_revision);
  const auto intent = std::ranges::find(state.intents, candidate->summary_id,
                                        &ConversationSummaryIntent::summary_id);
  const auto* parent = candidate->edited_from
                           ? find_candidate(state, *candidate->edited_from)
                           : nullptr;
  auto expected = action.activation;
  if (intent == state.intents.end() ||
      !seal_conversation_summary_activation(expected, *candidate, *intent,
                                            parent) ||
      expected != action.activation ||
      !validate_conversation_summary_replacement(
          state.active, action.replaced_versions, action.activation))
    return invalid("summary activation coverage or replacement is invalid",
                   ConversationSummaryErrorCode::invalid_activation);
  std::erase_if(state.active, [&](const auto& prior) {
    return std::ranges::find(action.replaced_versions, prior.candidate) !=
           action.replaced_versions.end();
  });
  state.active.push_back(action.activation);
  return {};
}

auto disable(ConversationSummarySnapshot& state,
             const ConversationSummaryDisabled& action)
    -> ConversationSummaryStatus {
  const auto found =
      std::ranges::find_if(state.active, [&](const auto& active) {
        return active.candidate == action.candidate &&
               active.activation_event_id == action.activation_event_id;
      });
  if (found == state.active.end())
    return invalid("summary disable targets an unavailable activation",
                   ConversationSummaryErrorCode::invalid_activation);
  state.active.erase(found);
  return {};
}

auto apply_summary_event(ConversationSummarySnapshot& state,
                         const SessionEventLog& log,
                         std::span<const RunEvent> events, std::size_t index,
                         std::size_t& text_bytes, CatalogBudget& budget)
    -> ConversationSummaryStatus {
  const auto& event = events[index];
  ConversationSummaryStatus applied;
  if (const auto* intent =
          std::get_if<domain::ConversationSummaryGenerationIntentRecorded>(
              &event.payload)) {
    applied = add_intent(state, log, events, index, intent->intent, budget);
  } else {
    const auto* published =
        std::get_if<domain::ConversationSummaryCandidatePublished>(
            &event.payload);
    const auto* activated =
        std::get_if<domain::ConversationSummaryActivated>(&event.payload);
    const auto* disabled =
        std::get_if<domain::ConversationSummaryDisabled>(&event.payload);
    if (published == nullptr && activated == nullptr && disabled == nullptr)
      return {};
    if (!control_transaction(events, index))
      return invalid("summary control transaction is incomplete");
    if (published != nullptr)
      applied =
          add_candidate(state, log, event, published->candidate, text_bytes);
    else if (activated != nullptr)
      applied = activate(state, *activated, event);
    else
      applied = disable(state, *disabled);
  }
  return applied;
}

struct OutputState {
  bool inference_started{};
  bool assistant_started{};
  bool assistant_finished{};
  bool inference_finished{};
  bool completed{};
  std::optional<EventId> output_event_id;
  std::uint64_t output_sequence{};
  std::string text;
};

auto append_output(OutputState& state, const AssistantContentDeltaAdded& delta,
                   const ConversationSummaryIntent& intent)
    -> ConversationSummaryStatus {
  const auto* text = std::get_if<TextBlock>(&delta.delta);
  if (!state.assistant_started || state.assistant_finished ||
      delta.message_id != intent.output_message_id ||
      delta.inference_id != intent.producing_inference_id || text == nullptr ||
      text->text.size() > intent.maximum_output_bytes - state.text.size())
    return invalid("summary output is malformed or exceeds its bound");
  state.text += text->text;
  return {};
}

auto finish_output(OutputState& state, const AssistantContentFinished& finished,
                   const EventMetadata& metadata,
                   const ConversationSummaryIntent& intent)
    -> ConversationSummaryStatus {
  if (!state.assistant_started || state.assistant_finished ||
      finished.message_id != intent.output_message_id ||
      finished.inference_id != intent.producing_inference_id)
    return invalid("summary output completion is inconsistent");
  state.assistant_finished = true;
  state.output_event_id = metadata.event_id;
  state.output_sequence = metadata.sequence;
  return {};
}

auto consume_output(OutputState& state, const RunEvent& event,
                    const ConversationSummaryIntent& intent)
    -> ConversationSummaryStatus {
  if (const auto* started = std::get_if<InferenceStarted>(&event.payload)) {
    if (state.inference_started ||
        started->inference_id != intent.producing_inference_id ||
        started->model_id != intent.model_id)
      return invalid("summary producer inference is inconsistent");
    state.inference_started = true;
  } else if (const auto* started =
                 std::get_if<AssistantContentStarted>(&event.payload)) {
    if (!state.inference_started || state.assistant_started ||
        started->message_id != intent.output_message_id ||
        started->inference_id != intent.producing_inference_id)
      return invalid("summary output start is inconsistent");
    state.assistant_started = true;
  } else if (const auto* delta =
                 std::get_if<AssistantContentDeltaAdded>(&event.payload)) {
    return append_output(state, *delta, intent);
  } else if (const auto* finished =
                 std::get_if<AssistantContentFinished>(&event.payload)) {
    return finish_output(state, *finished, event.metadata, intent);
  } else if (const auto* finished =
                 std::get_if<InferenceFinished>(&event.payload)) {
    if (!state.assistant_finished || state.inference_finished ||
        finished->inference_id != intent.producing_inference_id ||
        finished->reason != FinishReason::stop)
      return invalid("summary inference did not finish successfully");
    state.inference_finished = true;
  } else if (std::holds_alternative<RunCompleted>(event.payload)) {
    if (!state.inference_finished || state.completed)
      return invalid("summary producer completed illegally");
    state.completed = true;
  } else if (std::holds_alternative<ToolProposed>(event.payload) ||
             std::holds_alternative<RunFailed>(event.payload) ||
             std::holds_alternative<RunCancelled>(event.payload) ||
             std::holds_alternative<RunCancelRequested>(event.payload) ||
             std::holds_alternative<InferenceFailed>(event.payload) ||
             std::holds_alternative<InferenceCancelled>(event.payload) ||
             std::holds_alternative<UnknownEvent>(event.payload)) {
    return invalid("summary producer is failed, cancelled or unsupported");
  }
  return {};
}
} // namespace

auto recover_conversation_summary_draft(
    const domain::SessionEventLog& log,
    const domain::ConversationSummaryIntent& intent,
    std::optional<std::uint64_t> snapshot_sequence)
    -> std::expected<ConversationSummaryDraft,
                     domain::ConversationSummaryError> {
  try {
    auto events = prefix(log, snapshot_sequence);
    if (!events) return std::unexpected(events.error());
    if (!domain::validate_conversation_summary_intent(intent) ||
        intent.sources.session_id != log.session_id())
      return invalid("summary draft intent is invalid");
    if (!recorded_intent_matches(*events, intent))
      return invalid("summary draft has no exact durable generation intent");
    OutputState state;
    for (const auto& event : *events) {
      if (event.metadata.run_id != intent.producing_run_id) continue;
      if (event.metadata.parent_run_id)
        return invalid("summary producer cannot be a child");
      const auto* started = std::get_if<domain::RunStarted>(&event.payload);
      if (started != nullptr) {
        if (event.metadata.schema_version != 3 ||
            started->purpose != domain::RunPurpose::summary)
          return invalid("summary producer purpose or schema is unsupported");
      } else if (event.metadata.schema_version != 1) {
        return invalid("summary producer event schema is unsupported");
      }
      if (auto consumed = consume_output(state, event, intent); !consumed)
        return std::unexpected(consumed.error());
    }
    if (!state.completed || !state.output_event_id ||
        !detail::is_safe_utf8_text(state.text))
      return invalid("summary output is not durably complete");
    return ConversationSummaryDraft{intent, *state.output_event_id,
                                    state.output_sequence,
                                    std::move(state.text)};
  } catch (...) {
    return invalid("summary draft recovery failed internally",
                   domain::ConversationSummaryErrorCode::internal_failure);
  }
}

auto recorded_conversation_summaries(
    const domain::SessionEventLog& log,
    std::optional<std::uint64_t> snapshot_sequence)
    -> std::expected<ConversationSummarySnapshot,
                     domain::ConversationSummaryError> {
  try {
    auto events = prefix(log, snapshot_sequence);
    if (!events) return std::unexpected(events.error());
    auto policy = recorded_conversation_policy(log, snapshot_sequence);
    if (!policy) return invalid("summary policy history is invalid");
    ConversationSummarySnapshot state;
    state.snapshot_sequence = snapshot_sequence.value_or(log.last_sequence());
    state.policy_revision = policy->policy.revision;
    std::size_t text_bytes{};
    CatalogBudget budget;
    for (std::size_t index = 0; index < events->size(); ++index) {
      const auto& event = (*events)[index];
      if (const auto* unknown =
              std::get_if<domain::UnknownEvent>(&event.payload);
          unknown != nullptr &&
          unknown->type_name.starts_with("session.conversation_summary_"))
        return invalid(
            "summary event schema is unsupported",
            domain::ConversationSummaryErrorCode::unsupported_version);
      auto applied =
          apply_summary_event(state, log, *events, index, text_bytes, budget);
      if (!applied) return std::unexpected(applied.error());
    }
    std::ranges::sort(state.active, [](const auto& left, const auto& right) {
      if (left.source_anchor_sequence != right.source_anchor_sequence)
        return left.source_anchor_sequence < right.source_anchor_sequence;
      return left.candidate.summary_id < right.candidate.summary_id;
    });
    return state;
  } catch (...) {
    return invalid("summary reconstruction failed internally",
                   domain::ConversationSummaryErrorCode::internal_failure);
  }
}
} // namespace aiforge::runtime
