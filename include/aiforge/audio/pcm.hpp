#pragma once

#include <cstdint>
#include <vector>

namespace aiforge::audio {

struct Signed16Format {
  std::uint32_t sample_rate{};
  std::uint16_t channels{};
  auto operator==(const Signed16Format&) const -> bool = default;
};

struct Signed16Buffer {
  Signed16Format format;
  std::vector<std::int16_t> interleaved_samples;
  auto operator==(const Signed16Buffer&) const -> bool = default;
};

} // namespace aiforge::audio
