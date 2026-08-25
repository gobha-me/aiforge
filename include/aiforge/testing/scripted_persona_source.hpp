#pragma once

#include <cstddef>
#include <variant>
#include <vector>

#include <aiforge/persona/source.hpp>

namespace aiforge::testing {

using PersonaListOutcome =
    std::variant<std::vector<domain::PersonaSummary>, persona::PersonaError>;
using PersonaLoadOutcome =
    std::variant<domain::PersonaDocument, persona::PersonaError>;

struct PersonaLoadExchange {
  std::string expected_name;
  PersonaLoadOutcome outcome;
  auto operator==(const PersonaLoadExchange&) const -> bool = default;
};

class ScriptedPersonaSource final : public persona::PersonaSource {
 public:
  explicit ScriptedPersonaSource(
      std::vector<PersonaListOutcome> list_outcomes = {},
      std::vector<PersonaLoadExchange> load_exchanges = {});

  [[nodiscard]] auto list(persona::PersonaLimits limits = {},
                          std::stop_token stop_token = {})
      -> std::expected<std::vector<domain::PersonaSummary>,
                       persona::PersonaError> override;
  [[nodiscard]] auto load(std::string name,
                          persona::PersonaLimits limits = {},
                          std::stop_token stop_token = {})
      -> std::expected<domain::PersonaDocument,
                       persona::PersonaError> override;

  [[nodiscard]] auto recorded_loads() const noexcept
      -> const std::vector<std::string>&;
  [[nodiscard]] auto remaining_lists() const noexcept -> std::size_t;
  [[nodiscard]] auto remaining_loads() const noexcept -> std::size_t;

 private:
  std::vector<PersonaListOutcome> m_list_outcomes;
  std::vector<PersonaLoadExchange> m_load_exchanges;
  std::vector<std::string> m_recorded_loads;
  std::size_t m_next_list{};
  std::size_t m_next_load{};
};

}  // namespace aiforge::testing
