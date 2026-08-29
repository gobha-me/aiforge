#include <aiforge/runtime/context_builder.hpp>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace aiforge::runtime {
namespace {

using namespace domain;

[[nodiscard]] auto error(const ContextBuildErrorCode code, std::string message,
                         std::optional<ContextEntryId> entry_id = std::nullopt)
    -> std::unexpected<ContextBuildError> {
  return std::unexpected(
      ContextBuildError{code, std::move(message), std::move(entry_id)});
}

[[nodiscard]] auto layer_rank(const InstructionLayer layer) -> std::uint32_t {
  switch (layer) {
    case InstructionLayer::application_runtime: return 0;
    case InstructionLayer::workspace: return 1;
    case InstructionLayer::project: return 2;
    case InstructionLayer::persona: return 3;
    case InstructionLayer::session: return 4;
    case InstructionLayer::task: return 5;
    case InstructionLayer::unknown:
      return std::numeric_limits<std::uint32_t>::max();
  }
  return std::numeric_limits<std::uint32_t>::max();
}

[[nodiscard]] auto valid_provenance(const ContextProvenance& provenance)
    -> bool {
  return (!provenance.source_location ||
          !provenance.source_location->empty()) &&
         (!provenance.digest || !provenance.digest->empty());
}

[[nodiscard]] auto contains_unknown(const Message& message) -> bool {
  return std::ranges::any_of(message.content, [](const ContentBlock& block) {
    return std::holds_alternative<UnknownContentBlock>(block);
  });
}

[[nodiscard]] auto has_control_character(const std::string_view value) -> bool {
  return std::ranges::any_of(value, [](const unsigned char character) {
    return character < 0x20U || character == 0x7FU;
  });
}

[[nodiscard]] auto add_checked(std::uint64_t& total, const std::uint64_t value)
    -> bool {
  if (value > std::numeric_limits<std::uint64_t>::max() - total) return false;
  total += value;
  return true;
}

[[nodiscard]] auto instruction_before(const ContextEntry& left,
                                      const ContextEntry& right) -> bool {
  const auto left_rank = layer_rank(*left.instruction_layer);
  const auto right_rank = layer_rank(*right.instruction_layer);
  if (left_rank != right_rank) return left_rank < right_rank;
  if (left.specificity != right.specificity)
    return left.specificity < right.specificity;
  if (left.order != right.order) return left.order < right.order;
  return left.entry_id < right.entry_id;
}

[[nodiscard]] auto content_before(const ContextEntry& left,
                                  const ContextEntry& right) -> bool {
  if (left.order != right.order) return left.order < right.order;
  return left.entry_id < right.entry_id;
}

} // namespace

