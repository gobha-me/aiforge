#pragma once

#include <cstdint>
#include <expected>
#include <string>

#include <aiforge/domain/context.hpp>
#include <aiforge/domain/user_global_instruction.hpp>

namespace aiforge::runtime {

enum class UserGlobalInstructionContextErrorCode {
  invalid_document,
  invalid_estimate,
  invalid_identity,
  internal_failure,
};

struct UserGlobalInstructionContextError {
  UserGlobalInstructionContextErrorCode code{
      UserGlobalInstructionContextErrorCode::internal_failure};
  std::string message;
  auto operator==(const UserGlobalInstructionContextError&) const
      -> bool = default;
};

[[nodiscard]] auto user_global_instruction_input(
    const domain::UserGlobalInstructionDocument& document,
    std::uint64_t estimated_tokens, std::uint64_t order = 1)
    -> std::expected<domain::InstructionInput,
                     UserGlobalInstructionContextError>;

} // namespace aiforge::runtime
