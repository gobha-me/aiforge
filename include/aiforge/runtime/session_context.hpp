#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <stop_token>
#include <string>

#include <aiforge/backend/backend.hpp>
#include <aiforge/runtime/conversation_context.hpp>
#include <aiforge/runtime/memory_controller.hpp>

namespace aiforge::runtime {

struct SessionContextRequest {
  const domain::SessionEventLog& log;
  domain::ModelId model_id;
  // Required instructions, one current user input, and explicit evidence only.
  // History and scoped memory are selected here. Required inputs and memory
  // preserve their source estimates; conversation v1 applies to reconstructed
  // history and active tool messages. Resource preflight does not replace
  // those estimates. External input (including tool declarations) is reserved
  // once in capacity.reserved_input_tokens.
  const domain::ContextBuildInput& mandatory;
  MemoryController* memory_controller{};
  // available_tokens is owned by this preparer; the supplied value is ignored.
  MemoryContextRequest memory;
  ConversationHistoryLimits history_limits;
  ConversationSelectionLimits selection_limits;
};

struct PreparedSessionContext {
  domain::ContextBuildInput input;
  domain::MemorySelection memory_selection;
  domain::ConversationAdmission conversation_admission;
  auto operator==(const PreparedSessionContext&) const -> bool = default;
};

enum class SessionContextErrorCode {
  invalid_mandatory,
  invalid_tool,
  unsupported_estimator,
  resource_exhausted,
  token_overflow,
  conversation_failed,
  memory_failed,
  context_failed,
  cancelled,
  internal_failure,
};

struct SessionContextError {
  SessionContextErrorCode code;
  std::string message;
  bool retryable{};
  auto operator==(const SessionContextError&) const -> bool = default;
};

// Reserves pins in rolling mode, or all history in full mode, before selecting
// scoped memory. Remaining capacity admits a contiguous newest history suffix.
// The final content order is memory, history, then required content. Both
// manifests are sealed against those orders, and ContextBuilder validates the
// merged input. No history/policy events or provider requests are produced.
[[nodiscard]] auto prepare_session_context(const SessionContextRequest& request,
                                           std::stop_token stop = {})
    -> std::expected<PreparedSessionContext, SessionContextError>;

// Version 1 counts every byte of name, description, schema media type and
// schema data, plus a 32-token declaration envelope. This conservative neutral
// estimate is not a provider tokenizer. Authority metadata is not provider
// input.
[[nodiscard]] auto estimate_session_tool_declarations(
    std::span<const backend::ToolDeclaration> declarations,
    std::uint32_t estimator_version = conversation_estimator_version,
    const ConversationHistoryLimits& limits = {}, std::stop_token stop = {})
    -> std::expected<std::uint64_t, SessionContextError>;

} // namespace aiforge::runtime
