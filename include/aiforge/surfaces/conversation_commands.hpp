#pragma once

#include <aiforge/domain/conversation_admission.hpp>
#include <aiforge/domain/ids.hpp>
#include <expected>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace aiforge::surfaces {
struct InspectConversation {};
struct SetConversationMode {
  domain::ConversationMode mode;
};
struct PinConversationRun {
  domain::RunId run_id;
  bool pinned;
};
struct SetContextToolbar {
  bool visible;
};
struct InspectConversationSummaries {};
struct GenerateConversationSummary {
  std::vector<domain::RunId> run_ids;
};
struct ReviewConversationSummary {
  domain::ConversationSummaryId summary_id;
};
struct EditConversationSummary {
  domain::ConversationSummaryId summary_id;
};
struct PreviewConversationSummary {
  domain::ConversationSummaryId summary_id;
  std::vector<domain::ConversationSummaryId> replacements;
};
struct ApplyConversationSummary {};
struct DiscardConversationReview {};
struct DisableConversationSummary {
  domain::ConversationSummaryId summary_id;
};
using ConversationCommand =
    std::variant<InspectConversation, SetConversationMode, PinConversationRun,
                 SetContextToolbar, InspectConversationSummaries,
                 GenerateConversationSummary, ReviewConversationSummary,
                 EditConversationSummary, PreviewConversationSummary,
                 ApplyConversationSummary, DiscardConversationReview,
                 DisableConversationSummary>;
enum class ConversationCommandErrorCode {
  invalid_input,
  invalid_arguments,
  resource_exhausted,
  cancelled,
  internal_failure
};
struct ConversationCommandError {
  ConversationCommandErrorCode code;
  std::string message;
};
// Parses the conversation-control tail of /context. Repository add/remove/clear
// remain separate commands. All returned identities and lists are bounded.
[[nodiscard]] auto parse_conversation_command(std::string_view arguments,
                                              std::stop_token stop = {})
    -> std::expected<ConversationCommand, ConversationCommandError>;
} // namespace aiforge::surfaces
