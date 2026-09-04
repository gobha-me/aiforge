#include "device_contract.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

namespace aiforge::evaluation::audio_device {
namespace {

enum class CallbackFailure {
  none,
  playback_underrun,
  capture_overrun,
  device_lost,
  contract_violation,
};

[[nodiscard]] auto valid_format(const Signed16Format format) noexcept -> bool {
  return (format.channels == 1 || format.channels == 2) &&
         format.sample_rate >= minimum_sample_rate &&
         format.sample_rate <= maximum_sample_rate;
}

[[nodiscard]] auto stats_from(
    const std::atomic<std::size_t>& callbacks,
    const std::atomic<std::size_t>& frames,
    const std::atomic<std::size_t>& xruns,
    const std::atomic<std::size_t>& late_callbacks) noexcept -> OperationStats {
  return {callbacks.load(), frames.load(), xruns.load(), late_callbacks.load()};
}

class ContractCallback final : public DeviceCallback {
 public:
  ContractCallback(const AudioDirection direction, const Signed16Format format,
                   const std::size_t total_frames,
                   const CancellationFlag* cancellation,
                   const std::vector<std::int16_t>* playback,
                   std::vector<std::int16_t>* capture) noexcept
      : m_direction{direction}, m_format{format}, m_total_frames{total_frames},
        m_cancellation{cancellation}, m_playback{playback}, m_capture{capture} {
  }

  [[nodiscard]] auto process(CallbackBuffer buffer) noexcept
      -> CallbackDecision override {
    if (!m_accepting.load()) {
      m_late_callbacks.fetch_add(1);
      return CallbackDecision::abort;
    }
    m_callbacks.fetch_add(1);
    if (m_terminal.load())
      return abort_with(CallbackFailure::contract_violation);
    if (m_cancellation != nullptr && m_cancellation->requested()) {
      m_terminal.store(true);
      return CallbackDecision::abort;
    }
    const auto sample_count = checked_sample_count(buffer.frames);
    if (!sample_count || !valid_buffers(buffer, *sample_count))
      return abort_with(CallbackFailure::contract_violation);
    if (const auto status = handle_status(buffer); status) return *status;

    const auto completed = m_frames.load();
    if (completed > m_total_frames)
      return abort_with(CallbackFailure::contract_violation);
    const auto remaining = m_total_frames - completed;
    if (m_direction == AudioDirection::capture && buffer.frames > remaining) {
      m_xruns.fetch_add(1);
      return abort_with(CallbackFailure::capture_overrun);
    }

    const auto accepted_frames = std::min(buffer.frames, remaining);
    transfer(buffer, completed, accepted_frames);
    m_frames.fetch_add(accepted_frames);
    if (accepted_frames == remaining) {
      m_terminal.store(true);
      return CallbackDecision::complete;
    }
    return CallbackDecision::continue_operation;
  }

  void deactivate() noexcept { m_accepting.store(false); }

  [[nodiscard]] auto failure() const noexcept -> CallbackFailure {
    return m_failure.load();
  }

  [[nodiscard]] auto stats() const noexcept -> OperationStats {
    return stats_from(m_callbacks, m_frames, m_xruns, m_late_callbacks);
  }

 private:
  [[nodiscard]] auto checked_sample_count(
      const std::size_t frames) const noexcept -> std::optional<std::size_t> {
    const auto channels = static_cast<std::size_t>(m_format.channels);
    if (frames == 0 ||
        frames > std::numeric_limits<std::size_t>::max() / channels)
      return std::nullopt;
    return frames * channels;
  }

  [[nodiscard]] auto valid_buffers(const CallbackBuffer& buffer,
                                   const std::size_t samples) const noexcept
      -> bool {
    if (m_direction == AudioDirection::playback)
      return buffer.playback_output.size() == samples &&
             buffer.capture_input.empty() && m_playback != nullptr;
    return buffer.capture_input.size() == samples &&
           buffer.playback_output.empty() && m_capture != nullptr;
  }

  [[nodiscard]] auto handle_status(CallbackBuffer& buffer) noexcept
      -> std::optional<CallbackDecision> {
    if (buffer.status == CallbackStatus::normal) return std::nullopt;
    if (buffer.status == CallbackStatus::device_lost)
      return abort_with(CallbackFailure::device_lost);

    m_xruns.fetch_add(1);
    if (buffer.status == CallbackStatus::playback_underrun) {
      if (m_direction == AudioDirection::playback)
        std::ranges::fill(buffer.playback_output, std::int16_t{});
      return abort_with(m_direction == AudioDirection::playback
                            ? CallbackFailure::playback_underrun
                            : CallbackFailure::contract_violation);
    }
    return abort_with(m_direction == AudioDirection::capture
                          ? CallbackFailure::capture_overrun
                          : CallbackFailure::contract_violation);
  }

