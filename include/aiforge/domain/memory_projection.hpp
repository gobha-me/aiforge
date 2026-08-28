#pragma once

#include <map>
#include <set>
#include <span>
#include <vector>

#include <aiforge/domain/events.hpp>

namespace aiforge::domain {

enum class ProjectedMemoryProposalState { pending, accepted, rejected };
enum class ProjectedMemoryRecordState { current, superseded, expired };

struct ProjectedMemoryProposal {
  MemoryProposal proposal;
  EventId proposal_event_id;
  EventTimestamp proposed_at;
  ProjectedMemoryProposalState state{ProjectedMemoryProposalState::pending};
  std::optional<EventTimestamp> state_changed_at;
  std::optional<MemoryPolicyEvaluation> policy;
  std::optional<EventId> decision_event_id;
  auto operator==(const ProjectedMemoryProposal&) const -> bool = default;
};

struct ProjectedMemoryRecord {
  MemoryRecord record;
  EventId record_event_id;
  EventTimestamp accepted_at;
  ProjectedMemoryRecordState state{ProjectedMemoryRecordState::current};
  std::optional<EventTimestamp> state_changed_at;
  std::optional<MemoryRecordId> replacement_record_id;
  std::optional<std::string> expiry_reason;
  auto operator==(const ProjectedMemoryRecord&) const -> bool = default;
};

class MemoryProjection final {
 public:
  [[nodiscard]] auto apply(const RunEvent& event,
                           const MemoryLimits& limits = {})
      -> std::expected<void, MemoryError>;
  [[nodiscard]] static auto rebuild(std::span<const RunEvent> events,
                                    const MemoryLimits& limits = {})
      -> std::expected<MemoryProjection, MemoryError>;

  [[nodiscard]] auto proposals() const noexcept
      -> const std::vector<ProjectedMemoryProposal>& {
    return m_proposals;
  }
  [[nodiscard]] auto records() const noexcept
      -> const std::vector<ProjectedMemoryRecord>& {
    return m_records;
  }
  [[nodiscard]] auto find_proposal(const MemoryProposalId& id) const noexcept
      -> const ProjectedMemoryProposal*;
  [[nodiscard]] auto find_record(const MemoryRecordId& id) const noexcept
      -> const ProjectedMemoryRecord*;

 private:
  std::vector<ProjectedMemoryProposal> m_proposals;
  std::vector<ProjectedMemoryRecord> m_records;
  std::set<EventId> m_event_ids;
  std::uint64_t m_last_sequence{};
};

} // namespace aiforge::domain
