#include <aiforge/testing/scripted_provider_character_catalog_source.hpp>

#include <optional>
#include <string>
#include <utility>

namespace aiforge::testing {
namespace {

[[nodiscard]] auto failure(backend::ProviderCharacterErrorCode code,
                           std::string message)
    -> std::unexpected<backend::ProviderCharacterError> {
  return std::unexpected(backend::ProviderCharacterError{
      code, std::move(message), false, std::nullopt});
}

[[nodiscard]] auto limits_are_usable(
    const backend::ProviderCharacterLimits& limits) noexcept -> bool {
  return limits.maximum_entries != 0 && limits.maximum_text_bytes != 0 &&
         limits.maximum_tags_per_entry != 0 &&
         limits.maximum_total_text_bytes != 0 &&
         limits.maximum_response_bytes != 0;
}

} // namespace

ScriptedProviderCharacterCatalogSource::ScriptedProviderCharacterCatalogSource(
    std::vector<ProviderCharacterListOutcome> list_outcomes,
    std::vector<ProviderCharacterLookupExchange> lookup_exchanges)
    : m_list_outcomes(std::move(list_outcomes)),
      m_lookup_exchanges(std::move(lookup_exchanges)) {
}

auto ScriptedProviderCharacterCatalogSource::list(
    const backend::ProviderCharacterLimits limits,
    const std::stop_token stop_token)
    -> std::expected<backend::ProviderCharacterCatalog,
                     backend::ProviderCharacterError> {
  try {
    if (stop_token.stop_requested()) {
      return failure(backend::ProviderCharacterErrorCode::cancelled,
                     "provider character listing cancelled");
    }
    if (!limits_are_usable(limits)) {
      return failure(backend::ProviderCharacterErrorCode::invalid_request,
                     "provider character limits are invalid");
    }
    m_recorded_list_limits.push_back(limits);
    if (m_next_list >= m_list_outcomes.size()) {
      return failure(backend::ProviderCharacterErrorCode::internal_failure,
                     "scripted provider character listing exhausted");
    }
    const auto& outcome = m_list_outcomes[m_next_list++];
    if (auto* error = std::get_if<backend::ProviderCharacterError>(&outcome)) {
      return std::unexpected(*error);
    }
    return std::get<backend::ProviderCharacterCatalog>(outcome);
  } catch (...) {
    return failure(backend::ProviderCharacterErrorCode::internal_failure,
                   "scripted provider character listing failed internally");
  }
}

auto ScriptedProviderCharacterCatalogSource::lookup(
    const domain::ProviderCharacterId& id,
    const backend::ProviderCharacterLimits limits,
    const std::stop_token stop_token)
    -> std::expected<backend::ProviderCharacterSummary,
                     backend::ProviderCharacterError> {
  try {
    if (stop_token.stop_requested()) {
      return failure(backend::ProviderCharacterErrorCode::cancelled,
                     "provider character lookup cancelled");
    }
    if (!limits_are_usable(limits)) {
      return failure(backend::ProviderCharacterErrorCode::invalid_request,
                     "provider character limits are invalid");
    }
    m_recorded_lookups.push_back(ProviderCharacterLookupRequest{id, limits});
    if (m_next_lookup >= m_lookup_exchanges.size()) {
      return failure(backend::ProviderCharacterErrorCode::internal_failure,
                     "scripted provider character lookup exhausted");
    }
    const auto& exchange = m_lookup_exchanges[m_next_lookup];
    if (exchange.expected_id != id) {
      return failure(backend::ProviderCharacterErrorCode::invalid_request,
                     "provider character request did not match script");
    }
    ++m_next_lookup;
    if (auto* error =
            std::get_if<backend::ProviderCharacterError>(&exchange.outcome)) {
      return std::unexpected(*error);
    }
    return std::get<backend::ProviderCharacterSummary>(exchange.outcome);
  } catch (...) {
    return failure(backend::ProviderCharacterErrorCode::internal_failure,
                   "scripted provider character lookup failed internally");
  }
}

auto ScriptedProviderCharacterCatalogSource::recorded_list_limits()
    const noexcept -> const std::vector<backend::ProviderCharacterLimits>& {
  return m_recorded_list_limits;
}

auto ScriptedProviderCharacterCatalogSource::recorded_lookups() const noexcept
    -> const std::vector<ProviderCharacterLookupRequest>& {
  return m_recorded_lookups;
}

auto ScriptedProviderCharacterCatalogSource::remaining_lists() const noexcept
    -> std::size_t {
  return m_list_outcomes.size() - m_next_list;
}

auto ScriptedProviderCharacterCatalogSource::remaining_lookups() const noexcept
    -> std::size_t {
  return m_lookup_exchanges.size() - m_next_lookup;
}

} // namespace aiforge::testing