  void transfer(CallbackBuffer& buffer, const std::size_t completed,
                const std::size_t accepted_frames) noexcept {
    const auto channels = static_cast<std::size_t>(m_format.channels);
    const auto accepted_samples = accepted_frames * channels;
    const auto sample_offset = completed * channels;
    if (m_direction == AudioDirection::playback) {
      std::copy_n(m_playback->data() + sample_offset, accepted_samples,
                  buffer.playback_output.data());
      std::fill(buffer.playback_output.begin() +
                    static_cast<std::ptrdiff_t>(accepted_samples),
                buffer.playback_output.end(), std::int16_t{});
      return;
    }
    std::copy_n(buffer.capture_input.data(), accepted_samples,
                m_capture->data() + sample_offset);
  }

  [[nodiscard]] auto abort_with(const CallbackFailure failure) noexcept
      -> CallbackDecision {
    fail(failure);
    m_terminal.store(true);
    return CallbackDecision::abort;
  }

  void fail(const CallbackFailure failure) noexcept {
    auto expected = CallbackFailure::none;
    static_cast<void>(m_failure.compare_exchange_strong(expected, failure));
  }

  AudioDirection m_direction;
  Signed16Format m_format;
  std::size_t m_total_frames;
  const CancellationFlag* m_cancellation;
  const std::vector<std::int16_t>* m_playback;
  std::vector<std::int16_t>* m_capture;
  std::atomic<bool> m_accepting{true};
  std::atomic<bool> m_terminal{false};
  std::atomic<CallbackFailure> m_failure{CallbackFailure::none};
  std::atomic<std::size_t> m_callbacks{};
  std::atomic<std::size_t> m_frames{};
  std::atomic<std::size_t> m_xruns{};
  std::atomic<std::size_t> m_late_callbacks{};
};

[[nodiscard]] auto error(const DeviceContractErrorCode code,
                         const DeviceStage stage, std::string message,
                         const OperationStats stats = {})
    -> std::unexpected<DeviceContractError> {
  return std::unexpected(
      DeviceContractError{code, stage, std::move(message), stats});
}

[[nodiscard]] auto mapped_device_error(const DeviceFailureCode code,
                                       const DeviceStage stage,
                                       const OperationStats stats)
    -> DeviceContractError {
  switch (code) {
    case DeviceFailureCode::unsupported_format:
      return {DeviceContractErrorCode::unsupported_format, stage,
              "audio device rejected the signed-16 format", stats};
    case DeviceFailureCode::permission_denied:
      return {DeviceContractErrorCode::permission_denied, stage,
              "audio device permission was denied", stats};
    case DeviceFailureCode::unavailable:
      return {DeviceContractErrorCode::unavailable, stage,
              "audio device is unavailable", stats};
    case DeviceFailureCode::device_lost:
      return {DeviceContractErrorCode::device_lost, stage,
              "audio device was lost", stats};
    case DeviceFailureCode::internal_failure:
      return {DeviceContractErrorCode::internal_failure, stage,
              "audio device failed internally", stats};
  }
  return {DeviceContractErrorCode::internal_failure, stage,
          "audio device failed internally", stats};
}

[[nodiscard]] auto mapped_callback_error(const CallbackFailure failure,
                                         const OperationStats stats)
    -> DeviceContractError {
  switch (failure) {
    case CallbackFailure::playback_underrun:
      return {DeviceContractErrorCode::playback_underrun, DeviceStage::stream,
              "audio playback underrun was observed", stats};
    case CallbackFailure::capture_overrun:
      return {DeviceContractErrorCode::capture_overrun, DeviceStage::stream,
              "audio capture exceeded its fixed buffer", stats};
    case CallbackFailure::device_lost:
      return {DeviceContractErrorCode::device_lost, DeviceStage::stream,
              "audio device was lost", stats};
    case CallbackFailure::contract_violation:
      return {DeviceContractErrorCode::callback_contract_violation,
              DeviceStage::stream,
              "audio callback violated its signed-16 buffer contract", stats};
    case CallbackFailure::none: break;
  }
  return {DeviceContractErrorCode::internal_failure, DeviceStage::stream,
          "audio callback failed internally", stats};
}

class ActiveGuard {
 public:
  explicit ActiveGuard(std::atomic<bool>& active) noexcept : m_active{active} {}
  ActiveGuard(const ActiveGuard&) = delete;
  auto operator=(const ActiveGuard&) -> ActiveGuard& = delete;
  ~ActiveGuard() { m_active.store(false); }

