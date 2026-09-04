#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <future>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

#include "playback_device.hpp"

#ifdef AIFORGE_TEST_RTAUDIO
#include "rtaudio_playback.hpp"
#endif

namespace {

using namespace aiforge;
using adapters::PlaybackCallbackDecision;
using adapters::PlaybackCallbackStatus;
using adapters::PlaybackDeviceFailure;
using adapters::PlaybackDeviceFailureCode;
using audio::PlaybackErrorCode;
using audio::PlaybackStage;

struct CallbackStep {
  std::size_t frames{};
  PlaybackCallbackStatus status{PlaybackCallbackStatus::normal};
};

struct StageFailure {
  PlaybackStage stage{PlaybackStage::open};
  PlaybackDeviceFailureCode code{PlaybackDeviceFailureCode::internal_failure};
};

struct DestructionObservation {
  bool callback_invoked{};
  bool callback_aborted{};
  bool output_was_zero{};
};

struct DevicePlan {
  std::vector<CallbackStep> callbacks{{4, PlaybackCallbackStatus::normal}};
  std::vector<StageFailure> failures;
  std::optional<PlaybackStage> barrier;
  bool callback_during_stop{};
  bool callback_during_close{};
  bool continue_after_terminal{};
  bool concurrent_callbacks{};
  bool retain_after_close{};
  bool return_while_callback_in_flight{};
  bool callback_during_destruction{};
  adapters::PlaybackCallbackTestFence* callback_fence{};
  DestructionObservation* destruction_observation{};
};

class ScriptedDevice final : public adapters::BufferedPlaybackDevice {
 public:
  explicit ScriptedDevice(DevicePlan plan = {}) : m_plan{std::move(plan)} {}
  ~ScriptedDevice() override {
    if (m_plan.callback_fence != nullptr) {
      m_plan.callback_fence->release.store(true, std::memory_order_release);
      m_plan.callback_fence->release.notify_all();
    }
    join_worker();
    if (m_plan.callback_during_destruction) destruction_callback();
  }

  auto open(const adapters::PlaybackDeviceOpenRequest& request,
            adapters::PlaybackDeviceCallback& callback) noexcept
      -> std::expected<void, PlaybackDeviceFailure> override {
    {
      std::lock_guard lock{m_mutex};
      m_request = request;
      m_callback = &callback;
    }
    return stage(PlaybackStage::open);
  }

  auto start() noexcept -> std::expected<void, PlaybackDeviceFailure> override {
    return stage(PlaybackStage::start);
  }

  auto stream(std::stop_token) noexcept
      -> std::expected<void, PlaybackDeviceFailure> override {
    auto entered = stage(PlaybackStage::stream);
    if (!entered) return entered;
    try {
      if (m_plan.concurrent_callbacks) {
        concurrent_callbacks();
      } else if (m_plan.return_while_callback_in_flight) {
        m_worker = std::thread{[this] { callbacks(); }};
        if (m_plan.callback_fence != nullptr) {
          while (
              !m_plan.callback_fence->entered.load(std::memory_order_acquire)) {
            m_plan.callback_fence->entered.wait(false,
                                                std::memory_order_relaxed);
          }
        }
      } else {
        std::thread worker{[this] { callbacks(); }};
        worker.join();
      }
      return {};
    } catch (...) {
      return std::unexpected(
          PlaybackDeviceFailure{PlaybackDeviceFailureCode::internal_failure});
    }
  }

  auto stop() noexcept -> std::expected<void, PlaybackDeviceFailure> override {
    auto result = stage(PlaybackStage::stop);
    if (m_plan.callback_during_stop) retained_callback();
    return result;
  }

  auto close() noexcept -> std::expected<void, PlaybackDeviceFailure> override {
    auto result = stage(PlaybackStage::close);
    if (m_plan.callback_during_close) retained_callback();
    if (result && !m_plan.retain_after_close) {
      std::lock_guard lock{m_mutex};
      m_callback = nullptr;
    }
    return result;
  }

