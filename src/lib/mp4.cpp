#include <aiforge/video/mp4.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <utility>

namespace aiforge::video {
namespace {

using BoxType = std::array<char, 4>;

struct Box {
  BoxType type{};
  std::size_t payload_offset{};
  std::size_t end_offset{};
};

[[nodiscard]] auto failure(const Mp4ErrorCode code, std::string message)
    -> std::unexpected<Mp4Error> {
  return std::unexpected(Mp4Error{code, std::move(message)});
}

[[nodiscard]] auto u32(const std::span<const std::byte> bytes,
                       const std::size_t offset) -> std::uint32_t {
  return static_cast<std::uint32_t>(
             std::to_integer<unsigned char>(bytes[offset]))
             << 24U |
         static_cast<std::uint32_t>(
             std::to_integer<unsigned char>(bytes[offset + 1]))
             << 16U |
         static_cast<std::uint32_t>(
             std::to_integer<unsigned char>(bytes[offset + 2]))
             << 8U |
         static_cast<std::uint32_t>(
             std::to_integer<unsigned char>(bytes[offset + 3]));
}

[[nodiscard]] auto u64(const std::span<const std::byte> bytes,
                       const std::size_t offset) -> std::uint64_t {
  return static_cast<std::uint64_t>(u32(bytes, offset)) << 32U |
         static_cast<std::uint64_t>(u32(bytes, offset + 4));
}

[[nodiscard]] auto box_type(const std::span<const std::byte> bytes,
                            const std::size_t offset) -> BoxType {
  BoxType result{};
  for (std::size_t index{}; index < result.size(); ++index) {
    result[index] = static_cast<char>(
        std::to_integer<unsigned char>(bytes[offset + index]));
  }
  return result;
}

[[nodiscard]] auto type_is(const BoxType& type, const std::string_view expected)
    -> bool {
  if (expected.size() != type.size()) return false;
  for (std::size_t index{}; index < type.size(); ++index) {
    if (type[index] != expected[index]) return false;
  }
  return true;
}

[[nodiscard]] auto allowed_major_brand(const BoxType& brand) -> bool {
  constexpr std::array<std::string_view, 14> allowed{
      "isom", "iso2", "iso3", "iso4", "iso5", "iso6", "iso7",
      "iso8", "iso9", "isoa", "isob", "isoc", "mp41", "mp42"};
  return std::ranges::any_of(
      allowed, [&](const auto candidate) { return type_is(brand, candidate); });
}

class Parser final {
 public:
  Parser(const std::span<const std::byte> encoded, const Mp4Limits limits,
         const std::stop_token stop_token)
      : m_encoded(encoded), m_limits(limits), m_stop_token(stop_token) {}

  [[nodiscard]] auto parse() -> std::expected<Mp4Info, Mp4Error> {
    std::size_t offset{};
    std::size_t top_level_index{};
    while (offset < m_encoded.size()) {
      auto box = read_box(offset, m_encoded.size(), true, 0);
      if (!box) return std::unexpected(std::move(box.error()));
      auto parsed = parse_top_level_box(*box, top_level_index);
      if (!parsed) return std::unexpected(std::move(parsed.error()));
      offset = box->end_offset;
      ++top_level_index;
    }
    if (!m_found_file_type || !m_found_movie || !m_found_media_data ||
        !m_found_video_handler) {
      return failure(Mp4ErrorCode::malformed,
                     "MP4 required video structure is incomplete");
    }
    return Mp4Info{m_major_brand, m_compatible_brands, m_boxes, m_tracks};
  }

 private:
  [[nodiscard]] auto parse_top_level_box(const Box& box,
                                         const std::size_t index)
      -> std::expected<void, Mp4Error> {
    if (type_is(box.type, "ftyp")) {
      if (index != 0 || m_found_file_type) {
        return failure(Mp4ErrorCode::malformed,
                       "MP4 file type box is duplicated or misplaced");
      }
      auto parsed = parse_file_type(box);
      if (!parsed) return std::unexpected(std::move(parsed.error()));
      m_found_file_type = true;
      return {};
    }
    if (index == 0) {
      return failure(Mp4ErrorCode::malformed,
                     "MP4 file type box must be first");
    }
    if (type_is(box.type, "moov")) {
      if (m_found_movie) {
        return failure(Mp4ErrorCode::malformed, "MP4 movie box is duplicated");
      }
      m_found_movie = true;
      return parse_movie(box.payload_offset, box.end_offset);
    }
    if (type_is(box.type, "mdat") && box.end_offset > box.payload_offset) {
      m_found_media_data = true;
    }
    return {};
  }

