#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include <aiforge/domain/conversation_admission.hpp>
#include <aiforge/runtime/conversation_history.hpp>

namespace aiforge::runtime {

struct ConversationContextRequest {
  const domain::SessionEventLog& log;
  domain::ModelId model_id;
  domain::ContextCapacity capacity;
  // Required input and memory only; active summary estimates are added here.
  std::uint64_t mandatory_input_tokens{};
  // Summary evidence then selected history follow already admitted memory.
  // The legacy field name denotes the first order available to this binder.
  std::uint64_t first_history_order{1};
  ConversationHistoryLimits history_limits;
  ConversationSelectionLimits selection_limits;
};

struct PreparedConversationContext {
  ConversationSelectionResult selection;
  domain::ConversationAdmission admission;
  std::vector<domain::ContextContentInput> summary_content{};
  auto operator==(const PreparedConversationContext&) const -> bool = default;
};

enum class ConversationContextErrorCode {
  invalid_policy,
  invalid_admission,
  invalid_history,
  invalid_order,
  selection_failed,
  source_mismatch,
  resource_exhausted,
  cancelled,
  internal_failure,
};

struct ConversationContextError {
  ConversationContextErrorCode code;
  std::string message;
  std::optional<domain::RunId> run_id;
  auto operator==(const ConversationContextError&) const -> bool = default;
};

// Resolves current durable policy and complete source groups, selects once,
// then seals exact identities, estimates, normalized message digests and order.
// The caller persists this admission atomically with its new top-level run.
[[nodiscard]] auto prepare_conversation_context(
    const ConversationContextRequest& request, std::stop_token stop = {})
    -> std::expected<PreparedConversationContext, ConversationContextError>;

// Resolves the recorded source prefix and original policy. Later history and
// policy changes cannot replace the admitted base. Does not run the selector,
// dispatch providers, fetch artifacts or mutate the log.
[[nodiscard]] auto recover_conversation_context(
    const domain::SessionEventLog& log,
    const domain::ConversationAdmission& admission,
    const ConversationHistoryLimits& limits = {}, std::stop_token stop = {})
    -> std::expected<std::vector<ConversationHistoryGroup>,
                     ConversationContextError>;

} // namespace aiforge::runtime
