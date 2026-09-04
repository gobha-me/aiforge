#pragma once

#include <array>
#include <cstddef>
#include <expected>
#include <span>
#include <stop_token>
#include <string>

namespace aiforge::video {

struct Mp4Limits {
  std::size_t maximum_bytes{std::size_t{32} * 1024U * 1024U};
  std::size_t maximum_boxes{4096};
  std::size_t maximum_nesting_depth{16};
  std::size_t maximum_tracks{16};
  std::size_t maximum_compatible_brands{64};
  auto operator==(const Mp4Limits&) const -> bool = default;
};

struct Mp4Info {
  std::array<char, 4> major_brand{};
  std::size_t compatible_brand_count{};
  std::size_t box_count{};
  std::size_t track_count{};
  auto operator==(const Mp4Info&) const -> bool = default;
};

enum class Mp4ErrorCode {
  invalid_limits,
  empty,
  too_large,
  malformed,
  unsupported,
  cancelled,
};

struct Mp4Error {
  Mp4ErrorCode code{Mp4ErrorCode::malformed};
  std::string message;
  auto operator==(const Mp4Error&) const -> bool = default;
};

[[nodiscard]] auto validate_mp4(std::span<const std::byte> encoded,
                                Mp4Limits limits = {},
                                std::stop_token stop_token = {})
    -> std::expected<Mp4Info, Mp4Error>;

} // namespace aiforge::video
