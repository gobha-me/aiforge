#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <future>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

#include "capture_device.hpp"
#include "playback_device.hpp"

#ifdef AIFORGE_TEST_RTAUDIO_CAPTURE
#include "rtaudio_capture.hpp"
#endif

namespace {

using namespace aiforge;
using adapters::CaptureCallbackDecision;
using adapters::CaptureCallbackStatus;
using adapters::CaptureDeviceFailure;
using adapters::CaptureDeviceFailureCode;
using audio::CaptureErrorCode;
using audio::CaptureStage;

struct CallbackStep {
  std::size_t frames{};
  std::vector<std::int16_t> samples;
  CaptureCallbackStatus status{CaptureCallbackStatus::normal};
};

struct StageFailure {
  CaptureStage stage{CaptureStage::open};
  CaptureDeviceFailureCode code{CaptureDeviceFailureCode::internal_failure};
};

struct DevicePlan {
  std::vector<CallbackStep> callbacks{
      {2, {1, 2}, CaptureCallbackStatus::normal}};
  std::vector<StageFailure> failures;
  std::optional<CaptureStage> barrier;
  bool callback_during_close{};
  bool continue_after_terminal{};
  bool malformed_span{};
};

class ScriptedDevice final : public adapters::BufferedCaptureDevice {
 public:
  explicit ScriptedDevice(DevicePlan plan = {}) : m_plan{std::move(plan)} {}

  auto open(const adapters::CaptureDeviceOpenRequest& request,
            adapters::CaptureDeviceCallback& callback) noexcept
      -> std::expected<void, CaptureDeviceFailure> override {
    {
      std::lock_guard lock{m_mutex};
      m_request = request;
      m_callback = &callback;
    }
    return stage(CaptureStage::open);
  }
  auto start() noexcept -> std::expected<void, CaptureDeviceFailure> override {
    return stage(CaptureStage::start);
  }
  auto stream(std::stop_token) noexcept
      -> std::expected<void, CaptureDeviceFailure> override {
    auto result = stage(CaptureStage::stream);
    if (!result) return result;
    try {
      for (const auto& step : m_plan.callbacks) {
        auto samples = step.samples;
        if (m_plan.malformed_span && !samples.empty()) samples.pop_back();
        const auto decision =
            m_callback->process(samples, step.frames, step.status);
        m_decisions.push_back(decision);
        if (decision != CaptureCallbackDecision::continue_operation &&
            !m_plan.continue_after_terminal)
          break;
      }
      return {};
    } catch (...) {
      return std::unexpected(
          CaptureDeviceFailure{CaptureDeviceFailureCode::internal_failure});
    }
  }
  auto stop() noexcept -> std::expected<void, CaptureDeviceFailure> override {
    return stage(CaptureStage::stop);
  }
  auto close() noexcept -> std::expected<void, CaptureDeviceFailure> override {
    auto result = stage(CaptureStage::close);
    if (m_plan.callback_during_close && m_callback != nullptr) {
      std::array<std::int16_t, 1> late{9};
      m_decisions.push_back(
          m_callback->process(late, 1, CaptureCallbackStatus::normal));
    }
    if (result) m_callback = nullptr;
    return result;
  }

  auto wait_until_blocked(CaptureStage expected) -> void {
    std::unique_lock lock{m_mutex};
    REQUIRE(m_condition.wait_for(lock, std::chrono::seconds{2}, [&] {
      return m_barrier_reached && m_plan.barrier == expected;
    }));
  }
  auto release_barrier() -> void {
    {
      std::lock_guard lock{m_mutex};
      m_barrier_released = true;
    }
    m_condition.notify_all();
  }
  [[nodiscard]] auto calls() const -> std::vector<CaptureStage> {
    std::lock_guard lock{m_mutex};
    return m_calls;
  }
  [[nodiscard]] auto request() const
      -> std::optional<adapters::CaptureDeviceOpenRequest> {
    std::lock_guard lock{m_mutex};
    return m_request;
  }

 private:
  auto stage(CaptureStage current) noexcept
      -> std::expected<void, CaptureDeviceFailure> {
    try {
      {
        std::unique_lock lock{m_mutex};
        m_calls.push_back(current);
        if (m_plan.barrier == current && !m_barrier_reached) {
          m_barrier_reached = true;
          m_condition.notify_all();
          m_condition.wait(lock, [&] { return m_barrier_released; });
        }
      }
      for (const auto& failure : m_plan.failures) {
        if (failure.stage == current)
          return std::unexpected(CaptureDeviceFailure{failure.code});
      }
      return {};
    } catch (...) {
      return std::unexpected(
          CaptureDeviceFailure{CaptureDeviceFailureCode::internal_failure});
    }
  }

