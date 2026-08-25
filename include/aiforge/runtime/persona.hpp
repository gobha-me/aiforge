#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>

#include <aiforge/domain/context.hpp>
#include <aiforge/domain/event_log.hpp>
#include <aiforge/domain/persona.hpp>

namespace aiforge::runtime {

enum class PersonaContextErrorCode {
  invalid_document,
  invalid_estimate,
  invalid_identity,
  invalid_history,
  internal_failure,
};

struct PersonaContextError {
  PersonaContextErrorCode code{PersonaContextErrorCode::internal_failure};
  std::string message;
  auto operator==(const PersonaContextError&) const -> bool = default;
};

[[nodiscard]] auto persona_instruction_input(
    const domain::PersonaDocument& document, std::uint64_t estimated_tokens,
    std::uint64_t order = 1)
    -> std::expected<domain::InstructionInput, PersonaContextError>;

[[nodiscard]] auto latest_persona_selection(
    const domain::SessionEventLog& event_log)
    -> std::expected<std::optional<domain::PersonaSelection>,
                     PersonaContextError>;

}  // namespace aiforge::runtime
