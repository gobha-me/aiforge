#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <stop_token>

#include <aiforge/audio/capture.hpp>

#include "audio_device_gate.hpp"

namespace aiforge::adapters {

inline constexpr std::size_t maximum_capture_bytes =
    std::size_t{32} * 1024U * 1024U;
inline constexpr std::size_t maximum_capture_frames =
    maximum_capture_bytes / sizeof(std::int16_t);

enum class CaptureDeviceFailureCode {
  unsupported_format,
  permission_denied,
  unavailable,
  device_lost,
  internal_failure,
};

struct CaptureDeviceFailure {
  CaptureDeviceFailureCode code{CaptureDeviceFailureCode::internal_failure};
};

enum class CaptureCallbackStatus { normal, overrun, device_lost };
enum class CaptureCallbackDecision { continue_operation, complete, abort };

class CaptureDeviceCallback {
 public:
  virtual ~CaptureDeviceCallback() = default;
  [[nodiscard]] virtual auto process(std::span<const std::int16_t> input,
                                     std::size_t frames,
                                     CaptureCallbackStatus status) noexcept
      -> CaptureCallbackDecision = 0;
};

struct CaptureDeviceOpenRequest {
  audio::Signed16Format format;
  std::size_t frames{};
  auto operator==(const CaptureDeviceOpenRequest&) const -> bool = default;
};

class BufferedCaptureDevice {
 public:
  virtual ~BufferedCaptureDevice() = default;
  [[nodiscard]] virtual auto open(const CaptureDeviceOpenRequest& request,
                                  CaptureDeviceCallback& callback) noexcept
      -> std::expected<void, CaptureDeviceFailure> = 0;
  [[nodiscard]] virtual auto start() noexcept
      -> std::expected<void, CaptureDeviceFailure> = 0;
  [[nodiscard]] virtual auto stream(std::stop_token stop_token) noexcept
      -> std::expected<void, CaptureDeviceFailure> = 0;
  [[nodiscard]] virtual auto stop() noexcept
      -> std::expected<void, CaptureDeviceFailure> = 0;
  [[nodiscard]] virtual auto close() noexcept
      -> std::expected<void, CaptureDeviceFailure> = 0;
};

struct CaptureCallbackTestFence {
  std::atomic<bool> entered{};
  std::atomic<bool> release{};
};

struct CaptureControllerLimits {
  std::size_t maximum_buffer_bytes{maximum_capture_bytes};
  std::size_t maximum_buffer_frames{maximum_capture_frames};
  CaptureCallbackTestFence* callback_test_fence{};
  auto operator==(const CaptureControllerLimits&) const -> bool = default;
};

class CaptureController final : public audio::CapturePort {
 public:
  CaptureController(AudioDeviceGate& gate, BufferedCaptureDevice& device,
                    CaptureControllerLimits limits = {});

  [[nodiscard]] auto capture(
      audio::CaptureRequest request, std::stop_token stop_token = {},
      audio::CaptureObserver* observer = nullptr) noexcept
      -> std::expected<audio::CaptureResult, audio::CaptureError> override;

 private:
  AudioDeviceGate& m_gate;
  BufferedCaptureDevice& m_device;
  CaptureControllerLimits m_limits;
};

} // namespace aiforge::adapters
