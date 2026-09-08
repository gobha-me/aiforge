#pragma once
#include <aiforge/domain/conversation_admission.hpp>
#include <aiforge/domain/conversation_summary_reference.hpp>
#include <span>

namespace aiforge::domain {
inline constexpr std::size_t summary_maximum_groups = 128;
inline constexpr std::size_t summary_maximum_entries = 4096;
inline constexpr std::size_t summary_maximum_manifest_bytes =
    std::size_t{2} * 1024 * 1024;
inline constexpr std::size_t summary_maximum_source_bytes =
    std::size_t{16} * 1024 * 1024;
inline constexpr std::size_t summary_maximum_text_bytes =
    std::size_t{64} * 1024;

struct ConversationSummarySourceGroup {
  RunId run_id;
  EventId terminal_event_id;
  std::uint64_t terminal_sequence{};
  std::vector<ConversationAdmittedEntry> entries;
  auto operator==(const ConversationSummarySourceGroup&) const
      -> bool = default;
};
// Policy-independent exact requested coverage, not a rolling admission. The
// runtime resolves every source run's completion and all eligible messages;
// shape/seal validation alone cannot prove a referenced event's actual type.
// Version1 accepts original source runs only, with no summary ancestry.
struct ConversationSummarySources {
  std::uint32_t version{1};
  std::uint32_t estimator_version{1};
  SessionId session_id;
  std::uint64_t snapshot_sequence{};
  std::vector<ConversationSummarySourceGroup> groups;
  std::optional<ContentDigest> source_digest;
  auto operator==(const ConversationSummarySources&) const -> bool = default;
};

struct ConversationSummaryIntent {
  std::uint32_t version{1};
  std::uint32_t format_version{1};
  ConversationSummaryId summary_id;
  ConversationSummarySources sources;
  RunId producing_run_id;
  InferenceId producing_inference_id;
  ModelId model_id;
  MessageId output_message_id;
  std::string runtime_version;
  ContextCapacity capacity;
  // Source plus runtime task estimates; external reservation is separate.
  std::uint64_t estimated_input_tokens{};
  std::size_t maximum_output_bytes{summary_maximum_text_bytes};
  std::optional<ContentDigest> intent_digest;
  auto operator==(const ConversationSummaryIntent&) const -> bool = default;
};

enum class ConversationSummaryAuthor { model, user_edit };
struct ConversationSummaryCandidate {
  std::uint32_t version{1};
  SessionId session_id;
  ConversationSummaryId summary_id;
  std::uint64_t revision{1};
  ContentDigest source_digest;
  ContentDigest intent_digest;
  // Edits retain original model output provenance, but are attributed to the
  // user and link the exact prior immutable candidate version.
  EventId output_event_id;
  std::uint64_t output_sequence{};
  EventId created_event_id;
  std::uint64_t created_sequence{};
  ConversationSummaryAuthor author{ConversationSummaryAuthor::model};
  std::optional<ConversationSummaryVersion> edited_from;
  std::string text;
  std::optional<ContentDigest> candidate_digest;
  auto operator==(const ConversationSummaryCandidate&) const -> bool = default;
};

struct ConversationSummaryActivation {
  std::uint32_t version{1};
  SessionId session_id;
  ConversationSummaryVersion candidate;
  ContentDigest source_digest;
  std::vector<RunId> covered_run_ids;
  std::uint64_t source_anchor_sequence{};
  EventId activation_event_id;
  std::uint64_t activation_sequence{};
  std::optional<ContentDigest> activation_digest;
  auto operator==(const ConversationSummaryActivation&) const -> bool = default;
};

enum class ConversationSummaryErrorCode {
  unsupported_version,
  invalid_source,
  invalid_intent,
  invalid_candidate,
  invalid_activation,
  invalid_digest,
  foreign_scope,
  stale_revision,
  overlap,
  resource_exhausted,
  token_overflow,
  capacity_exceeded,
  internal_failure,
};
struct ConversationSummaryError {
  ConversationSummaryErrorCode code;
  std::string message;
  auto operator==(const ConversationSummaryError&) const -> bool = default;
};
using ConversationSummaryStatus = std::expected<void, ConversationSummaryError>;

[[nodiscard]] auto seal_conversation_summary_sources(
    ConversationSummarySources&) -> ConversationSummaryStatus;
[[nodiscard]] auto validate_conversation_summary_sources(
    const ConversationSummarySources&) -> ConversationSummaryStatus;
[[nodiscard]] auto seal_conversation_summary_intent(ConversationSummaryIntent&)
    -> ConversationSummaryStatus;
[[nodiscard]] auto validate_conversation_summary_intent(
    const ConversationSummaryIntent&) -> ConversationSummaryStatus;
// Supply the exact previous candidate for an edit; generation has no parent.
[[nodiscard]] auto seal_conversation_summary_candidate(
    ConversationSummaryCandidate&, const ConversationSummaryIntent&,
    const ConversationSummaryCandidate* previous = nullptr)
    -> ConversationSummaryStatus;
[[nodiscard]] auto validate_conversation_summary_candidate(
    const ConversationSummaryCandidate&, const ConversationSummaryIntent&,
    const ConversationSummaryCandidate* previous = nullptr)
    -> ConversationSummaryStatus;
[[nodiscard]] auto seal_conversation_summary_activation(
    ConversationSummaryActivation&, const ConversationSummaryCandidate&,
    const ConversationSummaryIntent&,
    const ConversationSummaryCandidate* previous = nullptr)
    -> ConversationSummaryStatus;
[[nodiscard]] auto validate_conversation_summary_activation(
    const ConversationSummaryActivation&) -> ConversationSummaryStatus;
// Existing activation references must already have been checked against their
// candidate and source when created. Replacement removes only exact versions,
// then validates scope, order and disjoint coverage without mutating inputs.
[[nodiscard]] auto validate_conversation_summary_replacement(
    std::span<const ConversationSummaryActivation> active,
    std::span<const ConversationSummaryVersion> replaced,
    const ConversationSummaryActivation& replacement)
    -> ConversationSummaryStatus;
} // namespace aiforge::domain
