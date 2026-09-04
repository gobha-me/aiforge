#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace aiforge::evaluation::audio_device {

inline constexpr std::size_t maximum_signed16_buffer_bytes =
    std::size_t{32} * 1024U * 1024U;
inline constexpr std::size_t maximum_signed16_buffer_frames =
    maximum_signed16_buffer_bytes / sizeof(std::int16_t);
inline constexpr std::uint32_t minimum_sample_rate = 8000;
inline constexpr std::uint32_t maximum_sample_rate = 192000;

enum class AudioDirection { playback, capture };

enum class DeviceStage { open, start, stream, stop, close };

struct Signed16Format {
  std::uint32_t sample_rate{};
  std::uint16_t channels{};
  auto operator==(const Signed16Format&) const -> bool = default;
};

struct PlaybackBuffer {
  Signed16Format format;
  std::vector<std::int16_t> interleaved_samples;
  auto operator==(const PlaybackBuffer&) const -> bool = default;
};

struct CaptureRequest {
  Signed16Format format;
  std::size_t frames{};
  auto operator==(const CaptureRequest&) const -> bool = default;
};

struct DeviceContractLimits {
  std::size_t maximum_buffer_bytes{maximum_signed16_buffer_bytes};
  std::size_t maximum_buffer_frames{maximum_signed16_buffer_frames};
  auto operator==(const DeviceContractLimits&) const -> bool = default;
};

enum class DeviceFailureCode {
  unsupported_format,
  permission_denied,
  unavailable,
  device_lost,
  internal_failure,
};

struct DeviceFailure {
  DeviceFailureCode code{DeviceFailureCode::internal_failure};
  std::string message;
  auto operator==(const DeviceFailure&) const -> bool = default;
};

enum class CallbackStatus {
  normal,
  playback_underrun,
  capture_overrun,
  device_lost
};

struct CallbackBuffer {
  std::size_t frames{};
  std::span<std::int16_t> playback_output;
  std::span<const std::int16_t> capture_input;
  CallbackStatus status{CallbackStatus::normal};
};

enum class CallbackDecision { continue_operation, complete, abort };

// Invoked serially on one device-owned callback thread. Implementations must
// not allocate, lock, perform I/O, throw, or call back into the device. The
// callback object remains valid through close() so a late invocation can fail
// safely. stream() ends the accepted callback interval; stop() and close() must
// not invoke it, and close() returns only after every callback is quiescent and
// the device has forgotten the object.
class DeviceCallback {
 public:
  virtual ~DeviceCallback() = default;
  [[nodiscard]] virtual auto process(CallbackBuffer buffer) noexcept
      -> CallbackDecision = 0;
};

struct DeviceOpenRequest {
  AudioDirection direction{AudioDirection::playback};
  Signed16Format format;
  std::size_t frames{};
  auto operator==(const DeviceOpenRequest&) const -> bool = default;
};

// The control-thread lifecycle is exact: open -> start -> stream -> stop ->
// close. stream() does not return until the current streaming interval has no
// callback in flight. stop() and close() are safe after a failed same-stage
// predecessor, which lets the controller clean up an indeterminate native
// result without guessing whether it partly applied.
class BufferedAudioDevice {
 public:
  virtual ~BufferedAudioDevice() = default;
  [[nodiscard]] virtual auto open(const DeviceOpenRequest& request,
                                  DeviceCallback& callback) noexcept
      -> std::expected<void, DeviceFailure> = 0;
  [[nodiscard]] virtual auto start() noexcept
      -> std::expected<void, DeviceFailure> = 0;
  [[nodiscard]] virtual auto stream() noexcept
      -> std::expected<void, DeviceFailure> = 0;
  [[nodiscard]] virtual auto stop() noexcept
      -> std::expected<void, DeviceFailure> = 0;
  [[nodiscard]] virtual auto close() noexcept
      -> std::expected<void, DeviceFailure> = 0;
};

class CancellationFlag {
 public:
  void request() noexcept;
  [[nodiscard]] auto requested() const noexcept -> bool;

 private:
  std::atomic<bool> m_requested{false};
};

struct OperationStats {
  std::size_t callbacks{};
  std::size_t frames{};
  std::size_t xruns{};
  std::size_t late_callbacks{};
  auto operator==(const OperationStats&) const -> bool = default;
};

struct CaptureResult {
  Signed16Format format;
  std::vector<std::int16_t> interleaved_samples;
  OperationStats stats;
  auto operator==(const CaptureResult&) const -> bool = default;
};

enum class DeviceContractErrorCode {
  invalid_limits,
  invalid_format,
  invalid_buffer,
  too_large,
  operation_in_progress,
  cancelled,
  unsupported_format,
  permission_denied,
  unavailable,
  device_lost,
  playback_underrun,
  capture_overrun,
  callback_contract_violation,
  incomplete_stream,
  late_callback,
  cleanup_failed,
  internal_failure,
};

struct DeviceContractError {
  DeviceContractErrorCode code{DeviceContractErrorCode::internal_failure};
  DeviceStage stage{DeviceStage::open};
  std::string message;
  OperationStats stats;
  auto operator==(const DeviceContractError&) const -> bool = default;
};

class DeviceContractController {
 public:
  explicit DeviceContractController(DeviceContractLimits limits = {});

  DeviceContractController(const DeviceContractController&) = delete;
  auto operator=(const DeviceContractController&)
      -> DeviceContractController& = delete;

  [[nodiscard]] auto play(
      BufferedAudioDevice& device, PlaybackBuffer buffer,
      const CancellationFlag* cancellation = nullptr) noexcept
      -> std::expected<OperationStats, DeviceContractError>;

  [[nodiscard]] auto capture(
      BufferedAudioDevice& device, CaptureRequest request,
      const CancellationFlag* cancellation = nullptr) noexcept
      -> std::expected<CaptureResult, DeviceContractError>;

  [[nodiscard]] auto active() const noexcept -> bool;

 private:
  DeviceContractLimits m_limits;
  std::atomic<bool> m_active{false};
};

} // namespace aiforge::evaluation::audio_device
