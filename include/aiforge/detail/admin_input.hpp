#pragma once

#include <algorithm>
#include <string_view>

namespace aiforge::detail {
[[nodiscard]] inline auto admin_alphanumeric(char byte) noexcept -> bool {
  return (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9');
}
[[nodiscard]] inline auto valid_admin_target(std::string_view value) noexcept
    -> bool {
  return !value.empty() && value.size() <= 64 &&
         admin_alphanumeric(value.front()) &&
         admin_alphanumeric(value.back()) &&
         std::ranges::all_of(value, [](char byte) {
           return admin_alphanumeric(byte) || byte == '-' || byte == '_';
         });
}
[[nodiscard]] inline auto valid_admin_service(std::string_view value) noexcept
    -> bool {
  return value.size() >= 9 && value.size() <= 255 && value.front() != '-' &&
         value.ends_with(".service") &&
         std::ranges::all_of(value, [](char byte) {
           return admin_alphanumeric(byte) || (byte >= 'A' && byte <= 'Z') ||
                  byte == '_' || byte == '-' || byte == '.' || byte == '@' ||
                  byte == ':';
         });
}
} // namespace aiforge::detail
