#include <aiforge/adapters/venice_model_catalog_source.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <ranges>
#include <utility>

#include <venice/auth.hpp>
#include <venice/client.hpp>

namespace aiforge::adapters {
namespace {

[[nodiscard]] auto error(const model::CatalogErrorCode code,
                         std::string message, const bool retryable = false)
    -> std::unexpected<model::CatalogError> {
  return std::unexpected(
      model::CatalogError{code, std::move(message), retryable});
}

[[nodiscard]] auto map_error(const venice::Error& value)
    -> model::CatalogError {
  switch (value.kind) {
    case venice::ErrorKind::Cancelled:
      return {model::CatalogErrorCode::cancelled,
              "Venice model catalog request cancelled", false};
    case venice::ErrorKind::Parse:
    case venice::ErrorKind::ResponseTooLarge:
      return {model::CatalogErrorCode::invalid_data,
              "Venice returned an invalid model catalog", false};
    case venice::ErrorKind::Network:
    case venice::ErrorKind::RateLimited:
      return {model::CatalogErrorCode::unavailable,
              "Venice model catalog is unavailable", true};
    case venice::ErrorKind::Auth:
    case venice::ErrorKind::PaymentRequired:
    case venice::ErrorKind::InvalidArg:
    case venice::ErrorKind::Http:
    default:
      return {model::CatalogErrorCode::unavailable,
              "Venice model catalog request failed", value.status >= 500};
  }
}

[[nodiscard]] auto decimal(const double value)
    -> std::expected<domain::DecimalAmount, model::CatalogError> {
  if (!std::isfinite(value) || value < 0.0) {
    return error(model::CatalogErrorCode::invalid_data,
                 "Venice returned an invalid model price");
  }
  if (value == 0.0) return domain::DecimalAmount::from("0").value();
  std::array<char, 128> text{};
  const auto [end, conversion_error] =
      std::to_chars(text.data(), text.data() + text.size(), value);
  if (conversion_error != std::errc{}) {
    return error(model::CatalogErrorCode::invalid_data,
                 "Venice returned an unrepresentable model price");
  }
  auto amount = domain::DecimalAmount::from(std::string_view{
      text.data(), static_cast<std::size_t>(end - text.data())});
  if (!amount) {
    return error(model::CatalogErrorCode::invalid_data,
                 "Venice returned an unrepresentable model price");
  }
  return *amount;
}

[[nodiscard]] auto price(const venice::Price& value)
    -> std::expected<model::Price, model::CatalogError> {
  model::Price result;
  if (value.usd) {
    auto converted = decimal(*value.usd);
    if (!converted) return std::unexpected(std::move(converted.error()));
    result.usd = *converted;
  }
  if (value.diem) {
    auto converted = decimal(*value.diem);
    if (!converted) return std::unexpected(std::move(converted.error()));
    result.diem = *converted;
  }
  return result;
}

[[nodiscard]] auto price_tier(const venice::PriceTier& value)
    -> std::expected<model::PriceTier, model::CatalogError> {
  const auto optional_price = [](const std::optional<venice::Price>& source)
      -> std::expected<std::optional<model::Price>, model::CatalogError> {
    if (!source) return std::nullopt;
    auto converted = price(*source);
    if (!converted) return std::unexpected(std::move(converted.error()));
    return std::optional<model::Price>{std::move(*converted)};
  };
  auto input = optional_price(value.input);
  auto output = optional_price(value.output);
  auto cache_input = optional_price(value.cache_input);
  auto cache_write = optional_price(value.cache_write);
  if (!input) return std::unexpected(std::move(input.error()));
  if (!output) return std::unexpected(std::move(output.error()));
  if (!cache_input) return std::unexpected(std::move(cache_input.error()));
  if (!cache_write) return std::unexpected(std::move(cache_write.error()));
  return model::PriceTier{std::move(*input), std::move(*output),
                          std::move(*cache_input), std::move(*cache_write)};
}

[[nodiscard]] auto pricing(const venice::Pricing& value)
    -> std::expected<model::Pricing, model::CatalogError> {
  auto base = price_tier(value.base);
  if (!base) return std::unexpected(std::move(base.error()));
  model::Pricing result{std::move(*base)};
  if (value.extended_threshold_tokens &&
      *value.extended_threshold_tokens >= 0) {
    result.extended_threshold_tokens =
        static_cast<std::uint64_t>(*value.extended_threshold_tokens);
  }
  if (value.extended) {
    auto extended = price_tier(*value.extended);
    if (!extended) return std::unexpected(std::move(extended.error()));
    result.extended = std::move(*extended);
  }
  if (value.generation) {
    auto generation = price(*value.generation);
    if (!generation) return std::unexpected(std::move(generation.error()));
    result.generation = std::move(*generation);
  }
  return result;
}

auto add_capability(std::vector<model::CapabilitySupport>& result,
                    const model::Capability capability,
                    const std::optional<bool> supported) -> void {
  result.push_back({capability, supported});
}

[[nodiscard]] auto capabilities(const venice::ModelCapabilities& value)
    -> std::vector<model::CapabilitySupport> {
  std::vector<model::CapabilitySupport> result;
  result.reserve(14);
  add_capability(result, model::Capability::tool_calling,
                 value.supports_function_calling);
  add_capability(result, model::Capability::vision, value.supports_vision);
  add_capability(result, model::Capability::multiple_images,
                 value.supports_multiple_images);
  add_capability(result, model::Capability::video_input,
                 value.supports_video_input);
  add_capability(result, model::Capability::audio_input,
                 value.supports_audio_input);
  add_capability(result, model::Capability::reasoning,
                 value.supports_reasoning);
  add_capability(result, model::Capability::reasoning_effort,
                 value.supports_reasoning_effort);
  add_capability(result, model::Capability::response_schema,
                 value.supports_response_schema);
  add_capability(result, model::Capability::log_probabilities,
                 value.supports_log_probs);
  add_capability(result, model::Capability::web_search,
                 value.supports_web_search);
  add_capability(result, model::Capability::x_search, value.supports_x_search);
  add_capability(result, model::Capability::tee_attestation,
                 value.supports_tee_attestation);
  add_capability(result, model::Capability::end_to_end_encryption,
                 value.supports_e2ee);
  add_capability(result, model::Capability::optimized_for_code,
                 value.optimized_for_code);
  return result;
}

[[nodiscard]] auto positive(const std::optional<int>& value)
    -> std::optional<std::uint64_t> {
  return value && *value > 0
             ? std::optional<std::uint64_t>{static_cast<std::uint64_t>(*value)}
             : std::nullopt;
}

} // namespace

struct VeniceModelCatalogSource::Impl {
  explicit Impl(VeniceModelCatalogOptions value)
      : options(std::move(value)),
        client(venice::Authentication::public_access(), options.base_url) {}

