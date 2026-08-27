#include <aiforge/domain/project_backlog_projection.hpp>

#include <algorithm>
#include <ranges>
#include <set>
#include <utility>

#include <aiforge/domain/events.hpp>
#include <aiforge/domain/plan_projection.hpp>

namespace aiforge::domain {
namespace {

[[nodiscard]] auto failure(const ProjectBacklogErrorCode code,
                           std::string message,
                           std::optional<ProjectBacklogItemId> item_id = {})
    -> std::unexpected<ProjectBacklogError> {
  return std::unexpected(
      ProjectBacklogError{code, std::move(message), std::move(item_id)});
}

[[nodiscard]] auto valid_limits(const ProjectBacklogLimits& limits) -> bool {
  return limits.maximum_items > 0 && limits.maximum_reason_bytes > 0;
}

[[nodiscard]] auto valid_reason(const std::optional<std::string>& reason,
                                const ProjectBacklogLimits& limits) -> bool {
  if (!reason) return true;
  if (reason->empty() || reason->size() > limits.maximum_reason_bytes) {
    return false;
  }
  return std::ranges::none_of(*reason, [](const unsigned char value) {
    return value == 0 || value == 0x7FU ||
           (value < 0x20U && value != '\n' && value != '\t');
  });
}

} // namespace

auto validate_project_backlog_item(const ProjectBacklogItem& item,
                                   const ProjectBacklogLimits& limits)
    -> std::expected<void, ProjectBacklogError> {
  if (!valid_limits(limits)) {
    return failure(ProjectBacklogErrorCode::invalid_limits,
                   "project-backlog limits must be positive");
  }
  auto standalone = item.task;
  standalone.parent_task_id.reset();
  standalone.dependency_task_ids.clear();
  PlanRevision revision{item.origin.plan_id,
                        item.origin.revision_id,
                        std::nullopt,
                        "promoted project backlog task",
                        std::nullopt,
                        {std::move(standalone)},
                        {}};
  std::set<PlanTaskId> dependencies;
  if ((item.task.parent_task_id &&
       *item.task.parent_task_id == item.task.task_id) ||
      std::ranges::any_of(item.task.dependency_task_ids,
                          [&](const auto& dependency) {
                            return dependency == item.task.task_id ||
                                   !dependencies.insert(dependency).second;
                          }) ||
      !validate_plan_revision(revision)) {
    return failure(ProjectBacklogErrorCode::invalid_item,
                   "promoted project-backlog task is invalid", item.item_id);
  }
  if (item.origin.task_id != item.task.task_id) {
    return failure(ProjectBacklogErrorCode::invalid_item,
                   "project-backlog origin does not match its task",
                   item.item_id);
  }
  return {};
}

auto validate_project_backlog_status_change(
    const ProjectBacklogStatusChange& change,
    const ProjectBacklogLimits& limits)
    -> std::expected<void, ProjectBacklogError> {
  if (!valid_limits(limits)) {
    return failure(ProjectBacklogErrorCode::invalid_limits,
                   "project-backlog limits must be positive");
  }
  if ((change.status != ProjectBacklogItemStatus::open &&
       change.status != ProjectBacklogItemStatus::resolved) ||
      !valid_reason(change.reason, limits)) {
    return failure(ProjectBacklogErrorCode::invalid_item,
                   "project-backlog status change is invalid", change.item_id);
  }
  if (change.status == ProjectBacklogItemStatus::resolved && !change.reason) {
    return failure(ProjectBacklogErrorCode::invalid_item,
                   "resolving a project-backlog item requires a reason",
                   change.item_id);
  }
  return {};
}

auto ProjectBacklogProjection::apply(const SessionId& session_id,
                                     const RunEvent& event,
                                     const ProjectBacklogLimits& limits)
    -> std::expected<void, ProjectBacklogError> {
  try {
    if (!valid_limits(limits)) {
      return failure(ProjectBacklogErrorCode::invalid_limits,
                     "project-backlog limits must be positive");
    }
    const auto previous = m_last_sequences.contains(session_id)
                              ? m_last_sequences.at(session_id)
                              : 0U;
    if (event.metadata.sequence == 0 || event.metadata.sequence <= previous) {
      return failure(ProjectBacklogErrorCode::non_monotonic_sequence,
                     "project-backlog event sequence does not increase");
    }
    if (m_event_ids.contains({session_id, event.metadata.event_id})) {
      return failure(ProjectBacklogErrorCode::duplicate_event,
                     "project-backlog event identity is duplicated");
    }

    auto next = *this;
    if (const auto* promoted =
            std::get_if<ProjectBacklogItemPromoted>(&event.payload)) {
      if (auto valid = validate_project_backlog_item(promoted->item, limits);
          !valid) {
        return std::unexpected(std::move(valid.error()));
      }
      if (promoted->item.repository_id != m_repository_id) {
        next.m_event_ids.insert({session_id, event.metadata.event_id});
        next.m_last_sequences.insert_or_assign(session_id,
                                               event.metadata.sequence);
        *this = std::move(next);
        return {};
      }
      if (promoted->item.origin.session_id != session_id) {
        return failure(
            ProjectBacklogErrorCode::invalid_item,
            "project-backlog origin does not match its event session",
            promoted->item.item_id);
      }
      if (next.m_items.size() >= limits.maximum_items) {
        return failure(ProjectBacklogErrorCode::resource_exhausted,
                       "project-backlog item limit is exceeded",
                       promoted->item.item_id);
      }
      if (std::ranges::any_of(next.m_items, [&](const auto& item) {
            return item.item.item_id == promoted->item.item_id;
          })) {
        return failure(ProjectBacklogErrorCode::duplicate_identity,
                       "project-backlog item identity is duplicated",
                       promoted->item.item_id);
      }
      if (std::ranges::any_of(next.m_items, [&](const auto& item) {
            return item.item.origin == promoted->item.origin;
          })) {
        return failure(ProjectBacklogErrorCode::duplicate_origin,
                       "session task is already promoted",
                       promoted->item.item_id);
      }
      next.m_items.push_back({promoted->item, event.metadata.event_id,
                              ProjectBacklogItemStatus::open,
                              event.metadata.event_id, std::nullopt});
    } else if (const auto* changed =
                   std::get_if<ProjectBacklogItemStatusChanged>(
                       &event.payload)) {
      if (auto valid =
              validate_project_backlog_status_change(changed->change, limits);
          !valid) {
        return std::unexpected(std::move(valid.error()));
      }
      if (changed->change.repository_id != m_repository_id) {
        next.m_event_ids.insert({session_id, event.metadata.event_id});
        next.m_last_sequences.insert_or_assign(session_id,
                                               event.metadata.sequence);
        *this = std::move(next);
        return {};
      }
      auto found =
          std::ranges::find(next.m_items, changed->change.item_id,
                            [](const auto& item) { return item.item.item_id; });
      if (found == next.m_items.end()) {
        return failure(ProjectBacklogErrorCode::invalid_transition,
                       "project-backlog status targets an unknown item",
                       changed->change.item_id);
      }
      if (found->item.origin.session_id != session_id) {
        return failure(
            ProjectBacklogErrorCode::invalid_transition,
            "project-backlog status must remain in its source session",
            changed->change.item_id);
      }
      if (!changed->change.expected_status_event_id ||
          *changed->change.expected_status_event_id != found->status_event_id) {
        return failure(ProjectBacklogErrorCode::stale_status,
                       "project-backlog status precondition is stale",
                       changed->change.item_id);
      }
      if (found->status == changed->change.status) {
        return failure(ProjectBacklogErrorCode::invalid_transition,
                       "project-backlog status must change",
                       changed->change.item_id);
      }
      found->status = changed->change.status;
      found->status_event_id = event.metadata.event_id;
      found->status_reason = changed->change.reason;
    }

    next.m_event_ids.insert({session_id, event.metadata.event_id});
    next.m_last_sequences.insert_or_assign(session_id, event.metadata.sequence);
    *this = std::move(next);
    return {};
  } catch (...) {
    return failure(ProjectBacklogErrorCode::internal_failure,
                   "project-backlog projection failed internally");
  }
}

auto ProjectBacklogProjection::rebuild(
    RepositoryId repository_id,
    const std::span<const ProjectBacklogSessionEvents> histories,
    const ProjectBacklogLimits& limits)
    -> std::expected<ProjectBacklogProjection, ProjectBacklogError> {
  ProjectBacklogProjection result{std::move(repository_id)};
  for (const auto& history : histories) {
    for (const auto& event : history.events) {
      auto applied = result.apply(history.session_id, event, limits);
      if (!applied) return std::unexpected(std::move(applied.error()));
    }
  }
  return result;
}

auto ProjectBacklogProjection::find(const ProjectBacklogItemId& item_id)
    const noexcept -> const ProjectedBacklogItem* {
  const auto found = std::ranges::find(
      m_items, item_id, [](const auto& item) { return item.item.item_id; });
  return found == m_items.end() ? nullptr : &*found;
}

} // namespace aiforge::domain
