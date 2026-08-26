#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include <aiforge/domain/events.hpp>

namespace aiforge::domain {

struct PlanGraphLimits {
  std::size_t maximum_tasks{256};
  std::size_t maximum_dependencies_per_task{64};
  std::size_t maximum_acceptance_criteria_per_task{32};
  std::size_t maximum_resource_intents_per_task{32};
  std::size_t maximum_text_bytes{16U * 1024U};
  std::size_t maximum_metadata_bytes{4096};
  std::size_t maximum_total_text_bytes{1024U * 1024U};
  auto operator==(const PlanGraphLimits &) const -> bool = default;
};

enum class PlanGraphErrorCode {
  invalid_limits,
  invalid_plan,
  invalid_task,
  duplicate_identity,
  unknown_reference,
  cyclic_graph,
  resource_exhausted,
  invalid_decision,
  wrong_plan,
  wrong_revision,
  invalid_transition,
  invalid_envelope,
  non_monotonic_sequence,
  duplicate_event,
  internal_failure,
};

struct PlanGraphError {
  PlanGraphErrorCode code{PlanGraphErrorCode::internal_failure};
  std::string message;
  std::optional<PlanId> plan_id;
  std::optional<PlanRevisionId> revision_id;
  std::optional<PlanTaskId> task_id;
  auto operator==(const PlanGraphError &) const -> bool = default;
};

enum class PlanGraphState {
  not_started,
  proposed,
  revision_requested,
  approved,
  rejected,
};

struct ProjectedPlanRevision {
  PlanRevision revision;
  std::optional<EventId> proposal_event_id;
  std::optional<EventId> decision_event_id;
  std::optional<PlanRevisionDecision> decision;
  auto operator==(const ProjectedPlanRevision &) const -> bool = default;
};

[[nodiscard]] auto validate_plan_revision(const PlanRevision &revision,
                                          const PlanGraphLimits &limits = {})
    -> std::expected<void, PlanGraphError>;

[[nodiscard]] auto validate_plan_decision(const PlanRevisionDecision &decision,
                                          const PlanGraphLimits &limits = {})
    -> std::expected<void, PlanGraphError>;

class PlanGraphProjection final {
public:
  [[nodiscard]] auto apply(const RunEvent &event,
                           const PlanGraphLimits &limits = {})
      -> std::expected<void, PlanGraphError>;

  [[nodiscard]] static auto rebuild(std::span<const RunEvent> events,
                                    const PlanGraphLimits &limits = {})
      -> std::expected<PlanGraphProjection, PlanGraphError>;

  [[nodiscard]] auto plan_id() const noexcept -> const std::optional<PlanId> & {
    return m_plan_id;
  }
  [[nodiscard]] auto revisions() const noexcept
      -> const std::vector<ProjectedPlanRevision> & {
    return m_revisions;
  }
  [[nodiscard]] auto current_revision() const noexcept
      -> const ProjectedPlanRevision *;
  [[nodiscard]] auto state() const noexcept -> PlanGraphState;
  [[nodiscard]] auto last_sequence() const noexcept -> std::uint64_t {
    return m_last_sequence;
  }

private:
  [[nodiscard]] auto apply_in_place(const RunEvent &event,
                                    const PlanGraphLimits &limits)
      -> std::expected<void, PlanGraphError>;

  std::optional<PlanId> m_plan_id;
  std::vector<ProjectedPlanRevision> m_revisions;
  std::set<EventId> m_event_ids;
  std::uint64_t m_last_sequence{};
};

} // namespace aiforge::domain
