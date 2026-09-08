#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include <aiforge/domain/context.hpp>
#include <aiforge/domain/conversation_summary_reference.hpp>
#include <aiforge/domain/digest.hpp>

namespace aiforge::domain {

inline constexpr std::size_t conversation_maximum_groups = 4096;
inline constexpr std::size_t conversation_maximum_entries = 16384;
inline constexpr std::size_t conversation_maximum_pins = 4096;
inline constexpr std::size_t conversation_maximum_provenance_bytes = 4096;
inline constexpr std::size_t conversation_maximum_manifest_bytes =
    std::size_t{8} * 1024 * 1024;
inline constexpr std::size_t conversation_maximum_message_bytes =
    std::size_t{16} * 1024 * 1024;
inline constexpr std::size_t conversation_maximum_message_items = 65536;

enum class ConversationMode { full, rolling };

struct ConversationPolicy {
  std::uint64_t revision{};
  ConversationMode mode{ConversationMode::full};
  std::vector<RunId> pinned_run_ids;
  auto operator==(const ConversationPolicy&) const -> bool = default;
};

struct ConversationAdmittedEntry {
  EventId completed_event_id;
  std::uint64_t event_sequence{};
  ContextEntryId entry_id;
  MessageId message_id;
  ContextProvenance provenance;
  std::uint64_t order{};
  std::uint64_t estimated_tokens{};
  ContextContentKind kind{ContextContentKind::conversation};
  ContentDigest message_digest;
  auto operator==(const ConversationAdmittedEntry&) const -> bool = default;
};

struct ConversationAdmittedGroup {
  RunId run_id;
  std::vector<ConversationAdmittedEntry> entries;
  bool pinned{};
  auto operator==(const ConversationAdmittedGroup&) const -> bool = default;
};

// A derived evidence entry bound to one immutable candidate and activation.
// The runtime resolves the source text and enforces evidence role/kind, with
// no tools or instruction layer. A later activation is a different identity.
struct ConversationAdmittedSummary {
  ConversationSummaryVersion candidate;
  EventId activation_event_id;
  std::uint64_t activation_sequence{};
  std::uint64_t source_anchor_sequence{};
  ContextEntryId entry_id;
  MessageId message_id;
  ContextProvenance provenance;
  std::uint64_t order{};
  std::uint64_t estimated_tokens{};
  ContentDigest message_digest;
  auto operator==(const ConversationAdmittedSummary&) const -> bool = default;
};

// Source event references are scoped to session_id. No source text is copied.
// A missing policy event with revision zero means the implicit full policy.
// An explicit empty admission is valid; absence is represented by its caller.
struct ConversationAdmission {
  std::uint32_t version{1};
  // Governs reconstructed history and active tool messages. Required input
  // and scoped memory retain the estimates supplied by their owning sources.
  std::uint32_t estimator_version{1};
  SessionId session_id;
  ModelId model_id;
  std::uint64_t source_snapshot_sequence{};
  std::optional<EventId> policy_event_id;
  std::uint64_t policy_revision{};
  ConversationMode mode{ConversationMode::full};
  ContextCapacity capacity;
  // Sum of supplied required-input and memory estimates, including admitted
  // summary evidence in v2, excluding original history and external input
  // reservation. Summary estimates are counted once, not added again.
  std::uint64_t mandatory_input_tokens{};
  std::vector<ConversationAdmittedGroup> groups;
  std::uint64_t omitted_group_count{};
  // Bounded summary of omitted source identities; never an unbounded ID list.
  std::optional<ContentDigest> omitted_groups_digest;
  std::optional<ContentDigest> admission_digest;
  // V1 forbids summaries and retains its original encoding. V2 explicitly
  // records even an empty summary selection; full mode requires it empty.
  std::vector<ConversationAdmittedSummary> summaries{};
  auto operator==(const ConversationAdmission&) const -> bool = default;
};

enum class ConversationAdmissionErrorCode {
  invalid_policy,
  invalid_admission,
  invalid_message,
  unsupported_version,
  foreign_scope,
  resource_exhausted,
  token_overflow,
  capacity_exceeded,
  invalid_digest,
  internal_failure,
};

struct ConversationAdmissionError {
  ConversationAdmissionErrorCode code;
  std::string message;
  auto operator==(const ConversationAdmissionError&) const -> bool = default;
};

[[nodiscard]] auto validate_conversation_policy(
    const ConversationPolicy& policy)
    -> std::expected<void, ConversationAdmissionError>;
[[nodiscard]] auto seal_conversation_admission(ConversationAdmission& admission)
    -> std::expected<void, ConversationAdmissionError>;
[[nodiscard]] auto validate_conversation_admission(
    const ConversationAdmission& admission)
    -> std::expected<void, ConversationAdmissionError>;
[[nodiscard]] auto validate_conversation_admission(
    const ConversationAdmission& admission, const SessionId& expected_session)
    -> std::expected<void, ConversationAdmissionError>;

// Hashes all neutral message fields with versioned length-prefix encoding.
// byte_size is canonical encoding size, not a model token estimate. Artifacts
// must first be normalized to metadata; unknown and raw artifact blocks fail.
[[nodiscard]] auto normalized_conversation_message_digest(
    const Message& message, std::uint32_t version = 1)
    -> std::expected<ContentDigest, ConversationAdmissionError>;

} // namespace aiforge::domain
