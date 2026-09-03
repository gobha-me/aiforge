#pragma once

#include <cstddef>
#include <expected>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include <aiforge/domain/ids.hpp>

namespace aiforge::backend {

struct ProviderCharacterSummary {
  explicit ProviderCharacterSummary(domain::ProviderCharacterId character_id)
      : id(std::move(character_id)) {}

  domain::ProviderCharacterId id;
  std::optional<std::string> name;
  std::optional<std::string> description;
  std::optional<domain::ModelId> model_id;
  bool featured{};
  bool web_enabled{};
  std::vector<std::string> tags;
  auto operator==(const ProviderCharacterSummary&) const -> bool = default;
};

struct ProviderCharacterCatalog {
  std::vector<ProviderCharacterSummary> entries;
  std::string source_id;
  auto operator==(const ProviderCharacterCatalog&) const -> bool = default;
};

struct ProviderCharacterLimits {
  std::size_t maximum_entries{4096};
  std::size_t maximum_text_bytes{4096};
  std::size_t maximum_tags_per_entry{256};
  std::size_t maximum_total_text_bytes{std::size_t{16U} * 1024U * 1024U};
  std::size_t maximum_response_bytes{std::size_t{4U} * 1024U * 1024U};
  auto operator==(const ProviderCharacterLimits&) const -> bool = default;
};

enum class ProviderCharacterErrorCode {
  invalid_request,
  invalid_data,
  too_large,
  authentication,
  unavailable,
  not_found,
  cancelled,
  internal_failure,
};

struct ProviderCharacterError {
  ProviderCharacterErrorCode code{ProviderCharacterErrorCode::internal_failure};
  std::string message;
  bool retryable{};
  std::optional<int> status_code;
  auto operator==(const ProviderCharacterError&) const -> bool = default;
};

class ProviderCharacterCatalogSource {
 public:
  virtual ~ProviderCharacterCatalogSource() = default;

  [[nodiscard]] virtual auto list(ProviderCharacterLimits limits = {},
                                  std::stop_token stop_token = {})
      -> std::expected<ProviderCharacterCatalog, ProviderCharacterError> = 0;
  [[nodiscard]] virtual auto lookup(const domain::ProviderCharacterId& id,
                                    ProviderCharacterLimits limits = {},
                                    std::stop_token stop_token = {})
      -> std::expected<ProviderCharacterSummary, ProviderCharacterError> = 0;
};

[[nodiscard]] auto validate_provider_character_catalog(
    const ProviderCharacterCatalog& catalog,
    ProviderCharacterLimits limits = {})
    -> std::expected<void, ProviderCharacterError>;

[[nodiscard]] auto validate_provider_character_summary(
    const ProviderCharacterSummary& summary,
    ProviderCharacterLimits limits = {})
    -> std::expected<void, ProviderCharacterError>;

} // namespace aiforge::backend
