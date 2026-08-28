#pragma once

#include <map>
#include <set>
#include <span>
#include <utility>
#include <vector>

#include <aiforge/domain/events.hpp>

namespace aiforge::domain {

struct ProjectBacklogSessionEvents {
  SessionId session_id;
  std::vector<RunEvent> events;
  auto operator==(const ProjectBacklogSessionEvents&) const -> bool = default;
};

class ProjectBacklogProjection final {
 public:
  explicit ProjectBacklogProjection(RepositoryId repository_id)
      : m_repository_id(std::move(repository_id)) {}

  [[nodiscard]] auto apply(const SessionId& session_id, const RunEvent& event,
                           const ProjectBacklogLimits& limits = {})
      -> std::expected<void, ProjectBacklogError>;
  [[nodiscard]] static auto rebuild(
      RepositoryId repository_id,
      std::span<const ProjectBacklogSessionEvents> histories,
      const ProjectBacklogLimits& limits = {})
      -> std::expected<ProjectBacklogProjection, ProjectBacklogError>;

  [[nodiscard]] auto repository_id() const noexcept -> const RepositoryId& {
    return m_repository_id;
  }
  [[nodiscard]] auto items() const noexcept
      -> const std::vector<ProjectedBacklogItem>& {
    return m_items;
  }
  [[nodiscard]] auto find(const ProjectBacklogItemId& item_id) const noexcept
      -> const ProjectedBacklogItem*;

 private:
  RepositoryId m_repository_id;
  std::vector<ProjectedBacklogItem> m_items;
  std::set<std::pair<SessionId, EventId>> m_event_ids;
  std::map<SessionId, std::uint64_t> m_last_sequences;
};

} // namespace aiforge::domain
