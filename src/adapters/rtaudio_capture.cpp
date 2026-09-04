#include "rtaudio_capture.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <limits>
#include <new>
#include <ranges>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <RtAudio.h>

namespace aiforge::adapters {
namespace {

constexpr std::size_t desired_callback_frames = 256;
constexpr auto polling_interval = std::chrono::milliseconds{2};
constexpr std::size_t cleanup_margin_polls = 2500;
constexpr std::uint64_t polls_per_second = 500;

[[nodiscard]] auto failure(const CaptureDeviceFailureCode code)
    -> std::unexpected<CaptureDeviceFailure> {
  return std::unexpected(CaptureDeviceFailure{code});
}

[[nodiscard]] auto native_failure(const RtAudioCaptureNativeError code)
    -> CaptureDeviceFailure {
  switch (code) {
    case RtAudioCaptureNativeError::unsupported_format:
      return {CaptureDeviceFailureCode::unsupported_format};
    case RtAudioCaptureNativeError::permission_denied:
      return {CaptureDeviceFailureCode::permission_denied};
    case RtAudioCaptureNativeError::unavailable:
      return {CaptureDeviceFailureCode::unavailable};
    case RtAudioCaptureNativeError::device_lost:
      return {CaptureDeviceFailureCode::device_lost};
    case RtAudioCaptureNativeError::internal_failure:
      return {CaptureDeviceFailureCode::internal_failure};
  }
  return {CaptureDeviceFailureCode::internal_failure};
}

[[nodiscard]] auto capture_poll_limit(
    const CaptureDeviceOpenRequest& request) noexcept
    -> std::optional<std::size_t> {
  if ((request.format.channels != 1 && request.format.channels != 2) ||
      request.format.sample_rate < 8000 ||
      request.format.sample_rate > 192000 || request.frames == 0 ||
      request.frames > maximum_capture_frames)
    return std::nullopt;
  const auto frames = static_cast<std::uint64_t>(request.frames);
  const auto sample_rate =
      static_cast<std::uint64_t>(request.format.sample_rate);
  const auto whole_seconds = frames / sample_rate;
  if (whole_seconds >
      (std::numeric_limits<std::uint64_t>::max() - cleanup_margin_polls) /
          polls_per_second)
    return std::nullopt;
  const auto remaining_frames = frames % sample_rate;
  const auto scaled_remainder = remaining_frames * polls_per_second;
  const auto partial =
      scaled_remainder / sample_rate +
      static_cast<std::uint64_t>(scaled_remainder % sample_rate != 0);
  const auto duration = whole_seconds * polls_per_second;
  if (duration > std::numeric_limits<std::uint64_t>::max() -
                     cleanup_margin_polls - partial)
    return std::nullopt;
  const auto total = duration + partial + cleanup_margin_polls;
  if (total > std::numeric_limits<std::size_t>::max()) return std::nullopt;
  return static_cast<std::size_t>(total);
}

[[nodiscard]] auto map_error(const RtAudioErrorType code)
    -> RtAudioCaptureNativeError {
  switch (code) {
    case RTAUDIO_NO_DEVICES_FOUND:
    case RTAUDIO_INVALID_DEVICE: return RtAudioCaptureNativeError::unavailable;
    case RTAUDIO_DEVICE_DISCONNECT:
      return RtAudioCaptureNativeError::device_lost;
    case RTAUDIO_INVALID_PARAMETER:
      return RtAudioCaptureNativeError::unsupported_format;
    case RTAUDIO_NO_ERROR:
    case RTAUDIO_WARNING:
    case RTAUDIO_UNKNOWN_ERROR:
    case RTAUDIO_MEMORY_ERROR:
    case RTAUDIO_INVALID_USE:
    case RTAUDIO_DRIVER_ERROR:
    case RTAUDIO_SYSTEM_ERROR:
    case RTAUDIO_THREAD_ERROR:
      return RtAudioCaptureNativeError::internal_failure;
  }
  return RtAudioCaptureNativeError::internal_failure;
}

class NativeRtAudioCapture final : public RtAudioCaptureNative {
 public:
  NativeRtAudioCapture()
      : m_audio{RtAudio::LINUX_ALSA,
                [this](const RtAudioErrorType type, const std::string&) {
                  m_last_error.store(type, std::memory_order_release);
                }} {}

  [[nodiscard]] auto usable() -> bool {
    std::vector<RtAudio::Api> compiled;
    RtAudio::getCompiledApi(compiled);
    return m_audio.getCurrentApi() == RtAudio::LINUX_ALSA &&
           std::ranges::find(compiled, RtAudio::LINUX_ALSA) != compiled.end();
  }

