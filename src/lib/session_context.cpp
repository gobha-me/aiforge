#include "conversation_context_internal.hpp"
#include <aiforge/runtime/session_context.hpp>

#include <aiforge/runtime/context_builder.hpp>

#include <algorithm>
#include <limits>
#include <set>
#include <string_view>
#include <utility>

namespace aiforge::runtime {
namespace {
using namespace domain;
using Code = SessionContextErrorCode;
using Status = std::expected<void, SessionContextError>;

auto failure(Code code, std::string message, bool retryable = false)
    -> std::unexpected<SessionContextError> {
  return std::unexpected(
      SessionContextError{code, std::move(message), retryable});
}

auto add(std::uint64_t& total, std::uint64_t amount) -> Status {
  if (amount > std::numeric_limits<std::uint64_t>::max() - total)
    return failure(Code::token_overflow,
                   "session context accounting overflowed");
  total += amount;
  return {};
}

struct MandatoryBudget {
  const ConversationHistoryLimits& limits;
  std::stop_token stop;
  std::uint64_t bytes{};
  std::size_t items{};

  auto charge(std::uint64_t amount) -> Status {
    if (stop.stop_requested())
      return failure(Code::cancelled, "context preparation cancelled");
    if (amount > limits.maximum_content_bytes - bytes)
      return failure(Code::resource_exhausted,
                     "required context exceeds the byte limit");
    bytes += amount;
    return {};
  }

  auto provenance(const ContextProvenance& value) -> Status {
    auto valid = charge(value.source_id.value().size());
    if (!valid) return valid;
    if (value.source_location) valid = charge(value.source_location->size());
    if (!valid) return valid;
    if (value.digest) valid = charge(value.digest->size());
    return valid;
  }

