#pragma once

#include <aiforge/domain/conversation_summary.hpp>
#include <aiforge/runtime/conversation_history.hpp>

namespace aiforge::runtime {

struct ConversationSummarySourceRequest {
  const domain::SessionEventLog& log;
  // Explicit original source runs; caller order does not change chronology.
  std::vector<domain::RunId> run_ids{};
  // Null selects the current end. Positive snapshots must identify an event.
  std::optional<std::uint64_t> snapshot_sequence{};
  ConversationHistoryLimits limits{};
};

struct PreparedConversationSummarySources {
  domain::ConversationSummarySources sources;
  // Complete provider exchanges, in source chronology, with contiguous orders
  // starting at one. The caller adds the summary task and validates capacity.
  std::vector<domain::ContextContentInput> content;
  std::uint64_t estimated_tokens{};
  auto operator==(const PreparedConversationSummarySources&) const
      -> bool = default;
};

enum class ConversationSummarySourceErrorCode {
  invalid_request,
  invalid_history,
  invalid_sources,
  foreign_scope,
  resource_exhausted,
  cancelled,
  internal_failure,
};

struct ConversationSummarySourceError {
  ConversationSummarySourceErrorCode code;
  std::string message;
  auto operator==(const ConversationSummarySourceError&) const
      -> bool = default;
};

// No policy selection, events, provider calls or artifact reads. Sources are
// whole completed original runs; failed/cancelled runs retain their user input
// only, consistently with conversation history. Live, child, control and
// summary runs cannot be requested. Unrelated runs are excluded before
// projection.
[[nodiscard]] auto prepare_conversation_summary_sources(
    const ConversationSummarySourceRequest& request, std::stop_token stop = {})
    -> std::expected<PreparedConversationSummarySources,
                     ConversationSummarySourceError>;

// Resolves the exact saved prefix and canonical entries, including terminal
// references and message digests. Later events and policy changes are
// irrelevant; missing, altered, partial or unsupported sources fail without
// partial output.
[[nodiscard]] auto resolve_conversation_summary_sources(
    const domain::SessionEventLog& log,
    const domain::ConversationSummarySources& sources,
    const ConversationHistoryLimits& limits = {}, std::stop_token stop = {})
    -> std::expected<PreparedConversationSummarySources,
                     ConversationSummarySourceError>;

} // namespace aiforge::runtime