  DevicePlan m_plan;
  mutable std::mutex m_mutex;
  std::condition_variable m_condition;
  std::vector<CaptureStage> m_calls;
  std::optional<adapters::CaptureDeviceOpenRequest> m_request;
  adapters::CaptureDeviceCallback* m_callback{};
  std::vector<CaptureCallbackDecision> m_decisions;
  bool m_barrier_reached{};
  bool m_barrier_released{};
};

[[nodiscard]] auto request(std::size_t frames = 2,
                           audio::Signed16Format format = {48000, 1})
    -> audio::CaptureRequest {
  return {format, frames};
}

template <typename Result>
auto require_error(const Result& result, CaptureErrorCode code) -> void {
  REQUIRE_FALSE(result);
  CHECK(result.error().code == code);
}

class RecordingObserver final : public audio::CaptureObserver {
 public:
  explicit RecordingObserver(const CaptureStage fail_at) : m_fail_at{fail_at} {}
  auto stage_changed(const CaptureStage stage) noexcept -> bool override {
    observed.push_back(stage);
    return stage != m_fail_at;
  }

  std::vector<CaptureStage> observed;

 private:
  CaptureStage m_fail_at;
};

TEST_CASE("capture rejects invalid requests before device access") {
  for (const auto value : {
           request(1, {7999, 1}),
           request(1, {192001, 1}),
           request(1, {48000, 0}),
           request(1, {48000, 3}),
           request(0),
           request(std::numeric_limits<std::size_t>::max(), {48000, 2}),
       }) {
    adapters::AudioDeviceGate gate;
    ScriptedDevice device;
    adapters::CaptureController controller{gate, device};
    const auto result = controller.capture(value);
    REQUIRE_FALSE(result);
    CHECK(device.calls().empty());
  }
}

TEST_CASE("capture publishes an owned buffer only after ordered cleanup") {
  DevicePlan plan;
  plan.callbacks = {{1, {1}, CaptureCallbackStatus::normal},
                    {2, {2, 3}, CaptureCallbackStatus::normal}};
  ScriptedDevice device{std::move(plan)};
  adapters::AudioDeviceGate gate;
  adapters::CaptureController controller{gate, device};
  const auto captured = controller.capture(request(3));
  REQUIRE(captured);
  CHECK(captured->buffer.interleaved_samples ==
        std::vector<std::int16_t>{1, 2, 3});
  CHECK(captured->stats == audio::CaptureStats{2, 3, 0, 0});
  CHECK(device.calls() == std::vector{CaptureStage::open, CaptureStage::start,
                                      CaptureStage::stream, CaptureStage::stop,
                                      CaptureStage::close});
}

TEST_CASE("partial malformed overrun and duplicate capture publish nothing") {
  for (auto plan : {
           DevicePlan{{{1, {1}, CaptureCallbackStatus::normal}}},
           DevicePlan{{{3, {1, 2, 3}, CaptureCallbackStatus::normal}}},
           DevicePlan{{{1, {1}, CaptureCallbackStatus::overrun}}},
           DevicePlan{{{2, {1, 2}, CaptureCallbackStatus::normal},
                       {1, {3}, CaptureCallbackStatus::normal}},
                      {},
                      std::nullopt,
                      false,
                      true},
           DevicePlan{{{2, {1, 2}, CaptureCallbackStatus::normal}},
                      {},
                      std::nullopt,
                      false,
                      false,
                      true},
       }) {
    ScriptedDevice device{std::move(plan)};
    adapters::AudioDeviceGate gate;
    adapters::CaptureController controller{gate, device};
    CHECK_FALSE(controller.capture(request(2)));
  }
}

TEST_CASE("capture device failures clean up and close failure dominates") {
  for (const auto failure : {
           StageFailure{CaptureStage::open,
                        CaptureDeviceFailureCode::permission_denied},
           StageFailure{CaptureStage::start,
                        CaptureDeviceFailureCode::unsupported_format},
           StageFailure{CaptureStage::stream,
                        CaptureDeviceFailureCode::device_lost},
       }) {
    DevicePlan plan;
    plan.failures = {failure};
    ScriptedDevice device{std::move(plan)};
    adapters::AudioDeviceGate gate;
    adapters::CaptureController controller{gate, device};
    CHECK_FALSE(controller.capture(request()));
    CHECK(device.calls().back() == CaptureStage::close);
  }

  DevicePlan plan;
  plan.failures = {
      {CaptureStage::stream, CaptureDeviceFailureCode::device_lost},
      {CaptureStage::close, CaptureDeviceFailureCode::internal_failure}};
  ScriptedDevice device{std::move(plan)};
  adapters::AudioDeviceGate gate;
  adapters::CaptureController controller{gate, device};
  require_error(controller.capture(request()),
                CaptureErrorCode::cleanup_failed);
  CHECK(gate.quarantined());
}

TEST_CASE("late capture callbacks fail and quarantine the shared gate") {
  DevicePlan plan;
  plan.callback_during_close = true;
  ScriptedDevice device{std::move(plan)};
  adapters::AudioDeviceGate gate;
  adapters::CaptureController controller{gate, device};
  require_error(controller.capture(request()), CaptureErrorCode::late_callback);
  CHECK(gate.quarantined());
}

TEST_CASE("one gate rejects concurrent capture device access") {
  DevicePlan plan;
  plan.barrier = CaptureStage::open;
  ScriptedDevice first_device{std::move(plan)};
  adapters::AudioDeviceGate gate;
  adapters::CaptureController first{gate, first_device};
  auto active =
      std::async(std::launch::async, [&] { return first.capture(request()); });
  first_device.wait_until_blocked(CaptureStage::open);

  ScriptedDevice second_device;
  adapters::CaptureController second{gate, second_device};
  require_error(second.capture(request()),
                CaptureErrorCode::operation_in_progress);
  CHECK(second_device.calls().empty());
  first_device.release_barrier();
  REQUIRE(active.get());
}

class NoopPlaybackDevice final : public adapters::BufferedPlaybackDevice {
 public:
  auto open(const adapters::PlaybackDeviceOpenRequest&,
            adapters::PlaybackDeviceCallback&) noexcept
      -> std::expected<void, adapters::PlaybackDeviceFailure> override {
    ++open_calls;
    return {};
  }
  auto start() noexcept
      -> std::expected<void, adapters::PlaybackDeviceFailure> override {
    return {};
  }
  auto stream(std::stop_token) noexcept
      -> std::expected<void, adapters::PlaybackDeviceFailure> override {
    return {};
  }
  auto stop() noexcept
      -> std::expected<void, adapters::PlaybackDeviceFailure> override {
    return {};
  }
  auto close() noexcept
      -> std::expected<void, adapters::PlaybackDeviceFailure> override {
    return {};
  }