  [[nodiscard]] auto read_box(const std::size_t offset,
                              const std::size_t parent_end,
                              const bool top_level, const std::size_t depth)
      -> std::expected<Box, Mp4Error> {
    if (m_stop_token.stop_requested()) {
      return failure(Mp4ErrorCode::cancelled, "MP4 validation cancelled");
    }
    if (depth > m_limits.maximum_nesting_depth ||
        ++m_boxes > m_limits.maximum_boxes) {
      return failure(Mp4ErrorCode::malformed,
                     "MP4 structural limits were exceeded");
    }
    if (offset > parent_end || parent_end - offset < 8) {
      return failure(Mp4ErrorCode::malformed, "MP4 box header is truncated");
    }
    const auto type = box_type(m_encoded, offset + 4);
    const auto size32 = u32(m_encoded, offset);
    std::uint64_t box_size{};
    std::size_t header_size{8};
    if (size32 == 0) {
      if (!top_level || !type_is(type, "mdat") ||
          parent_end - offset <= header_size) {
        return failure(Mp4ErrorCode::malformed,
                       "MP4 zero-sized box is not a final media-data box");
      }
      box_size = parent_end - offset;
    } else if (size32 == 1) {
      if (parent_end - offset < 16) {
        return failure(Mp4ErrorCode::malformed,
                       "MP4 extended box header is truncated");
      }
      header_size = 16;
      box_size = u64(m_encoded, offset + 8);
      if (box_size < header_size) {
        return failure(Mp4ErrorCode::malformed,
                       "MP4 extended box size is invalid");
      }
    } else {
      box_size = size32;
      if (box_size < header_size) {
        return failure(Mp4ErrorCode::malformed, "MP4 box size is invalid");
      }
    }
    const auto remaining = parent_end - offset;
    if (box_size > static_cast<std::uint64_t>(remaining)) {
      return failure(Mp4ErrorCode::malformed,
                     "MP4 box exceeds its containing bytes");
    }
    const auto checked_size = static_cast<std::size_t>(box_size);
    return Box{type, offset + header_size, offset + checked_size};
  }

  [[nodiscard]] auto parse_file_type(const Box& box)
      -> std::expected<void, Mp4Error> {
    const auto payload_size = box.end_offset - box.payload_offset;
    if (payload_size < 8 || (payload_size - 8) % 4 != 0) {
      return failure(Mp4ErrorCode::malformed, "MP4 file type box is malformed");
    }
    m_major_brand = box_type(m_encoded, box.payload_offset);
    if (!allowed_major_brand(m_major_brand)) {
      return failure(Mp4ErrorCode::unsupported,
                     "MP4 major brand is unsupported");
    }
    m_compatible_brands = (payload_size - 8) / 4;
    if (m_compatible_brands > m_limits.maximum_compatible_brands) {
      return failure(Mp4ErrorCode::malformed,
                     "MP4 compatible-brand limit was exceeded");
    }
    return {};
  }

  [[nodiscard]] auto parse_movie(const std::size_t begin, const std::size_t end)
      -> std::expected<void, Mp4Error> {
    std::size_t offset{begin};
    while (offset < end) {
      auto box = read_box(offset, end, false, 1);
      if (!box) return std::unexpected(std::move(box.error()));
      if (type_is(box->type, "trak")) {
        if (++m_tracks > m_limits.maximum_tracks) {
          return failure(Mp4ErrorCode::malformed,
                         "MP4 track limit was exceeded");
        }
        auto parsed = parse_track(box->payload_offset, box->end_offset);
        if (!parsed) return std::unexpected(std::move(parsed.error()));
      }
      offset = box->end_offset;
    }
    return {};
  }

  [[nodiscard]] auto parse_track(const std::size_t begin, const std::size_t end)
      -> std::expected<void, Mp4Error> {
    std::size_t offset{begin};
    while (offset < end) {
      auto box = read_box(offset, end, false, 2);
      if (!box) return std::unexpected(std::move(box.error()));
      if (type_is(box->type, "mdia")) {
        auto parsed = parse_media(box->payload_offset, box->end_offset);
        if (!parsed) return std::unexpected(std::move(parsed.error()));
      }
      offset = box->end_offset;
    }
    return {};
  }

  [[nodiscard]] auto parse_media(const std::size_t begin, const std::size_t end)
      -> std::expected<void, Mp4Error> {
    std::size_t offset{begin};
    while (offset < end) {
      auto box = read_box(offset, end, false, 3);
      if (!box) return std::unexpected(std::move(box.error()));
      if (type_is(box->type, "hdlr")) {
        const auto payload_size = box->end_offset - box->payload_offset;
        if (payload_size < 24) {
          return failure(Mp4ErrorCode::malformed,
                         "MP4 handler box is truncated");
        }
        const auto handler_type = box_type(m_encoded, box->payload_offset + 8);
        if (type_is(handler_type, "vide")) m_found_video_handler = true;
      }
      offset = box->end_offset;
    }
    return {};
  }

  std::span<const std::byte> m_encoded;
  Mp4Limits m_limits;
  std::stop_token m_stop_token;
  BoxType m_major_brand{};
  std::size_t m_compatible_brands{};
  std::size_t m_boxes{};
  std::size_t m_tracks{};
  bool m_found_file_type{};
  bool m_found_movie{};
  bool m_found_media_data{};
  bool m_found_video_handler{};
};

} // namespace

auto validate_mp4(const std::span<const std::byte> encoded,
                  const Mp4Limits limits, const std::stop_token stop_token)
    -> std::expected<Mp4Info, Mp4Error> {
  try {
    if (limits.maximum_bytes < 8 || limits.maximum_boxes == 0 ||
        limits.maximum_nesting_depth == 0 || limits.maximum_tracks == 0 ||
        limits.maximum_compatible_brands == 0) {
      return failure(Mp4ErrorCode::invalid_limits, "MP4 limits are invalid");
    }
    if (stop_token.stop_requested())
      return failure(Mp4ErrorCode::cancelled, "MP4 validation cancelled");
    if (encoded.empty())
      return failure(Mp4ErrorCode::empty, "MP4 input is empty");
    if (encoded.size() > limits.maximum_bytes)
      return failure(Mp4ErrorCode::too_large,
                     "MP4 input exceeds its byte limit");
    return Parser{encoded, limits, stop_token}.parse();
  } catch (...) {
    return failure(Mp4ErrorCode::malformed, "MP4 validation failed internally");
  }
}

} // namespace aiforge::video
