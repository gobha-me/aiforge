#include <aiforge/testing/scripted_language_analysis_source.hpp>

#include <utility>

namespace aiforge::testing {
namespace {

[[nodiscard]] auto cancelled_error() -> repository::LanguageAnalysisError {
  return {repository::LanguageAnalysisErrorCode::cancelled,
          "language analysis cancelled", std::nullopt, std::nullopt, false};
}

[[nodiscard]] auto script_error(std::string message)
    -> repository::LanguageAnalysisError {
  return {repository::LanguageAnalysisErrorCode::internal_failure,
          std::move(message), std::nullopt, std::nullopt, false};
}

}  // namespace

ScriptedLanguageAnalysisSource::ScriptedLanguageAnalysisSource(
    std::vector<LanguageAnalysisCapabilityExchange> capability_exchanges,
    std::vector<LanguageAnalysisExchange> analysis_exchanges)
    : m_capability_exchanges(std::move(capability_exchanges)),
      m_analysis_exchanges(std::move(analysis_exchanges)) {
}

auto ScriptedLanguageAnalysisSource::discover(
    repository::LanguageAnalysisCapabilityRequest request,
    const std::stop_token stop_token)
    -> std::expected<repository::LanguageAnalysisCapabilities,
                     repository::LanguageAnalysisError> {
  try {
    if (stop_token.stop_requested()) return std::unexpected(cancelled_error());
    m_recorded_capability_requests.push_back(request);
    if (m_next_capability_exchange >= m_capability_exchanges.size()) {
      return std::unexpected(
          script_error("scripted capability discovery is exhausted"));
    }
    const auto& exchange = m_capability_exchanges[m_next_capability_exchange];
    if (exchange.expected_request != request) {
      return std::unexpected(
          script_error("capability request did not match the script"));
    }
    ++m_next_capability_exchange;
    if (const auto* error =
            std::get_if<repository::LanguageAnalysisError>(&exchange.outcome)) {
      return std::unexpected(*error);
    }
    return std::get<repository::LanguageAnalysisCapabilities>(exchange.outcome);
  } catch (...) {
    return std::unexpected(
        script_error("scripted capability discovery failed internally"));
  }
}

auto ScriptedLanguageAnalysisSource::analyze(
    repository::LanguageAnalysisRequest request,
    const std::stop_token stop_token)
    -> std::expected<repository::LanguageAnalysisResult,
                     repository::LanguageAnalysisError> {
  try {
    if (stop_token.stop_requested()) return std::unexpected(cancelled_error());
    m_recorded_analysis_requests.push_back(request);
    if (m_next_analysis_exchange >= m_analysis_exchanges.size()) {
      return std::unexpected(script_error("scripted analysis is exhausted"));
    }
    const auto& exchange = m_analysis_exchanges[m_next_analysis_exchange];
    if (exchange.expected_request != request) {
      return std::unexpected(
          script_error("analysis request did not match the script"));
    }
    ++m_next_analysis_exchange;
    if (const auto* error =
            std::get_if<repository::LanguageAnalysisError>(&exchange.outcome)) {
      return std::unexpected(*error);
    }
    return std::get<repository::LanguageAnalysisResult>(exchange.outcome);
  } catch (...) {
    return std::unexpected(
        script_error("scripted language analysis failed internally"));
  }
}

auto ScriptedLanguageAnalysisSource::recorded_capability_requests()
    const noexcept
    -> const std::vector<repository::LanguageAnalysisCapabilityRequest>& {
  return m_recorded_capability_requests;
}

auto ScriptedLanguageAnalysisSource::recorded_analysis_requests() const noexcept
    -> const std::vector<repository::LanguageAnalysisRequest>& {
  return m_recorded_analysis_requests;
}

auto ScriptedLanguageAnalysisSource::remaining_capability_exchanges()
    const noexcept -> std::size_t {
  return m_capability_exchanges.size() - m_next_capability_exchange;
}

auto ScriptedLanguageAnalysisSource::remaining_analysis_exchanges()
    const noexcept -> std::size_t {
  return m_analysis_exchanges.size() - m_next_analysis_exchange;
}

}  // namespace aiforge::testing
