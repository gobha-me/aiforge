#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <utility>

#include <aiforge/domain/digest.hpp>
#include <aiforge/domain/ids.hpp>
#include <aiforge/domain/money.hpp>

namespace aiforge::domain {

struct PriceRate {
  std::optional<DecimalAmount> usd;
  std::optional<DecimalAmount> diem;
  auto operator==(const PriceRate &) const -> bool = default;
};

struct TextPriceTier {
  std::optional<PriceRate> input;
  std::optional<PriceRate> output;
  std::optional<PriceRate> cache_input;
  std::optional<PriceRate> cache_write;
  auto operator==(const TextPriceTier &) const -> bool = default;
};

struct TextPricing {
  explicit TextPricing(TextPriceTier base_value = {})
      : base(std::move(base_value)) {}

  TextPriceTier base;
  std::optional<std::uint64_t> extended_threshold_tokens;
  std::optional<TextPriceTier> extended;
  auto operator==(const TextPricing &) const -> bool = default;
};

enum class PricingCatalogOrigin {
  live,
  fresh_cache,
  stale_cache,
};

enum class PricingRateBasis {
  per_million_tokens,
};

struct PricingObservation {
  ModelId model_id;
  std::string source_id;
  std::optional<std::string> source_revision;
  std::chrono::sys_time<std::chrono::milliseconds> fetched_at;
  PricingCatalogOrigin origin{PricingCatalogOrigin::live};
  PricingRateBasis basis{PricingRateBasis::per_million_tokens};
  TextPricing pricing;
  ContentDigest rate_card_digest;
  auto operator==(const PricingObservation &) const -> bool = default;
};

enum class PricingErrorCode {
  invalid_source,
  invalid_pricing,
  invalid_digest,
  resource_exhausted,
};

struct PricingError {
  PricingErrorCode code{PricingErrorCode::invalid_pricing};
  std::string message;
  auto operator==(const PricingError &) const -> bool = default;
};

[[nodiscard]] auto make_pricing_observation(
    ModelId model_id, std::string source_id,
    std::optional<std::string> source_revision,
    std::chrono::sys_time<std::chrono::milliseconds> fetched_at,
    PricingCatalogOrigin origin, TextPricing pricing)
    -> std::expected<PricingObservation, PricingError>;

[[nodiscard]] auto
validate_pricing_observation(const PricingObservation &observation)
    -> std::expected<void, PricingError>;

} // namespace aiforge::domain
