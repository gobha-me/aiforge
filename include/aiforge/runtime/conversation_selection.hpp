#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include <aiforge/domain/context.hpp>

namespace aiforge::runtime {

enum class ConversationSelectionMode { full, rolling };

struct ConversationHistoryEntry {
  domain::ContextContentInput content;
  // The caller resolves this completed message's durable source before
  // selection. Selection validates identity/order, not the event store.
  domain::EventId completed_event_id;
  // Source sequence is unique and no earlier than this group's user input.
  // A tool error can precede AssistantContentFinished in durable history;
  // content.order, not completion sequence, records provider message order.
  std::uint64_t event_sequence{};
  auto operator==(const ConversationHistoryEntry&) const -> bool = default;
};

struct ConversationHistoryGroup {
  domain::RunId run_id;
  // One complete source run (possibly several tool exchanges), or the
  // preserved user input of a failed/cancelled run. Tool calls and all their
  // results belong to the same group.
  // A completed run whose final assistant answer was empty may end with a
  // fully matched tool result; the caller establishes source-run completion.
  std::vector<ConversationHistoryEntry> entries;
  auto operator==(const ConversationHistoryGroup&) const -> bool = default;
};

struct ConversationSelectionLimits {
  std::size_t maximum_groups{4096};
  std::size_t maximum_entries{16384};
  std::size_t maximum_source_references{16384};
  std::size_t maximum_pins{4096};
  // Aggregate content blocks and tool declarations bound zero-byte work too.
  std::size_t maximum_content_items{65536};
  // Includes content, arguments, tool names and optional provenance strings.
  std::size_t maximum_content_bytes{std::size_t{16} * 1024 * 1024};
  auto operator==(const ConversationSelectionLimits&) const -> bool = default;
};

struct ConversationSelectionRequest {
  ConversationSelectionMode mode{ConversationSelectionMode::full};
  domain::ContextCapacity capacity;
  // Instructions, current input, active summaries and preselected scoped
  // memory, excluding capacity.reserved_input_tokens (for example tool
  // schemas). Count once. Pinned history is reserved by this selector.
  std::uint64_t mandatory_input_tokens{};
  // Groups follow their first source event sequence. Sequences increase inside
  // each group, but runs may interleave. Content orders increase globally in
  // flattened group order. The selector never repairs any of these orders.
  std::vector<ConversationHistoryGroup> groups;
  std::vector<domain::RunId> pinned_run_ids;
  ConversationSelectionLimits limits;
  auto operator==(const ConversationSelectionRequest&) const -> bool = default;
};

enum class ConversationSelectionDecision {
  admitted_full,
  admitted_pin,
  admitted_recent,
  omitted_capacity,
  omitted_older,
};

struct ConversationSelectionDecisionRecord {
  domain::RunId run_id;
  ConversationSelectionDecision decision{
      ConversationSelectionDecision::omitted_older};
  std::size_t entry_count{};
  std::uint64_t estimated_tokens{};
  auto operator==(const ConversationSelectionDecisionRecord&) const
      -> bool = default;
};

struct ConversationSelectionResult {
  std::vector<ConversationHistoryGroup> selected_groups;
  // Exactly one decision per input group, in original chronology.
  std::vector<ConversationSelectionDecisionRecord> decisions;
  std::size_t selected_entry_count{};
  std::size_t omitted_group_count{};
  std::size_t omitted_entry_count{};
  std::uint64_t available_history_tokens{};
  std::uint64_t selected_history_tokens{};
  std::uint64_t omitted_history_tokens{};
  // Mandatory + reserved input + selected history; excludes reserved output.
  std::uint64_t estimated_input_tokens{};
  auto operator==(const ConversationSelectionResult&) const -> bool = default;
};

enum class ConversationSelectionErrorCode {
  invalid_mode,
  invalid_capacity,
  invalid_group,
  duplicate_identity,
  invalid_chronology,
  invalid_pin,
  resource_exhausted,
  token_overflow,
  mandatory_capacity_exceeded,
  history_capacity_exceeded,
  cancelled,
  internal_failure,
};

struct ConversationSelectionError {
  ConversationSelectionErrorCode code;
  std::string message;
  std::optional<domain::RunId> run_id;
  auto operator==(const ConversationSelectionError&) const -> bool = default;
};

// Pure bounded selection. No events, provider calls, tokenization or input
// mutation. ContextBuilder still validates the assembled provider request.
[[nodiscard]] auto select_conversation(
    const ConversationSelectionRequest& request, std::stop_token stop = {})
    -> std::expected<ConversationSelectionResult, ConversationSelectionError>;

} // namespace aiforge::runtime