auto ContextBuilder::build(ContextBuildInput input) const
    -> std::expected<ConstructedContext, ContextBuildError> {
  if (input.capacity.context_window_tokens == 0) {
    return error(ContextBuildErrorCode::invalid_capacity,
                 "context window capacity must be positive");
  }

  std::uint64_t reserved = input.capacity.reserved_output_tokens;
  if (!add_checked(reserved, input.capacity.reserved_input_tokens) ||
      reserved > input.capacity.context_window_tokens) {
    return error(ContextBuildErrorCode::invalid_capacity,
                 "reserved input and output exceed the context window");
  }

  std::set<ContextEntryId> entry_ids;
  std::set<MessageId> message_ids;
  std::map<ContextEntryId, const InstructionInput*> instructions_by_id;
  bool has_runtime_instruction{};
  for (const auto& instruction : input.instructions) {
    if (!entry_ids.insert(instruction.entry_id).second) {
      return error(ContextBuildErrorCode::duplicate_entry_id,
                   "context entry IDs must be unique", instruction.entry_id);
    }
    instructions_by_id.emplace(instruction.entry_id, &instruction);
    if (!valid_provenance(instruction.provenance)) {
      return error(ContextBuildErrorCode::invalid_provenance,
                   "instruction provenance contains an empty value",
                   instruction.entry_id);
    }
    if (instruction.order == 0) {
      return error(ContextBuildErrorCode::invalid_instruction,
                   "instruction order must be positive", instruction.entry_id);
    }
    if (instruction.layer == InstructionLayer::unknown ||
        instruction.operation == InstructionOperation::unknown) {
      return error(ContextBuildErrorCode::invalid_instruction,
                   "unknown instruction layer or operation is unsupported",
                   instruction.entry_id);
    }
    if (instruction.layer != InstructionLayer::project &&
        instruction.specificity != 0) {
      return error(ContextBuildErrorCode::invalid_instruction,
                   "only project instructions may have subtree specificity",
                   instruction.entry_id);
    }
    if (instruction.layer == InstructionLayer::application_runtime &&
        instruction.operation != InstructionOperation::add) {
      return error(
          ContextBuildErrorCode::runtime_instruction_mutation,
          "application runtime instructions cannot be replaced or disabled",
          instruction.entry_id);
    }
    has_runtime_instruction =
        has_runtime_instruction ||
        instruction.layer == InstructionLayer::application_runtime;

    const bool adds_message =
        instruction.operation != InstructionOperation::disable;
    const bool targets_entry =
        instruction.operation != InstructionOperation::add;
    if (adds_message != instruction.message.has_value() ||
        targets_entry != instruction.target_entry_id.has_value() ||
        (adds_message && instruction.estimated_tokens == 0) ||
        (!adds_message && instruction.estimated_tokens != 0)) {
      return error(ContextBuildErrorCode::invalid_instruction,
                   "instruction operation has inconsistent message, target, or "
                   "size fields",
                   instruction.entry_id);
    }
    if (instruction.message) {
      if (instruction.message->role != Role::system ||
          instruction.message->invocation_id ||
          !instruction.message->tool_calls.empty() ||
          instruction.message->content.empty()) {
        return error(ContextBuildErrorCode::invalid_instruction,
                     "instruction messages must be nonempty system messages "
                     "without an invocation",
                     instruction.entry_id);
      }
      if (contains_unknown(*instruction.message)) {
        return error(
            ContextBuildErrorCode::unsupported_content,
            "unknown instruction content cannot enter a backend request",
            instruction.entry_id);
      }
      if (!message_ids.insert(instruction.message->message_id).second) {
        return error(ContextBuildErrorCode::duplicate_message_id,
                     "context message IDs must be unique",
                     instruction.entry_id);
      }
    }
  }

  std::set<InvocationId> tool_call_ids;
  for (const auto& content : input.content) {
    if (!entry_ids.insert(content.entry_id).second) {
      return error(ContextBuildErrorCode::duplicate_entry_id,
                   "context entry IDs must be unique", content.entry_id);
    }
    if (!message_ids.insert(content.message.message_id).second) {
      return error(ContextBuildErrorCode::duplicate_message_id,
                   "context message IDs must be unique", content.entry_id);
    }
    if (!valid_provenance(content.provenance)) {
      return error(ContextBuildErrorCode::invalid_provenance,
                   "content provenance contains an empty value",
                   content.entry_id);
    }
    if (content.order == 0 || content.estimated_tokens == 0 ||
        (content.message.content.empty() &&
         content.message.tool_calls.empty())) {
      return error(ContextBuildErrorCode::invalid_content,
                   "context content must have positive order and size and "
                   "nonempty blocks",
                   content.entry_id);
    }
    if (contains_unknown(content.message)) {
      return error(ContextBuildErrorCode::unsupported_content,
                   "unknown content cannot enter a backend request",
                   content.entry_id);
    }
    if (std::ranges::any_of(
            content.message.tool_calls, [&](const ToolCall& call) {
              return !tool_call_ids.insert(call.invocation_id).second ||
                     call.tool_name.empty() || call.tool_name.size() > 128 ||
                     has_control_character(call.tool_name) ||
                     call.arguments.media_type != "application/json" ||
                     call.arguments.data.empty();
            })) {
      return error(ContextBuildErrorCode::invalid_content,
                   "assistant tool calls are malformed or ambiguous",
                   content.entry_id);
    }
    const bool role_is_valid =
        (content.kind == ContextContentKind::conversation &&
         ((content.message.role == Role::user &&
           content.message.tool_calls.empty()) ||
          content.message.role == Role::assistant) &&
         !content.message.invocation_id) ||
        (content.kind == ContextContentKind::evidence &&
         content.message.role == Role::evidence &&
         !content.message.invocation_id &&
         content.message.tool_calls.empty()) ||
        (content.kind == ContextContentKind::tool_result &&
         content.message.role == Role::tool && content.message.invocation_id &&
         content.message.tool_calls.empty());
    if (!role_is_valid) {
      return error(content.kind == ContextContentKind::unknown
                       ? ContextBuildErrorCode::unsupported_content
                       : ContextBuildErrorCode::invalid_content,
                   "context content kind and message role are inconsistent",
                   content.entry_id);
    }
  }
  if (!has_runtime_instruction) {
    return error(
        ContextBuildErrorCode::missing_runtime_instruction,
        "constructed context requires an application runtime instruction");
  }

  std::vector<const InstructionInput*> operations;
  operations.reserve(input.instructions.size());
  for (const auto& instruction : input.instructions)
    operations.push_back(&instruction);
  std::ranges::sort(operations, [](const InstructionInput* left,
                                   const InstructionInput* right) {
    if (left->order != right->order) return left->order < right->order;
    return left->entry_id < right->entry_id;
  });

  std::map<ContextEntryId, ContextEntry> active_instructions;
  std::map<ContextEntryId, ContextDecisionRecord> decisions;
  for (const auto* instruction : operations) {
    if (instruction->operation != InstructionOperation::add) {
      const auto target_definition =
          instructions_by_id.find(*instruction->target_entry_id);
      if (target_definition == instructions_by_id.end()) {
        return error(ContextBuildErrorCode::unknown_target,
                     "instruction replacement target does not exist",
                     instruction->entry_id);
      }
      if (target_definition->second->order >= instruction->order) {
        return error(
            ContextBuildErrorCode::target_not_earlier,
            "instruction replacement target must precede its operation",
            instruction->entry_id);
      }
      if (target_definition->second->layer != instruction->layer) {
        return error(
            ContextBuildErrorCode::cross_layer_replacement,
            "instruction operations cannot affect another authority layer",
            instruction->entry_id);
      }
      const auto active =
          active_instructions.find(*instruction->target_entry_id);
      if (active == active_instructions.end()) {
        return error(ContextBuildErrorCode::unknown_target,
                     "instruction replacement target is no longer active",
                     instruction->entry_id);
      }
      decisions.insert_or_assign(
          active->first,
          ContextDecisionRecord{active->first,
                                instruction->operation ==
                                        InstructionOperation::replace
                                    ? ContextDecision::superseded
                                    : ContextDecision::disabled,
                                instruction->entry_id});
      active_instructions.erase(active);
    }

    if (instruction->operation == InstructionOperation::disable) {
      decisions.insert_or_assign(
          instruction->entry_id,
          ContextDecisionRecord{instruction->entry_id,
                                ContextDecision::disabled,
                                instruction->target_entry_id});
      continue;
    }

    active_instructions.emplace(
        instruction->entry_id,
        ContextEntry{instruction->entry_id, ContextEntryKind::instruction,
                     instruction->layer, *instruction->message,
                     instruction->provenance, instruction->specificity,
                     instruction->order, instruction->estimated_tokens});
    decisions.insert_or_assign(instruction->entry_id,
                               ContextDecisionRecord{instruction->entry_id,
                                                     ContextDecision::admitted,
                                                     std::nullopt});
  }

  std::vector<ContextEntry> instruction_entries;
  instruction_entries.reserve(active_instructions.size());
  for (auto& [entry_id, entry] : active_instructions) {
    static_cast<void>(entry_id);
    instruction_entries.push_back(std::move(entry));
  }
  std::ranges::sort(instruction_entries, instruction_before);

  std::vector<ContextEntry> content_entries;
  content_entries.reserve(input.content.size());
  for (auto& content : input.content) {
    ContextEntryKind kind{};
    switch (content.kind) {
      case ContextContentKind::conversation:
        kind = ContextEntryKind::conversation;
        break;
      case ContextContentKind::evidence:
        kind = ContextEntryKind::evidence;
        break;
      case ContextContentKind::tool_result:
        kind = ContextEntryKind::tool_result;
        break;
      case ContextContentKind::unknown:
        return error(
            ContextBuildErrorCode::unsupported_content,
            "unknown context content kind cannot enter a backend request",
            content.entry_id);
    }
    content_entries.push_back(
        ContextEntry{content.entry_id, kind, std::nullopt,
                     std::move(content.message), std::move(content.provenance),
                     0, content.order, content.estimated_tokens});
    decisions.insert_or_assign(content.entry_id,
                               ContextDecisionRecord{content.entry_id,
                                                     ContextDecision::admitted,
                                                     std::nullopt});
  }
  std::ranges::sort(content_entries, content_before);

  std::uint64_t estimated_input = input.capacity.reserved_input_tokens;
  for (const auto& entry : instruction_entries) {
    if (!add_checked(estimated_input, entry.estimated_tokens)) {
      return error(ContextBuildErrorCode::token_overflow,
                   "context token estimate overflowed", entry.entry_id);
    }
  }
  for (const auto& entry : content_entries) {
    if (!add_checked(estimated_input, entry.estimated_tokens)) {
      return error(ContextBuildErrorCode::token_overflow,
                   "context token estimate overflowed", entry.entry_id);
    }
  }
  if (estimated_input > input.capacity.context_window_tokens -
                            input.capacity.reserved_output_tokens) {
    return error(ContextBuildErrorCode::capacity_exceeded,
                 "preselected context exceeds the target model capacity");
  }

  instruction_entries.insert(instruction_entries.end(),
                             std::make_move_iterator(content_entries.begin()),
                             std::make_move_iterator(content_entries.end()));
  std::vector<ContextDecisionRecord> decision_records;
  decision_records.reserve(decisions.size());
  for (auto& [entry_id, decision] : decisions) {
    static_cast<void>(entry_id);
    decision_records.push_back(std::move(decision));
  }

  return ConstructedContext{std::move(instruction_entries),
                            std::move(decision_records), input.capacity,
                            estimated_input};
}

} // namespace aiforge::runtime
