#pragma once

#include <cstddef>
#include <variant>
#include <vector>

#include <aiforge/repository/language_analysis.hpp>

namespace aiforge::testing {

using LanguageAnalysisCapabilityOutcome =
    std::variant<repository::LanguageAnalysisCapabilities,
                 repository::LanguageAnalysisError>;

struct LanguageAnalysisCapabilityExchange {
  repository::LanguageAnalysisCapabilityRequest expected_request;
  LanguageAnalysisCapabilityOutcome outcome;
  auto operator==(const LanguageAnalysisCapabilityExchange&) const
      -> bool = default;
};

using LanguageAnalysisOutcome = std::variant<repository::LanguageAnalysisResult,
                                             repository::LanguageAnalysisError>;

struct LanguageAnalysisExchange {
  repository::LanguageAnalysisRequest expected_request;
  LanguageAnalysisOutcome outcome;
  auto operator==(const LanguageAnalysisExchange&) const -> bool = default;
};

class ScriptedLanguageAnalysisSource final
    : public repository::LanguageAnalysisSource {
 public:
  explicit ScriptedLanguageAnalysisSource(
      std::vector<LanguageAnalysisCapabilityExchange> capability_exchanges = {},
      std::vector<LanguageAnalysisExchange> analysis_exchanges = {});

  [[nodiscard]] auto discover(
      repository::LanguageAnalysisCapabilityRequest request,
      std::stop_token stop_token = {})
      -> std::expected<repository::LanguageAnalysisCapabilities,
                       repository::LanguageAnalysisError> override;

  [[nodiscard]] auto analyze(repository::LanguageAnalysisRequest request,
                             std::stop_token stop_token = {})
      -> std::expected<repository::LanguageAnalysisResult,
                       repository::LanguageAnalysisError> override;

  [[nodiscard]] auto recorded_capability_requests() const noexcept
      -> const std::vector<repository::LanguageAnalysisCapabilityRequest>&;
  [[nodiscard]] auto recorded_analysis_requests() const noexcept
      -> const std::vector<repository::LanguageAnalysisRequest>&;
  [[nodiscard]] auto remaining_capability_exchanges() const noexcept
      -> std::size_t;
  [[nodiscard]] auto remaining_analysis_exchanges() const noexcept
      -> std::size_t;

 private:
  std::vector<LanguageAnalysisCapabilityExchange> m_capability_exchanges;
  std::vector<LanguageAnalysisExchange> m_analysis_exchanges;
  std::vector<repository::LanguageAnalysisCapabilityRequest>
      m_recorded_capability_requests;
  std::vector<repository::LanguageAnalysisRequest> m_recorded_analysis_requests;
  std::size_t m_next_capability_exchange{};
  std::size_t m_next_analysis_exchange{};
};

} // namespace aiforge::testing
