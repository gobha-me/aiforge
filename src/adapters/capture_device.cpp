#include "capture_device.hpp"

#include <atomic>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

namespace aiforge::adapters {
namespace {

enum class CallbackFailure { none, overrun, device_lost, contract_violation };

[[nodiscard]] auto failure(audio::CaptureErrorCode code,
                           audio::CaptureStage stage, std::string message,
                           audio::CaptureStats stats = {})
    -> std::unexpected<audio::CaptureError> {
  return std::unexpected(
      audio::CaptureError{code, stage, std::move(message), stats});
}

[[nodiscard]] auto valid_format(const audio::Signed16Format format) noexcept
    -> bool {
  return (format.channels == 1 || format.channels == 2) &&
         format.sample_rate >= 8000 && format.sample_rate <= 192000;
}

class ContractCallback final : public CaptureDeviceCallback {
 public:
  ContractCallback(const audio::Signed16Format format,
                   std::vector<std::int16_t> samples,
                   const std::stop_token stop_token,
                   CaptureCallbackTestFence* test_fence) noexcept
      : m_format{format}, m_samples{std::move(samples)},
        m_stop_token{stop_token}, m_test_fence{test_fence} {}

  [[nodiscard]] auto process(const std::span<const std::int16_t> input,
                             const std::size_t frames,
                             const CaptureCallbackStatus status) noexcept
      -> CaptureCallbackDecision override {
    const auto prior_in_flight =
        m_in_flight.fetch_add(1, std::memory_order_acq_rel);
    struct CallbackGuard final {
      std::atomic<std::size_t>& count;
      ~CallbackGuard() {
        count.fetch_sub(1, std::memory_order_acq_rel);
        count.notify_all();
      }
    } callback_guard{m_in_flight};
    if (prior_in_flight != 0)
      return abort_with(CallbackFailure::contract_violation);
    if (m_test_fence != nullptr) {
      m_test_fence->entered.store(true, std::memory_order_release);
      m_test_fence->entered.notify_all();
      while (!m_test_fence->release.load(std::memory_order_acquire))
        m_test_fence->release.wait(false, std::memory_order_relaxed);
    }
    if (!m_accepting.load(std::memory_order_acquire)) {
      m_late_callbacks.fetch_add(1, std::memory_order_relaxed);
      return CaptureCallbackDecision::abort;
    }
    m_callbacks.fetch_add(1, std::memory_order_relaxed);
    if (m_terminal.load(std::memory_order_acquire))
      return abort_with(CallbackFailure::contract_violation);
    const auto channels = static_cast<std::size_t>(m_format.channels);
    if (frames == 0 ||
        frames > std::numeric_limits<std::size_t>::max() / channels ||
        input.size() != frames * channels)
      return abort_with(CallbackFailure::contract_violation);
    if (m_stop_token.stop_requested()) {
      m_terminal.store(true, std::memory_order_release);
      return CaptureCallbackDecision::abort;
    }
    if (status == CaptureCallbackStatus::overrun) {
      m_overruns.fetch_add(1, std::memory_order_relaxed);
      return abort_with(CallbackFailure::overrun);
    }
    if (status == CaptureCallbackStatus::device_lost)
      return abort_with(CallbackFailure::device_lost);

    const auto completed = m_frames.load(std::memory_order_relaxed);
    const auto total_frames = m_samples.size() / channels;
    if (completed > total_frames)
      return abort_with(CallbackFailure::contract_violation);
    const auto remaining = total_frames - completed;
    if (frames > remaining) {
      m_overruns.fetch_add(1, std::memory_order_relaxed);
      return abort_with(CallbackFailure::overrun);
    }
    const auto destination = std::span<std::int16_t>{m_samples}.subspan(
        completed * channels, frames * channels);
    std::ranges::copy(input, destination.begin());
    m_frames.fetch_add(frames, std::memory_order_relaxed);
    if (frames == remaining) {
      m_terminal.store(true, std::memory_order_release);
      return CaptureCallbackDecision::complete;
    }
    return CaptureCallbackDecision::continue_operation;
  }

  auto deactivate() noexcept -> void {
    m_accepting.store(false, std::memory_order_release);
  }
  [[nodiscard]] auto callback_failure() const noexcept -> CallbackFailure {
    return m_failure.load(std::memory_order_acquire);
  }
  [[nodiscard]] auto stats() const noexcept -> audio::CaptureStats {
    return {m_callbacks.load(std::memory_order_relaxed),
            m_frames.load(std::memory_order_relaxed),
            m_overruns.load(std::memory_order_relaxed),
            m_late_callbacks.load(std::memory_order_relaxed)};
  }
  [[nodiscard]] auto quiescent() const noexcept -> bool {
    return m_in_flight.load(std::memory_order_acquire) == 0;
  }
  [[nodiscard]] auto take_buffer() noexcept -> audio::Signed16Buffer {
    return {m_format, std::move(m_samples)};
  }

