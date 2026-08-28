#pragma once

#include <cstddef>
#include <expected>
#include <variant>
#include <vector>

#include <aiforge/runtime/review_gate.hpp>

namespace aiforge::testing {

using ScriptedHostedReviewCheckOutcome =
    std::variant<runtime::HostedReviewCheckConfirmation,
                 runtime::HostedReviewCheckError>;

struct ScriptedHostedReviewCheckExchange {
  runtime::HostedReviewCheckUpdate expected_update;
  ScriptedHostedReviewCheckOutcome outcome;
};

class ScriptedHostedReviewCheck final : public runtime::HostedReviewCheckPort {
 public:
  explicit ScriptedHostedReviewCheck(
      std::vector<ScriptedHostedReviewCheckExchange> exchanges = {});

  [[nodiscard]] auto publish(const runtime::HostedReviewCheckUpdate& update)
      -> std::expected<runtime::HostedReviewCheckConfirmation,
                       runtime::HostedReviewCheckError> override;

  [[nodiscard]] auto recorded_updates() const noexcept
      -> const std::vector<runtime::HostedReviewCheckUpdate>& {
    return m_recorded_updates;
  }
  [[nodiscard]] auto remaining_exchanges() const noexcept -> std::size_t;

 private:
  std::vector<ScriptedHostedReviewCheckExchange> m_exchanges;
  std::vector<runtime::HostedReviewCheckUpdate> m_recorded_updates;
  std::size_t m_next{};
};

} // namespace aiforge::testing
