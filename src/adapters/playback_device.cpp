#include "playback_device.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <utility>

namespace aiforge::adapters {
namespace {

enum class CallbackFailure { none, underrun, device_lost, contract_violation };

[[nodiscard]] auto failure(audio::PlaybackErrorCode code,
                           audio::PlaybackStage stage, std::string message,
                           audio::PlaybackStats stats = {})
    -> std::unexpected<audio::PlaybackError> {
  return std::unexpected(
      audio::PlaybackError{code, stage, std::move(message), stats});
}

[[nodiscard]] auto valid_format(const audio::Signed16Format format) noexcept
    -> bool {
  return (format.channels == 1 || format.channels == 2) &&
         format.sample_rate >= 8000 && format.sample_rate <= 192000;
}

class ContractCallback final : public PlaybackDeviceCallback {
 public:
  ContractCallback(const audio::Signed16Format format,
                   std::vector<std::int16_t> samples,
                   const std::stop_token stop_token,
                   PlaybackCallbackTestFence* test_fence) noexcept
      : m_format{format}, m_samples{std::move(samples)},
        m_stop_token{stop_token}, m_test_fence{test_fence} {}

  [[nodiscard]] auto process(const std::span<std::int16_t> output,
                             const std::size_t frames,
                             const PlaybackCallbackStatus status) noexcept
      -> PlaybackCallbackDecision override {
    const auto prior_in_flight =
        m_in_flight.fetch_add(1, std::memory_order_acq_rel);
    struct CallbackGuard final {
      std::atomic<std::size_t>& count;
      ~CallbackGuard() {
        count.fetch_sub(1, std::memory_order_acq_rel);
        count.notify_all();
      }
    } callback_guard{m_in_flight};
    if (prior_in_flight != 0) {
      std::ranges::fill(output, std::int16_t{});
      return abort_with(CallbackFailure::contract_violation);
    }
    if (m_test_fence != nullptr) {
      m_test_fence->entered.store(true, std::memory_order_release);
      m_test_fence->entered.notify_all();
      while (!m_test_fence->release.load(std::memory_order_acquire))
        m_test_fence->release.wait(false, std::memory_order_relaxed);
    }
    if (!m_accepting.load(std::memory_order_acquire)) {
      m_late_callbacks.fetch_add(1, std::memory_order_relaxed);
      std::ranges::fill(output, std::int16_t{});
      return PlaybackCallbackDecision::abort;
    }
    m_callbacks.fetch_add(1, std::memory_order_relaxed);
    if (m_terminal.load(std::memory_order_acquire)) {
      std::ranges::fill(output, std::int16_t{});
      return abort_with(CallbackFailure::contract_violation);
    }
    const auto channels = static_cast<std::size_t>(m_format.channels);
    if (frames == 0 ||
        frames > std::numeric_limits<std::size_t>::max() / channels ||
        output.size() != frames * channels) {
      std::ranges::fill(output, std::int16_t{});
      return abort_with(CallbackFailure::contract_violation);
    }
    if (m_stop_token.stop_requested()) {
      std::ranges::fill(output, std::int16_t{});
      m_terminal.store(true, std::memory_order_release);
      return PlaybackCallbackDecision::abort;
    }
    if (status != PlaybackCallbackStatus::normal) {
      std::ranges::fill(output, std::int16_t{});
      if (status == PlaybackCallbackStatus::underrun) {
        m_underruns.fetch_add(1, std::memory_order_relaxed);
        return abort_with(CallbackFailure::underrun);
      }
      return abort_with(CallbackFailure::device_lost);
    }

    const auto completed = m_frames.load(std::memory_order_relaxed);
    const auto total_frames = m_samples.size() / channels;
    if (completed > total_frames) {
      std::ranges::fill(output, std::int16_t{});
      return abort_with(CallbackFailure::contract_violation);
    }
    const auto remaining = total_frames - completed;
    const auto accepted_frames = std::min(frames, remaining);
    const auto accepted_samples = accepted_frames * channels;
    std::copy_n(m_samples.data() + completed * channels, accepted_samples,
                output.data());
    std::fill(output.begin() + static_cast<std::ptrdiff_t>(accepted_samples),
              output.end(), std::int16_t{});
    m_frames.fetch_add(accepted_frames, std::memory_order_relaxed);
    if (accepted_frames == remaining) {
      m_terminal.store(true, std::memory_order_release);
      return PlaybackCallbackDecision::complete;
    }
    return PlaybackCallbackDecision::continue_operation;
  }