  int open_calls{};
};

class ConcurrentCallbackDevice final : public adapters::BufferedCaptureDevice {
 public:
  explicit ConcurrentCallbackDevice(adapters::CaptureCallbackTestFence& fence)
      : m_fence{fence} {}

  auto open(const adapters::CaptureDeviceOpenRequest&,
            adapters::CaptureDeviceCallback& callback) noexcept
      -> std::expected<void, CaptureDeviceFailure> override {
    m_callback = &callback;
    return {};
  }
  auto start() noexcept -> std::expected<void, CaptureDeviceFailure> override {
    return {};
  }
  auto stream(std::stop_token) noexcept
      -> std::expected<void, CaptureDeviceFailure> override {
    try {
      std::array<std::int16_t, 1> first_input{1};
      auto first = std::async(std::launch::async, [&] {
        return m_callback->process(first_input, 1,
                                   CaptureCallbackStatus::normal);
      });
      while (!m_fence.entered.load(std::memory_order_acquire))
        m_fence.entered.wait(false, std::memory_order_relaxed);
      std::array<std::int16_t, 1> second_input{2};
      second_decision =
          m_callback->process(second_input, 1, CaptureCallbackStatus::normal);
      m_fence.release.store(true, std::memory_order_release);
      m_fence.release.notify_all();
      first_decision = first.get();
      return {};
    } catch (...) {
      m_fence.release.store(true, std::memory_order_release);
      m_fence.release.notify_all();
      return std::unexpected(
          CaptureDeviceFailure{CaptureDeviceFailureCode::internal_failure});
    }
  }
  auto stop() noexcept -> std::expected<void, CaptureDeviceFailure> override {
    return {};
  }
  auto close() noexcept -> std::expected<void, CaptureDeviceFailure> override {
    m_callback = nullptr;
    return {};
  }

