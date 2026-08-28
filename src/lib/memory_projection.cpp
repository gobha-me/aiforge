#include <aiforge/domain/memory_projection.hpp>

#include <algorithm>
#include <ranges>

namespace aiforge::domain {
namespace {

[[nodiscard]] auto failure(
    const MemoryErrorCode code, std::string message,
    std::optional<MemoryProposalId> proposal_id = std::nullopt,
    std::optional<MemoryRecordId> record_id = std::nullopt)
    -> std::unexpected<MemoryError> {
  return std::unexpected(MemoryError{
      code, std::move(message), std::move(proposal_id), std::move(record_id)});
}

[[nodiscard]] auto record_matches_proposal(const MemoryRecord& record,
                                           const MemoryProposal& proposal,
                                           const bool allow_edited_content)
    -> bool {
  return record.record_id == proposal.record_id &&
         record.proposal_id == proposal.proposal_id &&
         record.scope == proposal.scope &&
         record.repository_id == proposal.repository_id &&
         record.kind == proposal.kind &&
         (allow_edited_content || record.content == proposal.content) &&
         record.rationale == proposal.rationale &&
         record.source == proposal.source &&
         record.producer == proposal.producer;
}

} // namespace

auto MemoryProjection::find_proposal(const MemoryProposalId& id) const noexcept
    -> const ProjectedMemoryProposal* {
  const auto found = std::ranges::find(m_proposals, id, [](const auto& value) {
    return value.proposal.proposal_id;
  });
  return found == m_proposals.end() ? nullptr : &*found;
}

auto MemoryProjection::find_record(const MemoryRecordId& id) const noexcept
    -> const ProjectedMemoryRecord* {
  const auto found = std::ranges::find(
      m_records, id, [](const auto& value) { return value.record.record_id; });
  return found == m_records.end() ? nullptr : &*found;
}

auto MemoryProjection::apply(const RunEvent& event, const MemoryLimits& limits)
    -> std::expected<void, MemoryError> {
  try {
    if (event.metadata.sequence == 0 ||
        event.metadata.sequence <= m_last_sequence) {
      return failure(MemoryErrorCode::non_monotonic_sequence,
                     "memory event sequence does not increase");
    }
    if (m_event_ids.contains(event.metadata.event_id)) {
      return failure(MemoryErrorCode::duplicate_event,
                     "memory event identity is duplicated");
    }
    auto next = *this;
    if (const auto* proposed = std::get_if<MemoryProposed>(&event.payload)) {
      if (auto valid = validate_memory_proposal(proposed->proposal, limits);
          !valid) {
        return std::unexpected(std::move(valid.error()));
      }
      if (next.m_proposals.size() >= limits.maximum_records) {
        return failure(MemoryErrorCode::resource_exhausted,
                       "memory proposal limit is exceeded");
      }
      if (next.find_proposal(proposed->proposal.proposal_id)) {
        return failure(MemoryErrorCode::duplicate_identity,
                       "memory proposal identity is duplicated",
                       proposed->proposal.proposal_id);
      }
      if (std::ranges::any_of(next.m_proposals, [&](const auto& value) {
            return value.proposal.source.session_id ==
                       proposed->proposal.source.session_id &&
                   value.proposal.source.invocation_id ==
                       proposed->proposal.source.invocation_id;
          })) {
        return failure(MemoryErrorCode::duplicate_origin,
                       "memory proposal origin is duplicated",
                       proposed->proposal.proposal_id);
      }
      next.m_proposals.push_back({proposed->proposal, event.metadata.event_id,
                                  event.metadata.timestamp,
                                  ProjectedMemoryProposalState::pending,
                                  std::nullopt, std::nullopt, std::nullopt});
    } else if (const auto* decided =
                   std::get_if<MemoryPolicyDecided>(&event.payload)) {
      auto found = std::ranges::find(
          next.m_proposals, decided->evaluation.proposal_id,
          [](const auto& value) { return value.proposal.proposal_id; });
      if (found == next.m_proposals.end() ||
          found->proposal_event_id !=
              decided->evaluation.expected_proposal_event_id ||
          found->policy ||
          decided->evaluation.source != MemoryDecisionSource::policy ||
          !memory_text_is_safe(decided->evaluation.reason) ||
          (decided->evaluation.action != MemoryPolicyAction::stage &&
           decided->evaluation.action != MemoryPolicyAction::accept &&
           decided->evaluation.action != MemoryPolicyAction::reject)) {
        return failure(MemoryErrorCode::stale_state,
                       "memory policy decision is stale",
                       decided->evaluation.proposal_id);
      }
      found->policy = decided->evaluation;
      found->decision_event_id = event.metadata.event_id;
    } else if (const auto* accepted =
                   std::get_if<MemoryAccepted>(&event.payload)) {
      if (auto valid =
              validate_memory_record(accepted->acceptance.record, limits);
          !valid) {
        return std::unexpected(std::move(valid.error()));
      }
      auto found = std::ranges::find(
          next.m_proposals, accepted->acceptance.record.proposal_id,
          [](const auto& value) { return value.proposal.proposal_id; });
      if (found == next.m_proposals.end() ||
          found->state != ProjectedMemoryProposalState::pending ||
          found->proposal_event_id !=
              accepted->acceptance.expected_proposal_event_id ||
          found->proposal.record_id != accepted->acceptance.record.record_id ||
          !record_matches_proposal(accepted->acceptance.record, found->proposal,
                                   false) ||
          (accepted->acceptance.source == MemoryDecisionSource::policy &&
           (!found->policy ||
            found->policy->action != MemoryPolicyAction::accept)) ||
          (accepted->acceptance.source == MemoryDecisionSource::user &&
           found->policy &&
           found->policy->action != MemoryPolicyAction::stage) ||
          next.find_record(accepted->acceptance.record.record_id)) {
        return failure(MemoryErrorCode::stale_state,
                       "memory acceptance is stale or inconsistent",
                       accepted->acceptance.record.proposal_id,
                       accepted->acceptance.record.record_id);
      }
      if (!found->proposal.overlap_record_ids.empty() ||
          found->proposal.replacement_record_id) {
        return failure(MemoryErrorCode::invalid_transition,
                       "related memory acceptance requires edited replacement",
                       found->proposal.proposal_id, found->proposal.record_id);
      }
      found->state = ProjectedMemoryProposalState::accepted;
      found->state_changed_at = event.metadata.timestamp;
      found->decision_event_id = event.metadata.event_id;
      next.m_records.push_back(
          {accepted->acceptance.record, event.metadata.event_id,
           event.metadata.timestamp, ProjectedMemoryRecordState::current,
           std::nullopt, std::nullopt, std::nullopt});
    } else if (const auto* edited =
                   std::get_if<MemoryEditedAndAccepted>(&event.payload)) {
      const auto& acceptance = edited->acceptance.acceptance;
      if (auto valid = validate_memory_record(acceptance.record, limits);
          !valid) {
        return std::unexpected(std::move(valid.error()));
      }
      auto proposal = std::ranges::find(
          next.m_proposals, acceptance.record.proposal_id,
          [](const auto& value) { return value.proposal.proposal_id; });
      auto replaced = std::ranges::find(
          next.m_records, edited->acceptance.replaced_record_id,
          [](const auto& value) { return value.record.record_id; });
      if (proposal == next.m_proposals.end() ||
          proposal->state != ProjectedMemoryProposalState::pending ||
          proposal->proposal_event_id !=
              acceptance.expected_proposal_event_id ||
          replaced == next.m_records.end() ||
          replaced->state != ProjectedMemoryRecordState::current ||
          replaced->record_event_id !=
              edited->acceptance.expected_record_event_id ||
          acceptance.source != MemoryDecisionSource::user ||
          (proposal->policy &&
           proposal->policy->action != MemoryPolicyAction::stage) ||
          proposal->proposal.replacement_record_id !=
              edited->acceptance.replaced_record_id ||
          !std::ranges::contains(proposal->proposal.overlap_record_ids,
                                 edited->acceptance.replaced_record_id) ||
          !record_matches_proposal(acceptance.record, proposal->proposal,
                                   true) ||
          next.find_record(acceptance.record.record_id)) {
        return failure(
            MemoryErrorCode::stale_state, "edited memory acceptance is stale",
            acceptance.record.proposal_id, acceptance.record.record_id);
      }
      proposal->state = ProjectedMemoryProposalState::accepted;
      proposal->state_changed_at = event.metadata.timestamp;
      proposal->decision_event_id = event.metadata.event_id;
      replaced->state = ProjectedMemoryRecordState::superseded;
      replaced->state_changed_at = event.metadata.timestamp;
      replaced->replacement_record_id = acceptance.record.record_id;
      replaced->record_event_id = event.metadata.event_id;
      next.m_records.push_back({acceptance.record, event.metadata.event_id,
                                event.metadata.timestamp,
                                ProjectedMemoryRecordState::current,
                                std::nullopt, std::nullopt, std::nullopt});
    } else if (const auto* rejected =
                   std::get_if<MemoryRejected>(&event.payload)) {
      auto found = std::ranges::find(
          next.m_proposals, rejected->rejection.proposal_id,
          [](const auto& value) { return value.proposal.proposal_id; });
      if (found == next.m_proposals.end() ||
          found->state != ProjectedMemoryProposalState::pending ||
          found->proposal_event_id !=
              rejected->rejection.expected_proposal_event_id ||
          (rejected->rejection.source == MemoryDecisionSource::policy &&
           (!found->policy ||
            found->policy->action != MemoryPolicyAction::reject)) ||
          (rejected->rejection.source == MemoryDecisionSource::user &&
           found->policy &&
           found->policy->action != MemoryPolicyAction::stage) ||
          !memory_text_is_safe(rejected->rejection.reason)) {
        return failure(MemoryErrorCode::stale_state,
                       "memory rejection is stale or invalid",
                       rejected->rejection.proposal_id);
      }
      found->state = ProjectedMemoryProposalState::rejected;
      found->state_changed_at = event.metadata.timestamp;
      found->decision_event_id = event.metadata.event_id;
    } else if (const auto* superseded =
                   std::get_if<MemorySuperseded>(&event.payload)) {
      auto found = std::ranges::find(
          next.m_records, superseded->supersession.record_id,
          [](const auto& value) { return value.record.record_id; });
      if (found == next.m_records.end() ||
          found->state != ProjectedMemoryRecordState::current ||
          found->record_event_id !=
              superseded->supersession.expected_record_event_id ||
          !next.find_record(superseded->supersession.replacement_record_id)) {
        return failure(MemoryErrorCode::stale_state,
                       "memory supersession is stale", std::nullopt,
                       superseded->supersession.record_id);
      }
      found->state = ProjectedMemoryRecordState::superseded;
      found->state_changed_at = event.metadata.timestamp;
      found->replacement_record_id =
          superseded->supersession.replacement_record_id;
      found->record_event_id = event.metadata.event_id;
    } else if (const auto* expired =
                   std::get_if<MemoryExpired>(&event.payload)) {
      auto found = std::ranges::find(
          next.m_records, expired->expiry.record_id,
          [](const auto& value) { return value.record.record_id; });
      if (found == next.m_records.end() ||
          found->state != ProjectedMemoryRecordState::current ||
          found->record_event_id != expired->expiry.expected_record_event_id ||
          !memory_text_is_safe(expired->expiry.reason)) {
        return failure(MemoryErrorCode::stale_state,
                       "memory expiry is stale or invalid", std::nullopt,
                       expired->expiry.record_id);
      }
      found->state = ProjectedMemoryRecordState::expired;
      found->state_changed_at = event.metadata.timestamp;
      found->expiry_reason = expired->expiry.reason;
      found->record_event_id = event.metadata.event_id;
    }
    next.m_event_ids.insert(event.metadata.event_id);
    next.m_last_sequence = event.metadata.sequence;
    *this = std::move(next);
    return {};
  } catch (...) {
    return failure(MemoryErrorCode::internal_failure,
                   "memory projection failed internally");
  }
}

auto MemoryProjection::rebuild(const std::span<const RunEvent> events,
                               const MemoryLimits& limits)
    -> std::expected<MemoryProjection, MemoryError> {
  MemoryProjection result;
  for (const auto& event : events) {
    auto applied = result.apply(event, limits);
    if (!applied) return std::unexpected(std::move(applied.error()));
  }
  return result;
}

} // namespace aiforge::domain
