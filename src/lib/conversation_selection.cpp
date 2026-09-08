#include <aiforge/runtime/conversation_selection.hpp>

#include <algorithm>
#include <concepts>
#include <limits>
#include <set>
#include <type_traits>
#include <utility>
#include <variant>

namespace aiforge::runtime {
namespace {

using namespace domain;
using Code = ConversationSelectionErrorCode;
using Decision = ConversationSelectionDecision;

auto failure(Code code, std::string message,
             std::optional<RunId> run_id = std::nullopt)
    -> std::unexpected<ConversationSelectionError> {
  return std::unexpected(
      ConversationSelectionError{code, std::move(message), std::move(run_id)});
}

auto add_tokens(std::uint64_t& total, std::uint64_t value) -> bool {
  if (value > std::numeric_limits<std::uint64_t>::max() - total) return false;
  total += value;
  return true;
}

struct ContentBudget {
  const ConversationSelectionLimits& limits;
  std::size_t items{};
  std::size_t bytes{};

  auto add_bytes(std::size_t size) -> bool {
    if (size > limits.maximum_content_bytes - bytes) return false;
    bytes += size;
    return true;
  }

  auto add_item() -> bool {
    if (items == limits.maximum_content_items) return false;
    ++items;
    return true;
  }
};

auto count_block(const ContentBlock& block, ContentBudget& budget) -> bool {
  return std::visit(
      [&budget](const auto& value) {
        using T = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::same_as<T, TextBlock>) {
          return budget.add_bytes(value.text.size());
        } else if constexpr (std::same_as<T, StructuredDataBlock>) {
          return budget.add_bytes(value.media_type.size()) &&
                 budget.add_bytes(value.data.size());
        } else if constexpr (std::same_as<T, CitationBlock>) {
          return budget.add_bytes(value.uri.size()) &&
                 (!value.title || budget.add_bytes(value.title->size()));
        } else if constexpr (std::same_as<T, ArtifactReferenceBlock>) {
          return budget.add_bytes(value.artifact_id.value().size()) &&
                 (!value.label || budget.add_bytes(value.label->size()));
        } else {
          return false;
        }
      },
      block);
}

using Status = std::expected<void, ConversationSelectionError>;

struct ValidationState {
  explicit ValidationState(const ConversationSelectionLimits& limits,
                           std::stop_token cancellation)
      : budget{limits}, stop{std::move(cancellation)} {}

  ContentBudget budget;
  std::stop_token stop;
  std::set<RunId> runs;
  std::set<EventId> events;
  std::set<std::uint64_t> event_sequences;
  std::set<MessageId> messages;
  std::set<ContextEntryId> entries;
  std::set<ContextSourceId> sources;
  std::set<InvocationId> invocations;
  std::vector<std::uint64_t> group_tokens;
  std::uint64_t previous_group_sequence{};
  std::uint64_t previous_order{};
  std::uint64_t history_tokens{};
  std::size_t entry_count{};

  auto validate_identity(const ConversationHistoryEntry& entry,
                         const RunId& run_id) -> Status {
    const auto& value = entry.content;
    if (!events.insert(entry.completed_event_id).second ||
        !messages.insert(value.message.message_id).second ||
        !entries.insert(value.entry_id).second ||
        !sources.insert(value.provenance.source_id).second)
      return failure(Code::duplicate_identity,
                     "duplicate conversation source identity", run_id);
    if (entry.event_sequence < previous_group_sequence ||
        value.order <= previous_order ||
        !event_sequences.insert(entry.event_sequence).second)
      return failure(Code::invalid_chronology,
                     "conversation source sequence or message order is invalid",
                     run_id);
    previous_order = value.order;
    if (value.estimated_tokens == 0 ||
        (value.provenance.source_location &&
         value.provenance.source_location->empty()) ||
        (value.provenance.digest && value.provenance.digest->empty()))
      return failure(Code::invalid_group,
                     "invalid conversation estimate or provenance", run_id);
    return {};
  }