  CaptureCallbackDecision first_decision{
      CaptureCallbackDecision::continue_operation};
  CaptureCallbackDecision second_decision{
      CaptureCallbackDecision::continue_operation};

 private:
  adapters::CaptureCallbackTestFence& m_fence;
  adapters::CaptureDeviceCallback* m_callback{};
};

TEST_CASE("one gate excludes playback while capture owns the device") {
  DevicePlan plan;
  plan.barrier = CaptureStage::open;
  ScriptedDevice capture_device{std::move(plan)};
  adapters::AudioDeviceGate gate;
  adapters::CaptureController capture{gate, capture_device};
  auto active = std::async(std::launch::async,
                           [&] { return capture.capture(request()); });
  capture_device.wait_until_blocked(CaptureStage::open);

  NoopPlaybackDevice playback_device;
  adapters::PlaybackController playback{gate, playback_device};
  const auto played = playback.play({{48000, 1}, {1}});
  REQUIRE_FALSE(played);
  CHECK(played.error().code == audio::PlaybackErrorCode::operation_in_progress);
  CHECK(playback_device.open_calls == 0);
  capture_device.release_barrier();
  REQUIRE(active.get());
}

TEST_CASE("concurrent capture callbacks fail closed without publishing") {
  adapters::CaptureCallbackTestFence fence;
  ConcurrentCallbackDevice device{fence};
  adapters::AudioDeviceGate gate;
  adapters::CaptureController controller{
      gate,
      device,
      {.maximum_buffer_bytes = adapters::maximum_capture_bytes,
       .maximum_buffer_frames = adapters::maximum_capture_frames,
       .callback_test_fence = &fence}};
  require_error(controller.capture(request(1)),
                CaptureErrorCode::callback_contract_violation);
  CHECK(device.first_decision == CaptureCallbackDecision::abort);
  CHECK(device.second_decision == CaptureCallbackDecision::abort);
}

TEST_CASE("capture cancellation is deterministic at every lifecycle stage") {
  for (const auto stage :
       {CaptureStage::open, CaptureStage::start, CaptureStage::stream,
        CaptureStage::stop, CaptureStage::close}) {
    DevicePlan plan;
    plan.barrier = stage;
    ScriptedDevice device{std::move(plan)};
    adapters::AudioDeviceGate gate;
    adapters::CaptureController controller{gate, device};
    std::stop_source cancellation;
    auto active = std::async(std::launch::async, [&] {
      return controller.capture(request(), cancellation.get_token());
    });
    device.wait_until_blocked(stage);
    cancellation.request_stop();
    device.release_barrier();
    require_error(active.get(), CaptureErrorCode::cancelled);
    CHECK(device.calls().back() == CaptureStage::close);
  }
}

TEST_CASE("capture status failure remains fallible and still cleans up") {
  for (const auto stage :
       {CaptureStage::open, CaptureStage::start, CaptureStage::stream,
        CaptureStage::stop, CaptureStage::close}) {
    ScriptedDevice device;
    adapters::AudioDeviceGate gate;
    adapters::CaptureController controller{gate, device};
    RecordingObserver observer{stage};
    require_error(controller.capture(request(), {}, &observer),
                  CaptureErrorCode::internal_failure);
    if (stage == CaptureStage::open)
      CHECK(device.calls().empty());
    else
      CHECK(device.calls().back() == CaptureStage::close);
    CHECK_FALSE(gate.quarantined());
  }
}

#ifdef AIFORGE_TEST_RTAUDIO_CAPTURE
namespace {

struct NativeDestructionObservation {
  bool callback_invoked{};
  bool callback_aborted{};
};

class FakeRtAudioCaptureNative final : public adapters::RtAudioCaptureNative {
 public:
  ~FakeRtAudioCaptureNative() override {
    if (destruction_observation == nullptr || attached_callback == nullptr)
      return;
    std::array<std::int16_t, 1> input{7};
    destruction_observation->callback_invoked = true;
    destruction_observation->callback_aborted =
        attached_callback(input.data(), 1,
                          adapters::RtAudioCaptureNativeStatus::normal,
                          attached_user_data) == 2;
  }