  auto wait_until_blocked(const PlaybackStage expected) -> void {
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

  auto wait_until_overlap_rejected() -> void {
    std::unique_lock lock{m_mutex};
    REQUIRE(m_condition.wait_for(lock, std::chrono::seconds{2},
                                 [&] { return m_overlap_rejected; }));
  }

  auto join_worker() noexcept -> void {
    try {
      if (m_worker.joinable()) m_worker.join();
    } catch (...) {
    }
  }

  [[nodiscard]] auto calls() const -> std::vector<PlaybackStage> {
    std::lock_guard lock{m_mutex};
    return m_calls;
  }

  [[nodiscard]] auto request() const
      -> std::optional<adapters::PlaybackDeviceOpenRequest> {
    std::lock_guard lock{m_mutex};
    return m_request;
  }

  [[nodiscard]] auto played() const -> std::vector<std::int16_t> {
    std::lock_guard lock{m_mutex};
    return m_played;
  }

  [[nodiscard]] auto rejected_output_was_zero() const -> bool {
    return m_rejected_output_zero.load();
  }

  [[nodiscard]] auto has_retained_callback() const -> bool {
    std::lock_guard lock{m_mutex};
    return m_callback != nullptr;
  }

  auto invoke_retained() -> PlaybackCallbackDecision {
    adapters::PlaybackDeviceCallback* callback{};
    {
      std::lock_guard lock{m_mutex};
      callback = m_callback;
    }
    REQUIRE(callback != nullptr);
    std::array<std::int16_t, 1> output{1777};
    const auto decision =
        callback->process(output, 1, PlaybackCallbackStatus::normal);
    m_rejected_output_zero.store(output.front() == 0);
    return decision;
  }

 private:
  auto stage(const PlaybackStage current) noexcept
      -> std::expected<void, PlaybackDeviceFailure> {
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
      const auto found =
          std::ranges::find(m_plan.failures, current, &StageFailure::stage);
      if (found != m_plan.failures.end())
        return std::unexpected(PlaybackDeviceFailure{found->code});
      return {};
    } catch (...) {
      return std::unexpected(
          PlaybackDeviceFailure{PlaybackDeviceFailureCode::internal_failure});
    }
  }

  auto callbacks() noexcept -> void {
    adapters::PlaybackDeviceCallback* callback{};
    std::uint16_t channels{};
    {
      std::lock_guard lock{m_mutex};
      callback = m_callback;
      channels = m_request ? m_request->format.channels : 0;
    }
    if (callback == nullptr || channels == 0) return;
    for (const auto& step : m_plan.callbacks) {
      std::vector<std::int16_t> output(step.frames * channels, 1777);
      const auto decision = callback->process(output, step.frames, step.status);
      if (decision == PlaybackCallbackDecision::abort &&
          std::ranges::all_of(output,
                              [](const auto value) { return value == 0; })) {
        m_rejected_output_zero.store(true);
      }
      {
        std::lock_guard lock{m_mutex};
        m_played.insert(m_played.end(), output.begin(), output.end());
      }
      if (decision != PlaybackCallbackDecision::continue_operation &&
          !m_plan.continue_after_terminal)
        return;
    }
  }

  auto concurrent_callbacks() -> void {
    adapters::PlaybackDeviceCallback* callback{};
    std::size_t frames{};
    {
      std::lock_guard lock{m_mutex};
      callback = m_callback;
      frames = m_request ? m_request->frames : 0;
    }
    if (callback == nullptr || frames == 0) return;
    std::atomic<int> ready{};
    std::atomic<bool> go{};
    auto invoke = [&] {
      std::vector<std::int16_t> output(frames, 1777);
      ready.fetch_add(1);
      while (!go.load())
        std::this_thread::yield();
      const auto decision =
          callback->process(output, frames, PlaybackCallbackStatus::normal);
      if (decision == PlaybackCallbackDecision::abort &&
          std::ranges::all_of(output,
                              [](const auto value) { return value == 0; })) {
        m_rejected_output_zero.store(true);
        {
          std::lock_guard lock{m_mutex};
          m_overlap_rejected = true;
        }
        m_condition.notify_all();
      }
    };
    std::thread first{invoke};
    std::thread second{invoke};
    while (ready.load() != 2)
      std::this_thread::yield();
    go.store(true);
    first.join();
    second.join();
  }

  auto retained_callback() noexcept -> void {
    try {
      std::array<std::int16_t, 1> output{1777};
      adapters::PlaybackDeviceCallback* callback{};
      {
        std::lock_guard lock{m_mutex};
        callback = m_callback;
      }
      if (callback == nullptr) return;
      const auto decision =
          callback->process(output, 1, PlaybackCallbackStatus::normal);
      m_rejected_output_zero.store(
          decision == PlaybackCallbackDecision::abort && output.front() == 0);
    } catch (...) {
    }
  }

