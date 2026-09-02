#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace aiforge::detail {

[[nodiscard]] constexpr auto is_unsafe_text_control(
    const std::uint32_t codepoint) noexcept -> bool {
  if ((codepoint <= 0x1fU && codepoint != '\n' && codepoint != '\r' &&
       codepoint != '\t') ||
      (codepoint >= 0x7fU && codepoint <= 0x9fU)) {
    return true;
  }

  // Unicode format controls can make reviewed text render differently from
  // the bytes that are saved. Personas intentionally reject them rather than
  // trying to assign display semantics to invisible or bidi-affecting input.
  return codepoint == 0x00adU ||
         (codepoint >= 0x0600U && codepoint <= 0x0605U) ||
         codepoint == 0x061cU || codepoint == 0x06ddU || codepoint == 0x070fU ||
         (codepoint >= 0x0890U && codepoint <= 0x0891U) ||
         codepoint == 0x08e2U || codepoint == 0x180eU ||
         (codepoint >= 0x200bU && codepoint <= 0x200fU) ||
         (codepoint >= 0x202aU && codepoint <= 0x202eU) ||
         (codepoint >= 0x2060U && codepoint <= 0x2064U) ||
         (codepoint >= 0x2066U && codepoint <= 0x206fU) ||
         codepoint == 0xfeffU ||
         (codepoint >= 0xfff9U && codepoint <= 0xfffbU) ||
         codepoint == 0x110bdU || codepoint == 0x110cdU ||
         (codepoint >= 0x13430U && codepoint <= 0x1343fU) ||
         (codepoint >= 0x1bca0U && codepoint <= 0x1bca3U) ||
         (codepoint >= 0x1d173U && codepoint <= 0x1d17aU) ||
         codepoint == 0xe0001U ||
         (codepoint >= 0xe0020U && codepoint <= 0xe007fU);
}

struct Utf8Codepoint {
  std::uint32_t value{};
  std::size_t bytes{};
};

[[nodiscard]] constexpr auto decode_utf8_codepoint(
    const std::string_view value, const std::size_t index) noexcept
    -> std::optional<Utf8Codepoint> {
  const auto first = static_cast<unsigned char>(value[index]);
  if (first <= 0x7fU) return Utf8Codepoint{first, 1};

  Utf8Codepoint decoded;
  if (first >= 0xc2U && first <= 0xdfU) {
    decoded = {static_cast<std::uint32_t>(first & 0x1fU), 2};
  } else if (first >= 0xe0U && first <= 0xefU) {
    decoded = {static_cast<std::uint32_t>(first & 0x0fU), 3};
  } else if (first >= 0xf0U && first <= 0xf4U) {
    decoded = {static_cast<std::uint32_t>(first & 0x07U), 4};
  } else {
    return std::nullopt;
  }
  if (decoded.bytes > value.size() - index) return std::nullopt;
  for (std::size_t offset = 1; offset < decoded.bytes; ++offset) {
    const auto next = static_cast<unsigned char>(value[index + offset]);
    if ((next & 0xc0U) != 0x80U) return std::nullopt;
    decoded.value = (decoded.value << 6U) | (next & 0x3fU);
  }
  if ((decoded.bytes == 3 && decoded.value < 0x800U) ||
      (decoded.bytes == 4 && decoded.value < 0x10000U) ||
      (decoded.value >= 0xd800U && decoded.value <= 0xdfffU) ||
      decoded.value > 0x10ffffU) {
    return std::nullopt;
  }
  return decoded;
}

[[nodiscard]] constexpr auto is_safe_utf8_text(
    const std::string_view value) noexcept -> bool {
  if (value.empty()) return false;
  std::size_t index{};
  while (index < value.size()) {
    const auto decoded = decode_utf8_codepoint(value, index);
    if (!decoded || is_unsafe_text_control(decoded->value)) return false;
    index += decoded->bytes;
  }
  return true;
}

} // namespace aiforge::detail
