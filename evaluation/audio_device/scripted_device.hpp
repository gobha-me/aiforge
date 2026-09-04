#pragma once

#include "device_contract.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace aiforge::evaluation::audio_device {

struct ScriptedCallbackStep {
  std::size_t frames{};
  std::vector<std::int16_t> capture_input;
  CallbackStatus status{CallbackStatus::normal};
};

struct ScriptedStageFailure {
  DeviceStage stage{DeviceStage::open};
  DeviceFailureCode code{DeviceFailureCode::internal_failure};
  auto operator==(const ScriptedStageFailure&) const -> bool = default;
};

struct ScriptedDevicePlan {
  std::vector<ScriptedCallbackStep> callbacks{};
  std::vector<ScriptedStageFailure> failures{};
  std::optional<DeviceStage> barrier{};
  bool callback_during_close{false};
  bool continue_after_terminal{false};
};

// Hermetic device fake. A configured barrier blocks exactly once at the named
// lifecycle stage until release_barrier() is called. wait_until_blocked() and
// release_barrier() use condition variables, so cancellation tests need no
// sleeps or scheduler timing assumptions.
class ScriptedDevice final : public BufferedAudioDevice {
 public:
  explicit ScriptedDevice(ScriptedDevicePlan plan = {});

  [[nodiscard]] auto open(const DeviceOpenRequest& request,
                          DeviceCallback& callback) noexcept
      -> std::expected<void, DeviceFailure> override;
  [[nodiscard]] auto start() noexcept
      -> std::expected<void, DeviceFailure> override;
  [[nodiscard]] auto stream() noexcept
      -> std::expected<void, DeviceFailure> override;
  [[nodiscard]] auto stop() noexcept
      -> std::expected<void, DeviceFailure> override;
  [[nodiscard]] auto close() noexcept
      -> std::expected<void, DeviceFailure> override;

  void wait_until_blocked(DeviceStage stage);
  void release_barrier();

  [[nodiscard]] auto calls() const -> std::vector<DeviceStage>;
  [[nodiscard]] auto open_request() const -> std::optional<DeviceOpenRequest>;
  [[nodiscard]] auto played_samples() const -> std::vector<std::int16_t>;
  [[nodiscard]] auto callback_decisions() const
      -> std::vector<CallbackDecision>;
  [[nodiscard]] auto callback_thread() const -> std::thread::id;
  [[nodiscard]] auto close_callback_output_unchanged() const -> bool;

 private:
  [[nodiscard]] auto run_stage(DeviceStage stage) noexcept
      -> std::expected<void, DeviceFailure>;
  void barrier(DeviceStage stage);
  [[nodiscard]] auto failure(DeviceStage stage) const
      -> std::optional<DeviceFailure>;
  void run_callbacks() noexcept;
  void run_close_callback() noexcept;

  ScriptedDevicePlan m_plan;
  mutable std::mutex m_mutex;
  std::condition_variable m_condition;
  std::vector<DeviceStage> m_calls;
  std::optional<DeviceOpenRequest> m_open_request;
  DeviceCallback* m_callback{};
  std::vector<std::int16_t> m_played_samples;
  std::vector<CallbackDecision> m_callback_decisions;
  std::atomic<bool> m_worker_failed{false};
  std::thread::id m_callback_thread;
  bool m_barrier_reached{false};
  bool m_barrier_released{false};
  bool m_close_callback_output_unchanged{true};
};

} // namespace aiforge::evaluation::audio_device