  [[nodiscard]] auto open(RtAudioCaptureNativeOpenRequest& request,
                          const RtAudioCaptureNativeCallback callback,
                          void* user_data) noexcept
      -> std::expected<void, RtAudioCaptureNativeError> override {
    try {
      if (!request.alsa_api || !request.default_input ||
          !request.signed16_interleaved || request.channels == 0 ||
          request.callback_frames == 0 ||
          request.callback_frames > std::numeric_limits<unsigned int>::max())
        return std::unexpected(RtAudioCaptureNativeError::unsupported_format);
      m_last_error.store(RTAUDIO_NO_ERROR, std::memory_order_release);
      RtAudio::StreamParameters input{m_audio.getDefaultInputDevice(),
                                      request.channels, 0};
      if (input.deviceId == 0)
        return std::unexpected(RtAudioCaptureNativeError::unavailable);
      auto callback_frames = static_cast<unsigned int>(request.callback_frames);
      RtAudio::StreamOptions options;
      options.flags = RTAUDIO_ALSA_USE_DEFAULT;
      m_bridge = {callback, user_data};
      errno = 0;
      const auto opened = m_audio.openStream(
          nullptr, &input, RTAUDIO_SINT16, request.sample_rate,
          &callback_frames,
          [](void*, void* input_buffer, const unsigned int frames, double,
             const RtAudioStreamStatus status, void* callback_data) -> int {
            auto* bridge = static_cast<NativeCallbackBridge*>(callback_data);
            auto mapped = RtAudioCaptureNativeStatus::normal;
            if ((status & RTAUDIO_INPUT_OVERFLOW) != 0U)
              mapped = RtAudioCaptureNativeStatus::input_overflow;
            return bridge->callback(input_buffer, frames, mapped,
                                    bridge->user_data);
          },
          &m_bridge, &options);
      const auto open_errno = errno;
      if ((opened == RTAUDIO_DRIVER_ERROR || opened == RTAUDIO_SYSTEM_ERROR) &&
          (open_errno == EACCES || open_errno == EPERM))
        return std::unexpected(RtAudioCaptureNativeError::permission_denied);
      if (opened != RTAUDIO_NO_ERROR) return std::unexpected(map_error(opened));
      request.callback_frames = callback_frames;
      return {};
    } catch (...) {
      return std::unexpected(RtAudioCaptureNativeError::internal_failure);
    }
  }

  [[nodiscard]] auto start() noexcept
      -> std::expected<void, RtAudioCaptureNativeError> override {
    return call([this] { return m_audio.startStream(); });
  }
  [[nodiscard]] auto running() const noexcept -> bool override {
    try {
      return m_audio.isStreamRunning();
    } catch (...) {
      return false;
    }
  }
  [[nodiscard]] auto stream_failure() const noexcept
      -> std::optional<RtAudioCaptureNativeError> override {
    const auto observed = m_last_error.load(std::memory_order_acquire);
    if (observed == RTAUDIO_NO_ERROR || observed == RTAUDIO_WARNING)
      return std::nullopt;
    return map_error(observed);
  }
  [[nodiscard]] auto abort() noexcept
      -> std::expected<void, RtAudioCaptureNativeError> override {
    if (!running()) return {};
    return call([this] { return m_audio.abortStream(); });
  }
  [[nodiscard]] auto stop() noexcept
      -> std::expected<void, RtAudioCaptureNativeError> override {
    if (!running()) return {};
    return call([this] { return m_audio.stopStream(); });
  }
  [[nodiscard]] auto close() noexcept
      -> std::expected<void, RtAudioCaptureNativeError> override {
    try {
      m_last_error.store(RTAUDIO_NO_ERROR, std::memory_order_release);
      if (m_audio.isStreamOpen()) m_audio.closeStream();
      if (m_audio.isStreamOpen())
        return std::unexpected(RtAudioCaptureNativeError::internal_failure);
      const auto observed = m_last_error.load(std::memory_order_acquire);
      if (observed != RTAUDIO_NO_ERROR && observed != RTAUDIO_WARNING)
        return std::unexpected(map_error(observed));
      m_bridge = {};
      return {};
    } catch (...) {
      return std::unexpected(RtAudioCaptureNativeError::internal_failure);
    }
  }
  auto wait_for_progress(const std::chrono::milliseconds duration) noexcept
      -> void override {
    std::this_thread::sleep_for(duration);
  }