  auto destruction_callback() noexcept -> void {
    if (m_plan.destruction_observation == nullptr) return;
    std::array<std::int16_t, 1> output{1777};
    adapters::PlaybackDeviceCallback* callback{};
    {
      std::lock_guard lock{m_mutex};
      callback = m_callback;
    }
    m_plan.destruction_observation->callback_invoked = callback != nullptr;
    if (callback == nullptr) return;
    const auto decision =
        callback->process(output, 1, PlaybackCallbackStatus::normal);
    m_plan.destruction_observation->callback_aborted =
        decision == PlaybackCallbackDecision::abort;
    m_plan.destruction_observation->output_was_zero = output.front() == 0;
  }

  DevicePlan m_plan;
  mutable std::mutex m_mutex;
  std::condition_variable m_condition;
  std::vector<PlaybackStage> m_calls;
  std::optional<adapters::PlaybackDeviceOpenRequest> m_request;
  adapters::PlaybackDeviceCallback* m_callback{};
  std::vector<std::int16_t> m_played;
  std::atomic<bool> m_rejected_output_zero{};
  std::thread m_worker;
  bool m_barrier_reached{};
  bool m_barrier_released{};
  bool m_overlap_rejected{};
};

class RecordingObserver final : public audio::PlaybackObserver {
 public:
  explicit RecordingObserver(std::optional<PlaybackStage> fail = {})
      : m_fail{fail} {}

  auto stage_changed(const PlaybackStage stage) noexcept -> bool override {
    try {
      m_stages.push_back(stage);
      m_threads.push_back(std::this_thread::get_id());
      return m_fail != stage;
    } catch (...) {
      return false;
    }
  }

  std::vector<PlaybackStage> m_stages;
  std::vector<std::thread::id> m_threads;
  std::optional<PlaybackStage> m_fail;
};

[[nodiscard]] auto buffer(std::size_t frames = 4,
                          audio::Signed16Format format = {48000, 1})
    -> audio::Signed16Buffer {
  std::vector<std::int16_t> samples(frames * format.channels);
  for (std::size_t index{}; index < samples.size(); ++index)
    samples[index] = static_cast<std::int16_t>(index + 1);
  return {format, std::move(samples)};
}

template <typename Result>
auto require_error(const Result& result, const PlaybackErrorCode code) -> void {
  REQUIRE_FALSE(result);
  CHECK(result.error().code == code);
}

[[nodiscard]] auto callback_plan(std::vector<CallbackStep> callbacks)
    -> DevicePlan {
  DevicePlan plan;
  plan.callbacks = std::move(callbacks);
  return plan;
}

[[nodiscard]] auto limits_with_fence(adapters::PlaybackCallbackTestFence& fence)
    -> adapters::PlaybackControllerLimits {
  adapters::PlaybackControllerLimits limits;
  limits.callback_test_fence = &fence;
  return limits;
}

} // namespace

TEST_CASE("playback rejects invalid input before device access") {
  for (const auto format :
       {audio::Signed16Format{7999, 1}, audio::Signed16Format{192001, 1},
        audio::Signed16Format{48000, 0}, audio::Signed16Format{48000, 3}}) {
    adapters::AudioDeviceGate gate;
    ScriptedDevice device;
    adapters::PlaybackController controller{gate, device};
    require_error(controller.play(buffer(1, format)),
                  PlaybackErrorCode::invalid_format);
    CHECK(device.calls().empty());
  }

  adapters::AudioDeviceGate gate;
  ScriptedDevice device;
  adapters::PlaybackController controller{gate, device};
  require_error(controller.play({{48000, 1}, {}}),
                PlaybackErrorCode::invalid_buffer);
  require_error(controller.play({{48000, 2}, {1, 2, 3}}),
                PlaybackErrorCode::invalid_buffer);
  CHECK(device.calls().empty());

  adapters::PlaybackController bounded{
      gate, device, {.maximum_buffer_bytes = 4, .maximum_buffer_frames = 2}};
  require_error(bounded.play(buffer(3)), PlaybackErrorCode::too_large);
  CHECK(device.calls().empty());
}

