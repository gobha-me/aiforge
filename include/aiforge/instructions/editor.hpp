#pragma once

#include <expected>
#include <optional>
#include <stop_token>
#include <string>

#include <aiforge/instructions/source.hpp>

namespace aiforge::instructions {

struct UserGlobalInstructionWrite {
  // Missing means the canonical document must not exist. An engaged reference
  // is an exact digest precondition for replacement.
  std::optional<domain::UserGlobalInstructionReference> expected;
  std::string text;
  UserGlobalInstructionLimits limits{};
  auto operator==(const UserGlobalInstructionWrite&) const -> bool = default;
};

struct UserGlobalInstructionWriteReceipt {
  std::optional<domain::UserGlobalInstructionReference> previous;
  domain::UserGlobalInstructionReference resulting;
  auto operator==(const UserGlobalInstructionWriteReceipt&) const
      -> bool = default;
};

enum class UserGlobalInstructionEditorErrorCode {
  invalid_request,
  malformed_text,
  already_exists,
  not_found,
  source_mismatch,
  concurrent_change,
  path_escape,
  unsupported_entry,
  resource_exhausted,
  permission_denied,
  durability_failure,
  cancelled,
  io_failure,
  internal_failure,
};

struct UserGlobalInstructionEditorError {
  UserGlobalInstructionEditorErrorCode code{
      UserGlobalInstructionEditorErrorCode::internal_failure};
  std::string message;
  std::optional<domain::UserGlobalInstructionReference> observed;
  bool retryable{};
  // Publication may have completed. Callers must reload before retrying or
  // changing live state when this is true.
  bool may_have_applied{};
  auto operator==(const UserGlobalInstructionEditorError&) const
      -> bool = default;
};

class UserGlobalInstructionEditor {
 public:
  virtual ~UserGlobalInstructionEditor() = default;

  [[nodiscard]] virtual auto write(UserGlobalInstructionWrite request,
                                   std::stop_token stop_token = {})
      -> std::expected<UserGlobalInstructionWriteReceipt,
                       UserGlobalInstructionEditorError> = 0;
};

[[nodiscard]] auto prepare_user_global_instruction_write(
    const UserGlobalInstructionWrite& request)
    -> std::expected<domain::UserGlobalInstructionDocument,
                     UserGlobalInstructionEditorError>;
[[nodiscard]] auto validate_user_global_instruction_write_receipt(
    const UserGlobalInstructionWrite& request,
    const UserGlobalInstructionWriteReceipt& receipt)
    -> std::expected<void, UserGlobalInstructionEditorError>;

} // namespace aiforge::instructions
