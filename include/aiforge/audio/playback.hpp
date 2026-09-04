#pragma once

#include <cstddef>
#include <expected>
#include <stop_token>
#include <string>

#include <aiforge/audio/pcm.hpp>

namespace aiforge::audio {

enum class PlaybackStage { open, start, stream, stop, close };

class PlaybackObserver {
 public:
  virtual ~PlaybackObserver() = default;
  [[nodiscard]] virtual auto stage_changed(PlaybackStage stage) noexcept
      -> bool = 0;
};

struct PlaybackStats {
  std::size_t callbacks{};
  std::size_t frames{};
  std::size_t underruns{};
  std::size_t late_callbacks{};
  auto operator==(const PlaybackStats&) const -> bool = default;
};

enum class PlaybackErrorCode {
  invalid_format,
  invalid_buffer,
  too_large,
  operation_in_progress,
  device_quarantined,
  cancelled,
  unsupported_format,
  permission_denied,
  unavailable,
  device_lost,
  underrun,
  callback_contract_violation,
  incomplete_stream,
  late_callback,
  cleanup_failed,
  internal_failure,
};

struct PlaybackError {
  PlaybackErrorCode code{PlaybackErrorCode::internal_failure};
  PlaybackStage stage{PlaybackStage::open};
  std::string message;
  PlaybackStats stats;
  auto operator==(const PlaybackError&) const -> bool = default;
};

class PlaybackPort {
 public:
  virtual ~PlaybackPort() = default;

  [[nodiscard]] virtual auto play(Signed16Buffer buffer,
                                  std::stop_token stop_token = {},
                                  PlaybackObserver* observer = nullptr) noexcept
      -> std::expected<PlaybackStats, PlaybackError> = 0;
};

} // namespace aiforge::audio
