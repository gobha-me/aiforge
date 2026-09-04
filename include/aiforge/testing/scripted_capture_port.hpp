#pragma once

#include <cstddef>
#include <expected>
#include <mutex>
#include <stop_token>
#include <vector>

#include <aiforge/audio/capture.hpp>

namespace aiforge::testing {

struct ScriptedCaptureExchange {
  audio::CaptureRequest expected;
  std::expected<audio::CaptureResult, audio::CaptureError> result;
};

class ScriptedCapturePort final : public audio::CapturePort {
 public:
  explicit ScriptedCapturePort(
      std::vector<ScriptedCaptureExchange> exchanges = {});

  [[nodiscard]] auto capture(
      audio::CaptureRequest request, std::stop_token stop_token = {},
      audio::CaptureObserver* observer = nullptr) noexcept
      -> std::expected<audio::CaptureResult, audio::CaptureError> override;

  [[nodiscard]] auto remaining_exchanges() const -> std::size_t;
  [[nodiscard]] auto recorded() const -> std::vector<audio::CaptureRequest>;

 private:
  mutable std::mutex m_mutex;
  std::vector<ScriptedCaptureExchange> m_exchanges;
  std::vector<audio::CaptureRequest> m_recorded;
  std::size_t m_next{};
};

} // namespace aiforge::testing