TEST_CASE("playback owns samples and reports ordered controller stages") {
  adapters::AudioDeviceGate gate;
  ScriptedDevice device{callback_plan({{1, {}}, {3, {}}})};
  adapters::PlaybackController controller{gate, device};
  RecordingObserver observer;
  const auto owner = std::this_thread::get_id();
  auto result = controller.play(buffer(), {}, &observer);
  REQUIRE(result);
  CHECK(*result == audio::PlaybackStats{2, 4, 0, 0});
  CHECK(device.played() == std::vector<std::int16_t>{1, 2, 3, 4});
  CHECK(observer.m_stages ==
        std::vector{PlaybackStage::open, PlaybackStage::start,
                    PlaybackStage::stream, PlaybackStage::stop,
                    PlaybackStage::close});
  CHECK(std::ranges::all_of(
      observer.m_threads, [&](const auto thread) { return thread == owner; }));
  CHECK_FALSE(gate.active());
  CHECK_FALSE(gate.quarantined());
}

TEST_CASE("final playback callback is zero filled past the owned buffer") {
  adapters::AudioDeviceGate gate;
  ScriptedDevice device{callback_plan({{3, {}}, {3, {}}})};
  adapters::PlaybackController controller{gate, device};
  REQUIRE(controller.play(buffer()));
  CHECK(device.played() == std::vector<std::int16_t>{1, 2, 3, 4, 0, 0});
}

TEST_CASE("device and callback failures clean up with fixed precedence") {
  struct FailureCase {
    PlaybackStage stage;
    PlaybackDeviceFailureCode native;
    PlaybackErrorCode expected;
  };
  for (const auto& test : {
           FailureCase{PlaybackStage::open,
                       PlaybackDeviceFailureCode::permission_denied,
                       PlaybackErrorCode::permission_denied},
           FailureCase{PlaybackStage::open,
                       PlaybackDeviceFailureCode::unavailable,
                       PlaybackErrorCode::unavailable},
           FailureCase{PlaybackStage::start,
                       PlaybackDeviceFailureCode::unsupported_format,
                       PlaybackErrorCode::unsupported_format},
           FailureCase{PlaybackStage::stream,
                       PlaybackDeviceFailureCode::device_lost,
                       PlaybackErrorCode::device_lost},
       }) {
    adapters::AudioDeviceGate gate;
    DevicePlan plan;
    plan.failures = {{test.stage, test.native}};
    ScriptedDevice device{std::move(plan)};
    adapters::PlaybackController controller{gate, device};
    auto result = controller.play(buffer());
    require_error(result, test.expected);
    CHECK(device.calls().back() == PlaybackStage::close);
  }

  for (const auto& test : {
           std::pair{PlaybackCallbackStatus::underrun,
                     PlaybackErrorCode::underrun},
           std::pair{PlaybackCallbackStatus::device_lost,
                     PlaybackErrorCode::device_lost},
       }) {
    adapters::AudioDeviceGate gate;
    ScriptedDevice device{callback_plan({{1, test.first}})};
    adapters::PlaybackController controller{gate, device};
    auto result = controller.play(buffer());
    require_error(result, test.second);
    CHECK(device.played() == std::vector<std::int16_t>{0});
  }
}

TEST_CASE("incomplete duplicate and concurrent callbacks fail closed") {
  {
    adapters::AudioDeviceGate gate;
    ScriptedDevice device{callback_plan({{1, {}}})};
    adapters::PlaybackController controller{gate, device};
    require_error(controller.play(buffer()),
                  PlaybackErrorCode::incomplete_stream);
  }
  {
    adapters::AudioDeviceGate gate;
    auto plan = callback_plan({{4, {}}, {1, {}}});
    plan.continue_after_terminal = true;
    ScriptedDevice device{std::move(plan)};
    adapters::PlaybackController controller{gate, device};
    require_error(controller.play(buffer()),
                  PlaybackErrorCode::callback_contract_violation);
    CHECK(device.played().back() == 0);
  }
  {
    adapters::AudioDeviceGate gate;
    adapters::PlaybackCallbackTestFence fence;
    DevicePlan plan;
    plan.concurrent_callbacks = true;
    plan.callback_fence = &fence;
    ScriptedDevice device{std::move(plan)};
    adapters::PlaybackController controller{gate, device,
                                            limits_with_fence(fence)};
    auto operation = std::async(std::launch::async, [&] {
      return controller.play(buffer(1024U * 1024U));
    });
    while (!fence.entered.load(std::memory_order_acquire))
      fence.entered.wait(false, std::memory_order_relaxed);
    device.wait_until_overlap_rejected();
    fence.release.store(true, std::memory_order_release);
    fence.release.notify_all();
    require_error(operation.get(),
                  PlaybackErrorCode::callback_contract_violation);
    CHECK(device.rejected_output_was_zero());
  }
}

