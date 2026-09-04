#include <aiforge/testing/scripted_playback_port.hpp>

#include <utility>

namespace aiforge::testing {
namespace {

[[nodiscard]] auto internal_failure() -> audio::PlaybackError {
  return {audio::PlaybackErrorCode::internal_failure,
          audio::PlaybackStage::open,
          "scripted playback exchange did not match",
          {}};
}

[[nodiscard]] auto cancelled() -> audio::PlaybackError {
  return {audio::PlaybackErrorCode::cancelled,
          audio::PlaybackStage::open,
          "scripted playback was cancelled",
          {}};
}

} // namespace

ScriptedPlaybackPort::ScriptedPlaybackPort(
    std::vector<ScriptedPlaybackExchange> exchanges)
    : m_exchanges{std::move(exchanges)} {
}

auto ScriptedPlaybackPort::play(audio::Signed16Buffer buffer,
                                const std::stop_token stop_token,
                                audio::PlaybackObserver* observer) noexcept
    -> std::expected<audio::PlaybackStats, audio::PlaybackError> {
  try {
    std::lock_guard lock{m_mutex};
    m_recorded.push_back(buffer);
    if (observer != nullptr &&
        !observer->stage_changed(audio::PlaybackStage::open)) {
      return std::unexpected(internal_failure());
    }
    if (stop_token.stop_requested()) return std::unexpected(cancelled());
    if (m_next >= m_exchanges.size() ||
        m_exchanges[m_next].expected != buffer) {
      return std::unexpected(internal_failure());
    }
    return m_exchanges[m_next++].result;
  } catch (...) {
    return std::unexpected(internal_failure());
  }
}

auto ScriptedPlaybackPort::remaining_exchanges() const -> std::size_t {
  std::lock_guard lock{m_mutex};
  return m_exchanges.size() - m_next;
}

auto ScriptedPlaybackPort::recorded() const
    -> std::vector<audio::Signed16Buffer> {
  std::lock_guard lock{m_mutex};
  return m_recorded;
}

} // namespace aiforge::testing
