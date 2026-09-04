#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include <aiforge/audio/pcm.hpp>

namespace aiforge::audio {

struct PcmWavLimits {
  std::size_t maximum_bytes{32U * 1024U * 1024U};
  std::size_t maximum_chunks{4096};
  std::uint16_t maximum_channels{8};
  std::uint32_t minimum_sample_rate{8000};
  std::uint32_t maximum_sample_rate{192000};
  auto operator==(const PcmWavLimits&) const -> bool = default;
};

struct PcmWavInfo {
  std::uint16_t channels{};
  std::uint32_t sample_rate{};
  std::uint16_t bits_per_sample{};
  std::uint64_t frames{};
  auto operator==(const PcmWavInfo&) const -> bool = default;
};

enum class PcmWavErrorCode {
  invalid_limits,
  empty,
  too_large,
  malformed,
  unsupported,
};

struct PcmWavError {
  PcmWavErrorCode code{PcmWavErrorCode::malformed};
  std::string message;
  auto operator==(const PcmWavError&) const -> bool = default;
};

[[nodiscard]] auto validate_pcm_wav(std::span<const std::byte> encoded,
                                    PcmWavLimits limits = {})
    -> std::expected<PcmWavInfo, PcmWavError>;

[[nodiscard]] auto decode_pcm16_wav(std::span<const std::byte> encoded,
                                    PcmWavLimits limits = {})
    -> std::expected<Signed16Buffer, PcmWavError>;

[[nodiscard]] auto encode_pcm16_wav(const Signed16Buffer& buffer,
                                    PcmWavLimits limits = {})
    -> std::expected<std::vector<std::byte>, PcmWavError>;

} // namespace aiforge::audio