TEST_CASE("callback straddling teardown is retained and quarantines the gate") {
  adapters::AudioDeviceGate gate;
  adapters::PlaybackCallbackTestFence fence;
  DevicePlan plan;
  plan.return_while_callback_in_flight = true;
  plan.callback_fence = &fence;
  ScriptedDevice device{std::move(plan)};
  adapters::PlaybackController controller{gate, device,
                                          limits_with_fence(fence)};

  auto result = controller.play(buffer());
  require_error(result, PlaybackErrorCode::late_callback);
  CHECK(gate.quarantined());
  fence.release.store(true, std::memory_order_release);
  fence.release.notify_all();
  device.join_worker();
  CHECK(device.rejected_output_was_zero());
}

TEST_CASE("late teardown callbacks zero output and quarantine the lease") {
  for (const auto during_stop : {true, false}) {
    adapters::AudioDeviceGate gate;
    DevicePlan plan;
    plan.callback_during_stop = during_stop;
    plan.callback_during_close = !during_stop;
    ScriptedDevice device{std::move(plan)};
    adapters::PlaybackController controller{gate, device};
    auto result = controller.play(buffer());
    require_error(result, PlaybackErrorCode::late_callback);
    CHECK(device.rejected_output_was_zero());
    CHECK(gate.quarantined());

    ScriptedDevice other;
    adapters::PlaybackController rejected{gate, other};
    require_error(rejected.play(buffer()),
                  PlaybackErrorCode::device_quarantined);
    CHECK(other.calls().empty());
  }
}

TEST_CASE("failed close retains callback state and permanently quarantines") {
  adapters::AudioDeviceGate gate;
  DevicePlan plan;
  plan.failures = {
      {PlaybackStage::stream, PlaybackDeviceFailureCode::device_lost},
      {PlaybackStage::stop, PlaybackDeviceFailureCode::internal_failure},
      {PlaybackStage::close, PlaybackDeviceFailureCode::internal_failure},
  };
  plan.retain_after_close = true;
  ScriptedDevice device{std::move(plan)};
  adapters::PlaybackController controller{gate, device};
  auto result = controller.play(buffer());
  require_error(result, PlaybackErrorCode::cleanup_failed);
  CHECK(result.error().stage == PlaybackStage::close);
  CHECK(gate.quarantined());
  CHECK(device.has_retained_callback());
  CHECK(device.invoke_retained() == PlaybackCallbackDecision::abort);
  CHECK(device.rejected_output_was_zero());
}

TEST_CASE("quarantined callback outlives a still-attached device") {
  DestructionObservation observation;
  {
    adapters::AudioDeviceGate gate;
    DevicePlan plan;
    plan.failures = {
        {PlaybackStage::close, PlaybackDeviceFailureCode::internal_failure},
    };
    plan.retain_after_close = true;
    plan.callback_during_destruction = true;
    plan.destruction_observation = &observation;
    ScriptedDevice device{std::move(plan)};
    adapters::PlaybackController controller{gate, device};
    require_error(controller.play(buffer()), PlaybackErrorCode::cleanup_failed);
    CHECK(gate.quarantined());
  }
  CHECK(observation.callback_invoked);
  CHECK(observation.callback_aborted);
  CHECK(observation.output_was_zero);
}

TEST_CASE("one gate rejects concurrent controller instances") {
  adapters::AudioDeviceGate gate;
  DevicePlan plan;
  plan.barrier = PlaybackStage::open;
  ScriptedDevice first_device{std::move(plan)};
  adapters::PlaybackController first{gate, first_device};
  auto operation =
      std::async(std::launch::async, [&] { return first.play(buffer()); });
  first_device.wait_until_blocked(PlaybackStage::open);

  ScriptedDevice second_device;
  adapters::PlaybackController second{gate, second_device};
  require_error(second.play(buffer()),
                PlaybackErrorCode::operation_in_progress);
  CHECK(second_device.calls().empty());
  first_device.release_barrier();
  REQUIRE(operation.get());
}