 private:
  [[nodiscard]] auto abort_with(const CallbackFailure failure) noexcept
      -> CaptureCallbackDecision {
    auto expected = CallbackFailure::none;
    static_cast<void>(m_failure.compare_exchange_strong(
        expected, failure, std::memory_order_acq_rel));
    m_terminal.store(true, std::memory_order_release);
    return CaptureCallbackDecision::abort;
  }

  audio::Signed16Format m_format;
  std::vector<std::int16_t> m_samples;
  std::stop_token m_stop_token;
  CaptureCallbackTestFence* m_test_fence{};
  std::atomic<std::size_t> m_in_flight{};
  std::atomic<bool> m_accepting{true};
  std::atomic<bool> m_terminal{false};
  std::atomic<CallbackFailure> m_failure{CallbackFailure::none};
  std::atomic<std::size_t> m_callbacks{};
  std::atomic<std::size_t> m_frames{};
  std::atomic<std::size_t> m_overruns{};
  std::atomic<std::size_t> m_late_callbacks{};
};

[[nodiscard]] auto map_device_failure(const CaptureDeviceFailureCode code,
                                      const audio::CaptureStage stage,
                                      const audio::CaptureStats stats)
    -> audio::CaptureError {
  switch (code) {
    case CaptureDeviceFailureCode::unsupported_format:
      return {audio::CaptureErrorCode::unsupported_format, stage,
              "audio device rejected the signed-16 format", stats};
    case CaptureDeviceFailureCode::permission_denied:
      return {audio::CaptureErrorCode::permission_denied, stage,
              "audio device permission was denied", stats};
    case CaptureDeviceFailureCode::unavailable:
      return {audio::CaptureErrorCode::unavailable, stage,
              "audio input is unavailable", stats};
    case CaptureDeviceFailureCode::device_lost:
      return {audio::CaptureErrorCode::device_lost, stage,
              "audio input was lost", stats};
    case CaptureDeviceFailureCode::internal_failure:
      return {audio::CaptureErrorCode::internal_failure, stage,
              "audio device failed internally", stats};
  }
  return {audio::CaptureErrorCode::internal_failure, stage,
          "audio device failed internally", stats};
}

[[nodiscard]] auto map_callback_failure(const CallbackFailure callback,
                                        const audio::CaptureStats stats)
    -> audio::CaptureError {
  switch (callback) {
    case CallbackFailure::overrun:
      return {audio::CaptureErrorCode::overrun, audio::CaptureStage::stream,
              "audio capture overrun was observed", stats};
    case CallbackFailure::device_lost:
      return {audio::CaptureErrorCode::device_lost, audio::CaptureStage::stream,
              "audio input was lost", stats};
    case CallbackFailure::contract_violation:
      return {audio::CaptureErrorCode::callback_contract_violation,
              audio::CaptureStage::stream,
              "audio callback violated its signed-16 buffer contract", stats};
    case CallbackFailure::none: break;
  }
  return {audio::CaptureErrorCode::internal_failure,
          audio::CaptureStage::stream, "audio callback failed internally",
          stats};
}

[[nodiscard]] auto observe(audio::CaptureObserver* observer,
                           const audio::CaptureStage stage) noexcept -> bool {
  return observer == nullptr || observer->stage_changed(stage);
}

struct LifecycleState {
  std::optional<audio::CaptureError> primary;
  std::optional<audio::CaptureError> stop_failure;
  std::optional<audio::CaptureError> close_failure;
  std::optional<audio::CaptureStage> cancellation_stage;
  bool open_attempted{};
  bool start_attempted{};
  bool close_complete{};

