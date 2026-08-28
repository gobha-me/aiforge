#include <aiforge/testing/scripted_persona_source.hpp>

#include <utility>

namespace aiforge::testing {
namespace {

[[nodiscard]] auto failure(persona::PersonaErrorCode code, std::string message,
                           std::optional<std::string> name = std::nullopt)
    -> std::unexpected<persona::PersonaError> {
  return std::unexpected(
      persona::PersonaError{code, std::move(message), std::move(name), false});
}

} // namespace

ScriptedPersonaSource::ScriptedPersonaSource(
    std::vector<PersonaListOutcome> list_outcomes,
    std::vector<PersonaLoadExchange> load_exchanges)
    : m_list_outcomes(std::move(list_outcomes)),
      m_load_exchanges(std::move(load_exchanges)) {
}

auto ScriptedPersonaSource::list(const persona::PersonaLimits limits,
                                 const std::stop_token stop_token)
    -> std::expected<std::vector<domain::PersonaSummary>,
                     persona::PersonaError> {
  try {
    if (stop_token.stop_requested()) {
      return failure(persona::PersonaErrorCode::cancelled,
                     "persona listing cancelled");
    }
    if (limits.maximum_personas == 0 || limits.maximum_name_bytes == 0 ||
        limits.maximum_file_bytes == 0 ||
        limits.maximum_description_bytes == 0) {
      return failure(persona::PersonaErrorCode::invalid_request,
                     "persona limits are invalid");
    }
    if (m_next_list >= m_list_outcomes.size()) {
      return failure(persona::PersonaErrorCode::internal_failure,
                     "scripted persona listing exhausted");
    }
    auto& outcome = m_list_outcomes[m_next_list++];
    if (auto* error = std::get_if<persona::PersonaError>(&outcome)) {
      return std::unexpected(*error);
    }
    return std::get<std::vector<domain::PersonaSummary>>(outcome);
  } catch (...) {
    return failure(persona::PersonaErrorCode::internal_failure,
                   "scripted persona listing failed internally");
  }
}

auto ScriptedPersonaSource::load(std::string name,
                                 const persona::PersonaLimits limits,
                                 const std::stop_token stop_token)
    -> std::expected<domain::PersonaDocument, persona::PersonaError> {
  try {
    if (stop_token.stop_requested()) {
      return failure(persona::PersonaErrorCode::cancelled,
                     "persona loading cancelled", std::move(name));
    }
    if (limits.maximum_personas == 0 || limits.maximum_name_bytes == 0 ||
        limits.maximum_file_bytes == 0 ||
        limits.maximum_description_bytes == 0) {
      return failure(persona::PersonaErrorCode::invalid_request,
                     "persona limits are invalid", std::move(name));
    }
    m_recorded_loads.push_back(name);
    if (m_next_load >= m_load_exchanges.size()) {
      return failure(persona::PersonaErrorCode::internal_failure,
                     "scripted persona loading exhausted", std::move(name));
    }
    auto& exchange = m_load_exchanges[m_next_load++];
    if (exchange.expected_name != name) {
      return failure(persona::PersonaErrorCode::invalid_request,
                     "persona request did not match script", std::move(name));
    }
    if (auto* error = std::get_if<persona::PersonaError>(&exchange.outcome)) {
      return std::unexpected(*error);
    }
    return std::get<domain::PersonaDocument>(exchange.outcome);
  } catch (...) {
    return failure(persona::PersonaErrorCode::internal_failure,
                   "scripted persona loading failed internally");
  }
}

auto ScriptedPersonaSource::recorded_loads() const noexcept
    -> const std::vector<std::string>& {
  return m_recorded_loads;
}

auto ScriptedPersonaSource::remaining_lists() const noexcept -> std::size_t {
  return m_list_outcomes.size() - m_next_list;
}

auto ScriptedPersonaSource::remaining_loads() const noexcept -> std::size_t {
  return m_load_exchanges.size() - m_next_load;
}

} // namespace aiforge::testing