  auto deactivate() noexcept -> void {
    m_accepting.store(false, std::memory_order_release);
  }

  [[nodiscard]] auto callback_failure() const noexcept -> CallbackFailure {
    return m_failure.load(std::memory_order_acquire);
  }

  [[nodiscard]] auto stats() const noexcept -> audio::PlaybackStats {
    return {m_callbacks.load(std::memory_order_relaxed),
            m_frames.load(std::memory_order_relaxed),
            m_underruns.load(std::memory_order_relaxed),
            m_late_callbacks.load(std::memory_order_relaxed)};
  }

  [[nodiscard]] auto quiescent() const noexcept -> bool {
    return m_in_flight.load(std::memory_order_acquire) == 0;
  }

 private:
  [[nodiscard]] auto abort_with(const CallbackFailure failure) noexcept
      -> PlaybackCallbackDecision {
    auto expected = CallbackFailure::none;
    static_cast<void>(m_failure.compare_exchange_strong(
        expected, failure, std::memory_order_acq_rel));
    m_terminal.store(true, std::memory_order_release);
    return PlaybackCallbackDecision::abort;
  }

  audio::Signed16Format m_format;
  std::vector<std::int16_t> m_samples;
  std::stop_token m_stop_token;
  PlaybackCallbackTestFence* m_test_fence{};
  std::atomic<std::size_t> m_in_flight{};
  std::atomic<bool> m_accepting{true};
  std::atomic<bool> m_terminal{false};
  std::atomic<CallbackFailure> m_failure{CallbackFailure::none};
  std::atomic<std::size_t> m_callbacks{};
  std::atomic<std::size_t> m_frames{};
  std::atomic<std::size_t> m_underruns{};
  std::atomic<std::size_t> m_late_callbacks{};
};

[[nodiscard]] auto map_device_failure(const PlaybackDeviceFailureCode code,
                                      const audio::PlaybackStage stage,
                                      const audio::PlaybackStats stats)
    -> audio::PlaybackError {
  switch (code) {
    case PlaybackDeviceFailureCode::unsupported_format:
      return {audio::PlaybackErrorCode::unsupported_format, stage,
              "audio device rejected the signed-16 format", stats};
    case PlaybackDeviceFailureCode::permission_denied:
      return {audio::PlaybackErrorCode::permission_denied, stage,
              "audio device permission was denied", stats};
    case PlaybackDeviceFailureCode::unavailable:
      return {audio::PlaybackErrorCode::unavailable, stage,
              "audio output is unavailable", stats};
    case PlaybackDeviceFailureCode::device_lost:
      return {audio::PlaybackErrorCode::device_lost, stage,
              "audio output was lost", stats};
    case PlaybackDeviceFailureCode::internal_failure:
      return {audio::PlaybackErrorCode::internal_failure, stage,
              "audio device failed internally", stats};
  }
  return {audio::PlaybackErrorCode::internal_failure, stage,
          "audio device failed internally", stats};
}

[[nodiscard]] auto map_callback_failure(const CallbackFailure callback,
                                        const audio::PlaybackStats stats)
    -> audio::PlaybackError {
  switch (callback) {
    case CallbackFailure::underrun:
      return {audio::PlaybackErrorCode::underrun, audio::PlaybackStage::stream,
              "audio playback underrun was observed", stats};
    case CallbackFailure::device_lost:
      return {audio::PlaybackErrorCode::device_lost,
              audio::PlaybackStage::stream, "audio output was lost", stats};
    case CallbackFailure::contract_violation:
      return {audio::PlaybackErrorCode::callback_contract_violation,
              audio::PlaybackStage::stream,
              "audio callback violated its signed-16 buffer contract", stats};
    case CallbackFailure::none: break;
  }
  return {audio::PlaybackErrorCode::internal_failure,
          audio::PlaybackStage::stream, "audio callback failed internally",
          stats};
}

[[nodiscard]] auto observe(audio::PlaybackObserver* observer,
                           const audio::PlaybackStage stage) noexcept -> bool {
  return observer == nullptr || observer->stage_changed(stage);
}

struct LifecycleState {
  std::optional<audio::PlaybackError> primary;
  std::optional<audio::PlaybackError> stop_failure;
  std::optional<audio::PlaybackError> close_failure;
  std::optional<audio::PlaybackStage> cancellation_stage;
  bool open_attempted{};
  bool start_attempted{};
  bool close_complete{};

