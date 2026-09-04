#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <stop_token>

#include "playback_device.hpp"

namespace aiforge::adapters {

enum class RtAudioNativeError {
  unsupported_format,
  permission_denied,
  unavailable,
  device_lost,
  internal_failure,
};

enum class RtAudioNativeStatus { normal, output_underflow, device_lost };

using RtAudioNativeCallback = auto (*)(void* output, std::size_t frames,
                                       RtAudioNativeStatus status,
                                       void* user_data) noexcept -> int;

struct RtAudioNativeOpenRequest {
  std::uint32_t sample_rate{};
  std::uint16_t channels{};
  std::size_t callback_frames{};
  bool alsa_api{};
  bool default_output{};
  bool signed16_interleaved{};
  auto operator==(const RtAudioNativeOpenRequest&) const -> bool = default;
};

class RtAudioNative {
 public:
  virtual ~RtAudioNative() = default;
  [[nodiscard]] virtual auto open(RtAudioNativeOpenRequest& request,
                                  RtAudioNativeCallback callback,
                                  void* user_data) noexcept
      -> std::expected<void, RtAudioNativeError> = 0;
  [[nodiscard]] virtual auto start() noexcept
      -> std::expected<void, RtAudioNativeError> = 0;
  [[nodiscard]] virtual auto running() const noexcept -> bool = 0;
  [[nodiscard]] virtual auto stream_failure() const noexcept
      -> std::optional<RtAudioNativeError> = 0;
  [[nodiscard]] virtual auto abort() noexcept
      -> std::expected<void, RtAudioNativeError> = 0;
  [[nodiscard]] virtual auto stop() noexcept
      -> std::expected<void, RtAudioNativeError> = 0;
  [[nodiscard]] virtual auto close() noexcept
      -> std::expected<void, RtAudioNativeError> = 0;
  virtual auto wait_for_progress(std::chrono::milliseconds duration) noexcept
      -> void = 0;
};

class RtAudioPlaybackDevice final : public BufferedPlaybackDevice {
 public:
  explicit RtAudioPlaybackDevice(std::unique_ptr<RtAudioNative> native);

  [[nodiscard]] auto open(const PlaybackDeviceOpenRequest& request,
                          PlaybackDeviceCallback& callback) noexcept
      -> std::expected<void, PlaybackDeviceFailure> override;
  [[nodiscard]] auto start() noexcept
      -> std::expected<void, PlaybackDeviceFailure> override;
  [[nodiscard]] auto stream(std::stop_token stop_token) noexcept
      -> std::expected<void, PlaybackDeviceFailure> override;
  [[nodiscard]] auto stop() noexcept
      -> std::expected<void, PlaybackDeviceFailure> override;
  [[nodiscard]] auto close() noexcept
      -> std::expected<void, PlaybackDeviceFailure> override;

 private:
  [[nodiscard]] static auto native_callback(void* output, std::size_t frames,
                                            RtAudioNativeStatus status,
                                            void* user_data) noexcept -> int;

  PlaybackDeviceCallback* m_callback{};
  std::size_t m_callback_frames{};
  std::size_t m_channels{};
  std::size_t m_maximum_polls{};
  std::atomic<bool> m_terminal{false};
  // Destroy the native stream first: it may retain callback_data pointing at
  // this object's callback-visible members until native teardown completes.
  std::unique_ptr<RtAudioNative> m_native;
};

[[nodiscard]] auto make_rtaudio_native()
    -> std::expected<std::unique_ptr<RtAudioNative>, PlaybackDeviceFailure>;

} // namespace aiforge::adapters