  auto message(const Message& value) -> Status {
    if (value.content.size() > limits.maximum_content_items - items)
      return failure(Code::resource_exhausted,
                     "required context has too many content items");
    items += value.content.size();
    if (value.tool_calls.size() > limits.maximum_content_items - items)
      return failure(Code::resource_exhausted,
                     "required context has too many calls");
    items += value.tool_calls.size();
    auto estimate = estimate_conversation_message(
        value, conversation_estimator_version, limits, stop);
    if (!estimate)
      return failure(estimate.error().code ==
                             ConversationHistoryErrorCode::cancelled
                         ? Code::cancelled
                         : Code::resource_exhausted,
                     "required context cannot be bounded");
    // Envelopes count towards this resource bound as well as actual content.
    auto valid = charge(*estimate);
    if (!valid) return valid;
    return charge(value.message_id.value().size());
  }
};

auto mandatory_content(const std::vector<ContextContentInput>& content,
                       MandatoryBudget& budget) -> Status {
  std::set<std::uint64_t> orders;
  std::size_t users{};
  for (const auto& entry : content) {
    const auto& message = entry.message;
    const bool user = entry.kind == ContextContentKind::conversation &&
                      message.role == Role::user;
    const bool evidence = entry.kind == ContextContentKind::evidence &&
                          message.role == Role::evidence;
    if ((!user && !evidence) || message.invocation_id ||
        !message.tool_calls.empty() || entry.order == 0 ||
        !orders.insert(entry.order).second)
      return failure(Code::invalid_mandatory,
                     "required content must contain only ordered current input "
                     "and evidence");
    if (user && ++users > 1)
      return failure(Code::invalid_mandatory,
                     "required context has multiple current inputs");
    auto valid = budget.charge(entry.entry_id.value().size());
    if (!valid) return valid;
    valid = budget.provenance(entry.provenance);
    if (!valid) return valid;
    valid = budget.message(message);
    if (!valid) return valid;
  }
  if (users != 1)
    return failure(Code::invalid_mandatory,
                   "required context has no current user input");
  return {};
}

auto preflight(const SessionContextRequest& request, std::stop_token stop)
    -> Status {
  if (stop.stop_requested())
    return failure(Code::cancelled, "context preparation cancelled");
  const auto& input = request.mandatory;
  const auto maximum = request.selection_limits.maximum_entries;
  if (input.instructions.size() > maximum ||
      input.content.size() > maximum - input.instructions.size())
    return failure(Code::resource_exhausted,
                   "required context has too many entries");
  MandatoryBudget budget{request.history_limits, stop};
  for (const auto& entry : input.instructions) {
    auto valid = budget.charge(entry.entry_id.value().size());
    if (!valid) return valid;
    valid = budget.provenance(entry.provenance);
    if (!valid) return valid;
    if (entry.target_entry_id)
      valid = budget.charge(entry.target_entry_id->value().size());
    if (!valid) return valid;
    if (entry.message) valid = budget.message(*entry.message);
    if (!valid) return valid;
  }
  return mandatory_content(input.content, budget);
}

auto preview(const SessionContextRequest& request, std::uint64_t mandatory,
             std::uint64_t first_order, std::stop_token stop,
             const context_detail::ConversationContextSources* sources)
    -> std::expected<PreparedConversationContext, SessionContextError> {
  const ConversationContextRequest conversation{request.log,
                                                request.model_id,
                                                request.mandatory.capacity,
                                                mandatory,
                                                first_order,
                                                request.history_limits,
                                                request.selection_limits};
  auto result = sources != nullptr
                    ? context_detail::prepare_conversation_context_from_sources(
                          conversation, *sources, stop)
                    : prepare_conversation_context(conversation, stop);
  if (!result)
    return failure(result.error().code ==
                           ConversationContextErrorCode::cancelled
                       ? Code::cancelled
                       : Code::conversation_failed,
                   result.error().message);
  return std::move(*result);
}

auto memory_capacity(const PreparedConversationContext& conversation)
    -> std::expected<std::uint64_t, SessionContextError> {
  const auto& admission = conversation.admission;
  std::uint64_t reserved{};
  for (const auto& group : admission.groups) {
    if (admission.mode == ConversationMode::rolling && !group.pinned) continue;
    for (const auto& entry : group.entries) {
      auto valid = add(reserved, entry.estimated_tokens);
      if (!valid) return std::unexpected(valid.error());
    }
  }
  // The preview already validated all capacity subtraction and mandatory pins.
  return conversation.selection.available_history_tokens - reserved;
}

auto memory_context(const SessionContextRequest& request,
                    std::uint64_t available, std::stop_token stop)
    -> std::expected<SelectedMemoryContext, SessionContextError> {
  if (stop.stop_requested())
    return failure(Code::cancelled, "context preparation cancelled");
  SelectedMemoryContext result{
      {1,
       request.memory.repository_id,
       request.memory.persona_id,
       request.memory_controller != nullptr ? request.memory.maximum_tokens : 0,
       available,
       {}},
      {}};
  if (request.memory_controller != nullptr) {
    auto memory_request = request.memory;
    memory_request.available_tokens = available;
    auto selected = select_memory_context_with_provenance(
        *request.memory_controller, memory_request);
    if (!selected)
      return failure(Code::memory_failed, selected.error().message,
                     selected.error().retryable);
    result = std::move(*selected);
  }
  if (stop.stop_requested())
    return failure(Code::cancelled, "context preparation cancelled");
  if (result.content.size() != result.selection.entries.size())
    return failure(Code::internal_failure,
                   "memory selection lineage is inconsistent");
  for (std::size_t index = 0; index < result.content.size(); ++index) {
    result.content[index].order = index + 1;
    result.selection.entries[index].order = index + 1;
  }
  auto sealed = seal_memory_selection(result.selection);
  if (!sealed) return failure(Code::memory_failed, sealed.error().message);
  return result;
}

auto merge(const SessionContextRequest& request, SelectedMemoryContext memory,
           PreparedConversationContext conversation, std::stop_token stop)
    -> std::expected<PreparedSessionContext, SessionContextError> {
  auto count = std::uint64_t{request.mandatory.instructions.size()};
  for (const auto amount :
       {request.mandatory.content.size(), memory.content.size(),
        conversation.summary_content.size(),
        conversation.selection.selected_entry_count}) {
    auto valid = add(count, amount);
    if (!valid) return std::unexpected(valid.error());
  }
  if (count > request.selection_limits.maximum_entries)
    return failure(Code::resource_exhausted,
                   "merged session context has too many entries");
  auto input = request.mandatory;
  std::ranges::sort(input.content, {}, &ContextContentInput::order);
  auto required = std::move(input.content);
  input.content = std::move(memory.content);
  for (auto& summary : conversation.summary_content)
    input.content.push_back(std::move(summary));
  for (auto& group : conversation.selection.selected_groups)
    for (auto& entry : group.entries)
      input.content.push_back(std::move(entry.content));
  for (auto& entry : required) {
    entry.order = input.content.size() + 1;
    input.content.push_back(std::move(entry));
  }
  if (stop.stop_requested())
    return failure(Code::cancelled, "context preparation cancelled");
  auto built = ContextBuilder{}.build(input);
  if (!built) return failure(Code::context_failed, built.error().message);
  if (!memory_selection_matches_context(memory.selection, *built))
    return failure(Code::memory_failed,
                   "memory selection does not match merged context");
  return PreparedSessionContext{std::move(input), std::move(memory.selection),
                                std::move(conversation.admission)};
}

auto prepare(
    const SessionContextRequest& request, std::stop_token stop,
    const context_detail::ConversationContextSources* sources = nullptr)
    -> std::expected<PreparedSessionContext, SessionContextError> {
  auto valid = preflight(request, stop);
  if (!valid) return std::unexpected(valid.error());
  auto required = ContextBuilder{}.build(request.mandatory);
  if (!required)
    return failure(Code::invalid_mandatory, required.error().message);
  auto mandatory = required->estimated_input_tokens -
                   request.mandatory.capacity.reserved_input_tokens;
  auto conversation = preview(request, mandatory, 1, stop, sources);
  if (!conversation) return std::unexpected(conversation.error());
  auto available = memory_capacity(*conversation);
  if (!available) return std::unexpected(available.error());
  auto memory = memory_context(request, *available, stop);
  if (!memory) return std::unexpected(memory.error());
  for (const auto& entry : memory->content) {
    valid = add(mandatory, entry.estimated_tokens);
    if (!valid) return std::unexpected(valid.error());
  }
  conversation =
      preview(request, mandatory, memory->content.size() + 1, stop, sources);
  if (!conversation) return std::unexpected(conversation.error());
  return merge(request, std::move(*memory), std::move(*conversation), stop);
}

auto valid_tool(const backend::ToolDeclaration& value) -> bool {
  return !value.name.empty() && value.name.size() <= 128 &&
         std::ranges::none_of(value.name,
                              [](const unsigned char character) {
                                return character < 0x20U || character == 0x7fU;
                              }) &&
         (value.input_schema.media_type == "application/json" ||
          value.input_schema.media_type == "application/schema+json") &&
         !value.input_schema.data.empty();
}

auto tool_estimate(std::span<const backend::ToolDeclaration> declarations,
                   std::uint32_t version,
                   const ConversationHistoryLimits& limits,
                   std::stop_token stop)
    -> std::expected<std::uint64_t, SessionContextError> {
  if (stop.stop_requested())
    return failure(Code::cancelled, "tool accounting cancelled");
  if (version != conversation_estimator_version)
    return failure(Code::unsupported_estimator,
                   "unsupported tool declaration estimator");
  if (declarations.size() > limits.maximum_content_items)
    return failure(Code::resource_exhausted, "too many tool declarations");
  std::set<std::string_view> names;
  std::uint64_t bytes{};
  std::uint64_t estimate{};
  for (const auto& declaration : declarations) {
    if (stop.stop_requested())
      return failure(Code::cancelled, "tool accounting cancelled");
    if (!valid_tool(declaration) || !names.insert(declaration.name).second)
      return failure(Code::invalid_tool,
                     "invalid or duplicate tool declaration");
    auto valid = add(estimate, 32);
    if (!valid) return std::unexpected(valid.error());
    for (const auto size :
         {declaration.name.size(), declaration.description.size(),
          declaration.input_schema.media_type.size(),
          declaration.input_schema.data.size()}) {
      if (size > limits.maximum_content_bytes - bytes)
        return failure(Code::resource_exhausted,
                       "tool declarations exceed the byte limit");
      bytes += size;
      valid = add(estimate, size);
      if (!valid) return std::unexpected(valid.error());
    }
  }
  return estimate;
}
} // namespace

auto prepare_session_context(const SessionContextRequest& request,
                             std::stop_token stop)
    -> std::expected<PreparedSessionContext, SessionContextError> {
  try {
    return prepare(request, stop);
  } catch (...) {
    return failure(Code::internal_failure,
                   "session context preparation failed");
  }
}

auto preview_session_context_after_summary_activation(
    const SessionContextRequest& request, std::span<const RunEvent> suffix,
    std::stop_token stop)
    -> std::expected<PreparedSessionContext, SessionContextError> {
  try {
    auto valid = preflight(request, stop);
    if (!valid) return std::unexpected(valid.error());
    auto sources = context_detail::resolve_summary_preview_sources(
        {request.log, request.model_id, request.mandatory.capacity, 0, 1,
         request.history_limits, request.selection_limits},
        suffix, stop);
    if (!sources)
      return failure(sources.error().code ==
                             ConversationContextErrorCode::cancelled
                         ? Code::cancelled
                         : Code::conversation_failed,
                     sources.error().message);
    auto readonly = request;
    readonly.memory.read_only = true;
    return prepare(readonly, stop, &*sources);
  } catch (...) {
    return failure(Code::internal_failure, "summary context preview failed");
  }
}

auto estimate_session_tool_declarations(
    std::span<const backend::ToolDeclaration> declarations,
    std::uint32_t estimator_version, const ConversationHistoryLimits& limits,
    std::stop_token stop) -> std::expected<std::uint64_t, SessionContextError> {
  try {
    return tool_estimate(declarations, estimator_version, limits, stop);
  } catch (...) {
    return failure(Code::internal_failure,
                   "tool declaration accounting failed");
  }
}
} // namespace aiforge::runtime
