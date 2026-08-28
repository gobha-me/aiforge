#pragma once

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include <aiforge/domain/ids.hpp>

namespace aiforge::domain {

enum class MemoryScope { global, project, unknown };
enum class MemoryKind {
  user_preference,
  project_convention,
  workflow,
  reusable_fact,
  unknown,
};
enum class MemoryCaptureMode { off, review, automatic };
enum class MemoryDecisionSource { user, policy };
enum class MemoryPolicyAction { stage, accept, reject };

struct MemoryProducer {
  ModelId model_id;
  std::string runtime_name;
  std::string runtime_version;
  auto operator==(const MemoryProducer&) const -> bool = default;
};

struct MemorySource {
  SessionId session_id;
  RunId run_id;
  InvocationId invocation_id;
  std::vector<EventId> event_ids;
  auto operator==(const MemorySource&) const -> bool = default;
};

struct MemoryProposal {
  MemoryProposalId proposal_id;
  MemoryRecordId record_id;
  MemoryScope scope{MemoryScope::global};
  std::optional<RepositoryId> repository_id;
  MemoryKind kind{MemoryKind::user_preference};
  std::string content;
  std::string rationale;
  std::string evidence_excerpt;
  MemorySource source;
  MemoryProducer producer;
  std::optional<MemoryRecordId> replacement_record_id;
  std::vector<MemoryRecordId> overlap_record_ids;
  auto operator==(const MemoryProposal&) const -> bool = default;
};

struct MemoryPolicyEvaluation {
  MemoryProposalId proposal_id;
  MemoryPolicyAction action{MemoryPolicyAction::stage};
  MemoryDecisionSource source{MemoryDecisionSource::policy};
  std::string reason;
  EventId expected_proposal_event_id;
  auto operator==(const MemoryPolicyEvaluation&) const -> bool = default;
};

struct MemoryRecord {
  MemoryRecordId record_id;
  MemoryProposalId proposal_id;
  MemoryScope scope{MemoryScope::global};
  std::optional<RepositoryId> repository_id;
  MemoryKind kind{MemoryKind::user_preference};
  std::string content;
  std::string rationale;
  MemorySource source;
  MemoryProducer producer;
  auto operator==(const MemoryRecord&) const -> bool = default;
};

struct MemoryAcceptance {
  MemoryRecord record;
  MemoryDecisionSource source{MemoryDecisionSource::user};
  EventId expected_proposal_event_id;
  auto operator==(const MemoryAcceptance&) const -> bool = default;
};

struct MemoryEditedAcceptance {
  MemoryAcceptance acceptance;
  MemoryRecordId replaced_record_id;
  EventId expected_record_event_id;
  auto operator==(const MemoryEditedAcceptance&) const -> bool = default;
};

struct MemoryRejection {
  MemoryProposalId proposal_id;
  MemoryDecisionSource source{MemoryDecisionSource::user};
  std::string reason;
  EventId expected_proposal_event_id;
  auto operator==(const MemoryRejection&) const -> bool = default;
};

struct MemorySupersession {
  MemoryRecordId record_id;
  MemoryRecordId replacement_record_id;
  MemoryDecisionSource source{MemoryDecisionSource::user};
  EventId expected_record_event_id;
  auto operator==(const MemorySupersession&) const -> bool = default;
};

struct MemoryExpiry {
  MemoryRecordId record_id;
  MemoryDecisionSource source{MemoryDecisionSource::user};
  std::string reason;
  EventId expected_record_event_id;
  auto operator==(const MemoryExpiry&) const -> bool = default;
};

struct MemoryLimits {
  std::size_t maximum_content_bytes{16U * 1024U};
  std::size_t maximum_rationale_bytes{4U * 1024U};
  std::size_t maximum_excerpt_bytes{4U * 1024U};
  std::size_t maximum_source_events{256};
  std::size_t maximum_relationships{64};
  std::size_t maximum_records{4096};
  auto operator==(const MemoryLimits&) const -> bool = default;
};

enum class MemoryErrorCode {
  invalid_limits,
  invalid_record,
  secret_rejected,
  wrong_scope,
  duplicate_identity,
  duplicate_origin,
  invalid_transition,
  stale_state,
  non_monotonic_sequence,
  duplicate_event,
  resource_exhausted,
  internal_failure,
};

struct MemoryError {
  MemoryErrorCode code{MemoryErrorCode::internal_failure};
  std::string message;
  std::optional<MemoryProposalId> proposal_id;
  std::optional<MemoryRecordId> record_id;
  auto operator==(const MemoryError&) const -> bool = default;
};

[[nodiscard]] auto validate_memory_proposal(const MemoryProposal& proposal,
                                            const MemoryLimits& limits = {})
    -> std::expected<void, MemoryError>;
[[nodiscard]] auto validate_memory_record(const MemoryRecord& record,
                                          const MemoryLimits& limits = {})
    -> std::expected<void, MemoryError>;
[[nodiscard]] auto memory_text_is_safe(std::string_view value) -> bool;
[[nodiscard]] auto memory_text_looks_secret(std::string_view value) -> bool;

} // namespace aiforge::domain
