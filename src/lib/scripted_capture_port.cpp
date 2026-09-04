#include <aiforge/testing/scripted_capture_port.hpp>

#include <utility>

namespace aiforge::testing {
namespace {

[[nodiscard]] auto internal_failure() -> audio::CaptureError {
  return {audio::CaptureErrorCode::internal_failure,
          audio::CaptureStage::open,
          "scripted capture exchange did not match",
          {}};
}

[[nodiscard]] auto cancelled() -> audio::CaptureError {
  return {audio::CaptureErrorCode::cancelled,
          audio::CaptureStage::open,
          "scripted capture was cancelled",
          {}};
}

} // namespace

ScriptedCapturePort::ScriptedCapturePort(
    std::vector<ScriptedCaptureExchange> exchanges)
    : m_exchanges{std::move(exchanges)} {
}

auto ScriptedCapturePort::capture(audio::CaptureRequest request,
                                  const std::stop_token stop_token,
                                  audio::CaptureObserver* observer) noexcept
    -> std::expected<audio::CaptureResult, audio::CaptureError> {
  try {
    std::lock_guard lock{m_mutex};
    m_recorded.push_back(request);
    if (observer != nullptr &&
        !observer->stage_changed(audio::CaptureStage::open))
      return std::unexpected(internal_failure());
    if (stop_token.stop_requested()) return std::unexpected(cancelled());
    if (m_next >= m_exchanges.size() || m_exchanges[m_next].expected != request)
      return std::unexpected(internal_failure());
    return m_exchanges[m_next++].result;
  } catch (...) {
    return std::unexpected(internal_failure());
  }
}

auto ScriptedCapturePort::remaining_exchanges() const -> std::size_t {
  std::lock_guard lock{m_mutex};
  return m_exchanges.size() - m_next;
}

auto ScriptedCapturePort::recorded() const
    -> std::vector<audio::CaptureRequest> {
  std::lock_guard lock{m_mutex};
  return m_recorded;
}

} // namespace aiforge::testing
