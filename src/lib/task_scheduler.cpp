#include <aiforge/domain/task_scheduler.hpp>

#include <algorithm>
#include <limits>
#include <ranges>
#include <set>
#include <string_view>
#include <utility>

namespace aiforge::domain {
namespace {

constexpr std::size_t maximum_concurrency{16};
constexpr std::uint32_t maximum_attempts{8};

[[nodiscard]] auto failure(const TaskSchedulingErrorCode code,
                           std::string message)
    -> std::unexpected<TaskSchedulingError> {
  return std::unexpected(TaskSchedulingError{code, std::move(message)});
}

[[nodiscard]] auto mutates(const PlanTask& task) -> bool {
  return std::ranges::any_of(task.intended_effects, [](const Effect effect) {
    return effect != Effect::read;
  });
}

[[nodiscard]] auto valid_component(const std::string_view value) -> bool {
  return !value.empty() && value.size() <= 4096 &&
         std::ranges::all_of(value, [](const unsigned char character) {
           return character >= 0x20U && character != 0x7FU;
         });
}

[[nodiscard]] auto valid_repository_path(const std::string_view value) -> bool {
  if (value.empty() || value.front() == '/' || value.back() == '/' ||
      value.contains('\\')) {
    return false;
  }
  std::size_t start{};
  while (start < value.size()) {
    const auto end = value.find('/', start);
    const auto part =
        value.substr(start, end == std::string_view::npos ? value.size() - start
                                                          : end - start);
    if (part.empty() || part == "." || part == "..") return false;
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return true;
}

[[nodiscard]] auto path_overlaps(const std::string_view left,
                                 const std::string_view right) -> bool {
  if (left == right) return true;
  const auto is_ancestor = [](const std::string_view ancestor,
                              const std::string_view descendant) {
    return descendant.size() > ancestor.size() &&
           descendant.starts_with(ancestor) &&
           descendant[ancestor.size()] == '/';
  };
  return is_ancestor(left, right) || is_ancestor(right, left);
}

enum class IntentRelation { disjoint, overlaps, ambiguous };

[[nodiscard]] auto intent_relation(const PlanResourceIntent& left,
                                   const PlanResourceIntent& right)
    -> IntentRelation {
  const auto left_path = left.kind == "repository_path";
  const auto right_path = right.kind == "repository_path";
  const auto left_component = left.kind == "repository_component";
  const auto right_component = right.kind == "repository_component";
  if (left_path && right_path) {
    if (!valid_repository_path(left.value) ||
        !valid_repository_path(right.value)) {
      return IntentRelation::ambiguous;
    }
    return path_overlaps(left.value, right.value) ? IntentRelation::overlaps
                                                  : IntentRelation::disjoint;
  }
  if (left_component && right_component) {
    if (!valid_component(left.value) || !valid_component(right.value)) {
      return IntentRelation::ambiguous;
    }
    return left.value == right.value ? IntentRelation::overlaps
                                     : IntentRelation::disjoint;
  }
  return IntentRelation::ambiguous;
}

template <typename Value>
[[nodiscard]] auto append_unique(std::vector<Value>& destination,
                                 const std::vector<Value>& source) -> bool {
  for (const auto& value : source) {
    if (std::ranges::find(destination, value) == destination.end()) {
      try {
        destination.push_back(value);
      } catch (...) {
        return false;
      }
    }
  }
  return true;
}

template <typename Value>
[[nodiscard]] auto checked_add(Value& total, const Value value) -> bool {
  if (value > std::numeric_limits<Value>::max() - total) return false;
  total += value;
  return true;
}

[[nodiscard]] auto retryable(const ProjectedPlanRevision::TaskExecution& task,
                             const TaskSchedulingPolicy& policy) -> bool {
  if (task.attempts.empty() || !task.attempts.back().result ||
      task.attempts.size() >= policy.maximum_attempts) {
    return false;
  }
  const auto& result = *task.attempts.back().result;
  return result.outcome == SessionTaskOutcome::unavailable && result.error &&
         result.error->retryable;
}

} // namespace

auto task_resource_conflict(const PlanTask& left,
                            const PlanTask& right) noexcept -> bool {
  try {
    if (!mutates(left) && !mutates(right)) return false;
    if ((mutates(left) && left.resource_intents.empty()) ||
        (mutates(right) && right.resource_intents.empty())) {
      return true;
    }
    for (const auto& left_intent : left.resource_intents) {
      for (const auto& right_intent : right.resource_intents) {
        if (left_intent.effect == Effect::read &&
            right_intent.effect == Effect::read) {
          continue;
        }
        if (intent_relation(left_intent, right_intent) !=
            IntentRelation::disjoint) {
          return true;
        }
      }
    }
    return false;
  } catch (...) {
    return true;
  }
}

auto analyze_task_schedule(const ProjectedPlanRevision& revision,
                           const TaskSchedulingPolicy& policy)
    -> std::expected<TaskScheduleAnalysis, TaskSchedulingError> {
  try {
    if (policy.maximum_concurrency == 0 ||
        policy.maximum_concurrency > maximum_concurrency ||
        policy.maximum_attempts == 0 ||
        policy.maximum_attempts > maximum_attempts) {
      return failure(TaskSchedulingErrorCode::invalid_policy,
                     "task scheduling policy is outside its bounds");
    }
    if (revision.revision.tasks.size() != revision.task_executions.size()) {
      return failure(TaskSchedulingErrorCode::invalid_projection,
                     "task scheduling projection is incomplete");
    }

    std::set<PlanTaskId> execution_ids;
    std::set<RunId> child_run_ids;
    for (const auto& execution : revision.task_executions) {
      if (!execution_ids.insert(execution.task_id).second ||
          execution.attempts.size() > maximum_attempts) {
        return failure(TaskSchedulingErrorCode::invalid_projection,
                       "task scheduling projection has invalid attempts");
      }
      for (std::size_t index = 0; index < execution.attempts.size(); ++index) {
        const auto& attempt = execution.attempts[index];
        const auto& descriptor = attempt.dispatch;
        if (!child_run_ids.insert(attempt.child_run_id).second ||
            descriptor.plan_id != revision.revision.plan_id ||
            descriptor.revision_id != revision.revision.revision_id ||
            descriptor.task_id != execution.task_id ||
            descriptor.attempt != index + 1U ||
            !validate_child_run_descriptor(descriptor) ||
            (index + 1U < execution.attempts.size() &&
             (!attempt.result ||
              attempt.result->outcome != SessionTaskOutcome::unavailable ||
              !attempt.result->error || !attempt.result->error->retryable))) {
          return failure(TaskSchedulingErrorCode::invalid_projection,
                         "task scheduling attempt history is invalid");
        }
        if (attempt.result &&
            (attempt.result->plan_id != descriptor.plan_id ||
             attempt.result->revision_id != descriptor.revision_id ||
             attempt.result->task_id != descriptor.task_id ||
             attempt.result->child_run_id != attempt.child_run_id ||
             !validate_session_task_result(*attempt.result,
                                           descriptor.budget))) {
          return failure(TaskSchedulingErrorCode::invalid_projection,
                         "task scheduling result history is invalid");
        }
      }
    }

    TaskScheduleAnalysis analysis;
    analysis.tasks.reserve(revision.revision.tasks.size());
    std::vector<const PlanTask*> running;
    std::vector<const PlanTask*> eligible;

    for (const auto& task : revision.revision.tasks) {
      const auto execution =
          std::ranges::find(revision.task_executions, task.task_id,
                            &ProjectedPlanRevision::TaskExecution::task_id);
      if (execution == revision.task_executions.end()) {
        return failure(TaskSchedulingErrorCode::invalid_projection,
                       "task scheduling projection lacks a task");
      }
      auto state = TaskReadinessState::ready;
      if (!execution->attempts.empty()) {
        const auto& attempt = execution->attempts.back();
        if (!attempt.result) {
          state = TaskReadinessState::running;
          running.push_back(&task);
        } else if (attempt.result->outcome == SessionTaskOutcome::completed) {
          state = TaskReadinessState::completed;
        } else if (!retryable(*execution, policy)) {
          state = TaskReadinessState::failed;
        }
      }
      analysis.tasks.push_back(
          {task.task_id,
           state,
           {},
           static_cast<std::uint32_t>(execution->attempts.size() + 1U)});
    }

    for (std::size_t pass = 0; pass < analysis.tasks.size(); ++pass) {
      auto changed = false;
      for (std::size_t index = 0; index < revision.revision.tasks.size();
           ++index) {
        auto& readiness = analysis.tasks[index];
        if (readiness.state == TaskReadinessState::running ||
            readiness.state == TaskReadinessState::completed ||
            readiness.state == TaskReadinessState::failed) {
          continue;
        }
        const auto previous = readiness.state;
        readiness.blockers.clear();
        auto permanently_blocked = false;
        for (const auto& dependency_id :
             revision.revision.tasks[index].dependency_task_ids) {
          const auto dependency = std::ranges::find(
              analysis.tasks, dependency_id, &TaskReadiness::task_id);
          if (dependency == analysis.tasks.end()) {
            return failure(TaskSchedulingErrorCode::invalid_projection,
                           "task dependency is absent from the projection");
          }
          if (dependency->state != TaskReadinessState::completed) {
            readiness.blockers.push_back(dependency_id);
            permanently_blocked =
                permanently_blocked ||
                dependency->state == TaskReadinessState::failed ||
                dependency->state == TaskReadinessState::blocked_by_dependency;
          }
        }
        readiness.state =
            readiness.blockers.empty()
                ? TaskReadinessState::ready
                : (permanently_blocked
                       ? TaskReadinessState::blocked_by_dependency
                       : TaskReadinessState::waiting_for_dependencies);
        changed = changed || readiness.state != previous;
      }
      if (!changed) break;
    }
    for (std::size_t index = 0; index < analysis.tasks.size(); ++index) {
      if (analysis.tasks[index].state == TaskReadinessState::ready) {
        eligible.push_back(&revision.revision.tasks[index]);
      }
    }

    const auto available = policy.maximum_concurrency > running.size()
                               ? policy.maximum_concurrency - running.size()
                               : 0U;
    std::vector<const PlanTask*> selected;
    for (const auto* task : eligible) {
      auto& readiness = *std::ranges::find(analysis.tasks, task->task_id,
                                           &TaskReadiness::task_id);
      for (const auto* active : running) {
        if (task_resource_conflict(*task, *active))
          readiness.blockers.push_back(active->task_id);
      }
      for (const auto* active : selected) {
        if (task_resource_conflict(*task, *active))
          readiness.blockers.push_back(active->task_id);
      }
      if (!readiness.blockers.empty()) {
        readiness.state = TaskReadinessState::blocked_by_resource;
        continue;
      }
      if (selected.size() >= available) {
        readiness.state = TaskReadinessState::waiting_for_capacity;
        continue;
      }
      selected.push_back(task);
      analysis.dispatchable_task_ids.push_back(task->task_id);
    }

    for (const auto& execution : revision.task_executions) {
      if (!execution.attempts.empty() && execution.attempts.back().result &&
          execution.attempts.back().result->outcome ==
              SessionTaskOutcome::completed) {
        ++analysis.rollup.completed_tasks;
      } else if (!execution.attempts.empty() &&
                 execution.attempts.back().result &&
                 !retryable(execution, policy)) {
        ++analysis.rollup.failed_tasks;
      }
      for (const auto& attempt : execution.attempts) {
        if (!attempt.result) continue;
        const auto& result = *attempt.result;
        if (!checked_add(analysis.rollup.consumption.inference_count,
                         result.consumption.inference_count) ||
            !checked_add(analysis.rollup.consumption.tool_invocation_count,
                         result.consumption.tool_invocation_count) ||
            !checked_add(analysis.rollup.consumption.usage.input_tokens,
                         result.consumption.usage.input_tokens) ||
            !checked_add(analysis.rollup.consumption.usage.output_tokens,
                         result.consumption.usage.output_tokens) ||
            !checked_add(analysis.rollup.consumption.usage.cached_input_tokens,
                         result.consumption.usage.cached_input_tokens) ||
            !checked_add(analysis.rollup.consumption.usage.reasoning_tokens,
                         result.consumption.usage.reasoning_tokens) ||
            !append_unique(analysis.rollup.evidence_ids, result.evidence_ids) ||
            !append_unique(analysis.rollup.artifact_ids, result.artifact_ids)) {
          return failure(TaskSchedulingErrorCode::resource_exhausted,
                         "task schedule rollup exceeds its bounds");
        }
      }
    }
    analysis.rollup.blocked_tasks =
        std::ranges::count_if(analysis.tasks, [](const auto& task) {
          return task.state == TaskReadinessState::blocked_by_dependency;
        });
    if (analysis.rollup.completed_tasks == revision.revision.tasks.size()) {
      analysis.rollup.state = TaskRollupState::completed;
    } else if (analysis.rollup.failed_tasks != 0 &&
               std::ranges::none_of(analysis.tasks, [](const auto& task) {
                 return task.state == TaskReadinessState::ready ||
                        task.state == TaskReadinessState::running ||
                        task.state ==
                            TaskReadinessState::waiting_for_dependencies ||
                        task.state ==
                            TaskReadinessState::waiting_for_capacity ||
                        task.state == TaskReadinessState::blocked_by_resource;
               })) {
      analysis.rollup.state = TaskRollupState::failed;
    }
    return analysis;
  } catch (...) {
    return failure(TaskSchedulingErrorCode::resource_exhausted,
                   "task schedule analysis failed internally");
  }
}

} // namespace aiforge::domain
