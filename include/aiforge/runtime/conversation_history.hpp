#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include <aiforge/domain/event_log.hpp>
#include <aiforge/runtime/conversation_selection.hpp>

namespace aiforge::runtime {

inline constexpr std::uint32_t conversation_estimator_version{1};

struct ConversationHistoryLimits {
  std::size_t maximum_events{65536};
  std::size_t maximum_runs{4096};
  std::size_t maximum_source_references{16384};
  std::size_t maximum_content_items{65536};
  // Preflight includes the existing artifact renderer's maximum expansion;
  // final output is also checked against this ceiling using actual bytes.
  std::size_t maximum_content_bytes{16 * 1024 * 1024};
  // Sum of relevant event count times potential message count per run bounds
  // the existing artifact continuation projector's nested lookup work.
  std::size_t maximum_projection_work{4 * 1024 * 1024};
  auto operator==(const ConversationHistoryLimits&) const -> bool = default;
};

struct ConversationHistoryRequest {
  const domain::SessionEventLog& log;
  // Additional caller exclusions, such as the active run. Typed child,
  // control, summary and nonterminal runs are automatically excluded;
  // prompts/surface names are never read.
  std::vector<domain::RunId> excluded_run_ids;
  std::uint32_t estimator_version{conversation_estimator_version};
  ConversationHistoryLimits limits;
  // Null selects the current log end. A positive cutoff must identify an
  // existing event; zero selects an empty prefix. Later events cannot change
  // completion, purpose or child classification in this snapshot.
  std::optional<std::uint64_t> source_snapshot_sequence{};
};

enum class ConversationHistoryErrorCode {
  unsupported_estimator,
  invalid_history,
  invalid_snapshot,
  invalid_exclusion,
  unsupported_content,
  resource_exhausted,
  token_overflow,
  cancelled,
  internal_failure,
};

struct ConversationHistoryError {
  ConversationHistoryErrorCode code;
  std::string message;
  std::optional<domain::RunId> run_id;
  auto operator==(const ConversationHistoryError&) const -> bool = default;
};

// V1 counts UTF-8 bytes and conservative message/block/tool envelopes. It is
// an explicit text estimate, not a provider tokenizer or image-token quote.
// Artifact references account for their metadata only; no artifact is fetched.
[[nodiscard]] auto estimate_conversation_message(
    const domain::Message& message,
    std::uint32_t version = conversation_estimator_version,
    const ConversationHistoryLimits& limits = {}, std::stop_token stop = {})
    -> std::expected<std::uint64_t, ConversationHistoryError>;

// Scans bounded event references before copying/projecting relevant content.
// Completed source runs retain all complete tool exchanges; failed/cancelled
// runs retain only their original user input. No external work or mutation.
// Empty successful answers preserve user input, matching legacy replay.
// Tool artifacts use the existing metadata projector. Unresolved direct
// user/assistant artifact references return unsupported_content.
[[nodiscard]] auto reconstruct_conversation_history(
    const ConversationHistoryRequest& request, std::stop_token stop = {})
    -> std::expected<std::vector<ConversationHistoryGroup>,
                     ConversationHistoryError>;

} // namespace aiforge::runtime
