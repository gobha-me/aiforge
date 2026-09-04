#include "rtaudio_playback.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <limits>
#include <new>
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

[[nodiscard]] auto failure(const PlaybackDeviceFailureCode code)
    -> std::unexpected<PlaybackDeviceFailure> {
  return std::unexpected(PlaybackDeviceFailure{code});
}

[[nodiscard]] auto native_failure(const RtAudioNativeError code)
    -> PlaybackDeviceFailure {
  switch (code) {
    case RtAudioNativeError::unsupported_format:
      return {PlaybackDeviceFailureCode::unsupported_format};
    case RtAudioNativeError::permission_denied:
      return {PlaybackDeviceFailureCode::permission_denied};
    case RtAudioNativeError::unavailable:
      return {PlaybackDeviceFailureCode::unavailable};
    case RtAudioNativeError::device_lost:
      return {PlaybackDeviceFailureCode::device_lost};
    case RtAudioNativeError::internal_failure:
      return {PlaybackDeviceFailureCode::internal_failure};
  }
  return {PlaybackDeviceFailureCode::internal_failure};
}

[[nodiscard]] auto playback_poll_limit(
    const PlaybackDeviceOpenRequest& request) noexcept
    -> std::optional<std::size_t> {
  if ((request.format.channels != 1 && request.format.channels != 2) ||
      request.format.sample_rate < 8000 ||
      request.format.sample_rate > 192000 || request.frames == 0 ||
      request.frames > maximum_playback_frames) {
    return std::nullopt;
  }
  const auto frames = static_cast<std::uint64_t>(request.frames);
  const auto sample_rate =
      static_cast<std::uint64_t>(request.format.sample_rate);
  const auto whole_seconds = frames / sample_rate;
  if (whole_seconds >
      (std::numeric_limits<std::uint64_t>::max() - cleanup_margin_polls) /
          polls_per_second) {
    return std::nullopt;
  }
  const auto remaining_frames = frames % sample_rate;
  const auto scaled_remainder = remaining_frames * polls_per_second;
  const auto partial_second_polls =
      scaled_remainder / sample_rate +
      static_cast<std::uint64_t>(scaled_remainder % sample_rate != 0);
  const auto duration_polls = whole_seconds * polls_per_second;
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  if (duration_polls > maximum - cleanup_margin_polls ||
      partial_second_polls > maximum - cleanup_margin_polls - duration_polls) {
    return std::nullopt;
  }
  const auto total_polls =
      duration_polls + partial_second_polls + cleanup_margin_polls;
  if (total_polls > std::numeric_limits<std::size_t>::max())
    return std::nullopt;
  return static_cast<std::size_t>(total_polls);
}

[[nodiscard]] auto map_error(const RtAudioErrorType code)
    -> RtAudioNativeError {
  switch (code) {
    case RTAUDIO_NO_DEVICES_FOUND:
    case RTAUDIO_INVALID_DEVICE: return RtAudioNativeError::unavailable;
    case RTAUDIO_DEVICE_DISCONNECT: return RtAudioNativeError::device_lost;
    case RTAUDIO_INVALID_PARAMETER:
      return RtAudioNativeError::unsupported_format;
    case RTAUDIO_NO_ERROR:
    case RTAUDIO_WARNING:
    case RTAUDIO_UNKNOWN_ERROR:
    case RTAUDIO_MEMORY_ERROR:
    case RTAUDIO_INVALID_USE:
    case RTAUDIO_DRIVER_ERROR:
    case RTAUDIO_SYSTEM_ERROR:
    case RTAUDIO_THREAD_ERROR: return RtAudioNativeError::internal_failure;
  }
  return RtAudioNativeError::internal_failure;
}

class NativeRtAudio final : public RtAudioNative {
 public:
  NativeRtAudio()
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

