#include <aiforge/detail/utf8_text.hpp>
#include <aiforge/surfaces/conversation_commands.hpp>
#include <algorithm>
#include <set>
#include <utility>

namespace aiforge::surfaces {
namespace {
using Code = ConversationCommandErrorCode;
auto failure(Code code, const char* message)
    -> std::unexpected<ConversationCommandError> {
  return std::unexpected(ConversationCommandError{code, message});
}
auto take(std::string_view& text) -> std::string_view {
  const auto start = text.find_first_not_of(" \t");
  if (start == std::string_view::npos) {
    text = {};
    return {};
  }
  text.remove_prefix(start);
  const auto end = text.find_first_of(" \t");
  if (end == std::string_view::npos) {
    const auto value = text;
    text = {};
    return value;
  }
  const auto value = text.substr(0, end);
  text.remove_prefix(end);
  return value;
}
auto empty(std::string_view text) -> bool {
  return take(text).empty();
}
template <typename Id>
auto identity(std::string_view text)
    -> std::expected<Id, ConversationCommandError> {
  auto value = Id::from(std::string{text});
  if (!value)
    return failure(Code::invalid_arguments,
                   "Context command identity is invalid");
  return std::move(*value);
}
template <typename Id>
auto identities(std::string_view text, std::size_t maximum,
                std::stop_token stop)
    -> std::expected<std::vector<Id>, ConversationCommandError> {
  std::vector<Id> result;
  std::set<Id> seen;
  while (!empty(text)) {
    if (stop.stop_requested())
      return failure(Code::cancelled, "Context command cancelled");
    if (result.size() == maximum)
      return failure(Code::resource_exhausted,
                     "Context command has too many identities");
    auto value = identity<Id>(take(text));
    if (!value) return std::unexpected(value.error());
    if (!seen.insert(*value).second)
      return failure(Code::invalid_arguments,
                     "Context command repeats an identity");
    result.push_back(std::move(*value));
  }
  return result;
}
auto preview(domain::ConversationSummaryId id, std::string_view text,
             std::stop_token stop)
    -> std::expected<ConversationCommand, ConversationCommandError> {
  std::vector<domain::ConversationSummaryId> replacements;
  if (!empty(text)) {
    if (take(text) != "replace")
      return failure(Code::invalid_arguments,
                     "Use summary preview <id> [replace <id> ...]");
    auto ids = identities<domain::ConversationSummaryId>(text, 32, stop);
    if (!ids) return std::unexpected(ids.error());
    if (ids->empty())
      return failure(Code::invalid_arguments,
                     "Summary replacement requires explicit IDs");
    replacements = std::move(*ids);
  }
  return PreviewConversationSummary{std::move(id), std::move(replacements)};
}
auto generate(std::string_view text, std::stop_token stop)
    -> std::expected<ConversationCommand, ConversationCommandError> {
  auto ids = identities<domain::RunId>(text, 128, stop);
  if (!ids) return std::unexpected(ids.error());
  if (ids->empty())
    return failure(Code::invalid_arguments,
                   "Summary generation requires explicit source run IDs");
  return GenerateConversationSummary{std::move(*ids)};
}
auto summary(std::string_view text, std::stop_token stop)
    -> std::expected<ConversationCommand, ConversationCommandError> {
  const auto action = take(text);
  if (action.empty() || action == "list") {
    if (!empty(text))
      return failure(Code::invalid_arguments,
                     "Summary list takes no arguments");
    return InspectConversationSummaries{};
  }
  if (action == "apply" || action == "discard") {
    if (!empty(text))
      return failure(Code::invalid_arguments,
                     "Summary apply/discard takes no arguments");
    if (action == "apply") return ApplyConversationSummary{};
    return DiscardConversationReview{};
  }
  if (action == "generate") return generate(text, stop);
  auto id = identity<domain::ConversationSummaryId>(take(text));
  if (!id) return std::unexpected(id.error());
  if (action == "preview") return preview(std::move(*id), text, stop);
  if (!empty(text))
    return failure(Code::invalid_arguments,
                   "Summary action takes one identity");
  if (action == "review") return ReviewConversationSummary{std::move(*id)};
  if (action == "edit") return EditConversationSummary{std::move(*id)};
  if (action == "disable") return DisableConversationSummary{std::move(*id)};
  return failure(
      Code::invalid_arguments,
      "Use summary list/generate/review/edit/preview/apply/discard/disable");
}
auto command(std::string_view text, std::stop_token stop)
    -> std::expected<ConversationCommand, ConversationCommandError> {
  const auto action = take(text);
  if (action == "summary") return summary(text, stop);
  if (action.empty() || action == "history") {
    if (!empty(text))
      return failure(Code::invalid_arguments,
                     "Context history takes no arguments");
    return InspectConversation{};
  }
  const auto value = take(text);
  if (!empty(text))
    return failure(Code::invalid_arguments,
                   "Context action has extra arguments");
  if (action == "mode") {
    if (value == "full")
      return SetConversationMode{domain::ConversationMode::full};
    if (value == "rolling")
      return SetConversationMode{domain::ConversationMode::rolling};
    return failure(Code::invalid_arguments, "Use context mode full or rolling");
  }
  if (action == "toolbar") {
    if (value == "show") return SetContextToolbar{true};
    if (value == "hide") return SetContextToolbar{false};
    return failure(Code::invalid_arguments, "Use context toolbar show or hide");
  }
  if (action == "pin" || action == "unpin") {
    auto run = identity<domain::RunId>(value);
    if (!run) return std::unexpected(run.error());
    return PinConversationRun{std::move(*run), action == "pin"};
  }
  return failure(Code::invalid_arguments,
                 "Use context history/mode/pin/unpin/toolbar/summary");
}
} // namespace
auto parse_conversation_command(std::string_view arguments,
                                std::stop_token stop)
    -> std::expected<ConversationCommand, ConversationCommandError> {
  try {
    if (stop.stop_requested())
      return failure(Code::cancelled, "Context command cancelled");
    if (arguments.size() > std::size_t{64} * 1024U)
      return failure(Code::resource_exhausted, "Context command is too large");
    if ((!arguments.empty() && !detail::is_safe_utf8_text(arguments)) ||
        arguments.find_first_of("\r\n") != std::string_view::npos)
      return failure(Code::invalid_input,
                     "Context command contains invalid text");
    return command(arguments, stop);
  } catch (...) {
    return failure(Code::internal_failure, "Context command failed internally");
  }
}
} // namespace aiforge::surfaces
