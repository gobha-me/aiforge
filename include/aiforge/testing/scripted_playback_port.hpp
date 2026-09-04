#pragma once

#include <cstddef>
#include <expected>
#include <mutex>
#include <optional>
#include <stop_token>
#include <vector>

#include <aiforge/audio/playback.hpp>

namespace aiforge::testing {

struct ScriptedPlaybackExchange {
  audio::Signed16Buffer expected;
  std::expected<audio::PlaybackStats, audio::PlaybackError> result;
};

class ScriptedPlaybackPort final : public audio::PlaybackPort {
 public:
  explicit ScriptedPlaybackPort(
      std::vector<ScriptedPlaybackExchange> exchanges = {});

  [[nodiscard]] auto play(audio::Signed16Buffer buffer,
                          std::stop_token stop_token = {},
                          audio::PlaybackObserver* observer = nullptr) noexcept
      -> std::expected<audio::PlaybackStats, audio::PlaybackError> override;

  [[nodiscard]] auto remaining_exchanges() const -> std::size_t;
  [[nodiscard]] auto recorded() const -> std::vector<audio::Signed16Buffer>;

 private:
  mutable std::mutex m_mutex;
  std::vector<ScriptedPlaybackExchange> m_exchanges;
  std::vector<audio::Signed16Buffer> m_recorded;
  std::size_t m_next{};
};

} // namespace aiforge::testing