  [[nodiscard]] auto open(RtAudioNativeOpenRequest& request,
                          const RtAudioNativeCallback callback,
                          void* user_data) noexcept
      -> std::expected<void, RtAudioNativeError> override {
    try {
      if (!request.alsa_api || !request.default_output ||
          !request.signed16_interleaved || request.channels == 0 ||
          request.callback_frames == 0 ||
          request.callback_frames >
              static_cast<std::size_t>(
                  std::numeric_limits<unsigned int>::max())) {
        return std::unexpected(RtAudioNativeError::unsupported_format);
      }
      m_last_error.store(RTAUDIO_NO_ERROR, std::memory_order_release);
      RtAudio::StreamParameters output{m_audio.getDefaultOutputDevice(),
                                       request.channels, 0};
      if (output.deviceId == 0)
        return std::unexpected(RtAudioNativeError::unavailable);
      auto callback_frames = static_cast<unsigned int>(request.callback_frames);
      RtAudio::StreamOptions options;
      options.flags = RTAUDIO_ALSA_USE_DEFAULT;
      m_bridge = {callback, user_data};
      errno = 0;
      const auto opened = m_audio.openStream(
          &output, nullptr, RTAUDIO_SINT16, request.sample_rate,
          &callback_frames,
          [](void* output_buffer, void*, const unsigned int frames, double,
             const RtAudioStreamStatus status, void* callback_data) -> int {
            auto* bridge = static_cast<NativeCallbackBridge*>(callback_data);
            auto mapped = RtAudioNativeStatus::normal;
            if ((status & RTAUDIO_OUTPUT_UNDERFLOW) != 0U)
              mapped = RtAudioNativeStatus::output_underflow;
            return bridge->callback(output_buffer, frames, mapped,
                                    bridge->user_data);
          },
          &m_bridge, &options);
      const auto open_errno = errno;
      if ((opened == RTAUDIO_DRIVER_ERROR || opened == RTAUDIO_SYSTEM_ERROR) &&
          (open_errno == EACCES || open_errno == EPERM)) {
        return std::unexpected(RtAudioNativeError::permission_denied);
      }
      if (opened != RTAUDIO_NO_ERROR) return std::unexpected(map_error(opened));
      request.callback_frames = callback_frames;
      return {};
    } catch (...) {
      return std::unexpected(RtAudioNativeError::internal_failure);
    }
  }

  [[nodiscard]] auto start() noexcept
      -> std::expected<void, RtAudioNativeError> override {
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
      -> std::optional<RtAudioNativeError> override {
    const auto observed = m_last_error.load(std::memory_order_acquire);
    if (observed == RTAUDIO_NO_ERROR || observed == RTAUDIO_WARNING)
      return std::nullopt;
    return map_error(observed);
  }

  [[nodiscard]] auto abort() noexcept
      -> std::expected<void, RtAudioNativeError> override {
    if (!running()) return {};
    return call([this] { return m_audio.abortStream(); });
  }

  [[nodiscard]] auto stop() noexcept
      -> std::expected<void, RtAudioNativeError> override {
    if (!running()) return {};
    return call([this] { return m_audio.stopStream(); });
  }

  [[nodiscard]] auto close() noexcept
      -> std::expected<void, RtAudioNativeError> override {
    try {
      m_last_error.store(RTAUDIO_NO_ERROR, std::memory_order_release);
      if (m_audio.isStreamOpen()) m_audio.closeStream();
      if (m_audio.isStreamOpen())
        return std::unexpected(RtAudioNativeError::internal_failure);
      const auto observed = m_last_error.load(std::memory_order_acquire);
      if (observed != RTAUDIO_NO_ERROR && observed != RTAUDIO_WARNING)
        return std::unexpected(map_error(observed));
      m_bridge = {};
      return {};
    } catch (...) {
      return std::unexpected(RtAudioNativeError::internal_failure);
    }
  }

  auto wait_for_progress(const std::chrono::milliseconds duration) noexcept
      -> void override {
    std::this_thread::sleep_for(duration);
  }

 private:
  struct NativeCallbackBridge {
    RtAudioNativeCallback callback{};
    void* user_data{};
  };

  template <typename Operation>
  [[nodiscard]] auto call(Operation operation) noexcept
      -> std::expected<void, RtAudioNativeError> {
    try {
      m_last_error.store(RTAUDIO_NO_ERROR, std::memory_order_release);
      const auto result = operation();
      if (result != RTAUDIO_NO_ERROR) return std::unexpected(map_error(result));
      return {};
    } catch (...) {
      return std::unexpected(RtAudioNativeError::internal_failure);
    }
  }

  // RtAudio is declared last so it is destroyed first while its error target
  // and callback bridge remain alive.
  std::atomic<RtAudioErrorType> m_last_error{RTAUDIO_NO_ERROR};
  NativeCallbackBridge m_bridge;
  RtAudio m_audio;
};

} // namespace

RtAudioPlaybackDevice::RtAudioPlaybackDevice(
    std::unique_ptr<RtAudioNative> native)
    : m_native{std::move(native)} {
}

