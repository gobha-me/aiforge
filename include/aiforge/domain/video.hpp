#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

#include <aiforge/domain/ids.hpp>
#include <aiforge/domain/money.hpp>

namespace aiforge::domain {

struct VideoGenerationSpec {
  ModelId model_id;
  std::string prompt;
  std::chrono::seconds duration;
  auto operator==(const VideoGenerationSpec&) const -> bool = default;
};

enum class VideoJobState {
  queued,
  processing,
  completed,
  failed,
};

[[nodiscard]] auto validate_video_job_state(VideoJobState state) noexcept
    -> bool;
[[nodiscard]] auto validate_video_generation_spec(
    const VideoGenerationSpec& spec) -> bool;

inline constexpr std::size_t maximum_video_prompt_bytes{std::size_t{1024} *
                                                        std::size_t{1024}};
inline constexpr std::size_t maximum_video_transcription_bytes{
    std::size_t{1024} * std::size_t{1024}};
inline constexpr std::size_t maximum_video_language_bytes{128U};
inline constexpr std::size_t maximum_video_transcription_envelope_bytes{4096U};
inline constexpr std::size_t maximum_video_transcription_response_bytes{
    maximum_video_transcription_bytes + maximum_video_language_bytes +
    maximum_video_transcription_envelope_bytes};
static_assert(maximum_video_transcription_bytes <=
              std::numeric_limits<std::size_t>::max() -
                  maximum_video_language_bytes -
                  maximum_video_transcription_envelope_bytes);
inline constexpr auto maximum_video_duration = std::chrono::hours{1};

} // namespace aiforge::domain
