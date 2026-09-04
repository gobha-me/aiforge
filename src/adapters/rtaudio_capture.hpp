#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <stop_token>

#include "capture_device.hpp"

namespace aiforge::adapters {

enum class RtAudioCaptureNativeError {
  unsupported_format,
  permission_denied,
  unavailable,
  device_lost,
  internal_failure,
};

enum class RtAudioCaptureNativeStatus { normal, input_overflow, device_lost };

using RtAudioCaptureNativeCallback = auto (*)(const void* input,
                                              std::size_t frames,
                                              RtAudioCaptureNativeStatus status,
                                              void* user_data) noexcept -> int;

struct RtAudioCaptureNativeOpenRequest {
  std::uint32_t sample_rate{};
  std::uint16_t channels{};
  std::size_t callback_frames{};
  bool alsa_api{};
  bool default_input{};
  bool signed16_interleaved{};
  auto operator==(const RtAudioCaptureNativeOpenRequest&) const
      -> bool = default;
};

class RtAudioCaptureNative {
 public:
  virtual ~RtAudioCaptureNative() = default;
  [[nodiscard]] virtual auto open(RtAudioCaptureNativeOpenRequest& request,
                                  RtAudioCaptureNativeCallback callback,
                                  void* user_data) noexcept
      -> std::expected<void, RtAudioCaptureNativeError> = 0;
  [[nodiscard]] virtual auto start() noexcept
      -> std::expected<void, RtAudioCaptureNativeError> = 0;
  [[nodiscard]] virtual auto running() const noexcept -> bool = 0;
  [[nodiscard]] virtual auto stream_failure() const noexcept
      -> std::optional<RtAudioCaptureNativeError> = 0;
  [[nodiscard]] virtual auto abort() noexcept
      -> std::expected<void, RtAudioCaptureNativeError> = 0;
  [[nodiscard]] virtual auto stop() noexcept
      -> std::expected<void, RtAudioCaptureNativeError> = 0;
  [[nodiscard]] virtual auto close() noexcept
      -> std::expected<void, RtAudioCaptureNativeError> = 0;
  virtual auto wait_for_progress(std::chrono::milliseconds duration) noexcept
      -> void = 0;
};

class RtAudioCaptureDevice final : public BufferedCaptureDevice {
 public:
  explicit RtAudioCaptureDevice(std::unique_ptr<RtAudioCaptureNative> native);

  [[nodiscard]] auto open(const CaptureDeviceOpenRequest& request,
                          CaptureDeviceCallback& callback) noexcept
      -> std::expected<void, CaptureDeviceFailure> override;
  [[nodiscard]] auto start() noexcept
      -> std::expected<void, CaptureDeviceFailure> override;
  [[nodiscard]] auto stream(std::stop_token stop_token) noexcept
      -> std::expected<void, CaptureDeviceFailure> override;
  [[nodiscard]] auto stop() noexcept
      -> std::expected<void, CaptureDeviceFailure> override;
  [[nodiscard]] auto close() noexcept
      -> std::expected<void, CaptureDeviceFailure> override;

 private:
  [[nodiscard]] static auto native_callback(const void* input,
                                            std::size_t frames,
                                            RtAudioCaptureNativeStatus status,
                                            void* user_data) noexcept -> int;

  CaptureDeviceCallback* m_callback{};
  std::size_t m_callback_frames{};
  std::size_t m_channels{};
  std::size_t m_maximum_polls{};
  std::atomic<bool> m_terminal{false};
  std::unique_ptr<RtAudioCaptureNative> m_native;
};

[[nodiscard]] auto make_rtaudio_capture_native()
    -> std::expected<std::unique_ptr<RtAudioCaptureNative>,
                     CaptureDeviceFailure>;

} // namespace aiforge::adapters
