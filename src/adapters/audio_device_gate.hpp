#pragma once

#include <atomic>
#include <memory>

namespace aiforge::adapters {

class AudioDeviceGate final {
 public:
  enum class BeginResult { acquired, operation_in_progress, quarantined };

  [[nodiscard]] auto begin() noexcept -> BeginResult;
  auto release() noexcept -> void;
  auto quarantine(std::shared_ptr<void> callback = {}) noexcept -> void;
  [[nodiscard]] auto active() const noexcept -> bool;
  [[nodiscard]] auto quarantined() const noexcept -> bool;

 private:
  enum class State { idle, active, quarantined };
  std::atomic<State> m_state{State::idle};
  std::shared_ptr<void> m_quarantined_callback;
};

} // namespace aiforge::adapters