  auto check_cancellation(const std::stop_token stop_token,
                          const audio::PlaybackStage stage) -> void {
    if (!cancellation_stage && stop_token.stop_requested())
      cancellation_stage = stage;
  }
};

void run_primary_lifecycle(BufferedPlaybackDevice& device,
                           ContractCallback& callback,
                           const PlaybackDeviceOpenRequest& request,
                           const std::stop_token stop_token,
                           audio::PlaybackObserver* observer,
                           LifecycleState& state) {
  state.check_cancellation(stop_token, audio::PlaybackStage::open);
  if (state.cancellation_stage) return;

  if (!observe(observer, audio::PlaybackStage::open)) {
    state.primary = {audio::PlaybackErrorCode::internal_failure,
                     audio::PlaybackStage::open,
                     "audio playback status output failed", callback.stats()};
    return;
  }
  state.open_attempted = true;
  const auto opened = device.open(request, callback);
  if (!opened)
    state.primary = map_device_failure(
        opened.error().code, audio::PlaybackStage::open, callback.stats());
  state.check_cancellation(stop_token, audio::PlaybackStage::open);
  if (state.primary || state.cancellation_stage) return;

  if (!observe(observer, audio::PlaybackStage::start)) {
    state.primary = {audio::PlaybackErrorCode::internal_failure,
                     audio::PlaybackStage::start,
                     "audio playback status output failed", callback.stats()};
    return;
  }
  state.start_attempted = true;
  const auto started = device.start();
  if (!started)
    state.primary = map_device_failure(
        started.error().code, audio::PlaybackStage::start, callback.stats());
  state.check_cancellation(stop_token, audio::PlaybackStage::start);
  if (state.primary || state.cancellation_stage) return;

  if (!observe(observer, audio::PlaybackStage::stream)) {
    state.primary = {audio::PlaybackErrorCode::internal_failure,
                     audio::PlaybackStage::stream,
                     "audio playback status output failed", callback.stats()};
    return;
  }
  const auto streamed = device.stream(stop_token);
  if (!streamed)
    state.primary = map_device_failure(
        streamed.error().code, audio::PlaybackStage::stream, callback.stats());
  state.check_cancellation(stop_token, audio::PlaybackStage::stream);
}

void run_cleanup(BufferedPlaybackDevice& device, ContractCallback& callback,
                 const std::stop_token stop_token,
                 audio::PlaybackObserver* observer, LifecycleState& state) {
  callback.deactivate();
  if (state.start_attempted) {
    if (!observe(observer, audio::PlaybackStage::stop) && !state.primary) {
      state.primary = {audio::PlaybackErrorCode::internal_failure,
                       audio::PlaybackStage::stop,
                       "audio playback status output failed", callback.stats()};
    }
    const auto stopped = device.stop();
    if (!stopped)
      state.stop_failure = map_device_failure(
          stopped.error().code, audio::PlaybackStage::stop, callback.stats());
    state.check_cancellation(stop_token, audio::PlaybackStage::stop);
  }
  if (state.open_attempted) {
    if (!observe(observer, audio::PlaybackStage::close) && !state.primary) {
      state.primary = {audio::PlaybackErrorCode::internal_failure,
                       audio::PlaybackStage::close,
                       "audio playback status output failed", callback.stats()};
    }
    const auto closed = device.close();
    state.close_complete = closed.has_value();
    if (!closed)
      state.close_failure = map_device_failure(
          closed.error().code, audio::PlaybackStage::close, callback.stats());
    state.check_cancellation(stop_token, audio::PlaybackStage::close);
  } else {
    state.close_complete = true;
  }
}

[[nodiscard]] auto lifecycle_result(const LifecycleState& state,
                                    const ContractCallback& callback,
                                    const std::size_t expected_frames,
                                    const bool callback_quiescent)
    -> std::expected<audio::PlaybackStats, audio::PlaybackError> {
  const auto stats = callback.stats();
  if (state.close_failure)
    return failure(audio::PlaybackErrorCode::cleanup_failed,
                   audio::PlaybackStage::close,
                   "audio device close did not complete", stats);
  if (stats.late_callbacks != 0)
    return failure(audio::PlaybackErrorCode::late_callback,
                   audio::PlaybackStage::close,
                   "audio device invoked a callback during teardown", stats);
  if (!callback_quiescent)
    return failure(audio::PlaybackErrorCode::late_callback,
                   audio::PlaybackStage::close,
                   "audio callback did not quiesce during teardown", stats);
  if (state.stop_failure)
    return failure(audio::PlaybackErrorCode::cleanup_failed,
                   audio::PlaybackStage::stop,
                   "audio device stop did not complete", stats);
  if (state.cancellation_stage)
    return failure(audio::PlaybackErrorCode::cancelled,
                   *state.cancellation_stage, "audio playback was cancelled",
                   stats);
  if (callback.callback_failure() != CallbackFailure::none)
    return std::unexpected(
        map_callback_failure(callback.callback_failure(), stats));
  if (state.primary) {
    auto primary = *state.primary;
    primary.stats = stats;
    return std::unexpected(std::move(primary));
  }
  if (stats.frames != expected_frames)
    return failure(audio::PlaybackErrorCode::incomplete_stream,
                   audio::PlaybackStage::stream,
                   "audio playback ended before its buffer completed", stats);
  return stats;
}

class GateGuard final {
 public:
  explicit GateGuard(AudioDeviceGate& gate) noexcept : m_gate{gate} {}
  GateGuard(const GateGuard&) = delete;
  auto operator=(const GateGuard&) -> GateGuard& = delete;
  ~GateGuard() {
    if (!m_finalized) m_gate.quarantine();
  }
  auto release() noexcept -> void {
    m_gate.release();
    m_finalized = true;
  }
  auto quarantine(std::shared_ptr<PlaybackDeviceCallback> callback) noexcept
      -> void {
    m_gate.quarantine(std::move(callback));
    m_finalized = true;
  }

