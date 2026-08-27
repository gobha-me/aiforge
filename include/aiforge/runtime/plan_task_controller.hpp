#pragma once

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include <aiforge/runtime/run_kernel.hpp>

namespace aiforge::runtime {

enum class PlanTaskControllerErrorCode {
  invalid_request,
  plan_unavailable,
  repository_unavailable,
  stale_state,
  storage_failure,
  runtime_failure,
  resource_exhausted,
  internal_failure,
};

struct PlanTaskControllerError {
  PlanTaskControllerErrorCode code{
      PlanTaskControllerErrorCode::internal_failure};
  std::string message;
  bool retryable{};
  auto operator==(const PlanTaskControllerError&) const -> bool = default;
};

struct PlanTaskControllerLimits {
  std::size_t maximum_backlog_sessions{1024};
  auto operator==(const PlanTaskControllerLimits&) const -> bool = default;
};

struct PlanTaskState {
  domain::SessionId session_id;
  std::optional<PendingPlanDecision> pending_decision;
  std::optional<domain::ProjectedPlanRevision> plan;
  domain::PlanGraphState plan_state{domain::PlanGraphState::not_started};
  std::optional<domain::TaskScheduleAnalysis> schedule;
  std::vector<ActiveSessionTask> session_tasks;
  std::vector<domain::ProjectedBacklogItem> project_backlog;
  auto operator==(const PlanTaskState&) const -> bool = default;
};

class PlanTaskController final {
public:
  PlanTaskController(RunKernel& kernel, storage::SessionStore* session_store,
                     PlanTaskControllerLimits limits = {})
      : m_kernel(kernel), m_session_store(session_store), m_limits(limits) {}

  [[nodiscard]] auto
  inspect(std::optional<domain::RepositoryId> repository_id = std::nullopt)
      -> std::expected<PlanTaskState, PlanTaskControllerError>;
  [[nodiscard]] auto decide(const domain::RunId& run_id,
                            domain::PlanRevisionDecision decision,
                            PlanApprovalEnvironment environment = {})
      -> std::expected<PlanDecisionOutcome, PlanTaskControllerError>;
  [[nodiscard]] auto promote(ProjectTaskPromotion promotion)
      -> std::expected<void, PlanTaskControllerError>;
  [[nodiscard]] auto set_backlog_status(ProjectTaskStatusUpdate update)
      -> std::expected<void, PlanTaskControllerError>;

private:
  RunKernel& m_kernel;
  storage::SessionStore* m_session_store{};
  PlanTaskControllerLimits m_limits;
};

} // namespace aiforge::runtime
