#pragma once

#include <cstddef>
#include <variant>
#include <vector>

#include <aiforge/backend/provider_character_catalog.hpp>

namespace aiforge::testing {

using ProviderCharacterListOutcome =
    std::variant<backend::ProviderCharacterCatalog,
                 backend::ProviderCharacterError>;
using ProviderCharacterLookupOutcome =
    std::variant<backend::ProviderCharacterSummary,
                 backend::ProviderCharacterError>;

struct ProviderCharacterLookupExchange {
  domain::ProviderCharacterId expected_id;
  ProviderCharacterLookupOutcome outcome;
  auto operator==(const ProviderCharacterLookupExchange&) const
      -> bool = default;
};

struct ProviderCharacterLookupRequest {
  domain::ProviderCharacterId id;
  backend::ProviderCharacterLimits limits;
  auto operator==(const ProviderCharacterLookupRequest&) const
      -> bool = default;
};

class ScriptedProviderCharacterCatalogSource final
    : public backend::ProviderCharacterCatalogSource {
 public:
  explicit ScriptedProviderCharacterCatalogSource(
      std::vector<ProviderCharacterListOutcome> list_outcomes = {},
      std::vector<ProviderCharacterLookupExchange> lookup_exchanges = {});

  [[nodiscard]] auto list(backend::ProviderCharacterLimits limits = {},
                          std::stop_token stop_token = {})
      -> std::expected<backend::ProviderCharacterCatalog,
                       backend::ProviderCharacterError> override;
  [[nodiscard]] auto lookup(const domain::ProviderCharacterId& id,
                            backend::ProviderCharacterLimits limits = {},
                            std::stop_token stop_token = {})
      -> std::expected<backend::ProviderCharacterSummary,
                       backend::ProviderCharacterError> override;

  [[nodiscard]] auto recorded_list_limits() const noexcept
      -> const std::vector<backend::ProviderCharacterLimits>&;
  [[nodiscard]] auto recorded_lookups() const noexcept
      -> const std::vector<ProviderCharacterLookupRequest>&;
  [[nodiscard]] auto remaining_lists() const noexcept -> std::size_t;
  [[nodiscard]] auto remaining_lookups() const noexcept -> std::size_t;

 private:
  std::vector<ProviderCharacterListOutcome> m_list_outcomes;
  std::vector<ProviderCharacterLookupExchange> m_lookup_exchanges;
  std::vector<backend::ProviderCharacterLimits> m_recorded_list_limits;
  std::vector<ProviderCharacterLookupRequest> m_recorded_lookups;
  std::size_t m_next_list{};
  std::size_t m_next_lookup{};
};

} // namespace aiforge::testing
