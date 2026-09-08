#pragma once

#include <aiforge/runtime/conversation_history.hpp>
#include <aiforge/runtime/conversation_summary_projection.hpp>
#include <span>

namespace aiforge::runtime {
struct PreparedConversationSummaryContext {
  std::vector<domain::ConversationAdmittedSummary> summaries;
  std::vector<domain::ContextContentInput> content;
  std::vector<domain::RunId> covered_run_ids;
  std::uint64_t estimated_tokens{};
  auto operator==(const PreparedConversationSummaryContext&) const
      -> bool = default;
};
enum class ConversationSummaryContextErrorCode {
  invalid_source,
  unavailable_activation,
  source_mismatch,
  invalid_order,
  resource_exhausted,
  token_overflow,
  cancelled,
  internal_failure,
};
struct ConversationSummaryContextError {
  ConversationSummaryContextErrorCode code;
  std::string message;
  auto operator==(const ConversationSummaryContextError&) const
      -> bool = default;
};

// Projects active reviewed summaries as mandatory derived untrusted evidence.
// The caller enables this only for rolling policy. Order follows source anchor
// then summary identity; memory may already occupy earlier content orders.
[[nodiscard]] auto prepare_conversation_summary_context(
    const domain::SessionEventLog& log, std::uint64_t first_order = 1,
    const ConversationHistoryLimits& limits = {}, std::stop_token stop = {})
    -> std::expected<PreparedConversationSummaryContext,
                     ConversationSummaryContextError>;

// Resolves exact saved versions and activation identities at the admitted
// prefix. By default, each activation must still be current. Only a runtime
// that has already pinned this immutable base may disable that availability
// check; source versions, provenance, order and digests are always checked.
[[nodiscard]] auto recover_conversation_summary_context(
    const domain::SessionEventLog& log,
    std::span<const domain::ConversationAdmittedSummary> summaries,
    std::uint64_t source_snapshot_sequence,
    const ConversationHistoryLimits& limits = {}, std::stop_token stop = {},
    bool require_current_activation = true)
    -> std::expected<PreparedConversationSummaryContext,
                     ConversationSummaryContextError>;
} // namespace aiforge::runtime