  auto check_cancellation(const std::stop_token stop_token,
                          const audio::CaptureStage stage) -> void {
    if (!cancellation_stage && stop_token.stop_requested())
      cancellation_stage = stage;
  }
};

auto status_failure(const audio::CaptureStage stage,
                    const audio::CaptureStats stats) -> audio::CaptureError {
  return {audio::CaptureErrorCode::internal_failure, stage,
          "audio capture status output failed", stats};
}

void run_primary_lifecycle(BufferedCaptureDevice& device,
                           ContractCallback& callback,
                           const CaptureDeviceOpenRequest& request,
                           const std::stop_token stop_token,
                           audio::CaptureObserver* observer,
                           LifecycleState& state) {
  state.check_cancellation(stop_token, audio::CaptureStage::open);
  if (state.cancellation_stage) return;
  if (!observe(observer, audio::CaptureStage::open)) {
    state.primary = status_failure(audio::CaptureStage::open, callback.stats());
    return;
  }
  state.open_attempted = true;
  const auto opened = device.open(request, callback);
  if (!opened)
    state.primary = map_device_failure(
        opened.error().code, audio::CaptureStage::open, callback.stats());
  state.check_cancellation(stop_token, audio::CaptureStage::open);
  if (state.primary || state.cancellation_stage) return;
  if (!observe(observer, audio::CaptureStage::start)) {
    state.primary =
        status_failure(audio::CaptureStage::start, callback.stats());
    return;
  }
  state.start_attempted = true;
  const auto started = device.start();
  if (!started)
    state.primary = map_device_failure(
        started.error().code, audio::CaptureStage::start, callback.stats());
  state.check_cancellation(stop_token, audio::CaptureStage::start);
  if (state.primary || state.cancellation_stage) return;
  if (!observe(observer, audio::CaptureStage::stream)) {
    state.primary =
        status_failure(audio::CaptureStage::stream, callback.stats());
    return;
  }
  const auto streamed = device.stream(stop_token);
  if (!streamed)
    state.primary = map_device_failure(
        streamed.error().code, audio::CaptureStage::stream, callback.stats());
  state.check_cancellation(stop_token, audio::CaptureStage::stream);
}

void run_cleanup(BufferedCaptureDevice& device, ContractCallback& callback,
                 const std::stop_token stop_token,
                 audio::CaptureObserver* observer, LifecycleState& state) {
  callback.deactivate();
  if (state.start_attempted) {
    if (!observe(observer, audio::CaptureStage::stop) && !state.primary)
      state.primary =
          status_failure(audio::CaptureStage::stop, callback.stats());
    const auto stopped = device.stop();
    if (!stopped)
      state.stop_failure = map_device_failure(
          stopped.error().code, audio::CaptureStage::stop, callback.stats());
    state.check_cancellation(stop_token, audio::CaptureStage::stop);
  }
  if (state.open_attempted) {
    if (!observe(observer, audio::CaptureStage::close) && !state.primary)
      state.primary =
          status_failure(audio::CaptureStage::close, callback.stats());
    const auto closed = device.close();
    state.close_complete = closed.has_value();
    if (!closed)
      state.close_failure = map_device_failure(
          closed.error().code, audio::CaptureStage::close, callback.stats());
    state.check_cancellation(stop_token, audio::CaptureStage::close);
  } else {
    state.close_complete = true;
  }
}

[[nodiscard]] auto lifecycle_error(const LifecycleState& state,
                                   const ContractCallback& callback,
                                   const std::size_t expected_frames,
                                   const bool callback_quiescent)
    -> std::optional<audio::CaptureError> {
  const auto stats = callback.stats();
  if (state.close_failure)
    return audio::CaptureError{audio::CaptureErrorCode::cleanup_failed,
                               audio::CaptureStage::close,
                               "audio device close did not complete", stats};
  if (stats.late_callbacks != 0)
    return audio::CaptureError{
        audio::CaptureErrorCode::late_callback, audio::CaptureStage::close,
        "audio device invoked a callback during teardown", stats};
  if (!callback_quiescent)
    return audio::CaptureError{
        audio::CaptureErrorCode::late_callback, audio::CaptureStage::close,
        "audio callback did not quiesce during teardown", stats};
  if (state.stop_failure)
    return audio::CaptureError{audio::CaptureErrorCode::cleanup_failed,
                               audio::CaptureStage::stop,
                               "audio device stop did not complete", stats};
  if (state.cancellation_stage)
    return audio::CaptureError{audio::CaptureErrorCode::cancelled,
                               *state.cancellation_stage,
                               "audio capture was cancelled", stats};
  if (callback.callback_failure() != CallbackFailure::none)
    return map_callback_failure(callback.callback_failure(), stats);
  if (state.primary) {
    auto primary = *state.primary;
    primary.stats = stats;
    return primary;
  }
  if (stats.frames != expected_frames)
    return audio::CaptureError{
        audio::CaptureErrorCode::incomplete_stream, audio::CaptureStage::stream,
        "audio capture ended before its buffer completed", stats};
  return std::nullopt;
}

class GateGuard final {
 public:
  explicit GateGuard(AudioDeviceGate& gate) noexcept : m_gate{gate} {}
  ~GateGuard() {
    if (!m_finalized) m_gate.quarantine();
  }
  GateGuard(const GateGuard&) = delete;
  auto operator=(const GateGuard&) -> GateGuard& = delete;
  auto release() noexcept -> void {
    m_gate.release();
    m_finalized = true;
  }
  auto quarantine(std::shared_ptr<void> callback) noexcept -> void {
    m_gate.quarantine(std::move(callback));
    m_finalized = true;
  }

