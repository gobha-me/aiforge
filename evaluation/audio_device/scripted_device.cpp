#include "scripted_device.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <utility>

namespace aiforge::evaluation::audio_device {
namespace {

[[nodiscard]] auto internal_failure() -> DeviceFailure {
  return {DeviceFailureCode::internal_failure,
          "scripted audio device failed internally"};
}

} // namespace

ScriptedDevice::ScriptedDevice(ScriptedDevicePlan plan)
    : m_plan{std::move(plan)} {
}

auto ScriptedDevice::failure(const DeviceStage stage) const
    -> std::optional<DeviceFailure> {
  const auto found =
      std::ranges::find(m_plan.failures, stage, &ScriptedStageFailure::stage);
  if (found == m_plan.failures.end()) return std::nullopt;
  return DeviceFailure{found->code, "scripted audio device stage failed"};
}

void ScriptedDevice::barrier(const DeviceStage stage) {
  std::unique_lock lock{m_mutex};
  if (m_plan.barrier != stage || m_barrier_reached) return;
  m_barrier_reached = true;
  m_condition.notify_all();
  m_condition.wait(lock, [&] { return m_barrier_released; });
}

auto ScriptedDevice::run_stage(const DeviceStage stage) noexcept
    -> std::expected<void, DeviceFailure> {
  try {
    {
      const std::lock_guard lock{m_mutex};
      m_calls.push_back(stage);
    }
    if (stage != DeviceStage::stream) barrier(stage);
    if (const auto scripted = failure(stage); scripted)
      return std::unexpected(*scripted);
    return {};
  } catch (...) {
    return std::unexpected(internal_failure());
  }
}

auto ScriptedDevice::open(const DeviceOpenRequest& request,
                          DeviceCallback& callback) noexcept
    -> std::expected<void, DeviceFailure> {
  try {
    {
      const std::lock_guard lock{m_mutex};
      m_open_request = request;
      m_callback = &callback;
    }
    return run_stage(DeviceStage::open);
  } catch (...) {
    return std::unexpected(internal_failure());
  }
}

auto ScriptedDevice::start() noexcept -> std::expected<void, DeviceFailure> {
  return run_stage(DeviceStage::start);
}

void ScriptedDevice::run_callbacks() noexcept {
  try {
    barrier(DeviceStage::stream);
    DeviceCallback* callback{};
    DeviceOpenRequest request;
    {
      const std::lock_guard lock{m_mutex};
      callback = m_callback;
      if (!m_open_request || callback == nullptr) {
        m_worker_failed.store(true);
        return;
      }
      request = *m_open_request;
      m_callback_thread = std::this_thread::get_id();
    }

    for (const auto& step : m_plan.callbacks) {
      const auto channels = static_cast<std::size_t>(request.format.channels);
      if (step.frames > std::numeric_limits<std::size_t>::max() / channels) {
        const std::lock_guard lock{m_mutex};
        m_worker_failed.store(true);
        return;
      }
      const auto sample_count = step.frames * channels;
      std::vector<std::int16_t> output;
      CallbackBuffer buffer;
      buffer.frames = step.frames;
      buffer.status = step.status;
      if (request.direction == AudioDirection::playback) {
        output.assign(sample_count, std::int16_t{});
        buffer.playback_output = output;
      } else {
        buffer.capture_input = step.capture_input;
      }

      const auto decision = callback->process(buffer);
      {
        const std::lock_guard lock{m_mutex};
        m_callback_decisions.push_back(decision);
        if (request.direction == AudioDirection::playback)
          m_played_samples.insert(m_played_samples.end(), output.begin(),
                                  output.end());
      }
      if (decision != CallbackDecision::continue_operation &&
          !m_plan.continue_after_terminal)
        break;
    }
  } catch (...) {
    m_worker_failed.store(true);
  }
}

auto ScriptedDevice::stream() noexcept -> std::expected<void, DeviceFailure> {
  auto entered = run_stage(DeviceStage::stream);
  if (!entered) return entered;
  try {
    {
      const std::lock_guard lock{m_mutex};
      m_worker_failed.store(false);
    }
    std::thread worker{[this] { run_callbacks(); }};
    worker.join();
    const std::lock_guard lock{m_mutex};
    if (m_worker_failed.load()) return std::unexpected(internal_failure());
    return {};
  } catch (...) {
    return std::unexpected(internal_failure());
  }
}

auto ScriptedDevice::stop() noexcept -> std::expected<void, DeviceFailure> {
  return run_stage(DeviceStage::stop);
}

void ScriptedDevice::run_close_callback() noexcept {
  try {
    DeviceCallback* callback{};
    DeviceOpenRequest request;
    {
      const std::lock_guard lock{m_mutex};
      callback = m_callback;
      if (!m_open_request || callback == nullptr) return;
      request = *m_open_request;
    }
    const auto sample_count = static_cast<std::size_t>(request.format.channels);
    std::vector<std::int16_t> output(sample_count, 1977);
    std::vector<std::int16_t> input(sample_count, 1977);
    CallbackBuffer buffer;
    buffer.frames = 1;
    if (request.direction == AudioDirection::playback)
      buffer.playback_output = output;
    else
      buffer.capture_input = input;
    const auto decision = callback->process(buffer);
    const bool unchanged =
        request.direction != AudioDirection::playback ||
        std::ranges::all_of(
            output, [](const std::int16_t value) { return value == 1977; });
    const std::lock_guard lock{m_mutex};
    m_callback_thread = std::this_thread::get_id();
    m_callback_decisions.push_back(decision);
    m_close_callback_output_unchanged = unchanged;
  } catch (...) {
    m_worker_failed.store(true);
  }
}

auto ScriptedDevice::close() noexcept -> std::expected<void, DeviceFailure> {
  auto result = run_stage(DeviceStage::close);
  try {
    if (m_plan.callback_during_close) {
      std::thread worker{[this] { run_close_callback(); }};
      worker.join();
    }
    const std::lock_guard lock{m_mutex};
    m_callback = nullptr;
    if (m_worker_failed.load() && result)
      result = std::unexpected(internal_failure());
  } catch (...) {
    return std::unexpected(internal_failure());
  }
  return result;
}

void ScriptedDevice::wait_until_blocked(const DeviceStage stage) {
  std::unique_lock lock{m_mutex};
  if (!m_condition.wait_for(lock, std::chrono::seconds{2}, [&] {
        return m_plan.barrier == stage && m_barrier_reached;
      })) {
    m_barrier_released = true;
    lock.unlock();
    m_condition.notify_all();
    throw std::runtime_error{"scripted audio device barrier was not reached"};
  }
}

void ScriptedDevice::release_barrier() {
  const std::lock_guard lock{m_mutex};
  m_barrier_released = true;
  m_condition.notify_all();
}

auto ScriptedDevice::calls() const -> std::vector<DeviceStage> {
  const std::lock_guard lock{m_mutex};
  return m_calls;
}

auto ScriptedDevice::open_request() const -> std::optional<DeviceOpenRequest> {
  const std::lock_guard lock{m_mutex};
  return m_open_request;
}

auto ScriptedDevice::played_samples() const -> std::vector<std::int16_t> {
  const std::lock_guard lock{m_mutex};
  return m_played_samples;
}

auto ScriptedDevice::callback_decisions() const
    -> std::vector<CallbackDecision> {
  const std::lock_guard lock{m_mutex};
  return m_callback_decisions;
}

auto ScriptedDevice::callback_thread() const -> std::thread::id {
  const std::lock_guard lock{m_mutex};
  return m_callback_thread;
}

auto ScriptedDevice::close_callback_output_unchanged() const -> bool {
  const std::lock_guard lock{m_mutex};
  return m_close_callback_output_unchanged;
}

} // namespace aiforge::evaluation::audio_device
