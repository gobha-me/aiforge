#include <aiforge/presentation/text.hpp>

#include <algorithm>
#include <cctype>
#include <limits>
#include <optional>
#include <utility>

namespace aiforge::presentation {
namespace {

[[nodiscard]] auto error(const TextErrorCode code, std::string message)
    -> std::unexpected<TextError> {
  return std::unexpected(TextError{code, std::move(message)});
}

auto append_replacement(std::string& output) -> void {
  output.append("\xEF\xBF\xBD");
}

struct Decoded {
  std::uint32_t codepoint{};
  std::size_t bytes{};
};

[[nodiscard]] auto decode(const std::string_view text,
                          const std::size_t position)
    -> std::optional<Decoded> {
  const auto first = static_cast<unsigned char>(text[position]);
  if (first <= 0x7FU) return Decoded{first, 1};

  std::size_t length{};
  std::uint32_t codepoint{};
  if (first >= 0xC2U && first <= 0xDFU) {
    length = 2;
    codepoint = first & 0x1FU;
  } else if (first >= 0xE0U && first <= 0xEFU) {
    length = 3;
    codepoint = first & 0x0FU;
  } else if (first >= 0xF0U && first <= 0xF4U) {
    length = 4;
    codepoint = first & 0x07U;
  } else {
    return std::nullopt;
  }
  if (length > text.size() - position) return std::nullopt;
  for (std::size_t offset = 1; offset < length; ++offset) {
    const auto next = static_cast<unsigned char>(text[position + offset]);
    if ((next & 0xC0U) != 0x80U) return std::nullopt;
    if (offset == 1 &&
        ((first == 0xE0U && next < 0xA0U) || (first == 0xEDU && next > 0x9FU) ||
         (first == 0xF0U && next < 0x90U) ||
         (first == 0xF4U && next > 0x8FU))) {
      return std::nullopt;
    }
    codepoint = (codepoint << 6U) | (next & 0x3FU);
  }
  return Decoded{codepoint, length};
}

[[nodiscard]] auto consume_escape(const std::string_view text,
                                  std::size_t position) -> std::size_t {
  ++position;
  if (position >= text.size()) return position;
  if (text[position] == '[') {
    ++position;
    while (position < text.size()) {
      const auto byte = static_cast<unsigned char>(text[position++]);
      if (byte >= 0x40U && byte <= 0x7EU) break;
    }
    return position;
  }
  if (text[position] == ']') {
    ++position;
    while (position < text.size()) {
      if (text[position] == '\a') return position + 1;
      if (text[position] == '\x1b' && position + 1 < text.size() &&
          text[position + 1] == '\\') {
        return position + 2;
      }
      ++position;
    }
    return position;
  }
  return std::min(text.size(), position + 1);
}

auto append_span(StyledLine& line, std::string text,
                 const TextSemantic semantic) -> void {
  if (text.empty()) return;
  if (!line.empty() && line.back().semantic == semantic) {
    line.back().text += text;
  } else {
    line.push_back(StyledSpan{std::move(text), semantic});
  }
}

[[nodiscard]] auto marker_end(const std::string_view line,
                              const std::size_t position, const char marker,
                              const std::size_t count) -> std::size_t {
  const std::string needle(count, marker);
  auto found = line.find(needle, position);
  while (found != std::string_view::npos) {
    if (found + count <= line.size()) return found;
    found = line.find(needle, found + 1);
  }
  return std::string_view::npos;
}

auto parse_inline(const std::string_view line, StyledLine& output,
                  const TextSemantic base = TextSemantic::none) -> void {
  std::size_t position{};
  std::size_t plain_start{};
  const auto flush_plain = [&](const std::size_t end) {
    if (end > plain_start) {
      append_span(output,
                  std::string{line.substr(plain_start, end - plain_start)},
                  base);
    }
  };

  while (position < line.size()) {
    if (line[position] == '`') {
      std::size_t ticks{1};
      while (position + ticks < line.size() && line[position + ticks] == '`') {
        ++ticks;
      }
      const auto close = marker_end(line, position + ticks, '`', ticks);
      if (close != std::string_view::npos && close > position + ticks) {
        flush_plain(position);
        append_span(output,
                    std::string{line.substr(position + ticks,
                                            close - position - ticks)},
                    base | TextSemantic::code);
        position = close + ticks;
        plain_start = position;
        continue;
      }
    }

    if ((line[position] == '*' || line[position] == '_') &&
        position + 1 < line.size() && line[position + 1] == line[position]) {
      const char marker = line[position];
      const auto close = marker_end(line, position + 2, marker, 2);
      if (close != std::string_view::npos && close > position + 2) {
        flush_plain(position);
        append_span(
            output,
            std::string{line.substr(position + 2, close - position - 2)},
            base | TextSemantic::strong);
        position = close + 2;
        plain_start = position;
        continue;
      }
    }

    if (line[position] == '*' || line[position] == '_') {
      const char marker = line[position];
      const auto close = line.find(marker, position + 1);
      if (close != std::string_view::npos && close > position + 1) {
        flush_plain(position);
        append_span(
            output,
            std::string{line.substr(position + 1, close - position - 1)},
            base | TextSemantic::emphasis);
        position = close + 1;
        plain_start = position;
        continue;
      }
    }
    ++position;
  }
  flush_plain(line.size());
}

[[nodiscard]] auto fence(const std::string_view line) -> bool {
  return line.starts_with("```");
}

} // namespace

auto sanitize_untrusted_text(const std::string_view text)
    -> std::expected<std::string, TextError> {
  try {
    std::string output;
    output.reserve(text.size());
    std::size_t position{};
    while (position < text.size()) {
      const auto byte = static_cast<unsigned char>(text[position]);
      if (byte == 0x1BU) {
        position = consume_escape(text, position);
        continue;
      }
      if (byte == '\r') {
        if (position + 1 < text.size() && text[position + 1] == '\n') {
          ++position;
        }
        output.push_back('\n');
        ++position;
        continue;
      }
      if (byte <= 0x7FU) {
        if (byte == '\n' || byte == '\t' || byte >= 0x20U) {
          if (byte != 0x7FU) output.push_back(static_cast<char>(byte));
        }
        ++position;
        continue;
      }
      const auto decoded = decode(text, position);
      if (!decoded) {
        append_replacement(output);
        ++position;
        continue;
      }
      if (decoded->codepoint < 0x80U ||
          (decoded->codepoint >= 0x80U && decoded->codepoint <= 0x9FU)) {
        position += decoded->bytes;
        continue;
      }
      output.append(text.substr(position, decoded->bytes));
      position += decoded->bytes;
    }
    return output;
  } catch (...) {
    return error(TextErrorCode::internal_failure,
                 "text sanitization failed internally");
  }
}

auto tokenize_markdown_lite(const std::string_view text,
                            const std::size_t maximum_bytes)
    -> std::expected<StyledDocument, TextError> {
  try {
    if (text.size() > maximum_bytes) {
      return error(TextErrorCode::input_too_large,
                   "Markdown input exceeds the configured byte limit");
    }
    auto clean = sanitize_untrusted_text(text);
    if (!clean) return std::unexpected(std::move(clean.error()));

    std::vector<std::string_view> lines;
    std::size_t start{};
    while (start <= clean->size()) {
      const auto end = clean->find('\n', start);
      if (end == std::string::npos) {
        lines.push_back(std::string_view{*clean}.substr(start));
        break;
      }
      lines.push_back(std::string_view{*clean}.substr(start, end - start));
      start = end + 1;
      if (start == clean->size()) {
        lines.emplace_back();
        break;
      }
    }

    StyledDocument document;
    document.reserve(lines.size());
    std::size_t index{};
    while (index < lines.size()) {
      const auto line = lines[index];
      if (fence(line)) {
        std::size_t closing = index + 1;
        while (closing < lines.size() && !fence(lines[closing]))
          ++closing;
        if (closing < lines.size()) {
          for (std::size_t code_line = index + 1; code_line < closing;
               ++code_line) {
            StyledLine rendered;
            append_span(rendered, std::string{lines[code_line]},
                        TextSemantic::code);
            document.push_back(std::move(rendered));
          }
          if (closing == index + 1) document.emplace_back();
          index = closing + 1;
          continue;
        }
      }

      StyledLine rendered;
      std::size_t heading_count{};
      while (heading_count < line.size() && heading_count < 6 &&
             line[heading_count] == '#') {
        ++heading_count;
      }
      if (heading_count > 0 && heading_count < line.size() &&
          line[heading_count] == ' ') {
        parse_inline(line.substr(heading_count + 1), rendered,
                     TextSemantic::heading | TextSemantic::strong);
        document.push_back(std::move(rendered));
        ++index;
        continue;
      }

      if (line.size() >= 2 &&
          (line[0] == '-' || line[0] == '*' || line[0] == '+') &&
          line[1] == ' ') {
        append_span(rendered, "\xE2\x80\xA2 ", TextSemantic::list_marker);
        parse_inline(line.substr(2), rendered);
        document.push_back(std::move(rendered));
        ++index;
        continue;
      }

      std::size_t digits{};
      while (digits < line.size() &&
             std::isdigit(static_cast<unsigned char>(line[digits])) != 0) {
        ++digits;
      }
      if (digits > 0 && digits + 1 < line.size() && line[digits] == '.' &&
          line[digits + 1] == ' ') {
        append_span(rendered, std::string{line.substr(0, digits + 2)},
                    TextSemantic::list_marker);
        parse_inline(line.substr(digits + 2), rendered);
      } else {
        parse_inline(line, rendered);
      }
      document.push_back(std::move(rendered));
      ++index;
    }
    return document;
  } catch (...) {
    return error(TextErrorCode::internal_failure,
                 "Markdown tokenization failed internally");
  }
}

auto flatten(const StyledDocument& document)
    -> std::expected<std::string, TextError> {
  try {
    std::string output;
    for (std::size_t line = 0; line < document.size(); ++line) {
      if (line != 0) output.push_back('\n');
      for (const auto& span : document[line])
        output += span.text;
    }
    return output;
  } catch (...) {
    return error(TextErrorCode::internal_failure,
                 "styled text flattening failed internally");
  }
}

} // namespace aiforge::presentation