TEST_CASE("cancellation is observed at every stage without callback progress") {
  for (const auto stage :
       {PlaybackStage::open, PlaybackStage::start, PlaybackStage::stream,
        PlaybackStage::stop, PlaybackStage::close}) {
    adapters::AudioDeviceGate gate;
    DevicePlan plan;
    plan.barrier = stage;
    if (stage == PlaybackStage::stream) plan.callbacks.clear();
    ScriptedDevice device{std::move(plan)};
    adapters::PlaybackController controller{gate, device};
    std::stop_source cancellation;
    auto operation = std::async(std::launch::async, [&] {
      return controller.play(buffer(), cancellation.get_token());
    });
    device.wait_until_blocked(stage);
    cancellation.request_stop();
    device.release_barrier();
    auto result = operation.get();
    require_error(result, PlaybackErrorCode::cancelled);
    CHECK(result.error().stage == stage);
    CHECK(device.calls().back() == PlaybackStage::close);
  }
}

TEST_CASE("observer failure remains fallible and never skips cleanup") {
  for (const auto stage :
       {PlaybackStage::open, PlaybackStage::start, PlaybackStage::stream,
        PlaybackStage::stop, PlaybackStage::close}) {
    adapters::AudioDeviceGate gate;
    ScriptedDevice device;
    adapters::PlaybackController controller{gate, device};
    RecordingObserver observer{stage};
    auto result = controller.play(buffer(), {}, &observer);
    require_error(result, PlaybackErrorCode::internal_failure);
    if (stage == PlaybackStage::open)
      CHECK(device.calls().empty());
    else
      CHECK(device.calls().back() == PlaybackStage::close);
    CHECK_FALSE(gate.quarantined());
  }
}

#ifdef AIFORGE_TEST_RTAUDIO
namespace {

struct NativeDestructionObservation {
  bool callback_invoked{};
  bool callback_aborted{};
  bool output_was_zero{};
};

class FakeRtAudioNative final : public adapters::RtAudioNative {
 public:
  ~FakeRtAudioNative() override {
    if (destruction_observation == nullptr || attached_callback == nullptr ||
        !open_request) {
      return;
    }
    std::array<std::int16_t, 2> output{1777, 1777};
    const auto decision = attached_callback(
        output.data(), 1, adapters::RtAudioNativeStatus::normal,
        attached_user_data);
    destruction_observation->callback_invoked = true;
    destruction_observation->callback_aborted = decision == 2;
    destruction_observation->output_was_zero =
        std::ranges::all_of(std::span{output}.first(open_request->channels),
                            [](const auto value) { return value == 0; });
  }

  auto open(adapters::RtAudioNativeOpenRequest& request,
            const adapters::RtAudioNativeCallback callback,
            void* user_data) noexcept
      -> std::expected<void, adapters::RtAudioNativeError> override {
    ++open_calls;
    open_request = request;
    attached_callback = callback;
    attached_user_data = user_data;
    if (open_failure) return std::unexpected(*open_failure);
    request.callback_frames = actual_callback_frames;
    return {};
  }

  auto start() noexcept
      -> std::expected<void, adapters::RtAudioNativeError> override {
    ++start_calls;
    if (start_failure) return std::unexpected(*start_failure);
    is_running = true;
    return {};
  }

  auto running() const noexcept -> bool override { return is_running; }

  auto stream_failure() const noexcept
      -> std::optional<adapters::RtAudioNativeError> override {
    return observed_stream_failure;
  }

  auto abort() noexcept
      -> std::expected<void, adapters::RtAudioNativeError> override {
    ++abort_calls;
    is_running = false;
    if (abort_failure) return std::unexpected(*abort_failure);
    return {};
  }

  auto stop() noexcept
      -> std::expected<void, adapters::RtAudioNativeError> override {
    ++stop_calls;
    is_running = false;
    if (stop_failure) return std::unexpected(*stop_failure);
    return {};
  }

  auto close() noexcept
      -> std::expected<void, adapters::RtAudioNativeError> override {
    ++close_calls;
    if (callback_during_close) invoke(1, adapters::RtAudioNativeStatus::normal);
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
    if (next_callback < callback_frames.size()) {
      invoke(callback_frames[next_callback++], callback_status);
    }
  }

