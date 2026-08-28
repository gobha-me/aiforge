#pragma once

#include <expected>
#include <optional>
#include <string>

#include <aiforge/domain/context.hpp>
#include <aiforge/runtime/context_selection.hpp>

namespace aiforge::runtime {

enum class ContextBuildErrorCode {
  invalid_capacity,
  missing_runtime_instruction,
  duplicate_entry_id,
  duplicate_message_id,
  invalid_provenance,
  invalid_instruction,
  invalid_content,
  unknown_target,
  target_not_earlier,
  cross_layer_replacement,
  runtime_instruction_mutation,
  unsupported_content,
  token_overflow,
  capacity_exceeded,
};

struct ContextBuildError {
  ContextBuildErrorCode code;
  std::string message;
  std::optional<domain::ContextEntryId> entry_id;
  auto operator==(const ContextBuildError&) const -> bool = default;
};

class ContextBuilder final {
 public:
  [[nodiscard]] auto build(domain::ContextBuildInput input) const
      -> std::expected<domain::ConstructedContext, ContextBuildError>;
  [[nodiscard]] auto select_and_build(ContextSelectionRequest request) const
      -> std::expected<ContextSelectionResult, ContextSelectionError>;
};

} // namespace aiforge::runtime