 private:
  std::atomic<bool>& m_active;
};

[[nodiscard]] auto cancellation_requested(
    const CancellationFlag* cancellation) noexcept -> bool {
  return cancellation != nullptr && cancellation->requested();
}

struct LifecycleInput {
  AudioDirection direction{AudioDirection::playback};
  Signed16Format format;
  std::size_t frames{};
};

struct LifecycleState {
  std::optional<DeviceContractError> primary;
  std::optional<DeviceContractError> stop_failure;
  std::optional<DeviceContractError> close_failure;
  std::optional<DeviceStage> cancellation_stage;
  bool open_attempted{false};
  bool start_attempted{false};

  void observe_cancellation(const CancellationFlag* cancellation,
                            const DeviceStage stage) {
    if (!cancellation_stage && cancellation_requested(cancellation))
      cancellation_stage = stage;
  }
};

void run_primary_lifecycle(BufferedAudioDevice& device,
                           ContractCallback& callback,
                           const LifecycleInput input,
                           const CancellationFlag* cancellation,
                           LifecycleState& state) {
  state.observe_cancellation(cancellation, DeviceStage::open);
  if (state.cancellation_stage) return;

  state.open_attempted = true;
  const auto opened = device.open(
      DeviceOpenRequest{input.direction, input.format, input.frames}, callback);
  if (!opened)
    state.primary = mapped_device_error(opened.error().code, DeviceStage::open,
                                        callback.stats());
  state.observe_cancellation(cancellation, DeviceStage::open);
  if (state.primary || state.cancellation_stage) return;

  state.start_attempted = true;
  const auto started = device.start();
  if (!started)
    state.primary = mapped_device_error(started.error().code,
                                        DeviceStage::start, callback.stats());
  state.observe_cancellation(cancellation, DeviceStage::start);
  if (state.primary || state.cancellation_stage) return;

  const auto streamed = device.stream();
  if (!streamed)
    state.primary = mapped_device_error(streamed.error().code,
                                        DeviceStage::stream, callback.stats());
  state.observe_cancellation(cancellation, DeviceStage::stream);
}

void run_cleanup(BufferedAudioDevice& device, ContractCallback& callback,
                 const CancellationFlag* cancellation, LifecycleState& state) {
  callback.deactivate();
  if (state.start_attempted) {
    const auto stopped = device.stop();
    if (!stopped)
      state.stop_failure = mapped_device_error(
          stopped.error().code, DeviceStage::stop, callback.stats());
    state.observe_cancellation(cancellation, DeviceStage::stop);
  }
  if (state.open_attempted) {
    const auto closed = device.close();
    if (!closed)
      state.close_failure = mapped_device_error(
          closed.error().code, DeviceStage::close, callback.stats());
    state.observe_cancellation(cancellation, DeviceStage::close);
  }
}

[[nodiscard]] auto lifecycle_result(const LifecycleState& state,
                                    const ContractCallback& callback,
                                    const LifecycleInput input)
    -> std::expected<OperationStats, DeviceContractError> {
  const auto stats = callback.stats();
  if (state.close_failure)
    return error(DeviceContractErrorCode::cleanup_failed, DeviceStage::close,
                 "audio device close did not complete", stats);
  if (state.stop_failure)
    return error(DeviceContractErrorCode::cleanup_failed, DeviceStage::stop,
                 "audio device stop did not complete", stats);
  if (stats.late_callbacks != 0)
    return error(DeviceContractErrorCode::late_callback, DeviceStage::close,
                 "audio device invoked a callback during teardown", stats);
  if (state.cancellation_stage)
    return error(DeviceContractErrorCode::cancelled, *state.cancellation_stage,
                 "audio device operation was cancelled", stats);
  if (callback.failure() != CallbackFailure::none)
    return std::unexpected(mapped_callback_error(callback.failure(), stats));
  if (state.primary) {
    auto primary = *state.primary;
    primary.stats = stats;
    return std::unexpected(std::move(primary));
  }
  if (stats.frames != input.frames)
    return error(DeviceContractErrorCode::incomplete_stream,
                 DeviceStage::stream,
                 "audio stream ended before its fixed buffer completed", stats);
  return stats;
}

[[nodiscard]] auto execute_lifecycle(BufferedAudioDevice& device,
                                     ContractCallback& callback,
                                     const LifecycleInput input,
                                     const CancellationFlag* cancellation)
    -> std::expected<OperationStats, DeviceContractError> {
  LifecycleState state;
  run_primary_lifecycle(device, callback, input, cancellation, state);
  run_cleanup(device, callback, cancellation, state);
  return lifecycle_result(state, callback, input);
}

} // namespace

void CancellationFlag::request() noexcept {
  m_requested.store(true);
}

auto CancellationFlag::requested() const noexcept -> bool {
  return m_requested.load();
}

DeviceContractController::DeviceContractController(
    const DeviceContractLimits limits)
    : m_limits{limits} {
}

auto DeviceContractController::play(
    BufferedAudioDevice& device, PlaybackBuffer buffer,
    const CancellationFlag* cancellation) noexcept
    -> std::expected<OperationStats, DeviceContractError> {
  try {
    if (m_limits.maximum_buffer_bytes == 0 ||
        m_limits.maximum_buffer_bytes > maximum_signed16_buffer_bytes ||
        m_limits.maximum_buffer_frames == 0 ||
        m_limits.maximum_buffer_frames > maximum_signed16_buffer_frames)
      return error(DeviceContractErrorCode::invalid_limits, DeviceStage::open,
                   "audio device limits are invalid");
    if (!valid_format(buffer.format))
      return error(DeviceContractErrorCode::invalid_format, DeviceStage::open,
                   "audio device format must be signed-16 mono or stereo");
    const auto channels = static_cast<std::size_t>(buffer.format.channels);
    if (buffer.interleaved_samples.empty() ||
        buffer.interleaved_samples.size() % channels != 0)
      return error(DeviceContractErrorCode::invalid_buffer, DeviceStage::open,
                   "audio playback buffer has incomplete sample frames");
    const auto frames = buffer.interleaved_samples.size() / channels;
    if (buffer.interleaved_samples.size() >
            m_limits.maximum_buffer_bytes / sizeof(std::int16_t) ||
        frames > m_limits.maximum_buffer_frames)
      return error(DeviceContractErrorCode::too_large, DeviceStage::open,
                   "audio playback buffer exceeds its configured limits");

    bool expected = false;
    if (!m_active.compare_exchange_strong(expected, true))
      return error(DeviceContractErrorCode::operation_in_progress,
                   DeviceStage::open,
                   "an audio device operation is already in progress");
    ActiveGuard active_guard{m_active};
    if (cancellation_requested(cancellation))
      return error(DeviceContractErrorCode::cancelled, DeviceStage::open,
                   "audio device operation was cancelled");

    ContractCallback callback{
        AudioDirection::playback,    buffer.format, frames, cancellation,
        &buffer.interleaved_samples, nullptr};
    return execute_lifecycle(
        device, callback,
        LifecycleInput{AudioDirection::playback, buffer.format, frames},
        cancellation);
  } catch (...) {
    return error(DeviceContractErrorCode::internal_failure, DeviceStage::open,
                 "audio playback controller failed internally");
  }
}

auto DeviceContractController::capture(
    BufferedAudioDevice& device, const CaptureRequest request,
    const CancellationFlag* cancellation) noexcept
    -> std::expected<CaptureResult, DeviceContractError> {
  try {
    if (m_limits.maximum_buffer_bytes == 0 ||
        m_limits.maximum_buffer_bytes > maximum_signed16_buffer_bytes ||
        m_limits.maximum_buffer_frames == 0 ||
        m_limits.maximum_buffer_frames > maximum_signed16_buffer_frames)
      return error(DeviceContractErrorCode::invalid_limits, DeviceStage::open,
                   "audio device limits are invalid");
    if (!valid_format(request.format))
      return error(DeviceContractErrorCode::invalid_format, DeviceStage::open,
                   "audio device format must be signed-16 mono or stereo");
    if (request.frames == 0)
      return error(DeviceContractErrorCode::invalid_buffer, DeviceStage::open,
                   "audio capture buffer must contain at least one frame");
    const auto bytes_per_frame =
        static_cast<std::size_t>(request.format.channels) *
        sizeof(std::int16_t);
    if (request.frames > m_limits.maximum_buffer_bytes / bytes_per_frame ||
        request.frames > m_limits.maximum_buffer_frames)
      return error(DeviceContractErrorCode::too_large, DeviceStage::open,
                   "audio capture buffer exceeds its configured limits");

    bool expected = false;
    if (!m_active.compare_exchange_strong(expected, true))
      return error(DeviceContractErrorCode::operation_in_progress,
                   DeviceStage::open,
                   "an audio device operation is already in progress");
    ActiveGuard active_guard{m_active};
    if (cancellation_requested(cancellation))
      return error(DeviceContractErrorCode::cancelled, DeviceStage::open,
                   "audio device operation was cancelled");

    const auto channels = static_cast<std::size_t>(request.format.channels);
    std::vector<std::int16_t> captured(request.frames * channels);
    ContractCallback callback{AudioDirection::capture,
                              request.format,
                              request.frames,
                              cancellation,
                              nullptr,
                              &captured};
    auto completed = execute_lifecycle(
        device, callback,
        LifecycleInput{AudioDirection::capture, request.format, request.frames},
        cancellation);
    if (!completed) return std::unexpected(std::move(completed.error()));
    return CaptureResult{request.format, std::move(captured), *completed};
  } catch (...) {
    return error(DeviceContractErrorCode::internal_failure, DeviceStage::open,
                 "audio capture controller failed internally");
  }
}

auto DeviceContractController::active() const noexcept -> bool {
  return m_active.load();
}

} // namespace aiforge::evaluation::audio_device
