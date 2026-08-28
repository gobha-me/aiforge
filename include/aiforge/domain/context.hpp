#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <aiforge/domain/content.hpp>
#include <aiforge/domain/ids.hpp>

namespace aiforge::domain {

enum class InstructionLayer {
  application_runtime,
  workspace,
  project,
  persona,
  session,
  task,
  unknown,
};

enum class InstructionOperation {
  add,
  replace,
  disable,
  unknown,
};

enum class ContextContentKind {
  conversation,
  evidence,
  tool_result,
  unknown,
};

struct ContextProvenance {
  ContextSourceId source_id;
  std::optional<std::string> source_location;
  std::optional<std::string> digest;
  auto operator==(const ContextProvenance&) const -> bool = default;
};

struct InstructionInput {
  ContextEntryId entry_id;
  InstructionLayer layer;
  InstructionOperation operation{InstructionOperation::add};
  std::optional<ContextEntryId> target_entry_id;
  std::optional<Message> message;
  ContextProvenance provenance;
  // Project specificity starts at zero for the repository root and increases
  // toward the applicable target subtree. Other layers require zero.
  std::uint32_t specificity{};
  // Order is a positive, caller-supplied stable sequence, not container order.
  std::uint64_t order{};
  // Estimates use the actual target model's token accounting.
  std::uint64_t estimated_tokens{};
  auto operator==(const InstructionInput&) const -> bool = default;
};

struct ContextContentInput {
  ContextEntryId entry_id;
  ContextContentKind kind{ContextContentKind::conversation};
  Message message;
  ContextProvenance provenance;
  std::uint64_t order{};
  std::uint64_t estimated_tokens{};
  auto operator==(const ContextContentInput&) const -> bool = default;
};

struct ContextCapacity {
  std::uint64_t context_window_tokens{};
  std::uint64_t reserved_output_tokens{};
  // Includes tool schemas and other request input outside the entries below.
  std::uint64_t reserved_input_tokens{};
  auto operator==(const ContextCapacity&) const -> bool = default;
};

struct ContextBuildInput {
  ContextCapacity capacity;
  std::vector<InstructionInput> instructions;
  std::vector<ContextContentInput> content;
  auto operator==(const ContextBuildInput&) const -> bool = default;
};

enum class ContextEntryKind {
  instruction,
  conversation,
  evidence,
  tool_result,
};

struct ContextEntry {
  ContextEntryId entry_id;
  ContextEntryKind kind;
  std::optional<InstructionLayer> instruction_layer;
  Message message;
  ContextProvenance provenance;
  std::uint32_t specificity{};
  std::uint64_t order{};
  std::uint64_t estimated_tokens{};
  auto operator==(const ContextEntry&) const -> bool = default;
};

enum class ContextDecision {
  admitted,
  superseded,
  disabled,
};

struct ContextDecisionRecord {
  ContextEntryId entry_id;
  ContextDecision decision;
  std::optional<ContextEntryId> decided_by_entry_id;
  auto operator==(const ContextDecisionRecord&) const -> bool = default;
};

struct ConstructedContext {
  // Only the ContextBuilder validates the invariants required by a backend.
  std::vector<ContextEntry> entries;
  std::vector<ContextDecisionRecord> decisions;
  ContextCapacity capacity;
  std::uint64_t estimated_input_tokens{};
  auto operator==(const ConstructedContext&) const -> bool = default;
};

} // namespace aiforge::domain
