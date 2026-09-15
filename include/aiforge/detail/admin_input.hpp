#pragma once

#include <aiforge/detail/utf8_text.hpp>
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
[[nodiscard]] inline auto valid_admin_pod(std::string_view value) noexcept
    -> bool {
  if (value.empty() || value.size() > 253) return false;
  while (true) {
    const auto dot = value.find('.');
    const auto label = value.substr(0, dot);
    if (label.empty() || label.size() > 63 ||
        !admin_alphanumeric(label.front()) ||
        !admin_alphanumeric(label.back()) ||
        !std::ranges::all_of(label, [](char byte) {
          return admin_alphanumeric(byte) || byte == '-';
        }))
      return false;
    if (dot == std::string_view::npos) return true;
    value.remove_prefix(dot + 1);
  }
}
[[nodiscard]] inline auto valid_admin_resource_uid(
    std::string_view value) noexcept -> bool {
  return !value.empty() && value.size() <= 128 && is_safe_utf8_text(value) &&
         std::ranges::all_of(value, [](unsigned char byte) {
           return byte >= 32 && byte != 127;
         });
}
} // namespace aiforge::detail
