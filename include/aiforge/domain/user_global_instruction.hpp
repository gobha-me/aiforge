#pragma once

#include <expected>
#include <string>
#include <string_view>

#include <aiforge/domain/context.hpp>
#include <aiforge/domain/digest.hpp>

namespace aiforge::domain {

inline constexpr std::string_view user_global_instruction_source_identity{
    "aiforge:user-global-instructions"};
inline constexpr std::string_view user_global_instruction_source_location{
    "instructions/global.md"};

struct UserGlobalInstructionReference {
  ContextSourceId source_id;
  std::string source_location;
  ContentDigest content_digest;
  auto operator==(const UserGlobalInstructionReference&) const
      -> bool = default;
};

struct UserGlobalInstructionDocument {
  UserGlobalInstructionReference reference;
  std::string text;
  auto operator==(const UserGlobalInstructionDocument&) const -> bool = default;
};

enum class UserGlobalInstructionValidationErrorCode {
  invalid_identity,
  invalid_location,
  invalid_digest,
  invalid_document,
};

struct UserGlobalInstructionValidationError {
  UserGlobalInstructionValidationErrorCode code{
      UserGlobalInstructionValidationErrorCode::invalid_document};
  std::string message;
  auto operator==(const UserGlobalInstructionValidationError&) const
      -> bool = default;
};

[[nodiscard]] auto validate_user_global_instruction_reference(
    const UserGlobalInstructionReference& reference)
    -> std::expected<void, UserGlobalInstructionValidationError>;
[[nodiscard]] auto validate_user_global_instruction_document(
    const UserGlobalInstructionDocument& document)
    -> std::expected<void, UserGlobalInstructionValidationError>;

} // namespace aiforge::domain
