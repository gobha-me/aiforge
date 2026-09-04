#include <aiforge/audio/wav.hpp>

#include <array>
#include <bit>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

namespace aiforge::audio {
namespace {

[[nodiscard]] auto failure(PcmWavErrorCode code, std::string message)
    -> std::unexpected<PcmWavError> {
  return std::unexpected(PcmWavError{code, std::move(message)});
}

[[nodiscard]] auto u16(const std::span<const std::byte> bytes,
                       const std::size_t offset) -> std::uint16_t {
  return static_cast<std::uint16_t>(
             std::to_integer<unsigned char>(bytes[offset])) |
         static_cast<std::uint16_t>(
             std::to_integer<unsigned char>(bytes[offset + 1]))
             << 8U;
}

[[nodiscard]] auto u32(const std::span<const std::byte> bytes,
                       const std::size_t offset) -> std::uint32_t {
  return static_cast<std::uint32_t>(
             std::to_integer<unsigned char>(bytes[offset])) |
         static_cast<std::uint32_t>(
             std::to_integer<unsigned char>(bytes[offset + 1]))
             << 8U |
         static_cast<std::uint32_t>(
             std::to_integer<unsigned char>(bytes[offset + 2]))
             << 16U |
         static_cast<std::uint32_t>(
             std::to_integer<unsigned char>(bytes[offset + 3]))
             << 24U;
}

[[nodiscard]] auto matches(const std::span<const std::byte> bytes,
                           const std::size_t offset,
                           const std::string_view expected) -> bool {
  if (expected.size() > bytes.size() - offset) return false;
  for (std::size_t index{}; index < expected.size(); ++index) {
    if (std::to_integer<unsigned char>(bytes[offset + index]) !=
        static_cast<unsigned char>(expected[index])) {
      return false;
    }
  }
  return true;
}

struct ParsedPcmWav {
  PcmWavInfo info;
  std::size_t data_offset{};
  std::size_t data_size{};
};

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- RIFF parsing.
[[nodiscard]] auto parse_pcm_wav(const std::span<const std::byte> encoded,
                                 const PcmWavLimits limits)
    -> std::expected<ParsedPcmWav, PcmWavError> {
  try {
    if (limits.maximum_bytes < 12 || limits.maximum_chunks == 0 ||
        limits.maximum_channels == 0 || limits.minimum_sample_rate == 0 ||
        limits.minimum_sample_rate > limits.maximum_sample_rate) {
      return failure(PcmWavErrorCode::invalid_limits,
                     "PCM WAV limits are invalid");
    }
    if (encoded.empty())
      return failure(PcmWavErrorCode::empty, "PCM WAV input is empty");
    if (encoded.size() > limits.maximum_bytes)
      return failure(PcmWavErrorCode::too_large,
                     "PCM WAV input exceeds its byte limit");
    if (encoded.size() < 12 || !matches(encoded, 0, "RIFF") ||
        !matches(encoded, 8, "WAVE")) {
      return failure(PcmWavErrorCode::malformed,
                     "PCM WAV RIFF header is invalid");
    }
    const auto riff_size = static_cast<std::uint64_t>(u32(encoded, 4)) + 8U;
    if (riff_size != encoded.size()) {
      return failure(PcmWavErrorCode::malformed,
                     "PCM WAV RIFF size does not match the input");
    }

    std::optional<PcmWavInfo> format;
    std::optional<std::uint32_t> block_align;
    std::optional<std::uint64_t> data_bytes;
    std::optional<std::size_t> data_offset;
    std::size_t offset{12};
    std::size_t chunks{};
    while (offset < encoded.size()) {
      if (++chunks > limits.maximum_chunks || encoded.size() - offset < 8) {
        return failure(PcmWavErrorCode::malformed,
                       "PCM WAV chunk table is invalid");
      }
      const auto size = static_cast<std::size_t>(u32(encoded, offset + 4));
      const auto payload = offset + 8;
      if (size > encoded.size() - payload) {
        return failure(PcmWavErrorCode::malformed,
                       "PCM WAV chunk exceeds the input");
      }
      if (matches(encoded, offset, "fmt ")) {
        if (format || size < 16) {
          return failure(PcmWavErrorCode::malformed,
                         "PCM WAV format chunk is invalid");
        }
        const auto encoding = u16(encoded, payload);
        const auto channels = u16(encoded, payload + 2);
        const auto sample_rate = u32(encoded, payload + 4);
        const auto byte_rate = u32(encoded, payload + 8);
        const auto alignment = u16(encoded, payload + 12);
        const auto bits = u16(encoded, payload + 14);
        if (encoding != 1) {
          return failure(PcmWavErrorCode::unsupported,
                         "WAV encoding is not uncompressed PCM");
        }
        if (channels == 0 || channels > limits.maximum_channels ||
            sample_rate < limits.minimum_sample_rate ||
            sample_rate > limits.maximum_sample_rate ||
            (bits != 8 && bits != 16 && bits != 24 && bits != 32)) {
          return failure(PcmWavErrorCode::unsupported,
                         "PCM WAV format is outside supported bounds");
        }
        const auto expected_alignment =
            static_cast<std::uint32_t>(channels) * (bits / 8U);
        const auto expected_rate =
            static_cast<std::uint64_t>(sample_rate) * expected_alignment;
        if (alignment != expected_alignment || byte_rate != expected_rate) {
          return failure(PcmWavErrorCode::malformed,
                         "PCM WAV rate metadata is inconsistent");
        }
        format = PcmWavInfo{channels, sample_rate, bits, 0};
        block_align = alignment;
      } else if (matches(encoded, offset, "data")) {
        if (data_bytes || size == 0) {
          return failure(PcmWavErrorCode::malformed,
                         "PCM WAV data chunk is invalid");
        }
        data_bytes = size;
        data_offset = payload;
      }
      const auto padded = size + (size & 1U);
      if (padded > encoded.size() - payload) {
        return failure(PcmWavErrorCode::malformed,
                       "PCM WAV chunk padding is invalid");
      }
      offset = payload + padded;
    }
    if (!format || !data_bytes || !data_offset || !block_align ||
        *data_bytes % *block_align != 0) {
      return failure(PcmWavErrorCode::malformed,
                     "PCM WAV format and sample data do not agree");
    }
    format->frames = *data_bytes / *block_align;
    return ParsedPcmWav{*format, *data_offset,
                        static_cast<std::size_t>(*data_bytes)};
  } catch (...) {
    return failure(PcmWavErrorCode::malformed,
                   "PCM WAV validation failed internally");
  }
}

} // namespace

auto validate_pcm_wav(const std::span<const std::byte> encoded,
                      const PcmWavLimits limits)
    -> std::expected<PcmWavInfo, PcmWavError> {
  auto parsed = parse_pcm_wav(encoded, limits);
  if (!parsed) return std::unexpected(std::move(parsed.error()));
  return parsed->info;
}

auto decode_pcm16_wav(const std::span<const std::byte> encoded,
                      const PcmWavLimits limits)
    -> std::expected<Signed16Buffer, PcmWavError> {
  try {
    auto parsed = parse_pcm_wav(encoded, limits);
    if (!parsed) return std::unexpected(std::move(parsed.error()));
    if (parsed->info.bits_per_sample != 16 ||
        (parsed->info.channels != 1 && parsed->info.channels != 2)) {
      return failure(PcmWavErrorCode::unsupported,
                     "PCM WAV playback requires signed-16 mono or stereo");
    }

    std::vector<std::int16_t> samples;
    samples.reserve(parsed->data_size / sizeof(std::int16_t));
    for (std::size_t index{}; index < parsed->data_size; index += 2) {
      samples.push_back(std::bit_cast<std::int16_t>(
          u16(encoded, parsed->data_offset + index)));
    }
    return Signed16Buffer{{parsed->info.sample_rate, parsed->info.channels},
                          std::move(samples)};
  } catch (...) {
    return failure(PcmWavErrorCode::malformed,
                   "PCM WAV decoding failed internally");
  }
}

} // namespace aiforge::audio
