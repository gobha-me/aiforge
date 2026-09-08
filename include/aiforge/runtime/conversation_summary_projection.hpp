#pragma once

#include <aiforge/domain/event_log.hpp>
#include <optional>

namespace aiforge::runtime {

inline constexpr std::size_t summary_maximum_intents = 256;
inline constexpr std::size_t summary_maximum_candidates = 1024;
inline constexpr std::size_t summary_maximum_retained_text_bytes =
    8U * 1024U * 1024U;

// A recoverable draft has no publication identity. Only the kernel's committed
// publication supplies created_event_id/sequence and seals a candidate.
struct ConversationSummaryDraft {
  domain::ConversationSummaryIntent intent;
  domain::EventId output_event_id;
  std::uint64_t output_sequence{};
  std::string text;
  auto operator==(const ConversationSummaryDraft&) const -> bool = default;
};
struct ConversationSummarySnapshot {
  std::uint64_t snapshot_sequence{};
  std::uint64_t policy_revision{};
  std::vector<domain::ConversationSummaryIntent> intents;
  std::vector<domain::ConversationSummaryCandidate> candidates;
  std::vector<domain::ConversationSummaryActivation> active;
};

// Pure bounded replay. Unsupported summary schemas, partial transactions and
// conflicting publication/activation histories fail closed. No external work.
[[nodiscard]] auto recorded_conversation_summaries(
    const domain::SessionEventLog& log,
    std::optional<std::uint64_t> snapshot_sequence = std::nullopt)
    -> std::expected<ConversationSummarySnapshot,
                     domain::ConversationSummaryError>;

// Resolves one completed producer without publishing or regenerating. The
// supplied intent must already have been resolved from the durable snapshot.
[[nodiscard]] auto recover_conversation_summary_draft(
    const domain::SessionEventLog& log,
    const domain::ConversationSummaryIntent& intent,
    std::optional<std::uint64_t> snapshot_sequence = std::nullopt)
    -> std::expected<ConversationSummaryDraft,
                     domain::ConversationSummaryError>;

} // namespace aiforge::runtime
