#pragma once

#include <aiforge/runtime/conversation_summary_sources.hpp>

namespace aiforge::runtime {
struct PreparedConversationSummaryGeneration {
  domain::ConversationSummaryIntent intent;
  domain::Message user_message;
  domain::ConstructedContext context;
  auto operator==(const PreparedConversationSummaryGeneration&) const
      -> bool = default;
};

enum class ConversationSummaryGenerationErrorCode {
  invalid_specification,
  invalid_sources,
  invalid_request,
  unsupported_content,
  resource_exhausted,
  capacity_exceeded,
  cancelled,
  internal_failure,
};
struct ConversationSummaryGenerationError {
  ConversationSummaryGenerationErrorCode code;
  std::string message;
  auto operator==(const ConversationSummaryGenerationError&) const
      -> bool = default;
};

// Specification supplies exact sealed sources, identities, model, capacity and
// runtime version. Its estimate must be zero and its intent seal absent. The
// canonical v1 request has runtime instructions, a task and inert source
// evidence only: no composer draft, scoped memory or executable tool calls.
[[nodiscard]] auto prepare_conversation_summary_generation(
    const domain::SessionEventLog& log,
    const domain::ConversationSummaryIntent& specification,
    const ConversationHistoryLimits& limits = {}, std::stop_token stop = {})
    -> std::expected<PreparedConversationSummaryGeneration,
                     ConversationSummaryGenerationError>;

// Rebuilds the canonical request and checks its exact recorded estimate/seal.
// Callers separately enforce no tools, provider options and spend
// authorization.
[[nodiscard]] auto reconstruct_conversation_summary_generation(
    const domain::SessionEventLog& log,
    const domain::ConversationSummaryIntent& intent,
    const ConversationHistoryLimits& limits = {}, std::stop_token stop = {})
    -> std::expected<PreparedConversationSummaryGeneration,
                     ConversationSummaryGenerationError>;

[[nodiscard]] auto validate_conversation_summary_generation(
    const domain::SessionEventLog& log,
    const domain::ConversationSummaryIntent& intent,
    const domain::Message& user_message,
    const domain::ConstructedContext& context,
    const ConversationHistoryLimits& limits = {}, std::stop_token stop = {})
    -> std::expected<void, ConversationSummaryGenerationError>;
} // namespace aiforge::runtime