auto RtAudioPlaybackDevice::open(const PlaybackDeviceOpenRequest& request,
                                 PlaybackDeviceCallback& callback) noexcept
    -> std::expected<void, PlaybackDeviceFailure> {
  try {
    const auto maximum_polls = playback_poll_limit(request);
    if (m_native == nullptr || !maximum_polls) {
      return failure(PlaybackDeviceFailureCode::unsupported_format);
    }
    m_callback = &callback;
    m_terminal.store(false, std::memory_order_release);
    RtAudioNativeOpenRequest native_request{
        request.format.sample_rate,
        request.format.channels,
        std::min(request.frames, desired_callback_frames),
        true,
        true,
        true};
    auto opened = m_native->open(native_request, &native_callback, this);
    if (!opened) return std::unexpected(native_failure(opened.error()));
    if (native_request.callback_frames == 0) {
      return failure(PlaybackDeviceFailureCode::internal_failure);
    }
    m_callback_frames = native_request.callback_frames;
    m_channels = request.format.channels;
    m_maximum_polls = *maximum_polls;
    return {};
  } catch (...) {
    return failure(PlaybackDeviceFailureCode::internal_failure);
  }
}

auto RtAudioPlaybackDevice::start() noexcept
    -> std::expected<void, PlaybackDeviceFailure> {
  if (m_native == nullptr)
    return failure(PlaybackDeviceFailureCode::internal_failure);
  auto started = m_native->start();
  if (!started) return std::unexpected(native_failure(started.error()));
  return {};
}

auto RtAudioPlaybackDevice::stream(const std::stop_token stop_token) noexcept
    -> std::expected<void, PlaybackDeviceFailure> {
  if (m_native == nullptr || m_callback == nullptr || m_callback_frames == 0 ||
      m_maximum_polls == 0) {
    return failure(PlaybackDeviceFailureCode::internal_failure);
  }
  for (std::size_t poll{}; poll < m_maximum_polls; ++poll) {
    if (m_terminal.load(std::memory_order_acquire)) return {};
    if (!m_native->running()) {
      if (const auto stream_failure = m_native->stream_failure())
        return std::unexpected(native_failure(*stream_failure));
      return failure(PlaybackDeviceFailureCode::device_lost);
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
  return failure(PlaybackDeviceFailureCode::device_lost);
}

auto RtAudioPlaybackDevice::stop() noexcept
    -> std::expected<void, PlaybackDeviceFailure> {
  if (m_native == nullptr)
    return failure(PlaybackDeviceFailureCode::internal_failure);
  auto stopped = m_native->stop();
  if (!stopped) return std::unexpected(native_failure(stopped.error()));
  return {};
}

auto RtAudioPlaybackDevice::close() noexcept
    -> std::expected<void, PlaybackDeviceFailure> {
  if (m_native == nullptr)
    return failure(PlaybackDeviceFailureCode::internal_failure);
  auto closed = m_native->close();
  if (!closed) return std::unexpected(native_failure(closed.error()));
  m_callback = nullptr;
  m_callback_frames = 0;
  m_channels = 0;
  m_maximum_polls = 0;
  return {};
}

auto RtAudioPlaybackDevice::native_callback(void* output,
                                            const std::size_t frames,
                                            const RtAudioNativeStatus status,
                                            void* user_data) noexcept -> int {
  auto* self = static_cast<RtAudioPlaybackDevice*>(user_data);
  if (self == nullptr || self->m_callback == nullptr || output == nullptr ||
      frames == 0 || self->m_channels == 0 ||
      frames > std::numeric_limits<std::size_t>::max() / self->m_channels) {
    return 2;
  }
  auto mapped = PlaybackCallbackStatus::normal;
  if (status == RtAudioNativeStatus::output_underflow)
    mapped = PlaybackCallbackStatus::underrun;
  else if (status == RtAudioNativeStatus::device_lost)
    mapped = PlaybackCallbackStatus::device_lost;
  const auto samples = frames * self->m_channels;
  auto output_samples = std::span{static_cast<std::int16_t*>(output), samples};
  const auto decision =
      self->m_callback->process(output_samples, frames, mapped);
  if (decision == PlaybackCallbackDecision::continue_operation) return 0;
  self->m_terminal.store(true, std::memory_order_release);
  return decision == PlaybackCallbackDecision::complete ? 1 : 2;
}

auto make_rtaudio_native()
    -> std::expected<std::unique_ptr<RtAudioNative>, PlaybackDeviceFailure> {
  try {
    auto native = std::make_unique<NativeRtAudio>();
    if (!native->usable())
      return failure(PlaybackDeviceFailureCode::unavailable);
    return native;
  } catch (const std::bad_alloc&) {
    return failure(PlaybackDeviceFailureCode::internal_failure);
  } catch (...) {
    return failure(PlaybackDeviceFailureCode::unavailable);
  }
}

} // namespace aiforge::adapters
