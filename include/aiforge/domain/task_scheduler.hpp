#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include <aiforge/domain/plan_projection.hpp>

namespace aiforge::domain {

struct TaskSchedulingPolicy {
  std::size_t maximum_concurrency{1};
  std::uint32_t maximum_attempts{1};
  auto operator==(const TaskSchedulingPolicy&) const -> bool = default;
};

enum class TaskReadinessState {
  ready,
  waiting_for_dependencies,
  blocked_by_dependency,
  blocked_by_resource,
  waiting_for_capacity,
  running,
  completed,
  failed,
};

struct TaskReadiness {
  PlanTaskId task_id;
  TaskReadinessState state{TaskReadinessState::waiting_for_dependencies};
  std::vector<PlanTaskId> blockers;
  std::uint32_t next_attempt{1};
  auto operator==(const TaskReadiness&) const -> bool = default;
};

enum class TaskRollupState {
  in_progress,
  completed,
  failed,
};

struct TaskScheduleRollup {
  TaskRollupState state{TaskRollupState::in_progress};
  std::size_t completed_tasks{};
  std::size_t failed_tasks{};
  std::size_t blocked_tasks{};
  ChildRunConsumption consumption;
  std::vector<EvidenceId> evidence_ids;
  std::vector<ArtifactId> artifact_ids;
  auto operator==(const TaskScheduleRollup&) const -> bool = default;
};

struct TaskScheduleAnalysis {
  std::vector<TaskReadiness> tasks;
  std::vector<PlanTaskId> dispatchable_task_ids;
  TaskScheduleRollup rollup;
  auto operator==(const TaskScheduleAnalysis&) const -> bool = default;
};

enum class TaskSchedulingErrorCode {
  invalid_policy,
  invalid_projection,
  resource_exhausted,
};

struct TaskSchedulingError {
  TaskSchedulingErrorCode code{TaskSchedulingErrorCode::invalid_projection};
  std::string message;
  auto operator==(const TaskSchedulingError&) const -> bool = default;
};

// True means the scheduler cannot prove the tasks independent. Unknown or
// malformed mutating intents therefore serialize instead of widening safety.
[[nodiscard]] auto task_resource_conflict(const PlanTask& left,
                                          const PlanTask& right) noexcept
    -> bool;

[[nodiscard]] auto analyze_task_schedule(
    const ProjectedPlanRevision& revision,
    const TaskSchedulingPolicy& policy = {})
    -> std::expected<TaskScheduleAnalysis, TaskSchedulingError>;

} // namespace aiforge::domain
