#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <stop_token>

#include <aiforge/audio/playback.hpp>

namespace aiforge::adapters {

inline constexpr std::size_t maximum_playback_bytes =
    std::size_t{32} * 1024U * 1024U;
inline constexpr std::size_t maximum_playback_frames =
    maximum_playback_bytes / sizeof(std::int16_t);

enum class PlaybackDeviceFailureCode {
  unsupported_format,
  permission_denied,
  unavailable,
  device_lost,
  internal_failure,
};

struct PlaybackDeviceFailure {
  PlaybackDeviceFailureCode code{PlaybackDeviceFailureCode::internal_failure};
};

enum class PlaybackCallbackStatus { normal, underrun, device_lost };
enum class PlaybackCallbackDecision { continue_operation, complete, abort };

class PlaybackDeviceCallback {
 public:
  virtual ~PlaybackDeviceCallback() = default;
  [[nodiscard]] virtual auto process(std::span<std::int16_t> output,
                                     std::size_t frames,
                                     PlaybackCallbackStatus status) noexcept
      -> PlaybackCallbackDecision = 0;
};

struct PlaybackDeviceOpenRequest {
  audio::Signed16Format format;
  std::size_t frames{};
  auto operator==(const PlaybackDeviceOpenRequest&) const -> bool = default;
};

class BufferedPlaybackDevice {
 public:
  virtual ~BufferedPlaybackDevice() = default;
  [[nodiscard]] virtual auto open(const PlaybackDeviceOpenRequest& request,
                                  PlaybackDeviceCallback& callback) noexcept
      -> std::expected<void, PlaybackDeviceFailure> = 0;
  [[nodiscard]] virtual auto start() noexcept
      -> std::expected<void, PlaybackDeviceFailure> = 0;
  [[nodiscard]] virtual auto stream(std::stop_token stop_token) noexcept
      -> std::expected<void, PlaybackDeviceFailure> = 0;
  [[nodiscard]] virtual auto stop() noexcept
      -> std::expected<void, PlaybackDeviceFailure> = 0;
  [[nodiscard]] virtual auto close() noexcept
      -> std::expected<void, PlaybackDeviceFailure> = 0;
};

class AudioDeviceGate final {
 public:
  enum class BeginResult { acquired, operation_in_progress, quarantined };

  [[nodiscard]] auto begin() noexcept -> BeginResult;
  auto release() noexcept -> void;
  auto quarantine(
      std::shared_ptr<PlaybackDeviceCallback> callback = {}) noexcept -> void;
  [[nodiscard]] auto active() const noexcept -> bool;
  [[nodiscard]] auto quarantined() const noexcept -> bool;

 private:
  enum class State { idle, active, quarantined };
  std::atomic<State> m_state{State::idle};
  // A quarantined callback must remain alive until the owning native device is
  // destroyed and can no longer invoke the retained address.
  std::shared_ptr<PlaybackDeviceCallback> m_quarantined_callback;
};

struct PlaybackCallbackTestFence {
  std::atomic<bool> entered{};
  std::atomic<bool> release{};
};

struct PlaybackControllerLimits {
  std::size_t maximum_buffer_bytes{maximum_playback_bytes};
  std::size_t maximum_buffer_frames{maximum_playback_frames};
  PlaybackCallbackTestFence* callback_test_fence{};
  auto operator==(const PlaybackControllerLimits&) const -> bool = default;
};

class PlaybackController final : public audio::PlaybackPort {
 public:
  PlaybackController(AudioDeviceGate& gate, BufferedPlaybackDevice& device,
                     PlaybackControllerLimits limits = {});

  [[nodiscard]] auto play(audio::Signed16Buffer buffer,
                          std::stop_token stop_token = {},
                          audio::PlaybackObserver* observer = nullptr) noexcept
      -> std::expected<audio::PlaybackStats, audio::PlaybackError> override;

 private:
  AudioDeviceGate& m_gate;
  BufferedPlaybackDevice& m_device;
  PlaybackControllerLimits m_limits;
};

} // namespace aiforge::adapters
