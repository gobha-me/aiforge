#pragma once

#include <cstddef>
#include <expected>
#include <optional>
#include <stop_token>
#include <string>

#include <aiforge/domain/user_global_instruction.hpp>

namespace aiforge::instructions {

struct UserGlobalInstructionLimits {
  std::size_t maximum_file_bytes{std::size_t{1024} * 1024U};
  auto operator==(const UserGlobalInstructionLimits&) const -> bool = default;
};

enum class UserGlobalInstructionErrorCode {
  invalid_request,
  missing_home,
  invalid_root,
  path_escape,
  unsupported_entry,
  malformed_text,
  unstable,
  resource_exhausted,
  permission_denied,
  cancelled,
  io_failure,
  internal_failure,
};

struct UserGlobalInstructionError {
  UserGlobalInstructionErrorCode code{
      UserGlobalInstructionErrorCode::internal_failure};
  std::string message;
  bool retryable{};
  auto operator==(const UserGlobalInstructionError&) const -> bool = default;
};

class UserGlobalInstructionSource {
 public:
  virtual ~UserGlobalInstructionSource() = default;

  [[nodiscard]] virtual auto load(UserGlobalInstructionLimits limits = {},
                                  std::stop_token stop_token = {})
      -> std::expected<std::optional<domain::UserGlobalInstructionDocument>,
                       UserGlobalInstructionError> = 0;
};

} // namespace aiforge::instructions