  auto open(adapters::RtAudioCaptureNativeOpenRequest& request,
            const adapters::RtAudioCaptureNativeCallback callback,
            void* user_data) noexcept
      -> std::expected<void, adapters::RtAudioCaptureNativeError> override {
    ++open_calls;
    open_request = request;
    attached_callback = callback;
    attached_user_data = user_data;
    if (open_failure) return std::unexpected(*open_failure);
    request.callback_frames = actual_callback_frames;
    return {};
  }
  auto start() noexcept
      -> std::expected<void, adapters::RtAudioCaptureNativeError> override {
    ++start_calls;
    if (start_failure) return std::unexpected(*start_failure);
    is_running = true;
    return {};
  }
  [[nodiscard]] auto running() const noexcept -> bool override {
    return is_running;
  }
  [[nodiscard]] auto stream_failure() const noexcept
      -> std::optional<adapters::RtAudioCaptureNativeError> override {
    return observed_stream_failure;
  }
  auto abort() noexcept
      -> std::expected<void, adapters::RtAudioCaptureNativeError> override {
    ++abort_calls;
    is_running = false;
    if (abort_failure) return std::unexpected(*abort_failure);
    return {};
  }
  auto stop() noexcept
      -> std::expected<void, adapters::RtAudioCaptureNativeError> override {
    ++stop_calls;
    is_running = false;
    if (stop_failure) return std::unexpected(*stop_failure);
    return {};
  }
  auto close() noexcept
      -> std::expected<void, adapters::RtAudioCaptureNativeError> override {
    ++close_calls;
    if (callback_during_close)
      static_cast<void>(
          invoke(1, adapters::RtAudioCaptureNativeStatus::normal));
    if (close_failure) return std::unexpected(*close_failure);
    attached_callback = nullptr;
    attached_user_data = nullptr;
    return {};
  }
  auto wait_for_progress(std::chrono::milliseconds) noexcept -> void override {
    if (cancel_on_wait != nullptr) {
      cancel_on_wait->request_stop();
      cancel_on_wait = nullptr;
      return;
    }
    if (next_callback < callback_frames.size())
      static_cast<void>(
          invoke(callback_frames[next_callback++], callback_status));
  }

  auto invoke(const std::size_t frames,
              const adapters::RtAudioCaptureNativeStatus status) noexcept
      -> int {
    try {
      if (attached_callback == nullptr || !open_request) return 2;
      std::vector<std::int16_t> input(frames * open_request->channels);
      for (auto& sample : input)
        sample = next_sample++;
      const auto decision =
          attached_callback(input.data(), frames, status, attached_user_data);
      inputs.push_back(std::move(input));
      callback_decisions.push_back(decision);
      return decision;
    } catch (...) {
      return 2;
    }
  }

  std::size_t actual_callback_frames{2};
  std::vector<std::size_t> callback_frames{2, 3};
  adapters::RtAudioCaptureNativeStatus callback_status{
      adapters::RtAudioCaptureNativeStatus::normal};
  std::optional<adapters::RtAudioCaptureNativeOpenRequest> open_request;
  adapters::RtAudioCaptureNativeCallback attached_callback{};
  void* attached_user_data{};
  std::optional<adapters::RtAudioCaptureNativeError> open_failure;
  std::optional<adapters::RtAudioCaptureNativeError> start_failure;
  std::optional<adapters::RtAudioCaptureNativeError> abort_failure;
  std::optional<adapters::RtAudioCaptureNativeError> stop_failure;
  std::optional<adapters::RtAudioCaptureNativeError> close_failure;
  std::optional<adapters::RtAudioCaptureNativeError> observed_stream_failure;
  std::stop_source* cancel_on_wait{};
  NativeDestructionObservation* destruction_observation{};
  std::vector<std::vector<std::int16_t>> inputs;
  std::vector<int> callback_decisions;
  std::size_t next_callback{};
  std::int16_t next_sample{1};
  int open_calls{};
  int start_calls{};
  int abort_calls{};
  int stop_calls{};
  int close_calls{};
  bool callback_during_close{};
  bool is_running{};
};

class NoopCaptureCallback final : public adapters::CaptureDeviceCallback {
 public:
  auto process(std::span<const std::int16_t>, std::size_t,
               adapters::CaptureCallbackStatus) noexcept
      -> CaptureCallbackDecision override {
    return CaptureCallbackDecision::complete;
  }
};

} // namespace

TEST_CASE("RtAudio capture rejects invalid bounds before native open") {
  for (const auto& value : {
           adapters::CaptureDeviceOpenRequest{{7999, 1}, 1},
           adapters::CaptureDeviceOpenRequest{{192001, 1}, 1},
           adapters::CaptureDeviceOpenRequest{{48000, 0}, 1},
           adapters::CaptureDeviceOpenRequest{{48000, 3}, 1},
           adapters::CaptureDeviceOpenRequest{{48000, 1}, 0},
           adapters::CaptureDeviceOpenRequest{
               {48000, 1}, adapters::maximum_capture_frames + 1},
       }) {
    auto native = std::make_unique<FakeRtAudioCaptureNative>();
    auto* observed = native.get();
    adapters::RtAudioCaptureDevice device{std::move(native)};
    NoopCaptureCallback callback;
    auto result = device.open(value, callback);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == CaptureDeviceFailureCode::unsupported_format);
    CHECK(observed->open_calls == 0);
  }
}

