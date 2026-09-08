#pragma once

#include <aiforge/runtime/conversation_context.hpp>
#include <aiforge/runtime/conversation_policy.hpp>
#include <aiforge/runtime/conversation_summary_context.hpp>

namespace aiforge::runtime::context_detail {
// Internal trusted projections, never caller-supplied application state.
struct ConversationContextSources {
  std::uint64_t snapshot_sequence{};
  ConversationPolicySnapshot policy;
  std::vector<ConversationHistoryGroup> history;
  ConversationSummarySnapshot summaries;
};
[[nodiscard]] auto prepare_summary_context_from_snapshot(
    const ConversationSummarySnapshot& snapshot, std::uint64_t first_order,
    const ConversationHistoryLimits& limits, std::stop_token stop)
    -> std::expected<PreparedConversationSummaryContext,
                     ConversationSummaryContextError>;
[[nodiscard]] auto resolve_summary_preview_sources(
    const ConversationContextRequest& request,
    std::span<const domain::RunEvent> suffix, std::stop_token stop)
    -> std::expected<ConversationContextSources, ConversationContextError>;
[[nodiscard]] auto prepare_conversation_context_from_sources(
    const ConversationContextRequest& request,
    const ConversationContextSources& sources, std::stop_token stop)
    -> std::expected<PreparedConversationContext, ConversationContextError>;
} // namespace aiforge::runtime::context_detail
