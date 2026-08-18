#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace aiforge::presentation {

enum class TextSemantic : std::uint8_t {
  none = 0,
  strong = 1U << 0U,
  emphasis = 1U << 1U,
  code = 1U << 2U,
  heading = 1U << 3U,
  list_marker = 1U << 4U,
};

[[nodiscard]] constexpr auto operator|(const TextSemantic left,
                                       const TextSemantic right)
    -> TextSemantic {
  return static_cast<TextSemantic>(static_cast<std::uint8_t>(left) |
                                   static_cast<std::uint8_t>(right));
}

[[nodiscard]] constexpr auto has_semantic(const TextSemantic value,
                                          const TextSemantic flag) -> bool {
  return (static_cast<std::uint8_t>(value) &
          static_cast<std::uint8_t>(flag)) != 0;
}

struct StyledSpan {
  std::string text;
  TextSemantic semantic{TextSemantic::none};
  auto operator==(const StyledSpan&) const -> bool = default;
};

using StyledLine = std::vector<StyledSpan>;
using StyledDocument = std::vector<StyledLine>;

enum class TextErrorCode {
  input_too_large,
  internal_failure,
};

struct TextError {
  TextErrorCode code;
  std::string message;
  auto operator==(const TextError&) const -> bool = default;
};

[[nodiscard]] auto sanitize_untrusted_text(std::string_view text)
    -> std::expected<std::string, TextError>;

[[nodiscard]] auto tokenize_markdown_lite(
    std::string_view text,
    std::size_t maximum_bytes = 16U * 1024U * 1024U)
    -> std::expected<StyledDocument, TextError>;

[[nodiscard]] auto flatten(const StyledDocument& document)
    -> std::expected<std::string, TextError>;

}  // namespace aiforge::presentation