  VeniceModelCatalogOptions options;
  venice::Client client;
};

VeniceModelCatalogSource::VeniceModelCatalogSource(
    VeniceModelCatalogOptions options)
    : m_impl(std::make_unique<Impl>(std::move(options))) {
}
VeniceModelCatalogSource::~VeniceModelCatalogSource() = default;
VeniceModelCatalogSource::VeniceModelCatalogSource(
    VeniceModelCatalogSource&&) noexcept = default;
auto VeniceModelCatalogSource::operator=(VeniceModelCatalogSource&&) noexcept
    -> VeniceModelCatalogSource& = default;

auto VeniceModelCatalogSource::fetch(const std::stop_token stop_token)
    -> std::expected<model::CatalogSnapshot, model::CatalogError> {
  try {
    if (stop_token.stop_requested()) {
      return error(model::CatalogErrorCode::cancelled,
                   "Venice model catalog request cancelled");
    }
    if (m_impl == nullptr) {
      return error(model::CatalogErrorCode::internal_failure,
                   "Venice model catalog source is not initialized");
    }
    venice::CancelToken cancellation;
    std::stop_callback callback{stop_token,
                                [&cancellation] { cancellation.cancel(); }};
    auto fetched = m_impl->client.models(
        "all", {m_impl->options.connect_timeout, m_impl->options.read_timeout,
                m_impl->options.write_timeout, &cancellation});
    if (!fetched) return std::unexpected(map_error(fetched.error()));
    model::CatalogSnapshot result{std::chrono::floor<std::chrono::milliseconds>(
        std::chrono::system_clock::now())};
    result.source_id = "venice.models";
    result.entries.reserve(fetched->size());
    for (const auto& source : *fetched) {
      auto id = domain::ModelId::from(source.id);
      if (!id || source.type.empty()) continue;
      auto context = positive(source.available_context_tokens);
      if (const auto top_level = positive(source.context_length); top_level) {
        context = context ? std::min(*context, *top_level) : top_level;
      }
      model::CatalogEntry entry{std::move(*id), source.type};
      entry.name = source.name;
      entry.context_window_tokens = context;
      entry.maximum_output_tokens = positive(source.max_completion_tokens);
      entry.offline = source.offline.value_or(false);
      entry.traits = source.traits;
      if (source.capabilities)
        entry.capabilities = capabilities(*source.capabilities);
      if (source.pricing) {
        auto mapped = pricing(*source.pricing);
        if (!mapped) return std::unexpected(std::move(mapped.error()));
        entry.pricing = std::move(*mapped);
      }
      result.entries.push_back(std::move(entry));
    }
    std::ranges::sort(result.entries, {}, [](const model::CatalogEntry& entry) {
      return std::pair{entry.type, std::string{entry.id.value()}};
    });
    return result;
  } catch (...) {
    return error(model::CatalogErrorCode::internal_failure,
                 "Venice model catalog mapping failed internally");
  }
}

} // namespace aiforge::adapters