 private:
  struct NativeCallbackBridge {
    RtAudioCaptureNativeCallback callback{};
    void* user_data{};
  };
  template <typename Operation>
  [[nodiscard]] auto call(Operation operation) noexcept
      -> std::expected<void, RtAudioCaptureNativeError> {
    try {
      m_last_error.store(RTAUDIO_NO_ERROR, std::memory_order_release);
      const auto result = operation();
      if (result != RTAUDIO_NO_ERROR) return std::unexpected(map_error(result));
      return {};
    } catch (...) {
      return std::unexpected(RtAudioCaptureNativeError::internal_failure);
    }
  }
  std::atomic<RtAudioErrorType> m_last_error{RTAUDIO_NO_ERROR};
  NativeCallbackBridge m_bridge;
  // RtAudio is destroyed first while its error target and callback bridge are
  // still alive.
  RtAudio m_audio;
};

} // namespace

RtAudioCaptureDevice::RtAudioCaptureDevice(
    std::unique_ptr<RtAudioCaptureNative> native)
    : m_native{std::move(native)} {
}

auto RtAudioCaptureDevice::open(const CaptureDeviceOpenRequest& request,
                                CaptureDeviceCallback& callback) noexcept
    -> std::expected<void, CaptureDeviceFailure> {
  try {
    const auto maximum_polls = capture_poll_limit(request);
    if (m_native == nullptr || !maximum_polls)
      return failure(CaptureDeviceFailureCode::unsupported_format);
    m_callback = &callback;
    m_terminal.store(false, std::memory_order_release);
    RtAudioCaptureNativeOpenRequest native_request{
        request.format.sample_rate,
        request.format.channels,
        std::min(request.frames, desired_callback_frames),
        true,
        true,
        true};
    auto opened = m_native->open(native_request, &native_callback, this);
    if (!opened) return std::unexpected(native_failure(opened.error()));
    if (native_request.callback_frames == 0)
      return failure(CaptureDeviceFailureCode::internal_failure);
    m_callback_frames = native_request.callback_frames;
    m_channels = request.format.channels;
    m_maximum_polls = *maximum_polls;
    return {};
  } catch (...) {
    return failure(CaptureDeviceFailureCode::internal_failure);
  }
}

auto RtAudioCaptureDevice::start() noexcept
    -> std::expected<void, CaptureDeviceFailure> {
  if (m_native == nullptr)
    return failure(CaptureDeviceFailureCode::internal_failure);
  auto result = m_native->start();
  if (!result) return std::unexpected(native_failure(result.error()));
  return {};
}

auto RtAudioCaptureDevice::stream(const std::stop_token stop_token) noexcept
    -> std::expected<void, CaptureDeviceFailure> {
  if (m_native == nullptr || m_callback == nullptr || m_callback_frames == 0 ||
      m_maximum_polls == 0)
    return failure(CaptureDeviceFailureCode::internal_failure);
  for (std::size_t poll{}; poll < m_maximum_polls; ++poll) {
    if (m_terminal.load(std::memory_order_acquire)) return {};
    if (!m_native->running()) {
      if (const auto observed = m_native->stream_failure())
        return std::unexpected(native_failure(*observed));
      return failure(CaptureDeviceFailureCode::device_lost);
    }
    if (stop_token.stop_requested()) {
      auto aborted = m_native->abort();
      if (!aborted) return std::unexpected(native_failure(aborted.error()));
      return {};
    }
    m_native->wait_for_progress(polling_interval);
  }
  auto aborted = m_native->abort();
  if (!aborted) return std::unexpected(native_failure(aborted.error()));
  return failure(CaptureDeviceFailureCode::device_lost);
}

auto RtAudioCaptureDevice::stop() noexcept
    -> std::expected<void, CaptureDeviceFailure> {
  if (m_native == nullptr)
    return failure(CaptureDeviceFailureCode::internal_failure);
  auto result = m_native->stop();
  if (!result) return std::unexpected(native_failure(result.error()));
  return {};
}

auto RtAudioCaptureDevice::close() noexcept
    -> std::expected<void, CaptureDeviceFailure> {
  if (m_native == nullptr)
    return failure(CaptureDeviceFailureCode::internal_failure);
  auto result = m_native->close();
  if (!result) return std::unexpected(native_failure(result.error()));
  m_callback = nullptr;
  m_callback_frames = 0;
  m_channels = 0;
  m_maximum_polls = 0;
  return {};
}

auto RtAudioCaptureDevice::native_callback(
    const void* input, const std::size_t frames,
    const RtAudioCaptureNativeStatus status, void* user_data) noexcept -> int {
  auto* self = static_cast<RtAudioCaptureDevice*>(user_data);
  if (self == nullptr || self->m_callback == nullptr || input == nullptr ||
      frames == 0 || self->m_channels == 0 ||
      frames > std::numeric_limits<std::size_t>::max() / self->m_channels)
    return 2;
  auto mapped = CaptureCallbackStatus::normal;
  if (status == RtAudioCaptureNativeStatus::input_overflow)
    mapped = CaptureCallbackStatus::overrun;
  else if (status == RtAudioCaptureNativeStatus::device_lost)
    mapped = CaptureCallbackStatus::device_lost;
  const auto samples = std::span{static_cast<const std::int16_t*>(input),
                                 frames * self->m_channels};
  const auto decision = self->m_callback->process(samples, frames, mapped);
  if (decision != CaptureCallbackDecision::continue_operation)
    self->m_terminal.store(true, std::memory_order_release);
  switch (decision) {
    case CaptureCallbackDecision::continue_operation: return 0;
    case CaptureCallbackDecision::complete: return 1;
    case CaptureCallbackDecision::abort: return 2;
  }
  return 2;
}

auto make_rtaudio_capture_native()
    -> std::expected<std::unique_ptr<RtAudioCaptureNative>,
                     CaptureDeviceFailure> {
  try {
    auto native = std::make_unique<NativeRtAudioCapture>();
    if (!native->usable())
      return failure(CaptureDeviceFailureCode::unavailable);
    return native;
  } catch (const std::bad_alloc&) {
    return failure(CaptureDeviceFailureCode::internal_failure);
  } catch (...) {
    return failure(CaptureDeviceFailureCode::unavailable);
  }
}

} // namespace aiforge::adapters
