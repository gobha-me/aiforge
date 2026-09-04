#include "audio_device_gate.hpp"

#include <utility>

namespace aiforge::adapters {

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

auto AudioDeviceGate::quarantine(std::shared_ptr<void> callback) noexcept
    -> void {
  m_quarantined_callback = std::move(callback);
  m_state.store(State::quarantined, std::memory_order_release);
}

auto AudioDeviceGate::active() const noexcept -> bool {
  return m_state.load(std::memory_order_acquire) == State::active;
}

auto AudioDeviceGate::quarantined() const noexcept -> bool {
  return m_state.load(std::memory_order_acquire) == State::quarantined;
}

} // namespace aiforge::adapters
