#include <aiforge/domain/transcript_projection.hpp>
#include <aiforge/repository/verification_evidence.hpp>

#include <algorithm>
#include <limits>
#include <ranges>
#include <type_traits>
#include <utility>

namespace aiforge::domain {
namespace {

template <typename... Callables> struct Overloaded : Callables... {
  using Callables::operator()...;
};

template <typename... Callables>
Overloaded(Callables...) -> Overloaded<Callables...>;

[[nodiscard]] auto projection_error(const ProjectionError& error)
    -> TranscriptProjectionError {
  switch (error.code) {
    case ProjectionErrorCode::invalid_envelope:
      return {TranscriptProjectionErrorCode::invalid_envelope, error.message};
    case ProjectionErrorCode::wrong_run:
      return {TranscriptProjectionErrorCode::wrong_run, error.message};
    case ProjectionErrorCode::non_monotonic_sequence:
      return {TranscriptProjectionErrorCode::non_monotonic_sequence,
              error.message};
    case ProjectionErrorCode::unknown_message:
      return {TranscriptProjectionErrorCode::unknown_message, error.message};
    case ProjectionErrorCode::wrong_inference:
      return {TranscriptProjectionErrorCode::unknown_inference, error.message};
    case ProjectionErrorCode::usage_overflow:
      return {TranscriptProjectionErrorCode::usage_overflow, error.message};
    case ProjectionErrorCode::cost_overflow:
      return {TranscriptProjectionErrorCode::cost_overflow, error.message};
    case ProjectionErrorCode::invalid_pricing:
      return {TranscriptProjectionErrorCode::invalid_transition, error.message};
    case ProjectionErrorCode::invalid_transition:
      return {TranscriptProjectionErrorCode::invalid_transition, error.message};
  }
  return {TranscriptProjectionErrorCode::internal_failure,
          "run projection failed internally"};
}

[[nodiscard]] auto error(const TranscriptProjectionErrorCode code,
                         std::string message)
    -> std::unexpected<TranscriptProjectionError> {
  return std::unexpected(TranscriptProjectionError{code, std::move(message)});
}

[[nodiscard]] auto add_checked(std::uint64_t& value,
                               const std::uint64_t addition) -> bool {
  if (addition > std::numeric_limits<std::uint64_t>::max() - value) {
    return false;
  }
  value += addition;
  return true;
}

[[nodiscard]] auto add_usage(Usage& value, const Usage& addition) -> bool {
  auto next = value;
  if (!add_checked(next.input_tokens, addition.input_tokens) ||
      !add_checked(next.output_tokens, addition.output_tokens) ||
      !add_checked(next.cached_input_tokens, addition.cached_input_tokens) ||
      !add_checked(next.reasoning_tokens, addition.reasoning_tokens)) {
    return false;
  }
  value = next;
  return true;
}

[[nodiscard]] auto answer_is_valid(const QuestionDefinition& question,
                                   const QuestionAnswer& answer) -> bool {
  std::set<std::string> selected;
  for (const auto& id : answer.selected_option_ids) {
    if (!selected.insert(id).second) return false;
    if (std::ranges::none_of(question.options, [&](const auto& option) {
          return option.option_id == id;
        })) {
      return false;
    }
  }
  const bool has_free_form = answer.free_form && !answer.free_form->empty();
  if (has_free_form && (!question.other || answer.free_form->size() >
                                               question.other->maximum_bytes)) {
    return false;
  }
  const auto answer_count =
      answer.selected_option_ids.size() + (has_free_form ? 1U : 0U);
  if (question.required && answer_count == 0) return false;
  if (!question.required && answer_count == 0) return true;
  if (answer_count < question.minimum_selections ||
      (question.maximum_selections &&
       answer_count > *question.maximum_selections)) {
    return false;
  }
  if (question.selection == QuestionSelection::one && answer_count > 1) {
    return false;
  }
  return true;
}

template <typename Payload>
[[nodiscard]] auto invocation_matches(const RunEvent& event,
                                      const Payload& payload) -> bool {
  return !event.metadata.invocation_id ||
         *event.metadata.invocation_id == payload.invocation_id;
}

} // namespace

auto TranscriptProjection::message(const MessageId& id) -> TranscriptMessage* {
  for (auto& item : m_items) {
    auto* value = std::get_if<TranscriptMessage>(&item);
    if (value != nullptr && value->message_id == id) return value;
  }
  return nullptr;
}

auto TranscriptProjection::inference_message(const InferenceId& id)
    -> TranscriptMessage* {
  for (auto& item : m_items) {
    auto* value = std::get_if<TranscriptMessage>(&item);
    if (value != nullptr && value->inference_id == id) return value;
  }
  return nullptr;
}

auto TranscriptProjection::tool(const InvocationId& id)
    -> TranscriptToolSummary* {
  for (auto& item : m_items) {
    auto* value = std::get_if<TranscriptToolSummary>(&item);
    if (value != nullptr && value->invocation_id == id) return value;
  }
  return nullptr;
}

auto TranscriptProjection::question(
    const QuestionId& id, const std::optional<InvocationId>& invocation_id)
    -> TranscriptQuestionSummary* {
  for (auto& item : m_items) {
    auto* value = std::get_if<TranscriptQuestionSummary>(&item);
    if (value != nullptr && value->question.question_id == id &&
        value->invocation_id == invocation_id) {
      return value;
    }
  }
  return nullptr;
}

auto TranscriptProjection::apply(const RunEvent& event)
    -> std::expected<void, TranscriptProjectionError> {
  try {
    auto candidate = *this;
    if (auto result = candidate.apply_in_place(event); !result) return result;
    *this = std::move(candidate);
    return {};
  } catch (...) {
    return error(TranscriptProjectionErrorCode::internal_failure,
                 "transcript projection failed internally");
  }
}

auto TranscriptProjection::rebuild(const std::span<const RunEvent> events)
    -> std::expected<TranscriptProjection, TranscriptProjectionError> {
  try {
    TranscriptProjection result;
    for (const auto& event : events) {
      if (auto applied = result.apply(event); !applied) {
        return std::unexpected(std::move(applied.error()));
      }
    }
    return result;
  } catch (...) {
    return error(TranscriptProjectionErrorCode::internal_failure,
                 "transcript rebuild failed internally");
  }
}

auto TranscriptProjection::apply_in_place(const RunEvent& event)
    -> std::expected<void, TranscriptProjectionError> {
  if (m_event_ids.contains(event.metadata.event_id)) {
    return error(TranscriptProjectionErrorCode::duplicate_event,
                 "event ID is already present in the transcript");
  }
  if (auto applied = m_run.apply(event); !applied) {
    return std::unexpected(projection_error(applied.error()));
  }

  const auto transition_error = [](std::string message) {
    return error(TranscriptProjectionErrorCode::invalid_transition,
                 std::move(message));
  };

  const auto result = std::visit(
      Overloaded{
          [&](const UserContentAdded& added)
              -> std::expected<void, TranscriptProjectionError> {
            m_items.emplace_back(
                TranscriptMessage{added.message.message_id,
                                  added.message.role,
                                  added.message.content,
                                  TranscriptMessageState::complete,
                                  std::nullopt,
                                  {},
                                  std::nullopt,
                                  {}});
            return {};
          },
          [&](const AssistantContentStarted& started)
              -> std::expected<void, TranscriptProjectionError> {
            auto usage = m_inference_usage[started.inference_id];
            m_items.emplace_back(
                TranscriptMessage{started.message_id,
                                  Role::assistant,
                                  {},
                                  TranscriptMessageState::streaming,
                                  started.inference_id,
                                  usage,
                                  std::nullopt,
                                  {}});
            return {};
          },
          [&](const AssistantContentDeltaAdded& added)
              -> std::expected<void, TranscriptProjectionError> {
            auto* target = message(added.message_id);
            if (target == nullptr) {
              return error(
                  TranscriptProjectionErrorCode::unknown_message,
                  "content delta refers to an unknown transcript message");
            }
            target->content.push_back(added.delta);
            return {};
          },
          [&](const AssistantContentFinished& finished)
              -> std::expected<void, TranscriptProjectionError> {
            auto* target = message(finished.message_id);
            if (target == nullptr) {
              return error(
                  TranscriptProjectionErrorCode::unknown_message,
                  "content finish refers to an unknown transcript message");
            }
            target->state = TranscriptMessageState::complete;
            return {};
          },
          [&](const UsageRecorded& recorded)
              -> std::expected<void, TranscriptProjectionError> {
            auto& usage = m_inference_usage[recorded.inference_id];
            if (!add_usage(usage, recorded.usage)) {
              return error(TranscriptProjectionErrorCode::usage_overflow,
                           "inference usage overflow");
            }
            if (auto* target = inference_message(recorded.inference_id)) {
              target->usage = usage;
            }
            return {};
          },
          [&](const InferenceFailed& failed)
              -> std::expected<void, TranscriptProjectionError> {
            if (auto* target = inference_message(failed.inference_id)) {
              target->state = TranscriptMessageState::failed;
              target->error = failed.error;
            } else {
              m_items.emplace_back(TranscriptNotice{
                  TranscriptNoticeKind::failed, failed.error.message});
            }
            return {};
          },
          [&](const InferenceCancelled& cancelled)
              -> std::expected<void, TranscriptProjectionError> {
            if (auto* target = inference_message(cancelled.inference_id)) {
              target->state = TranscriptMessageState::cancelled;
            } else {
              m_items.emplace_back(TranscriptNotice{
                  TranscriptNoticeKind::cancelled,
                  cancelled.reason.value_or("inference cancelled")});
            }
            return {};
          },
          [&](const RunFailed& failed)
              -> std::expected<void, TranscriptProjectionError> {
            const bool represented =
                std::ranges::any_of(m_items, [&](const TranscriptItem& item) {
                  if (const auto* message =
                          std::get_if<TranscriptMessage>(&item)) {
                    return message->state == TranscriptMessageState::failed &&
                           message->error == failed.error;
                  }
                  if (const auto* notice =
                          std::get_if<TranscriptNotice>(&item)) {
                    return notice->kind == TranscriptNoticeKind::failed &&
                           notice->message == failed.error.message;
                  }
                  return false;
                });
            if (!represented) {
              m_items.emplace_back(TranscriptNotice{
                  TranscriptNoticeKind::failed, failed.error.message});
            }
            return {};
          },
          [&](const RunCancelled& cancelled)
              -> std::expected<void, TranscriptProjectionError> {
            const bool represented =
                std::ranges::any_of(m_items, [](const TranscriptItem& item) {
                  const auto* message = std::get_if<TranscriptMessage>(&item);
                  return message != nullptr &&
                         message->state == TranscriptMessageState::cancelled;
                });
            if (!represented) {
              m_items.emplace_back(
                  TranscriptNotice{TranscriptNoticeKind::cancelled,
                                   cancelled.reason.value_or("run cancelled")});
            }
            return {};
          },
          [&](const ToolProposed& proposed)
              -> std::expected<void, TranscriptProjectionError> {
            if (!invocation_matches(event, proposed)) {
              return transition_error(
                  "tool event envelope and payload invocation IDs differ");
            }
            if (tool(proposed.invocation_id) != nullptr) {
              return transition_error("tool invocation is already present");
            }
            m_items.emplace_back(
                TranscriptToolSummary{proposed.invocation_id,
                                      proposed.tool_name,
                                      TranscriptToolState::proposed,
                                      {},
                                      {},
                                      std::nullopt});
            return {};
          },
          [&](const ToolPolicyDecided& decided)
              -> std::expected<void, TranscriptProjectionError> {
            if (!invocation_matches(event, decided)) {
              return transition_error(
                  "tool event envelope and payload invocation IDs differ");
            }
            auto* target = tool(decided.invocation_id);
            if (target == nullptr) {
              return error(TranscriptProjectionErrorCode::unknown_invocation,
                           "policy decision refers to an unknown invocation");
            }
            if (target->state != TranscriptToolState::proposed) {
              return transition_error("tool policy may be decided only once");
            }
            switch (decided.decision) {
              case PolicyDecision::allow:
                target->state = TranscriptToolState::allowed;
                break;
              case PolicyDecision::deny:
                target->state = TranscriptToolState::denied;
                break;
              case PolicyDecision::require_approval:
                target->state = TranscriptToolState::awaiting_approval;
                break;
            }
            return {};
          },
          [&](const ToolApprovalRequested& requested)
              -> std::expected<void, TranscriptProjectionError> {
            if (!invocation_matches(event, requested)) {
              return transition_error(
                  "tool event envelope and payload invocation IDs differ");
            }
            auto* target = tool(requested.invocation_id);
            if (target == nullptr) {
              return error(TranscriptProjectionErrorCode::unknown_invocation,
                           "approval request refers to an unknown invocation");
            }
            if (target->state != TranscriptToolState::awaiting_approval) {
              return transition_error(
                  "approval request requires an awaiting tool invocation");
            }
            return {};
          },
          [&](const ToolApprovalDecided& decided)
              -> std::expected<void, TranscriptProjectionError> {
            if (!invocation_matches(event, decided)) {
              return transition_error(
                  "tool event envelope and payload invocation IDs differ");
            }
            auto* target = tool(decided.invocation_id);
            if (target == nullptr) {
              return error(TranscriptProjectionErrorCode::unknown_invocation,
                           "approval decision refers to an unknown invocation");
            }
            if (target->state != TranscriptToolState::awaiting_approval) {
              return transition_error(
                  "approval decision requires an awaiting tool invocation");
            }
            switch (decided.decision) {
              case ApprovalDecision::approved:
                target->state = TranscriptToolState::allowed;
                break;
              case ApprovalDecision::denied:
                target->state = TranscriptToolState::denied;
                break;
              case ApprovalDecision::cancelled:
                target->state = TranscriptToolState::cancelled;
                break;
            }
            return {};
          },
          [&](const ToolPolicyFailed& failed)
              -> std::expected<void, TranscriptProjectionError> {
            if (!invocation_matches(event, failed)) {
              return transition_error(
                  "tool event envelope and payload invocation IDs differ");
            }
            if (tool(failed.invocation_id) == nullptr) {
              return error(TranscriptProjectionErrorCode::unknown_invocation,
                           "policy failure refers to an unknown invocation");
            }
            return {};
          },
          [&](const ToolStarted& started)
              -> std::expected<void, TranscriptProjectionError> {
            if (!invocation_matches(event, started)) {
              return transition_error(
                  "tool event envelope and payload invocation IDs differ");
            }
            auto* target = tool(started.invocation_id);
            if (target == nullptr) {
              return error(TranscriptProjectionErrorCode::unknown_invocation,
                           "tool start refers to an unknown invocation");
            }
            if (target->state != TranscriptToolState::allowed) {
              return transition_error("only an allowed tool may start");
            }
            target->state = TranscriptToolState::running;
            return {};
          },
          [&](const ToolProgressed& progressed)
              -> std::expected<void, TranscriptProjectionError> {
            if (!invocation_matches(event, progressed)) {
              return transition_error(
                  "tool event envelope and payload invocation IDs differ");
            }
            auto* target = tool(progressed.invocation_id);
            if (target == nullptr) {
              return error(TranscriptProjectionErrorCode::unknown_invocation,
                           "tool progress refers to an unknown invocation");
            }
            if (target->state != TranscriptToolState::running) {
              return transition_error(
                  "tool progress requires a running invocation");
            }
            target->progress.insert(target->progress.end(),
                                    progressed.content.begin(),
                                    progressed.content.end());
            return {};
          },
          [&](const ToolResultRecorded& recorded)
              -> std::expected<void, TranscriptProjectionError> {
            if (!invocation_matches(event, recorded)) {
              return transition_error(
                  "tool event envelope and payload invocation IDs differ");
            }
            auto* target = tool(recorded.invocation_id);
            if (target == nullptr) {
              return error(TranscriptProjectionErrorCode::unknown_invocation,
                           "tool result refers to an unknown invocation");
            }
            if (target->state != TranscriptToolState::running) {
              return transition_error(
                  "tool result requires a running invocation");
            }
            target->result = recorded.content;
            target->state = TranscriptToolState::complete;
            return {};
          },
          [&](const ToolErrored& failed)
              -> std::expected<void, TranscriptProjectionError> {
            if (!invocation_matches(event, failed)) {
              return transition_error(
                  "tool event envelope and payload invocation IDs differ");
            }
            auto* target = tool(failed.invocation_id);
            if (target == nullptr) {
              return error(TranscriptProjectionErrorCode::unknown_invocation,
                           "tool error refers to an unknown invocation");
            }
            if (target->state == TranscriptToolState::complete ||
                target->state == TranscriptToolState::failed || target->error) {
              return transition_error("tool invocation is already terminal");
            }
            if (failed.error.code == ErrorCode::cancelled) {
              target->state = TranscriptToolState::cancelled;
            } else if (target->state != TranscriptToolState::denied) {
              target->state = TranscriptToolState::failed;
            }
            target->error = failed.error;
            return {};
          },
          [&](const QuestionRequested& requested)
              -> std::expected<void, TranscriptProjectionError> {
            if (question(requested.question.question_id,
                         event.metadata.invocation_id) != nullptr) {
              return transition_error("question is already present");
            }
            m_items.emplace_back(TranscriptQuestionSummary{
                requested.question, TranscriptQuestionState::awaiting_answer,
                std::nullopt, std::nullopt, event.metadata.invocation_id});
            return {};
          },
          [&](const QuestionAnswered& answered)
              -> std::expected<void, TranscriptProjectionError> {
            auto* target = question(answered.answer.question_id,
                                    event.metadata.invocation_id);
            if (target == nullptr) {
              return error(TranscriptProjectionErrorCode::unknown_question,
                           "answer refers to an unknown question");
            }
            if (target->state != TranscriptQuestionState::awaiting_answer ||
                !answer_is_valid(target->question, answered.answer)) {
              return transition_error(
                  "question answer is invalid or duplicated");
            }
            target->state = TranscriptQuestionState::answered;
            target->answer = answered.answer;
            return {};
          },
          [&](const QuestionCancelled& cancelled)
              -> std::expected<void, TranscriptProjectionError> {
            auto* target =
                question(cancelled.question_id, event.metadata.invocation_id);
            if (target == nullptr) {
              return error(TranscriptProjectionErrorCode::unknown_question,
                           "cancellation refers to an unknown question");
            }
            if (target->state != TranscriptQuestionState::awaiting_answer) {
              return transition_error("question is already resolved");
            }
            target->state = TranscriptQuestionState::cancelled;
            target->cancellation_reason = cancelled.reason;
            return {};
          },
          [&](const ArtifactCreated& created)
              -> std::expected<void, TranscriptProjectionError> {
            if (!m_artifacts
                     .emplace(created.artifact.artifact_id, created.artifact)
                     .second) {
              return transition_error("artifact ID is already present");
            }
            return {};
          },
          [&](const ArtifactReferenced& referenced)
              -> std::expected<void, TranscriptProjectionError> {
            const auto found = m_artifacts.find(referenced.artifact_id);
            if (found == m_artifacts.end()) {
              return error(TranscriptProjectionErrorCode::unknown_artifact,
                           "artifact reference has no matching creation event");
            }
            if (referenced.message_id) {
              auto* target = message(*referenced.message_id);
              if (target == nullptr) {
                return error(TranscriptProjectionErrorCode::unknown_message,
                             "artifact reference targets an unknown message");
              }
              target->artifacts.push_back(found->second);
            }
            m_items.emplace_back(TranscriptArtifactReference{
                found->second, referenced.message_id});
            return {};
          },
          [&](const VerificationEvidenceRecorded& recorded)
              -> std::expected<void, TranscriptProjectionError> {
            const auto& evidence = recorded.evidence;
            if (!event.metadata.invocation_id ||
                *event.metadata.invocation_id !=
                    evidence.producer.invocation_id) {
              return transition_error(
                  "verification event envelope and producer invocation differ");
            }
            const auto* invocation = tool(evidence.producer.invocation_id);
            if (invocation == nullptr ||
                invocation->tool_name != evidence.producer.tool_name) {
              return error(
                  TranscriptProjectionErrorCode::unknown_invocation,
                  "verification evidence has no matching tool invocation");
            }
            if (invocation->state != TranscriptToolState::complete &&
                invocation->state != TranscriptToolState::failed &&
                invocation->state != TranscriptToolState::cancelled &&
                invocation->state != TranscriptToolState::denied) {
              return transition_error(
                  "verification evidence requires a terminal tool invocation");
            }
            if (!repository::validate_verification_evidence(evidence)) {
              return error(TranscriptProjectionErrorCode::invalid_verification,
                           "verification evidence is invalid");
            }
            if (m_verification_ids.contains(evidence.evidence_id) ||
                m_verification_invocations.contains(
                    evidence.producer.invocation_id)) {
              return transition_error("verification evidence or producing "
                                      "invocation is duplicated");
            }
            auto artifact_exists = [&](const ArtifactId& id) {
              return m_artifacts.contains(id);
            };
            for (const auto& artifact : evidence.artifacts) {
              if (!artifact_exists(artifact)) {
                return error(
                    TranscriptProjectionErrorCode::unknown_artifact,
                    "verification evidence references an unknown artifact");
              }
            }
            for (const auto& output : evidence.output) {
              if (output.complete_artifact_id &&
                  !artifact_exists(*output.complete_artifact_id)) {
                return error(
                    TranscriptProjectionErrorCode::unknown_artifact,
                    "verification output references an unknown artifact");
              }
            }
            m_verification_ids.insert(evidence.evidence_id);
            m_verification_invocations.insert(evidence.producer.invocation_id);
            m_items.emplace_back(TranscriptVerificationSummary{evidence});
            return {};
          },
          [&](const UnknownEvent&)
              -> std::expected<void, TranscriptProjectionError> { return {}; },
          [&](const auto&) -> std::expected<void, TranscriptProjectionError> {
            return {};
          },
      },
      event.payload);

  if (!result) return result;
  m_event_ids.insert(event.metadata.event_id);
  return {};
}

auto SessionTranscriptProjection::apply(const RunEvent& event)
    -> std::expected<void, TranscriptProjectionError> {
  try {
    auto candidate = *this;
    if (auto result = candidate.apply_in_place(event); !result) return result;
    *this = std::move(candidate);
    return {};
  } catch (...) {
    return error(TranscriptProjectionErrorCode::internal_failure,
                 "session transcript projection failed internally");
  }
}

auto SessionTranscriptProjection::rebuild(
    const std::span<const RunEvent> events)
    -> std::expected<SessionTranscriptProjection, TranscriptProjectionError> {
  try {
    SessionTranscriptProjection result;
    for (const auto& event : events) {
      if (auto applied = result.apply(event); !applied) {
        return std::unexpected(std::move(applied.error()));
      }
    }
    return result;
  } catch (...) {
    return error(TranscriptProjectionErrorCode::internal_failure,
                 "session transcript rebuild failed internally");
  }
}

auto SessionTranscriptProjection::apply_in_place(const RunEvent& event)
    -> std::expected<void, TranscriptProjectionError> {
  if (event.metadata.sequence == 0 || event.metadata.schema_version == 0) {
    return error(TranscriptProjectionErrorCode::invalid_envelope,
                 "event sequence and schema version must be positive");
  }
  if (m_event_ids.contains(event.metadata.event_id)) {
    return error(TranscriptProjectionErrorCode::duplicate_event,
                 "event ID is already present in the session transcript");
  }
  if (event.metadata.sequence <= m_last_sequence) {
    return error(TranscriptProjectionErrorCode::non_monotonic_sequence,
                 "session transcript event sequence did not increase");
  }

  auto found = m_run_indices.find(event.metadata.run_id);
  if (found == m_run_indices.end()) {
    const auto index = m_runs.size();
    m_runs.emplace_back();
    found = m_run_indices.emplace(event.metadata.run_id, index).first;
  }
  if (auto applied = m_runs[found->second].apply(event); !applied) {
    return std::unexpected(std::move(applied.error()));
  }

  m_event_ids.insert(event.metadata.event_id);
  m_last_sequence = event.metadata.sequence;
  return {};
}

} // namespace aiforge::domain