TEST_CASE("RtAudio capture requests exact ALSA default-input PCM16 contract") {
  auto native = std::make_unique<FakeRtAudioCaptureNative>();
  auto* observed = native.get();
  adapters::RtAudioCaptureDevice device{std::move(native)};
  adapters::AudioDeviceGate gate;
  adapters::CaptureController controller{gate, device};
  auto result = controller.capture(request(5, {44100, 2}));
  REQUIRE(result);
  REQUIRE(observed->open_request);
  CHECK(*observed->open_request == adapters::RtAudioCaptureNativeOpenRequest{
                                       44100, 2, 5, true, true, true});
  CHECK(observed->start_calls == 1);
  CHECK(observed->stop_calls == 1);
  CHECK(observed->close_calls == 1);
  CHECK(observed->callback_decisions == std::vector<int>{0, 1});
  CHECK(result->buffer.interleaved_samples ==
        std::vector<std::int16_t>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10});
  CHECK(observed->attached_callback == nullptr);
}

TEST_CASE("RtAudio capture maps failures and input overflow without details") {
  struct FailureCase {
    adapters::RtAudioCaptureNativeError native;
    CaptureErrorCode expected;
  };
  for (const auto& test : {
           FailureCase{adapters::RtAudioCaptureNativeError::permission_denied,
                       CaptureErrorCode::permission_denied},
           FailureCase{adapters::RtAudioCaptureNativeError::unavailable,
                       CaptureErrorCode::unavailable},
           FailureCase{adapters::RtAudioCaptureNativeError::unsupported_format,
                       CaptureErrorCode::unsupported_format},
       }) {
    auto native = std::make_unique<FakeRtAudioCaptureNative>();
    native->open_failure = test.native;
    adapters::RtAudioCaptureDevice device{std::move(native)};
    adapters::AudioDeviceGate gate;
    adapters::CaptureController controller{gate, device};
    require_error(controller.capture(request()), test.expected);
  }

  auto native = std::make_unique<FakeRtAudioCaptureNative>();
  native->callback_status =
      adapters::RtAudioCaptureNativeStatus::input_overflow;
  adapters::RtAudioCaptureDevice device{std::move(native)};
  adapters::AudioDeviceGate gate;
  adapters::CaptureController controller{gate, device};
  require_error(controller.capture(request()), CaptureErrorCode::overrun);
}

TEST_CASE("RtAudio capture cancellation needs no further native callback") {
  std::stop_source cancellation;
  auto native = std::make_unique<FakeRtAudioCaptureNative>();
  auto* observed = native.get();
  native->callback_frames.clear();
  native->cancel_on_wait = &cancellation;
  adapters::RtAudioCaptureDevice device{std::move(native)};
  adapters::AudioDeviceGate gate;
  adapters::CaptureController controller{gate, device};
  auto result = controller.capture(request(), cancellation.get_token());
  require_error(result, CaptureErrorCode::cancelled);
  CHECK(observed->abort_calls == 1);
  CHECK(observed->stop_calls == 1);
  CHECK(observed->close_calls == 1);
}

TEST_CASE("RtAudio capture close failure quarantines callback state") {
  NativeDestructionObservation observation;
  {
    adapters::AudioDeviceGate gate;
    auto native = std::make_unique<FakeRtAudioCaptureNative>();
    native->callback_during_close = true;
    native->close_failure =
        adapters::RtAudioCaptureNativeError::internal_failure;
    native->destruction_observation = &observation;
    adapters::RtAudioCaptureDevice device{std::move(native)};
    adapters::CaptureController controller{gate, device};
    require_error(controller.capture(request()),
                  CaptureErrorCode::cleanup_failed);
    CHECK(gate.quarantined());
  }
  CHECK(observation.callback_invoked);
  CHECK(observation.callback_aborted);
}
#endif

} // namespace
