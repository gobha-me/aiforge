#include <aiforge/domain/memory.hpp>

#include <algorithm>
#include <cctype>
#include <ranges>
#include <set>
#include <string_view>

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

[[nodiscard]] auto valid_limits(const MemoryLimits& limits) -> bool {
  return limits.maximum_content_bytes != 0 &&
         limits.maximum_rationale_bytes != 0 &&
         limits.maximum_excerpt_bytes != 0 &&
         limits.maximum_source_events != 0 &&
         limits.maximum_relationships != 0 && limits.maximum_records != 0;
}

[[nodiscard]] auto lower_ascii(std::string value) -> std::string {
  std::ranges::transform(value, value.begin(), [](const unsigned char value) {
    return static_cast<char>(std::tolower(value));
  });
  return value;
}

[[nodiscard]] auto shaped_like_secret(const std::string_view value) -> bool {
  const auto lower = lower_ascii(std::string{value});
  if (lower.contains("authorization:") || lower.contains("api_key=") ||
      lower.contains("api-key=") || lower.contains("access_token=") ||
      lower.contains("private key-----")) {
    return true;
  }
  const auto sk = lower.find("sk-");
  if (sk != std::string::npos && lower.size() - sk >= 12) return true;
  const auto bearer = lower.find("bearer ");
  return bearer != std::string::npos && lower.size() - bearer >= 16;
}

[[nodiscard]] auto valid_scope(const MemoryScope scope,
                               const std::optional<RepositoryId>& repository)
    -> bool {
  return (scope == MemoryScope::global && !repository) ||
         (scope == MemoryScope::project && repository);
}

[[nodiscard]] auto valid_producer(const MemoryProducer& producer) -> bool {
  return !producer.runtime_name.empty() &&
         producer.runtime_name.size() <= 128 &&
         !producer.runtime_version.empty() &&
         producer.runtime_version.size() <= 128 &&
         memory_text_is_safe(producer.runtime_name) &&
         memory_text_is_safe(producer.runtime_version);
}

[[nodiscard]] auto valid_source(const MemorySource& source,
                                const MemoryLimits& limits) -> bool {
  if (source.event_ids.empty() ||
      source.event_ids.size() > limits.maximum_source_events) {
    return false;
  }
  return std::set<EventId>{source.event_ids.begin(), source.event_ids.end()}
             .size() == source.event_ids.size();
}

} // namespace

auto memory_text_is_safe(const std::string_view value) -> bool {
  if (value.empty()) return false;
  return std::ranges::none_of(value, [](const unsigned char character) {
    return character == 0 || character == 0x1BU || character == 0x7FU ||
           (character < 0x20U && character != '\n' && character != '\t');
  });
}

auto memory_text_looks_secret(const std::string_view value) -> bool {
  return shaped_like_secret(value);
}

auto validate_memory_proposal(const MemoryProposal& proposal,
                              const MemoryLimits& limits)
    -> std::expected<void, MemoryError> {
  if (!valid_limits(limits)) {
    return failure(MemoryErrorCode::invalid_limits,
                   "memory limits must be positive");
  }
  if (!valid_scope(proposal.scope, proposal.repository_id)) {
    return failure(MemoryErrorCode::wrong_scope,
                   "memory scope and repository identity disagree",
                   proposal.proposal_id, proposal.record_id);
  }
  if (proposal.content.size() > limits.maximum_content_bytes ||
      proposal.rationale.size() > limits.maximum_rationale_bytes ||
      proposal.evidence_excerpt.size() > limits.maximum_excerpt_bytes ||
      !memory_text_is_safe(proposal.content) ||
      !memory_text_is_safe(proposal.rationale) ||
      !memory_text_is_safe(proposal.evidence_excerpt) ||
      !valid_source(proposal.source, limits) ||
      !valid_producer(proposal.producer) ||
      proposal.overlap_record_ids.size() > limits.maximum_relationships) {
    return failure(MemoryErrorCode::invalid_record,
                   "memory proposal is malformed or exceeds its limits",
                   proposal.proposal_id, proposal.record_id);
  }
  if (shaped_like_secret(proposal.content) ||
      shaped_like_secret(proposal.rationale) ||
      shaped_like_secret(proposal.evidence_excerpt)) {
    return failure(MemoryErrorCode::secret_rejected,
                   "memory proposal contains credential-shaped content",
                   proposal.proposal_id, proposal.record_id);
  }
  std::set<MemoryRecordId> relationships{proposal.overlap_record_ids.begin(),
                                         proposal.overlap_record_ids.end()};
  if (relationships.size() != proposal.overlap_record_ids.size() ||
      relationships.contains(proposal.record_id) ||
      (proposal.replacement_record_id &&
       (*proposal.replacement_record_id == proposal.record_id ||
        !relationships.contains(*proposal.replacement_record_id)))) {
    return failure(MemoryErrorCode::invalid_record,
                   "memory proposal relationships are inconsistent",
                   proposal.proposal_id, proposal.record_id);
  }
  return {};
}

auto validate_memory_record(const MemoryRecord& record,
                            const MemoryLimits& limits)
    -> std::expected<void, MemoryError> {
  MemoryProposal proposal{record.proposal_id, record.record_id,
                          record.scope,       record.repository_id,
                          record.kind,        record.content,
                          record.rationale,   "accepted-memory-source",
                          record.source,      record.producer,
                          std::nullopt,       {}};
  return validate_memory_proposal(proposal, limits);
}

} // namespace aiforge::domain
