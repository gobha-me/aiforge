#include <aiforge/domain/run_projection.hpp>

#include <algorithm>
#include <limits>
#include <utility>

namespace aiforge::domain {
namespace {

template <typename... Visitors> struct Overloaded : Visitors... {
  using Visitors::operator()...;
};

template <typename... Visitors>
Overloaded(Visitors...) -> Overloaded<Visitors...>;

[[nodiscard]] auto transition_error(std::string message)
    -> std::expected<void, ProjectionError> {
  return std::unexpected(ProjectionError{
      ProjectionErrorCode::invalid_transition, std::move(message)});
}

[[nodiscard]] auto add_checked(std::uint64_t& total, const std::uint64_t amount)
    -> bool {
  if (amount > std::numeric_limits<std::uint64_t>::max() - total) return false;
  total += amount;
  return true;
}

} // namespace

auto RunProjection::find_message(const MessageId& message_id)
    -> ProjectedMessage* {
  const auto found =
      std::ranges::find(m_messages, message_id, &ProjectedMessage::message_id);
  return found == m_messages.end() ? nullptr : &*found;
}

auto RunProjection::require_running() const
    -> std::expected<void, ProjectionError> {
  if (m_status != RunStatus::running && m_status != RunStatus::awaiting_input &&
      m_status != RunStatus::awaiting_approval) {
    return transition_error("event requires a live run");
  }
  return {};
}

auto RunProjection::apply(const RunEvent& event)
    -> std::expected<void, ProjectionError> {
  if (event.metadata.sequence == 0 || event.metadata.schema_version == 0) {
    return std::unexpected(
        ProjectionError{ProjectionErrorCode::invalid_envelope,
                        "event sequence and schema version must be positive"});
  }
  if (event.metadata.sequence <= m_last_sequence) {
    return std::unexpected(
        ProjectionError{ProjectionErrorCode::non_monotonic_sequence,
                        "event sequence did not increase"});
  }
  if (m_run_id && *m_run_id != event.metadata.run_id) {
    return std::unexpected(ProjectionError{ProjectionErrorCode::wrong_run,
                                           "event belongs to another run"});
  }

  const bool starts_run = std::holds_alternative<RunStarted>(event.payload);
  const bool unknown = std::holds_alternative<UnknownEvent>(event.payload);
  if (!m_run_id && !starts_run && !unknown) {
    return transition_error("the first known run event must be run.started");
  }
  if ((m_status == RunStatus::completed || m_status == RunStatus::failed ||
       m_status == RunStatus::cancelled) &&
      !unknown) {
    return transition_error("known events cannot follow a terminal run event");
  }

  const auto result = std::visit(
      Overloaded{
          [&](const RunStarted& started)
              -> std::expected<void, ProjectionError> {
            if (m_status != RunStatus::not_started || m_run_id) {
              return transition_error("run.started may occur only once");
            }
            m_run_id = event.metadata.run_id;
            m_status = RunStatus::running;
            m_persona_id = started.persona_id;
            return {};
          },
          [&](const RunProvenanceRecorded& recorded)
              -> std::expected<void, ProjectionError> {
            if (auto live = require_running(); !live) return live;
            if (m_provenance) {
              return transition_error(
                  "run provenance may be recorded only once");
            }
            m_provenance = recorded.provenance;
            return {};
          },
          [&](const PersonaSelectionRecorded& recorded)
              -> std::expected<void, ProjectionError> {
            if (auto live = require_running(); !live) return live;
            if (m_persona_selection || !m_messages.empty() ||
                !validate_persona_selection(recorded.selection)) {
              return transition_error(
                  "persona selection must be recorded once before run content");
            }
            const auto selected_id =
                recorded.selection.persona
                    ? std::optional<PersonaId>{recorded.selection.persona
                                                   ->persona_id}
                    : std::nullopt;
            if (selected_id != m_persona_id) {
              return transition_error(
                  "persona selection does not match run start metadata");
            }
            m_persona_selection = recorded.selection;
            return {};
          },
          [&](const RunAwaitingInput&) -> std::expected<void, ProjectionError> {
            if ((m_status != RunStatus::running &&
                 m_status != RunStatus::awaiting_approval) ||
                m_active_inference_id) {
              return transition_error(
                  "a run may await input only between inferences");
            }
            m_status = RunStatus::awaiting_input;
            return {};
          },
          [&](const RunResumed&) -> std::expected<void, ProjectionError> {
            if (m_status != RunStatus::awaiting_input) {
              return transition_error("only an awaiting run may resume");
            }
            m_status = m_pending_approval_ids.empty()
                           ? RunStatus::running
                           : RunStatus::awaiting_approval;
            return {};
          },
          [&](const ToolApprovalRequested& requested)
              -> std::expected<void, ProjectionError> {
            if ((m_status != RunStatus::running &&
                 m_status != RunStatus::awaiting_approval &&
                 m_status != RunStatus::awaiting_input) ||
                m_active_inference_id ||
                !m_pending_approval_ids.insert(requested.invocation_id)
                     .second) {
              return transition_error(
                  "tool approval request is duplicated or out of order");
            }
            if (m_status == RunStatus::running) {
              m_status = RunStatus::awaiting_approval;
            }
            return {};
          },
          [&](const ToolApprovalDecided& decided)
              -> std::expected<void, ProjectionError> {
            if ((m_status != RunStatus::awaiting_approval &&
                 m_status != RunStatus::awaiting_input) ||
                m_pending_approval_ids.erase(decided.invocation_id) != 1) {
              return transition_error(
                  "tool approval decision has no pending request");
            }
            if (m_status == RunStatus::awaiting_approval &&
                m_pending_approval_ids.empty()) {
              m_status = RunStatus::running;
            }
            return {};
          },
          [&](const PlanRevisionProposed&)
              -> std::expected<void, ProjectionError> {
            if ((m_status != RunStatus::running &&
                 m_status != RunStatus::awaiting_plan_revision) ||
                m_active_inference_id) {
              return transition_error(
                  "a plan may be proposed only between inference attempts");
            }
            m_status = RunStatus::awaiting_plan_decision;
            return {};
          },
          [&](const PlanRevisionDecisionRecorded& recorded)
              -> std::expected<void, ProjectionError> {
            if (m_status != RunStatus::awaiting_plan_decision) {
              return transition_error(
                  "a plan decision requires an awaiting plan revision");
            }
            m_status =
                recorded.decision.decision == PlanDecision::revision_requested
                    ? RunStatus::awaiting_plan_revision
                    : RunStatus::running;
            return {};
          },
          [&](const PlanRevisionInvalidated&)
              -> std::expected<void, ProjectionError> {
            if (m_status != RunStatus::running &&
                m_status != RunStatus::awaiting_plan_decision) {
              return transition_error(
                  "plan invalidation requires a live control run");
            }
            if (m_status == RunStatus::awaiting_plan_decision) {
              m_status = RunStatus::awaiting_plan_revision;
            }
            return {};
          },
          [&](const SessionTasksMaterialized&)
              -> std::expected<void, ProjectionError> {
            return require_running();
          },
          [&](const RunCompletionRequested&)
              -> std::expected<void, ProjectionError> {
            return require_running();
          },
          [&](const RunCompleted&) -> std::expected<void, ProjectionError> {
            if (auto live = require_running(); !live) return live;
            if (m_active_inference_id || !m_pending_approval_ids.empty()) {
              return transition_error("a run cannot complete with active work");
            }
            m_status = RunStatus::completed;
            return {};
          },
          [&](const RunFailed&) -> std::expected<void, ProjectionError> {
            if (auto live = require_running(); !live) return live;
            m_active_inference_id.reset();
            m_active_model_id.reset();
            m_active_inference_cost_recorded = false;
            m_active_inference_pricing_recorded = false;
            m_pending_approval_ids.clear();
            m_status = RunStatus::failed;
            return {};
          },
          [&](const RunCancelRequested&)
              -> std::expected<void, ProjectionError> {
            return require_running();
          },
          [&](const RunCancelled&) -> std::expected<void, ProjectionError> {
            if (auto live = require_running(); !live) return live;
            for (auto& message : m_messages) {
              if (message.inference_id == m_active_inference_id)
                message.complete = true;
            }
            m_active_inference_id.reset();
            m_active_model_id.reset();
            m_active_inference_cost_recorded = false;
            m_active_inference_pricing_recorded = false;
            m_pending_approval_ids.clear();
            m_status = RunStatus::cancelled;
            return {};
          },
          [&](const UserContentAdded& added)
              -> std::expected<void, ProjectionError> {
            if (auto live = require_running(); !live) return live;
            if (m_persona_id && !m_persona_selection) {
              return transition_error(
                  "selected persona provenance must precede run content");
            }
            if (added.message.role != Role::user) {
              return transition_error("user content must carry the user role");
            }
            if (find_message(added.message.message_id) != nullptr) {
              return transition_error(
                  "message ID is already present in the run");
            }
            m_messages.push_back(
                ProjectedMessage{added.message.message_id, Role::user,
                                 added.message.content, true, std::nullopt});
            return {};
          },
          [&](const InferenceStarted& started)
              -> std::expected<void, ProjectionError> {
            if (m_status != RunStatus::running) {
              return transition_error("inference requires a running run");
            }
            if (m_active_inference_id) {
              return transition_error(
                  "only one inference may be active in this projection");
            }
            m_active_inference_id = started.inference_id;
            m_active_model_id = started.model_id;
            m_active_inference_cost_recorded = false;
            m_active_inference_pricing_recorded = false;
            return {};
          },
          [&](const InferencePricingObserved& observed)
              -> std::expected<void, ProjectionError> {
            if (!m_active_inference_id ||
                *m_active_inference_id != observed.inference_id ||
                !m_active_model_id ||
                *m_active_model_id != observed.observation.model_id) {
              return std::unexpected(ProjectionError{
                  ProjectionErrorCode::wrong_inference,
                  "pricing observation has no matching inference"});
            }
            if (m_active_inference_pricing_recorded) {
              return transition_error(
                  "pricing may be observed only once per inference");
            }
            if (!validate_pricing_observation(observed.observation)) {
              return std::unexpected(
                  ProjectionError{ProjectionErrorCode::invalid_pricing,
                                  "pricing observation is malformed"});
            }
            m_pricing_observations.push_back(observed.observation);
            m_active_inference_pricing_recorded = true;
            return {};
          },
          [&](const AssistantContentStarted& started)
              -> std::expected<void, ProjectionError> {
            if (!m_active_inference_id ||
                *m_active_inference_id != started.inference_id) {
              return std::unexpected(ProjectionError{
                  ProjectionErrorCode::wrong_inference,
                  "assistant content has no matching inference"});
            }
            if (find_message(started.message_id) != nullptr) {
              return transition_error(
                  "message ID is already present in the run");
            }
            m_messages.push_back(ProjectedMessage{started.message_id,
                                                  Role::assistant,
                                                  {},
                                                  false,
                                                  started.inference_id});
            return {};
          },
          [&](const AssistantContentDeltaAdded& added)
              -> std::expected<void, ProjectionError> {
            if (!m_active_inference_id ||
                *m_active_inference_id != added.inference_id) {
              return std::unexpected(
                  ProjectionError{ProjectionErrorCode::wrong_inference,
                                  "content delta has no matching inference"});
            }
            auto* message = find_message(added.message_id);
            if (message == nullptr) {
              return std::unexpected(ProjectionError{
                  ProjectionErrorCode::unknown_message,
                  "content delta refers to an unknown message"});
            }
            if (message->complete ||
                message->inference_id != added.inference_id) {
              return transition_error(
                  "content delta targets a finished or unrelated message");
            }
            message->content.push_back(added.delta);
            return {};
          },
          [&](const AssistantContentFinished& finished)
              -> std::expected<void, ProjectionError> {
            if (!m_active_inference_id ||
                *m_active_inference_id != finished.inference_id) {
              return std::unexpected(
                  ProjectionError{ProjectionErrorCode::wrong_inference,
                                  "content finish has no matching inference"});
            }
            auto* message = find_message(finished.message_id);
            if (message == nullptr) {
              return std::unexpected(ProjectionError{
                  ProjectionErrorCode::unknown_message,
                  "content finish refers to an unknown message"});
            }
            if (message->complete ||
                message->inference_id != finished.inference_id) {
              return transition_error(
                  "assistant message is already finished or unrelated");
            }
            message->complete = true;
            return {};
          },
          [&](const ReasoningMetadataAdded& reasoning)
              -> std::expected<void, ProjectionError> {
            if (!m_active_inference_id ||
                *m_active_inference_id != reasoning.inference_id) {
              return std::unexpected(ProjectionError{
                  ProjectionErrorCode::wrong_inference,
                  "reasoning metadata has no matching inference"});
            }
            return {};
          },
          [&](const UsageRecorded& recorded)
              -> std::expected<void, ProjectionError> {
            if (!m_active_inference_id ||
                *m_active_inference_id != recorded.inference_id) {
              return std::unexpected(
                  ProjectionError{ProjectionErrorCode::wrong_inference,
                                  "usage has no matching inference"});
            }
            Usage next = m_usage;
            if (!add_checked(next.input_tokens, recorded.usage.input_tokens) ||
                !add_checked(next.output_tokens,
                             recorded.usage.output_tokens) ||
                !add_checked(next.cached_input_tokens,
                             recorded.usage.cached_input_tokens) ||
                !add_checked(next.reasoning_tokens,
                             recorded.usage.reasoning_tokens)) {
              return std::unexpected(ProjectionError{
                  ProjectionErrorCode::usage_overflow, "usage total overflow"});
            }
            m_usage = next;
            return {};
          },
          [&](const InferenceCostRecorded& recorded)
              -> std::expected<void, ProjectionError> {
            if (!m_active_inference_id ||
                *m_active_inference_id != recorded.inference_id) {
              return std::unexpected(
                  ProjectionError{ProjectionErrorCode::wrong_inference,
                                  "reported cost has no matching inference"});
            }
            if (m_active_inference_cost_recorded) {
              return transition_error(
                  "reported cost may be recorded only once per inference");
            }
            auto next = m_reported_cost;
            if (next) {
              auto combined = add(*next, recorded.cost);
              if (!combined) {
                return std::unexpected(
                    ProjectionError{ProjectionErrorCode::cost_overflow,
                                    "reported cost total overflow"});
              }
              next = std::move(*combined);
            } else {
              next = recorded.cost;
            }
            m_reported_cost = std::move(next);
            m_active_inference_cost_recorded = true;
            return {};
          },
          [&](const InferenceFinished& finished)
              -> std::expected<void, ProjectionError> {
            if (!m_active_inference_id ||
                *m_active_inference_id != finished.inference_id) {
              return std::unexpected(
                  ProjectionError{ProjectionErrorCode::wrong_inference,
                                  "finish has no matching inference"});
            }
            const bool incomplete =
                std::ranges::any_of(m_messages, [&](const auto& message) {
                  return message.inference_id == finished.inference_id &&
                         !message.complete;
                });
            if (incomplete) {
              return transition_error(
                  "inference finished before its assistant content");
            }
            m_active_inference_id.reset();
            m_active_model_id.reset();
            m_active_inference_cost_recorded = false;
            m_active_inference_pricing_recorded = false;
            return {};
          },
          [&](const InferenceFailed& failed)
              -> std::expected<void, ProjectionError> {
            if (!m_active_inference_id ||
                *m_active_inference_id != failed.inference_id) {
              return std::unexpected(
                  ProjectionError{ProjectionErrorCode::wrong_inference,
                                  "failure has no matching inference"});
            }
            for (auto& message : m_messages) {
              if (message.inference_id == failed.inference_id)
                message.complete = true;
            }
            m_active_inference_id.reset();
            m_active_model_id.reset();
            m_active_inference_cost_recorded = false;
            m_active_inference_pricing_recorded = false;
            return {};
          },
          [&](const InferenceCancelled& cancelled)
              -> std::expected<void, ProjectionError> {
            if (!m_active_inference_id ||
                *m_active_inference_id != cancelled.inference_id) {
              return std::unexpected(
                  ProjectionError{ProjectionErrorCode::wrong_inference,
                                  "cancellation has no matching inference"});
            }
            for (auto& message : m_messages) {
              if (message.inference_id == cancelled.inference_id)
                message.complete = true;
            }
            m_active_inference_id.reset();
            m_active_model_id.reset();
            m_active_inference_cost_recorded = false;
            m_active_inference_pricing_recorded = false;
            return {};
          },
          [&](const UnknownEvent&) -> std::expected<void, ProjectionError> {
            return {};
          },
          [&](const auto&) -> std::expected<void, ProjectionError> {
            return require_running();
          },
      },
      event.payload);

  if (result) m_last_sequence = event.metadata.sequence;
  return result;
}

} // namespace aiforge::domain
