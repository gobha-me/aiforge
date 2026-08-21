#include <aiforge/testing/scripted_hosted_review_check.hpp>

#include <utility>

namespace aiforge::testing {

ScriptedHostedReviewCheck::ScriptedHostedReviewCheck(
    std::vector<ScriptedHostedReviewCheckExchange> exchanges)
    : m_exchanges(std::move(exchanges)) {}

auto ScriptedHostedReviewCheck::publish(
    const runtime::HostedReviewCheckUpdate& update)
    -> std::expected<runtime::HostedReviewCheckConfirmation,
                     runtime::HostedReviewCheckError> {
  if (m_next >= m_exchanges.size()) {
    return std::unexpected(runtime::HostedReviewCheckError{
        runtime::HostedReviewCheckErrorCode::unavailable,
        "hosted review-check script is exhausted", false});
  }
  const auto& exchange = m_exchanges[m_next];
  if (exchange.expected_update != update) {
    return std::unexpected(runtime::HostedReviewCheckError{
        runtime::HostedReviewCheckErrorCode::rejected,
        "hosted review-check update did not match the script", false});
  }
  m_recorded_updates.push_back(update);
  ++m_next;
  if (const auto* error =
          std::get_if<runtime::HostedReviewCheckError>(&exchange.outcome)) {
    return std::unexpected(*error);
  }
  return std::get<runtime::HostedReviewCheckConfirmation>(exchange.outcome);
}

auto ScriptedHostedReviewCheck::remaining_exchanges() const noexcept
    -> std::size_t {
  return m_exchanges.size() - m_next;
}

}  // namespace aiforge::testing