  auto invoke(const std::size_t frames,
              const adapters::RtAudioNativeStatus status) noexcept -> int {
    try {
      if (attached_callback == nullptr || !open_request) return 2;
      std::vector<std::int16_t> output(frames * open_request->channels, 1777);
      const auto decision =
          attached_callback(output.data(), frames, status, attached_user_data);
      outputs.push_back(std::move(output));
      callback_decisions.push_back(decision);
      return decision;
    } catch (...) {
      return 2;
    }
  }

  std::size_t actual_callback_frames{2};
  std::vector<std::size_t> callback_frames{2, 3};
  adapters::RtAudioNativeStatus callback_status{
      adapters::RtAudioNativeStatus::normal};
  std::optional<adapters::RtAudioNativeOpenRequest> open_request;
  adapters::RtAudioNativeCallback attached_callback{};
  void* attached_user_data{};
  std::optional<adapters::RtAudioNativeError> open_failure;
  std::optional<adapters::RtAudioNativeError> start_failure;
  std::optional<adapters::RtAudioNativeError> abort_failure;
  std::optional<adapters::RtAudioNativeError> stop_failure;
  std::optional<adapters::RtAudioNativeError> close_failure;
  std::optional<adapters::RtAudioNativeError> observed_stream_failure;
  std::stop_source* cancel_on_wait{};
  NativeDestructionObservation* destruction_observation{};
  std::vector<std::vector<std::int16_t>> outputs;
  std::vector<int> callback_decisions;
  std::size_t next_callback{};
  int open_calls{};
  int start_calls{};
  int abort_calls{};
  int stop_calls{};
  int close_calls{};
  bool callback_during_close{};
  bool is_running{};
};

class NoopDeviceCallback final : public adapters::PlaybackDeviceCallback {
 public:
  auto process(std::span<std::int16_t> output, std::size_t,
               adapters::PlaybackCallbackStatus) noexcept
      -> PlaybackCallbackDecision override {
    std::ranges::fill(output, std::int16_t{});
    return PlaybackCallbackDecision::complete;
  }
};

} // namespace

TEST_CASE("RtAudio adapter rejects invalid bounds before native open") {
  for (const auto& request : {
           adapters::PlaybackDeviceOpenRequest{{7999, 1}, 1},
           adapters::PlaybackDeviceOpenRequest{{192001, 1}, 1},
           adapters::PlaybackDeviceOpenRequest{{48000, 0}, 1},
           adapters::PlaybackDeviceOpenRequest{{48000, 3}, 1},
           adapters::PlaybackDeviceOpenRequest{{48000, 1}, 0},
           adapters::PlaybackDeviceOpenRequest{
               {48000, 1}, adapters::maximum_playback_frames + 1},
       }) {
    auto native = std::make_unique<FakeRtAudioNative>();
    auto* observed = native.get();
    adapters::RtAudioPlaybackDevice device{std::move(native)};
    NoopDeviceCallback callback;
    auto result = device.open(request, callback);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == PlaybackDeviceFailureCode::unsupported_format);
    CHECK(observed->open_calls == 0);
  }
}

TEST_CASE("RtAudio adapter accepts bounded duration arithmetic extremes") {
  for (const auto& request : {
           adapters::PlaybackDeviceOpenRequest{{8000, 1}, 1},
           adapters::PlaybackDeviceOpenRequest{
               {8000, 2}, adapters::maximum_playback_frames},
           adapters::PlaybackDeviceOpenRequest{
               {192000, 2}, adapters::maximum_playback_frames},
       }) {
    auto native = std::make_unique<FakeRtAudioNative>();
    auto* observed = native.get();
    adapters::RtAudioPlaybackDevice device{std::move(native)};
    NoopDeviceCallback callback;
    REQUIRE(device.open(request, callback));
    CHECK(observed->open_calls == 1);
    REQUIRE(device.close());
  }
}

TEST_CASE("RtAudio adapter requests exact ALSA default-output PCM16 contract") {
  auto native = std::make_unique<FakeRtAudioNative>();
  auto* observed = native.get();
  adapters::RtAudioPlaybackDevice device{std::move(native)};
  adapters::AudioDeviceGate gate;
  adapters::PlaybackController controller{gate, device};
  auto result = controller.play(buffer(5, {44100, 2}));
  REQUIRE(result);
  REQUIRE(observed->open_request);
  CHECK(*observed->open_request ==
        adapters::RtAudioNativeOpenRequest{44100, 2, 5, true, true, true});
  CHECK(observed->open_calls == 1);
  CHECK(observed->start_calls == 1);
  CHECK(observed->stop_calls == 1);
  CHECK(observed->close_calls == 1);
  CHECK(observed->callback_decisions == std::vector<int>{0, 1});
  REQUIRE(observed->outputs.size() == 2);
  CHECK(observed->outputs[0] == std::vector<std::int16_t>{1, 2, 3, 4});
  CHECK(observed->outputs[1] == std::vector<std::int16_t>{5, 6, 7, 8, 9, 10});
  CHECK(observed->attached_callback == nullptr);
}

