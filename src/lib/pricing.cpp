#include <aiforge/domain/pricing.hpp>

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <utility>

namespace aiforge::domain {
namespace {

constexpr std::size_t kMaximumIdentityBytes = 256;

[[nodiscard]] auto failure(const PricingErrorCode code, std::string message)
    -> std::unexpected<PricingError> {
  return std::unexpected(PricingError{code, std::move(message)});
}

[[nodiscard]] auto valid_identity(const std::string_view value) -> bool {
  if (value.empty() || value.size() > kMaximumIdentityBytes) return false;
  return std::ranges::all_of(value, [](const unsigned char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '.' ||
           character == '_' || character == '-' || character == ':' ||
           character == '+';
  });
}

[[nodiscard]] auto valid_rate(const std::optional<PriceRate>& rate) -> bool {
  return !rate || rate->usd.has_value() || rate->diem.has_value();
}

[[nodiscard]] auto tier_has_rate(const TextPriceTier& tier) -> bool {
  return tier.input.has_value() || tier.output.has_value() ||
         tier.cache_input.has_value() || tier.cache_write.has_value();
}

[[nodiscard]] auto valid_tier(const TextPriceTier& tier) -> bool {
  return valid_rate(tier.input) && valid_rate(tier.output) &&
         valid_rate(tier.cache_input) && valid_rate(tier.cache_write);
}

auto append(std::string& canonical, const std::string_view value) -> void {
  canonical += std::to_string(value.size());
  canonical.push_back(':');
  canonical.append(value);
}

auto append_amount(std::string& canonical,
                   const std::optional<DecimalAmount>& value) -> void {
  append(canonical, value ? "present" : "absent");
  if (value) append(canonical, value->to_string());
}

auto append_rate(std::string& canonical, const std::optional<PriceRate>& value)
    -> void {
  append(canonical, value ? "present" : "absent");
  if (!value) return;
  append_amount(canonical, value->usd);
  append_amount(canonical, value->diem);
}

auto append_tier(std::string& canonical, const TextPriceTier& tier) -> void {
  append_rate(canonical, tier.input);
  append_rate(canonical, tier.output);
  append_rate(canonical, tier.cache_input);
  append_rate(canonical, tier.cache_write);
}

[[nodiscard]] auto pricing_digest(const ModelId& model_id,
                                  const TextPricing& pricing) -> ContentDigest {
  std::string canonical;
  append(canonical, model_id.value());
  append(canonical, "per-million-tokens");
  append_tier(canonical, pricing.base);
  append(canonical, pricing.extended_threshold_tokens ? "present" : "absent");
  if (pricing.extended_threshold_tokens) {
    append(canonical, std::to_string(*pricing.extended_threshold_tokens));
  }
  append(canonical, pricing.extended ? "present" : "absent");
  if (pricing.extended) append_tier(canonical, *pricing.extended);

  std::uint64_t hash{14695981039346656037ULL};
  for (const unsigned char byte : canonical) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  std::ostringstream value;
  value << std::hex << std::setfill('0') << std::setw(16) << hash;
  return {"fnv1a64", std::move(value).str(), canonical.size()};
}

[[nodiscard]] auto validate_pricing(const TextPricing& pricing)
    -> std::expected<void, PricingError> {
  if (!valid_tier(pricing.base) ||
      (pricing.extended && !valid_tier(*pricing.extended)) ||
      pricing.extended.has_value() !=
          pricing.extended_threshold_tokens.has_value() ||
      (pricing.extended_threshold_tokens &&
       *pricing.extended_threshold_tokens == 0) ||
      (!tier_has_rate(pricing.base) &&
       (!pricing.extended || !tier_has_rate(*pricing.extended)))) {
    return failure(PricingErrorCode::invalid_pricing,
                   "pricing rate card is empty or malformed");
  }
  return {};
}

} // namespace

auto make_pricing_observation(
    ModelId model_id, std::string source_id,
    std::optional<std::string> source_revision,
    const std::chrono::sys_time<std::chrono::milliseconds> fetched_at,
    const PricingCatalogOrigin origin, TextPricing pricing)
    -> std::expected<PricingObservation, PricingError> {
  try {
    if (!valid_identity(source_id) ||
        (source_revision && !valid_identity(*source_revision))) {
      return failure(PricingErrorCode::invalid_source,
                     "pricing source identity is empty or malformed");
    }
    if (auto valid = validate_pricing(pricing); !valid) {
      return std::unexpected(std::move(valid.error()));
    }
    auto digest = pricing_digest(model_id, pricing);
    return PricingObservation{std::move(model_id),
                              std::move(source_id),
                              std::move(source_revision),
                              fetched_at,
                              origin,
                              PricingRateBasis::per_million_tokens,
                              std::move(pricing),
                              std::move(digest)};
  } catch (...) {
    return failure(PricingErrorCode::resource_exhausted,
                   "pricing observation creation failed internally");
  }
}

auto validate_pricing_observation(const PricingObservation& observation)
    -> std::expected<void, PricingError> {
  try {
    if (!valid_identity(observation.source_id) ||
        (observation.source_revision &&
         !valid_identity(*observation.source_revision))) {
      return failure(PricingErrorCode::invalid_source,
                     "pricing source identity is empty or malformed");
    }
    if (observation.basis != PricingRateBasis::per_million_tokens) {
      return failure(PricingErrorCode::invalid_pricing,
                     "pricing rate basis is unsupported");
    }
    if (auto valid = validate_pricing(observation.pricing); !valid)
      return valid;
    if (observation.rate_card_digest !=
        pricing_digest(observation.model_id, observation.pricing)) {
      return failure(PricingErrorCode::invalid_digest,
                     "pricing rate-card digest does not match its values");
    }
    return {};
  } catch (...) {
    return failure(PricingErrorCode::resource_exhausted,
                   "pricing observation validation failed internally");
  }
}

} // namespace aiforge::domain