 private:
  AudioDeviceGate& m_gate;
  bool m_finalized{};
};

} // namespace

auto AudioDeviceGate::begin() noexcept -> BeginResult {
  auto expected = State::idle;
  if (m_state.compare_exchange_strong(expected, State::active,
                                      std::memory_order_acq_rel))
    return BeginResult::acquired;
  return expected == State::quarantined ? BeginResult::quarantined
                                        : BeginResult::operation_in_progress;
}

auto AudioDeviceGate::release() noexcept -> void {
  auto expected = State::active;
  static_cast<void>(m_state.compare_exchange_strong(expected, State::idle,
                                                    std::memory_order_acq_rel));
}

auto AudioDeviceGate::quarantine(
    std::shared_ptr<PlaybackDeviceCallback> callback) noexcept -> void {
  m_quarantined_callback = std::move(callback);
  m_state.store(State::quarantined, std::memory_order_release);
}

auto AudioDeviceGate::active() const noexcept -> bool {
  return m_state.load(std::memory_order_acquire) == State::active;
}

auto AudioDeviceGate::quarantined() const noexcept -> bool {
  return m_state.load(std::memory_order_acquire) == State::quarantined;
}

PlaybackController::PlaybackController(AudioDeviceGate& gate,
                                       BufferedPlaybackDevice& device,
                                       const PlaybackControllerLimits limits)
    : m_gate{gate}, m_device{device}, m_limits{limits} {
}

auto PlaybackController::play(audio::Signed16Buffer buffer,
                              const std::stop_token stop_token,
                              audio::PlaybackObserver* observer) noexcept
    -> std::expected<audio::PlaybackStats, audio::PlaybackError> {
  std::shared_ptr<ContractCallback> callback;
  bool gate_acquired{};
  try {
    if (m_limits.maximum_buffer_bytes == 0 ||
        m_limits.maximum_buffer_bytes > maximum_playback_bytes ||
        m_limits.maximum_buffer_frames == 0 ||
        m_limits.maximum_buffer_frames > maximum_playback_frames) {
      return failure(audio::PlaybackErrorCode::internal_failure,
                     audio::PlaybackStage::open,
                     "audio playback limits are invalid");
    }
    if (!valid_format(buffer.format))
      return failure(audio::PlaybackErrorCode::invalid_format,
                     audio::PlaybackStage::open,
                     "audio playback requires signed-16 mono or stereo");
    const auto channels = static_cast<std::size_t>(buffer.format.channels);
    if (buffer.interleaved_samples.empty() ||
        buffer.interleaved_samples.size() % channels != 0) {
      return failure(audio::PlaybackErrorCode::invalid_buffer,
                     audio::PlaybackStage::open,
                     "audio playback buffer has incomplete sample frames");
    }
    const auto frames = buffer.interleaved_samples.size() / channels;
    if (buffer.interleaved_samples.size() >
            m_limits.maximum_buffer_bytes / sizeof(std::int16_t) ||
        frames > m_limits.maximum_buffer_frames) {
      return failure(audio::PlaybackErrorCode::too_large,
                     audio::PlaybackStage::open,
                     "audio playback buffer exceeds its configured limits");
    }

    callback = std::make_shared<ContractCallback>(
        buffer.format, std::move(buffer.interleaved_samples), stop_token,
        m_limits.callback_test_fence);

    const auto acquired = m_gate.begin();
    if (acquired == AudioDeviceGate::BeginResult::operation_in_progress)
      return failure(audio::PlaybackErrorCode::operation_in_progress,
                     audio::PlaybackStage::open,
                     "an audio device operation is already in progress");
    if (acquired == AudioDeviceGate::BeginResult::quarantined)
      return failure(audio::PlaybackErrorCode::device_quarantined,
                     audio::PlaybackStage::open,
                     "audio device cleanup is indeterminate");
    gate_acquired = true;
    GateGuard gate_guard{m_gate};
    if (stop_token.stop_requested()) {
      gate_guard.release();
      return failure(audio::PlaybackErrorCode::cancelled,
                     audio::PlaybackStage::open,
                     "audio playback was cancelled");
    }

    LifecycleState state;
    run_primary_lifecycle(m_device, *callback,
                          PlaybackDeviceOpenRequest{buffer.format, frames},
                          stop_token, observer, state);
    run_cleanup(m_device, *callback, stop_token, observer, state);
    const auto callback_quiescent = callback->quiescent();
    const auto result =
        lifecycle_result(state, *callback, frames, callback_quiescent);
    if (!state.close_complete || callback->stats().late_callbacks != 0 ||
        !callback_quiescent) {
      gate_guard.quarantine(std::move(callback));
    } else {
      gate_guard.release();
    }
    return result;
  } catch (...) {
    if (gate_acquired) {
      m_gate.quarantine(std::move(callback));
    }
    return failure(audio::PlaybackErrorCode::internal_failure,
                   audio::PlaybackStage::open,
                   "audio playback controller failed internally");
  }
}

} // namespace aiforge::adapters
