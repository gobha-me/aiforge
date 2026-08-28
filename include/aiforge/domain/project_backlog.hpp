#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include <aiforge/domain/plan.hpp>

namespace aiforge::domain {

enum class ProjectBacklogDecisionSource {
  user,
  policy,
};

struct ProjectBacklogOrigin {
  SessionId session_id;
  PlanId plan_id;
  PlanRevisionId revision_id;
  PlanTaskId task_id;
  auto operator==(const ProjectBacklogOrigin&) const -> bool = default;
};

struct ProjectBacklogItem {
  ProjectBacklogItemId item_id;
  RepositoryId repository_id;
  ProjectBacklogOrigin origin;
  PlanTask task;
  ProjectBacklogDecisionSource source{ProjectBacklogDecisionSource::user};
  auto operator==(const ProjectBacklogItem&) const -> bool = default;
};

enum class ProjectBacklogItemStatus {
  open,
  resolved,
};

struct ProjectBacklogStatusChange {
  ProjectBacklogItemId item_id;
  RepositoryId repository_id;
  ProjectBacklogItemStatus status{ProjectBacklogItemStatus::open};
  ProjectBacklogDecisionSource source{ProjectBacklogDecisionSource::user};
  std::optional<std::string> reason;
  std::optional<EventId> expected_status_event_id;
  auto operator==(const ProjectBacklogStatusChange&) const -> bool = default;
};

struct ProjectedBacklogItem {
  ProjectBacklogItem item;
  EventId promotion_event_id;
  ProjectBacklogItemStatus status{ProjectBacklogItemStatus::open};
  EventId status_event_id;
  std::optional<std::string> status_reason;
  auto operator==(const ProjectedBacklogItem&) const -> bool = default;
};

struct ProjectBacklogLimits {
  std::size_t maximum_items{4096};
  std::size_t maximum_reason_bytes{16U * 1024U};
  auto operator==(const ProjectBacklogLimits&) const -> bool = default;
};

enum class ProjectBacklogErrorCode {
  invalid_limits,
  invalid_item,
  invalid_transition,
  wrong_repository,
  duplicate_identity,
  duplicate_origin,
  stale_status,
  non_monotonic_sequence,
  duplicate_event,
  resource_exhausted,
  internal_failure,
};

struct ProjectBacklogError {
  ProjectBacklogErrorCode code{ProjectBacklogErrorCode::internal_failure};
  std::string message;
  std::optional<ProjectBacklogItemId> item_id;
  auto operator==(const ProjectBacklogError&) const -> bool = default;
};

[[nodiscard]] auto validate_project_backlog_item(
    const ProjectBacklogItem& item, const ProjectBacklogLimits& limits = {})
    -> std::expected<void, ProjectBacklogError>;

[[nodiscard]] auto validate_project_backlog_status_change(
    const ProjectBacklogStatusChange& change,
    const ProjectBacklogLimits& limits = {})
    -> std::expected<void, ProjectBacklogError>;

} // namespace aiforge::domain