  auto validate_content(const ContextContentInput& value, const RunId& run_id)
      -> Status {
    if ((value.provenance.source_location &&
         !budget.add_bytes(value.provenance.source_location->size())) ||
        (value.provenance.digest &&
         !budget.add_bytes(value.provenance.digest->size())))
      return failure(Code::resource_exhausted,
                     "conversation content limit exceeded", run_id);
    const auto& message = value.message;
    if (message.content.empty() && message.tool_calls.empty())
      return failure(Code::invalid_group, "conversation message is empty",
                     run_id);
    for (const auto& block : message.content) {
      if (stop.stop_requested())
        return failure(Code::cancelled, "selection cancelled");
      if (std::holds_alternative<UnknownContentBlock>(block))
        return failure(Code::invalid_group, "unsupported conversation content",
                       run_id);
      if (!budget.add_item() || !count_block(block, budget))
        return failure(Code::resource_exhausted,
                       "conversation content limit exceeded", run_id);
    }
    return {};
  }

  auto validate_calls(const Message& message, const RunId& run_id,
                      std::set<InvocationId>& pending) -> Status {
    for (const auto& call : message.tool_calls) {
      if (stop.stop_requested())
        return failure(Code::cancelled, "selection cancelled");
      if (call.tool_name.empty() || call.tool_name.size() > 128 ||
          std::ranges::any_of(call.tool_name,
                              [](const unsigned char value) {
                                return value < 0x20 || value == 0x7f;
                              }) ||
          call.arguments.media_type != "application/json" ||
          call.arguments.data.empty())
        return failure(Code::invalid_group, "invalid conversation tool call",
                       run_id);
      if (!budget.add_item() || !budget.add_bytes(call.tool_name.size()) ||
          !budget.add_bytes(call.arguments.media_type.size()) ||
          !budget.add_bytes(call.arguments.data.size()))
        return failure(Code::resource_exhausted,
                       "conversation content limit exceeded", run_id);
      if (!invocations.insert(call.invocation_id).second)
        return failure(Code::duplicate_identity,
                       "duplicate conversation invocation", run_id);
      pending.insert(call.invocation_id);
    }
    return {};
  }
};

auto validate_role(const ContextContentInput& value, std::size_t index,
                   const RunId& run_id, std::set<InvocationId>& pending)
    -> Status {
  const auto& message = value.message;
  if (index == 0 && message.role != Role::user)
    return failure(Code::invalid_group,
                   "conversation group must start with user input", run_id);
  if (message.role == Role::tool) {
    if (value.kind != ContextContentKind::tool_result ||
        !message.invocation_id || !message.tool_calls.empty() ||
        pending.erase(*message.invocation_id) != 1)
      return failure(Code::invalid_group,
                     "tool result has no matching pending call", run_id);
    return {};
  }
  const bool allowed_role = message.role == Role::assistant ||
                            (message.role == Role::user && index == 0);
  if (value.kind != ContextContentKind::conversation || message.invocation_id ||
      !pending.empty() || !allowed_role ||
      (message.role != Role::assistant && !message.tool_calls.empty()))
    return failure(Code::invalid_group,
                   "invalid conversation role or incomplete tool exchange",
                   run_id);
  return {};
}

auto validate_group_header(const ConversationHistoryGroup& group,
                           ValidationState& state) -> Status {
  if (!state.runs.insert(group.run_id).second)
    return failure(Code::duplicate_identity, "duplicate conversation run",
                   group.run_id);
  if (group.entries.empty())
    return failure(Code::invalid_group, "conversation group is empty",
                   group.run_id);
  const auto& limits = state.budget.limits;
  if (group.entries.size() > limits.maximum_entries - state.entry_count ||
      group.entries.size() >
          limits.maximum_source_references - state.entry_count)
    return failure(Code::resource_exhausted,
                   "conversation entry/reference limit exceeded", group.run_id);
  state.entry_count += group.entries.size();
  if (group.entries.front().event_sequence <= state.previous_group_sequence)
    return failure(Code::invalid_chronology,
                   "conversation groups must follow first source order",
                   group.run_id);
  state.previous_group_sequence = group.entries.front().event_sequence;
  return {};
}

auto validate_entry(const ConversationHistoryEntry& entry, std::size_t index,
                    const RunId& run_id, ValidationState& state,
                    std::set<InvocationId>& pending) -> Status {
  auto valid = state.validate_identity(entry, run_id);
  if (!valid) return valid;
  valid = validate_role(entry.content, index, run_id, pending);
  if (!valid) return valid;
  valid = state.validate_content(entry.content, run_id);
  if (!valid) return valid;
  return state.validate_calls(entry.content.message, run_id, pending);
}

auto validate_group(const ConversationHistoryGroup& group,
                    ValidationState& state) -> Status {
  auto valid = validate_group_header(group, state);
  if (!valid) return valid;
  std::set<InvocationId> pending;
  std::uint64_t tokens{};
  for (std::size_t index = 0; index < group.entries.size(); ++index) {
    if (state.stop.stop_requested())
      return failure(Code::cancelled, "selection cancelled");
    const auto& entry = group.entries[index];
    valid = validate_entry(entry, index, group.run_id, state, pending);
    if (!valid) return valid;
    if (!add_tokens(tokens, entry.content.estimated_tokens))
      return failure(Code::token_overflow,
                     "conversation group estimate overflowed", group.run_id);
  }
  if (!pending.empty() ||
      (group.entries.size() > 1 &&
       group.entries.back().content.message.role != Role::assistant))
    return failure(Code::invalid_group, "conversation group is incomplete",
                   group.run_id);
  if (!add_tokens(state.history_tokens, tokens))
    return failure(Code::token_overflow,
                   "conversation history estimate overflowed");
  state.group_tokens.push_back(tokens);
  return {};
}

struct CapacityBudget {
  std::uint64_t reserved_input{};
  std::uint64_t available_history{};
};

auto capacity_budget(const ConversationSelectionRequest& request)
    -> std::expected<CapacityBudget, ConversationSelectionError> {
  if (request.mode != ConversationSelectionMode::full &&
      request.mode != ConversationSelectionMode::rolling)
    return failure(Code::invalid_mode, "unknown conversation selection mode");
  if (request.groups.size() > request.limits.maximum_groups ||
      request.pinned_run_ids.size() > request.limits.maximum_pins)
    return failure(Code::resource_exhausted,
                   "conversation selection input limit exceeded");
  if (request.capacity.context_window_tokens == 0)
    return failure(Code::invalid_capacity, "context capacity must be positive");
  auto reserved = request.capacity.reserved_input_tokens;
  if (!add_tokens(reserved, request.mandatory_input_tokens))
    return failure(Code::token_overflow, "mandatory input estimate overflowed");
  auto occupied = reserved;
  if (!add_tokens(occupied, request.capacity.reserved_output_tokens))
    return failure(Code::token_overflow,
                   "reserved capacity estimate overflowed");
  if (occupied > request.capacity.context_window_tokens)
    return failure(Code::mandatory_capacity_exceeded,
                   "mandatory input exceeds context capacity");
  return CapacityBudget{reserved,
                        request.capacity.context_window_tokens - occupied};
}

auto select_recent(const ValidationState& state, std::uint64_t available,
                   std::uint64_t selected, std::vector<Decision>& decisions)
    -> Status {
  for (auto index = decisions.size(); index > 0; --index) {
    if (state.stop.stop_requested())
      return failure(Code::cancelled, "selection cancelled");
    if (decisions[index - 1] == Decision::admitted_pin) continue;
    if (state.group_tokens[index - 1] > available - selected) {
      decisions[index - 1] = Decision::omitted_capacity;
      break;
    }
    decisions[index - 1] = Decision::admitted_recent;
    selected += state.group_tokens[index - 1];
  }
  return {};
}

auto select_groups(const ConversationSelectionRequest& request,
                   const ValidationState& state, std::uint64_t available)
    -> std::expected<std::vector<Decision>, ConversationSelectionError> {
  std::set<RunId> pins;
  for (const auto& pin : request.pinned_run_ids) {
    if (state.stop.stop_requested())
      return failure(Code::cancelled, "selection cancelled");
    if (!state.runs.contains(pin) || !pins.insert(pin).second)
      return failure(Code::invalid_pin,
                     "conversation pin must identify one distinct known group",
                     pin);
  }
  std::vector<Decision> decisions(request.groups.size(),
                                  Decision::omitted_older);
  std::uint64_t selected{};
  for (std::size_t index = 0; index < request.groups.size(); ++index) {
    if (pins.contains(request.groups[index].run_id)) {
      // Subset of the checked total history estimate.
      selected += state.group_tokens[index];
      decisions[index] = Decision::admitted_pin;
    }
  }
  if (selected > available)
    return failure(Code::mandatory_capacity_exceeded,
                   "pinned conversation exceeds context capacity");
  if (request.mode == ConversationSelectionMode::full) {
    if (state.history_tokens > available)
      return failure(Code::history_capacity_exceeded,
                     "full conversation exceeds context capacity");
    for (auto& decision : decisions)
      if (decision != Decision::admitted_pin)
        decision = Decision::admitted_full;
    return decisions;
  }
  const auto recent = select_recent(state, available, selected, decisions);
  if (!recent) return std::unexpected(recent.error());
  return decisions;
}

auto build_result(const ConversationSelectionRequest& request,
                  const ValidationState& state, const CapacityBudget& capacity,
                  const std::vector<Decision>& decisions)
    -> std::expected<ConversationSelectionResult, ConversationSelectionError> {
  ConversationSelectionResult result;
  result.available_history_tokens = capacity.available_history;
  for (std::size_t index = 0; index < request.groups.size(); ++index) {
    if (state.stop.stop_requested())
      return failure(Code::cancelled, "selection cancelled");
    const auto& group = request.groups[index];
    result.decisions.push_back({group.run_id, decisions[index],
                                group.entries.size(),
                                state.group_tokens[index]});
    if (decisions[index] == Decision::omitted_capacity ||
        decisions[index] == Decision::omitted_older) {
      ++result.omitted_group_count;
      result.omitted_entry_count += group.entries.size();
    } else {
      result.selected_groups.push_back(group);
      result.selected_entry_count += group.entries.size();
      result.selected_history_tokens += state.group_tokens[index];
    }
  }
  result.omitted_history_tokens =
      state.history_tokens - result.selected_history_tokens;
  result.estimated_input_tokens =
      capacity.reserved_input + result.selected_history_tokens;
  return result;
}

} // namespace

auto select_conversation(const ConversationSelectionRequest& request,
                         std::stop_token stop)
    -> std::expected<ConversationSelectionResult, ConversationSelectionError> {
  try {
    if (stop.stop_requested())
      return failure(Code::cancelled, "selection cancelled");
    const auto capacity = capacity_budget(request);
    if (!capacity) return std::unexpected(capacity.error());
    ValidationState state{request.limits, stop};
    for (const auto& group : request.groups) {
      if (stop.stop_requested())
        return failure(Code::cancelled, "selection cancelled");
      const auto valid = validate_group(group, state);
      if (!valid) return std::unexpected(valid.error());
    }
    const auto decisions =
        select_groups(request, state, capacity->available_history);
    if (!decisions) return std::unexpected(decisions.error());
    return build_result(request, state, *capacity, *decisions);
  } catch (...) {
    return failure(Code::internal_failure, "conversation selection failed");
  }
}

} // namespace aiforge::runtime