TEST_CASE("RtAudio facade maps native failures without native diagnostics") {
  struct FailureCase {
    adapters::RtAudioNativeError native;
    PlaybackErrorCode expected;
  };
  for (const auto& test : {
           FailureCase{adapters::RtAudioNativeError::permission_denied,
                       PlaybackErrorCode::permission_denied},
           FailureCase{adapters::RtAudioNativeError::unavailable,
                       PlaybackErrorCode::unavailable},
           FailureCase{adapters::RtAudioNativeError::unsupported_format,
                       PlaybackErrorCode::unsupported_format},
       }) {
    auto native = std::make_unique<FakeRtAudioNative>();
    native->open_failure = test.native;
    adapters::RtAudioPlaybackDevice device{std::move(native)};
    adapters::AudioDeviceGate gate;
    adapters::PlaybackController controller{gate, device};
    auto result = controller.play(buffer());
    require_error(result, test.expected);
  }

  auto native = std::make_unique<FakeRtAudioNative>();
  auto* observed = native.get();
  native->callback_status = adapters::RtAudioNativeStatus::output_underflow;
  adapters::RtAudioPlaybackDevice device{std::move(native)};
  adapters::AudioDeviceGate gate;
  adapters::PlaybackController controller{gate, device};
  auto result = controller.play(buffer());
  require_error(result, PlaybackErrorCode::underrun);
  REQUIRE_FALSE(observed->outputs.empty());
  CHECK(std::ranges::all_of(observed->outputs.front(),
                            [](const auto value) { return value == 0; }));
}

TEST_CASE("RtAudio stream cancellation needs no further native callback") {
  std::stop_source cancellation;
  auto native = std::make_unique<FakeRtAudioNative>();
  auto* observed = native.get();
  native->callback_frames.clear();
  native->cancel_on_wait = &cancellation;
  adapters::RtAudioPlaybackDevice device{std::move(native)};
  adapters::AudioDeviceGate gate;
  adapters::PlaybackController controller{gate, device};
  auto result = controller.play(buffer(), cancellation.get_token());
  require_error(result, PlaybackErrorCode::cancelled);
  CHECK(observed->abort_calls == 1);
  CHECK(observed->stop_calls == 1);
  CHECK(observed->close_calls == 1);
}

TEST_CASE("RtAudio close callback and failure quarantine native state") {
  auto native = std::make_unique<FakeRtAudioNative>();
  auto* observed = native.get();
  native->callback_during_close = true;
  native->close_failure = adapters::RtAudioNativeError::internal_failure;
  adapters::RtAudioPlaybackDevice device{std::move(native)};
  adapters::AudioDeviceGate gate;
  adapters::PlaybackController controller{gate, device};
  auto result = controller.play(buffer());
  require_error(result, PlaybackErrorCode::cleanup_failed);
  CHECK(gate.quarantined());
  CHECK(observed->attached_callback != nullptr);
  REQUIRE_FALSE(observed->outputs.empty());
  CHECK(observed->outputs.back() == std::vector<std::int16_t>{0});
}

TEST_CASE("RtAudio native teardown retains callback-visible device state") {
  NativeDestructionObservation observation;
  {
    adapters::AudioDeviceGate gate;
    auto native = std::make_unique<FakeRtAudioNative>();
    native->close_failure = adapters::RtAudioNativeError::internal_failure;
    native->destruction_observation = &observation;
    adapters::RtAudioPlaybackDevice device{std::move(native)};
    adapters::PlaybackController controller{gate, device};
    auto result = controller.play(buffer());
    require_error(result, PlaybackErrorCode::cleanup_failed);
    CHECK(gate.quarantined());
  }
  CHECK(observation.callback_invoked);
  CHECK(observation.callback_aborted);
  CHECK(observation.output_was_zero);
}
#endif