 private:
  AudioDeviceGate& m_gate;
  bool m_finalized{};
};

} // namespace

CaptureController::CaptureController(AudioDeviceGate& gate,
                                     BufferedCaptureDevice& device,
                                     const CaptureControllerLimits limits)
    : m_gate{gate}, m_device{device}, m_limits{limits} {
}

auto CaptureController::capture(audio::CaptureRequest request,
                                const std::stop_token stop_token,
                                audio::CaptureObserver* observer) noexcept
    -> std::expected<audio::CaptureResult, audio::CaptureError> {
  std::shared_ptr<ContractCallback> callback;
  bool gate_acquired{};
  try {
    if (m_limits.maximum_buffer_bytes == 0 ||
        m_limits.maximum_buffer_bytes > maximum_capture_bytes ||
        m_limits.maximum_buffer_frames == 0 ||
        m_limits.maximum_buffer_frames > maximum_capture_frames)
      return failure(audio::CaptureErrorCode::internal_failure,
                     audio::CaptureStage::open,
                     "audio capture limits are invalid");
    if (!valid_format(request.format))
      return failure(audio::CaptureErrorCode::invalid_format,
                     audio::CaptureStage::open,
                     "audio capture requires signed-16 mono or stereo");
    if (request.frames == 0)
      return failure(audio::CaptureErrorCode::invalid_request,
                     audio::CaptureStage::open,
                     "audio capture requires at least one frame");
    const auto channels = static_cast<std::size_t>(request.format.channels);
    const auto bytes_per_frame = channels * sizeof(std::int16_t);
    if (request.frames > m_limits.maximum_buffer_bytes / bytes_per_frame ||
        request.frames > m_limits.maximum_buffer_frames)
      return failure(audio::CaptureErrorCode::too_large,
                     audio::CaptureStage::open,
                     "audio capture exceeds its configured limits");

    std::vector<std::int16_t> samples(request.frames * channels);
    callback = std::make_shared<ContractCallback>(
        request.format, std::move(samples), stop_token,
        m_limits.callback_test_fence);
    const auto acquired = m_gate.begin();
    if (acquired == AudioDeviceGate::BeginResult::operation_in_progress)
      return failure(audio::CaptureErrorCode::operation_in_progress,
                     audio::CaptureStage::open,
                     "an audio device operation is already in progress");
    if (acquired == AudioDeviceGate::BeginResult::quarantined)
      return failure(audio::CaptureErrorCode::device_quarantined,
                     audio::CaptureStage::open,
                     "audio device cleanup is indeterminate");
    gate_acquired = true;
    GateGuard gate_guard{m_gate};
    if (stop_token.stop_requested()) {
      gate_guard.release();
      return failure(audio::CaptureErrorCode::cancelled,
                     audio::CaptureStage::open, "audio capture was cancelled");
    }

    LifecycleState state;
    run_primary_lifecycle(
        m_device, *callback,
        CaptureDeviceOpenRequest{request.format, request.frames}, stop_token,
        observer, state);
    run_cleanup(m_device, *callback, stop_token, observer, state);
    const auto callback_quiescent = callback->quiescent();
    const auto error =
        lifecycle_error(state, *callback, request.frames, callback_quiescent);
    if (!state.close_complete || callback->stats().late_callbacks != 0 ||
        !callback_quiescent) {
      gate_guard.quarantine(callback);
    } else {
      gate_guard.release();
    }
    if (error) return std::unexpected(*error);
    const auto stats = callback->stats();
    return audio::CaptureResult{callback->take_buffer(), stats};
  } catch (...) {
    if (gate_acquired) m_gate.quarantine(std::move(callback));
    return failure(audio::CaptureErrorCode::internal_failure,
                   audio::CaptureStage::open,
                   "audio capture controller failed internally");
  }
}

} // namespace aiforge::adapters
