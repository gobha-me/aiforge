#include <aiforge/runtime/plan_task_controller.hpp>

#include <algorithm>
#include <ranges>
#include <set>
#include <utility>

namespace aiforge::runtime {
namespace {

[[nodiscard]] auto failure(const PlanTaskControllerErrorCode code,
                           std::string message, const bool retryable = false)
    -> std::unexpected<PlanTaskControllerError> {
  return std::unexpected(
      PlanTaskControllerError{code, std::move(message), retryable});
}

[[nodiscard]] auto from_kernel(RunKernelError error)
    -> PlanTaskControllerError {
  PlanTaskControllerErrorCode code{
      PlanTaskControllerErrorCode::runtime_failure};
  switch (error.code) {
    case RunKernelErrorCode::wrong_plan:
    case RunKernelErrorCode::wrong_run:
    case RunKernelErrorCode::invalid_plan_state:
      code = PlanTaskControllerErrorCode::stale_state;
      break;
    case RunKernelErrorCode::storage_failure:
      code = PlanTaskControllerErrorCode::storage_failure;
      break;
    case RunKernelErrorCode::invalid_limits:
      code = PlanTaskControllerErrorCode::resource_exhausted;
      break;
    default: break;
  }
  return {code, std::move(error.message), error.retryable};
}

[[nodiscard]] auto valid_revision_reason(
    const domain::PlanRevisionDecision& decision) -> bool {
  if (decision.decision != domain::PlanDecision::revision_requested) {
    return true;
  }
  if (!decision.reason || decision.reason->empty() ||
      decision.reason->size() > 16U * 1024U) {
    return false;
  }
  return std::ranges::none_of(*decision.reason, [](const unsigned char value) {
    return value == 0 || value == 0x7FU ||
           (value < 0x20U && value != '\n' && value != '\t');
  });
}

} // namespace

auto PlanTaskController::inspect(
    std::optional<domain::RepositoryId> repository_id)
    -> std::expected<PlanTaskState, PlanTaskControllerError> {
  try {
    if (m_limits.maximum_backlog_sessions == 0 ||
        m_limits.maximum_backlog_sessions > 4096) {
      return failure(PlanTaskControllerErrorCode::resource_exhausted,
                     "plan-task controller limits are invalid");
    }
    PlanTaskState result{m_kernel.event_log().session_id(),
                         std::nullopt,
                         std::nullopt,
                         domain::PlanGraphState::not_started,
                         std::nullopt,
                         {},
                         {}};
    result.pending_decision = m_kernel.pending_plan_decision();
    const domain::PlanGraphProjection* projection{};
    if (result.pending_decision) {
      projection = m_kernel.plan_projection(result.pending_decision->plan_id);
    } else {
      for (auto event = m_kernel.event_log().events().rbegin();
           event != m_kernel.event_log().events().rend(); ++event) {
        if (const auto* proposed =
                std::get_if<domain::PlanRevisionProposed>(&event->payload)) {
          projection = m_kernel.plan_projection(proposed->revision.plan_id);
          if (projection != nullptr) break;
        }
      }
    }
    if (projection != nullptr) {
      result.plan_state = projection->state();
      if (const auto* current = projection->current_revision()) {
        result.plan = *current;
        auto scheduling_revision = *current;
        if (scheduling_revision.task_executions.empty()) {
          scheduling_revision.task_executions.reserve(
              scheduling_revision.revision.tasks.size());
          for (const auto& task : scheduling_revision.revision.tasks) {
            scheduling_revision.task_executions.push_back({task.task_id, {}});
          }
        }
        auto schedule = domain::analyze_task_schedule(scheduling_revision);
        if (!schedule) {
          return failure(PlanTaskControllerErrorCode::runtime_failure,
                         schedule.error().message);
        }
        result.schedule = std::move(*schedule);
      }
    }
    result.session_tasks = m_kernel.active_session_tasks();

    if (repository_id) {
      std::expected<domain::ProjectBacklogProjection, PlanTaskControllerError>
          backlog = std::unexpected(PlanTaskControllerError{
              PlanTaskControllerErrorCode::internal_failure,
              "project-backlog inspection failed", false});
      if (m_session_store != nullptr) {
        auto histories = m_session_store->replay_project_backlog(
            *repository_id, m_limits.maximum_backlog_sessions);
        if (!histories) {
          return failure(PlanTaskControllerErrorCode::storage_failure,
                         histories.error().message,
                         histories.error().retryable);
        }
        auto rebuilt = domain::ProjectBacklogProjection::rebuild(*repository_id,
                                                                 *histories);
        if (!rebuilt) {
          return failure(PlanTaskControllerErrorCode::storage_failure,
                         rebuilt.error().message);
        }
        backlog = std::move(*rebuilt);
      } else {
        auto projected = m_kernel.project_backlog(*repository_id);
        if (!projected) {
          return std::unexpected(from_kernel(std::move(projected.error())));
        }
        backlog = std::move(*projected);
      }
      result.project_backlog.assign(backlog->items().begin(),
                                    backlog->items().end());
    }
    return result;
  } catch (...) {
    return failure(PlanTaskControllerErrorCode::internal_failure,
                   "plan-task inspection failed internally");
  }
}

auto PlanTaskController::decide(const domain::RunId& run_id,
                                domain::PlanRevisionDecision decision,
                                PlanApprovalEnvironment environment)
    -> std::expected<PlanDecisionOutcome, PlanTaskControllerError> {
  if (!valid_revision_reason(decision)) {
    return failure(PlanTaskControllerErrorCode::invalid_request,
                   "revision requests require a bounded nonempty reason");
  }
  auto result =
      m_kernel.decide_plan(run_id, std::move(decision), std::move(environment));
  if (!result) return std::unexpected(from_kernel(std::move(result.error())));
  return *result;
}

auto PlanTaskController::promote(ProjectTaskPromotion promotion)
    -> std::expected<void, PlanTaskControllerError> {
  auto result = m_kernel.promote_project_task(std::move(promotion));
  if (!result) return std::unexpected(from_kernel(std::move(result.error())));
  return {};
}

auto PlanTaskController::set_backlog_status(ProjectTaskStatusUpdate update)
    -> std::expected<void, PlanTaskControllerError> {
  auto result = m_kernel.update_project_task_status(std::move(update));
  if (!result) return std::unexpected(from_kernel(std::move(result.error())));
  return {};
}

} // namespace aiforge::runtime
