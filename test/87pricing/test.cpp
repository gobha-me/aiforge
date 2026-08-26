#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <aiforge/domain/run_projection.hpp>
#include <aiforge/domain/usage_ledger.hpp>
#include <aiforge/runtime/run_kernel.hpp>
#include <aiforge/testing/scripted_backend.hpp>

namespace {

using namespace aiforge;

template <typename IdType> auto id(const std::string &value) -> IdType {
  return IdType::from(value).value();
}

auto amount(const std::string &value) -> domain::DecimalAmount {
  return domain::DecimalAmount::from(value).value();
}

auto pricing(const std::string &input = "1.42") -> domain::TextPricing {
  domain::TextPricing result;
  result.base.input = domain::PriceRate{amount(input), amount("2.5")};
  result.base.output = domain::PriceRate{amount("2.83"), std::nullopt};
  result.base.cache_input = domain::PriceRate{amount("0.23"), std::nullopt};
  return result;
}

auto observation(const std::string &input = "1.42")
    -> domain::PricingObservation {
  return domain::make_pricing_observation(
             id<domain::ModelId>("model"), "venice.models", std::nullopt,
             domain::EventTimestamp{std::chrono::milliseconds{123}},
             domain::PricingCatalogOrigin::fresh_cache, pricing(input))
      .value();
}

auto observation(domain::TextPricing value) -> domain::PricingObservation {
  return domain::make_pricing_observation(
             id<domain::ModelId>("model"), "venice.models", std::nullopt,
             domain::EventTimestamp{std::chrono::milliseconds{123}},
             domain::PricingCatalogOrigin::fresh_cache, std::move(value))
      .value();
}

auto usage_record(domain::Usage usage, domain::TextPricing prices)
    -> domain::InferenceUsageRecord {
  return {id<domain::RunId>("run"),
          id<domain::InferenceId>("inference"),
          id<domain::ModelId>("model"),
          domain::EventTimestamp{},
          std::nullopt,
          domain::InferenceUsageStatus::completed,
          usage,
          true,
          std::nullopt,
          observation(std::move(prices))};
}

template <typename Payload>
auto event(const std::uint64_t sequence, Payload payload,
           std::string run = "run") -> domain::RunEvent {
  return {{id<domain::EventId>("event-" + std::to_string(sequence)),
           id<domain::RunId>(std::move(run)), sequence, 1,
           domain::EventTimestamp{std::chrono::milliseconds{sequence}},
           std::nullopt, std::nullopt, std::nullopt},
          std::move(payload)};
}

auto context() -> domain::ConstructedContext {
  return {{{id<domain::ContextEntryId>("runtime-context"),
            domain::ContextEntryKind::instruction,
            domain::InstructionLayer::application_runtime,
            {id<domain::MessageId>("runtime-message"),
             domain::Role::system,
             {domain::TextBlock{"runtime contract"}},
             std::nullopt},
            {id<domain::ContextSourceId>("runtime-source"), std::nullopt,
             std::nullopt},
            0,
            1,
            2}},
          {{id<domain::ContextEntryId>("runtime-context"),
            domain::ContextDecision::admitted, std::nullopt}},
          {4096, 512, 0},
          2};
}

auto request() -> backend::BackendRequest {
  return {id<domain::InferenceId>("inference"),
          id<domain::MessageId>("assistant"),
          id<domain::ModelId>("model"),
          context(),
          {},
          {std::nullopt, 128, std::nullopt, {}}};
}

} // namespace

TEST_CASE("pricing observations validate provenance tiers and stable digests",
          "[pricing][failure]") {
  auto empty = domain::make_pricing_observation(
      id<domain::ModelId>("model"), "venice.models", std::nullopt,
      domain::EventTimestamp{}, domain::PricingCatalogOrigin::live,
      domain::TextPricing{});
  REQUIRE_FALSE(empty);
  REQUIRE(empty.error().code == domain::PricingErrorCode::invalid_pricing);

  auto malformed_source = domain::make_pricing_observation(
      id<domain::ModelId>("model"), "venice\nmodels", std::nullopt,
      domain::EventTimestamp{}, domain::PricingCatalogOrigin::live, pricing());
  REQUIRE_FALSE(malformed_source);

  auto mismatched_tier = pricing();
  mismatched_tier.extended_threshold_tokens = 200000;
  REQUIRE_FALSE(domain::make_pricing_observation(
      id<domain::ModelId>("model"), "venice.models", std::nullopt,
      domain::EventTimestamp{}, domain::PricingCatalogOrigin::live,
      std::move(mismatched_tier)));

  const auto first = observation();
  const auto repeated = observation();
  const auto changed = observation("1.43");
  REQUIRE(first.rate_card_digest == repeated.rate_card_digest);
  REQUIRE(first.rate_card_digest != changed.rate_card_digest);
  REQUIRE(first.basis == domain::PricingRateBasis::per_million_tokens);
  REQUIRE(first.origin == domain::PricingCatalogOrigin::fresh_cache);
  REQUIRE(first.pricing.base.cache_input.has_value());
  REQUIRE_FALSE(first.pricing.base.cache_write.has_value());

  auto forged = first;
  forged.rate_card_digest.value = "forged";
  REQUIRE_FALSE(domain::validate_pricing_observation(forged));
}

TEST_CASE(
    "pricing observations reject orphan duplicate wrong-run and late facts",
    "[pricing][ledger][failure]") {
  domain::UsageLedgerProjection ledger;
  const auto inference = id<domain::InferenceId>("inference");
  const auto observed = observation();

  auto orphan = ledger.apply(
      event(1, domain::InferencePricingObserved{inference, observed}));
  REQUIRE_FALSE(orphan);
  REQUIRE(orphan.error().code ==
          domain::UsageLedgerErrorCode::unknown_inference);

  REQUIRE(ledger.apply(event(
      1, domain::InferenceStarted{inference, id<domain::ModelId>("model")})));
  auto wrong = ledger.apply(
      event(2, domain::InferencePricingObserved{inference, observed}, "other"));
  REQUIRE_FALSE(wrong);
  REQUIRE(wrong.error().code == domain::UsageLedgerErrorCode::wrong_run);

  const auto wrong_model = domain::make_pricing_observation(
                               id<domain::ModelId>("other-model"),
                               "venice.models", std::nullopt,
                               domain::EventTimestamp{},
                               domain::PricingCatalogOrigin::live, pricing())
                               .value();
  auto mismatched = ledger.apply(
      event(2, domain::InferencePricingObserved{inference, wrong_model}));
  REQUIRE_FALSE(mismatched);
  REQUIRE(mismatched.error().code ==
          domain::UsageLedgerErrorCode::invalid_transition);

  REQUIRE(ledger.apply(
      event(2, domain::InferencePricingObserved{inference, observed})));
  REQUIRE(ledger.records().front().pricing_observation == observed);
  auto duplicate = ledger.apply(
      event(3, domain::InferencePricingObserved{inference, observed}));
  REQUIRE_FALSE(duplicate);
  REQUIRE(duplicate.error().code ==
          domain::UsageLedgerErrorCode::invalid_transition);

  REQUIRE(ledger.apply(event(
      3, domain::InferenceFinished{inference, domain::FinishReason::stop})));
  auto late = ledger.apply(
      event(4, domain::InferencePricingObserved{inference, observed}));
  REQUIRE_FALSE(late);
  REQUIRE(ledger.last_sequence() == 3);
}

TEST_CASE("run kernel records pricing before backend work and replay is "
          "deterministic",
          "[pricing][runtime][replay]") {
  auto backend_request = request();
  testing::ScriptedBackend backend{{testing::ScriptedExchange{
      backend_request,
      backend::BackendError{backend::BackendErrorKind::unavailable,
                            "unavailable", true, 503}}}};
  runtime::RunKernel kernel{id<domain::SessionId>("session"), backend};
  const auto observed = observation();
  runtime::RunStart start{
      id<domain::RunId>("run"),
      {id<domain::SurfaceId>("test"), id<domain::WorkspaceId>("chat"),
       id<domain::PermissionProfileId>("observe"), std::nullopt},
      {id<domain::MessageId>("user"),
       domain::Role::user,
       {domain::TextBlock{"hello"}},
       std::nullopt},
      std::move(backend_request),
      std::nullopt,
      std::nullopt,
      observed};
  REQUIRE(kernel.start(std::move(start)));

  const auto &events = kernel.event_log().events();
  const auto started = std::ranges::find_if(events, [](const auto &item) {
    return std::holds_alternative<domain::InferenceStarted>(item.payload);
  });
  const auto priced = std::ranges::find_if(events, [](const auto &item) {
    return std::holds_alternative<domain::InferencePricingObserved>(
        item.payload);
  });
  REQUIRE(started != events.end());
  REQUIRE(priced != events.end());
  REQUIRE(started->metadata.sequence < priced->metadata.sequence);

  domain::RunProjection replayed_run;
  domain::UsageLedgerProjection replayed_ledger;
  for (const auto &item : events) {
    REQUIRE(replayed_run.apply(item));
    REQUIRE(replayed_ledger.apply(item));
  }
  REQUIRE(replayed_run.pricing_observations() ==
          std::vector<domain::PricingObservation>{observed});
  REQUIRE(replayed_ledger.records().front().pricing_observation == observed);
}

TEST_CASE("catalog estimates keep token buckets exact without double charging",
          "[pricing][estimate]") {
  domain::TextPricing prices;
  prices.base.input = domain::PriceRate{amount("1"), amount("2")};
  prices.base.cache_input = domain::PriceRate{amount("0.5"), amount("1")};
  prices.base.output = domain::PriceRate{amount("2"), amount("4")};
  const auto record = usage_record({1'000'000, 500'000, 200'000, 100'000},
                                   std::move(prices));

  const auto usd = domain::estimate_inference_cost(
      record, domain::CostEstimateUnit::usd);
  REQUIRE(usd);
  REQUIRE(usd->amount.unit() == "USD");
  REQUIRE(usd->amount.amount().to_string() == "1.9");
  REQUIRE(usd->tier == domain::PricingTierSelection::base);
  REQUIRE(usd->rate_card_digest ==
          record.pricing_observation->rate_card_digest);

  const auto diem = domain::estimate_inference_cost(
      record, domain::CostEstimateUnit::venice_diem);
  REQUIRE(diem);
  REQUIRE(diem->amount.unit() == "venice.diem");
  REQUIRE(diem->amount.amount().to_string() == "3.8");

  auto lifecycle = record;
  for (const auto status : {domain::InferenceUsageStatus::active,
                            domain::InferenceUsageStatus::failed,
                            domain::InferenceUsageStatus::cancelled}) {
    lifecycle.status = status;
    REQUIRE(domain::estimate_inference_cost(
        lifecycle, domain::CostEstimateUnit::usd));
  }
}

TEST_CASE("catalog estimates select tiers only when both interpretations agree",
          "[pricing][estimate][failure]") {
  const auto prices = [] {
    domain::TextPricing value;
    value.base.input = domain::PriceRate{amount("1"), std::nullopt};
    value.base.output = domain::PriceRate{amount("1"), std::nullopt};
    value.extended_threshold_tokens = 100;
    domain::TextPriceTier extended;
    extended.input = domain::PriceRate{amount("2"), std::nullopt};
    extended.output = domain::PriceRate{amount("2"), std::nullopt};
    value.extended = std::move(extended);
    return value;
  };

  auto base = usage_record({60, 40, 0, 0}, prices());
  REQUIRE(domain::estimate_inference_cost(base,
                                          domain::CostEstimateUnit::usd)
              ->tier == domain::PricingTierSelection::base);

  auto extended = usage_record({101, 0, 0, 0}, prices());
  REQUIRE(domain::estimate_inference_cost(extended,
                                          domain::CostEstimateUnit::usd)
              ->tier == domain::PricingTierSelection::extended);

  auto ambiguous = usage_record({90, 20, 0, 0}, prices());
  const auto result = domain::estimate_inference_cost(
      ambiguous, domain::CostEstimateUnit::usd);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().reason ==
          domain::CostEstimateUnavailableReason::ambiguous_extended_tier);
}

TEST_CASE("catalog estimates fail closed on unobservable or inconsistent data",
          "[pricing][estimate][failure]") {
  auto prices = pricing();
  prices.base.cache_write =
      domain::PriceRate{amount("0.1"), amount("0.1")};
  auto cache_write = usage_record({10, 2, 0, 0}, std::move(prices));
  REQUIRE(domain::estimate_inference_cost(
              cache_write, domain::CostEstimateUnit::usd)
              .error()
              .reason == domain::CostEstimateUnavailableReason::
                             cache_write_usage_unavailable);

  auto inconsistent = usage_record({2, 1, 3, 0}, pricing());
  REQUIRE(domain::estimate_inference_cost(
              inconsistent, domain::CostEstimateUnit::usd)
              .error()
              .reason ==
          domain::CostEstimateUnavailableReason::inconsistent_usage);
  inconsistent.usage = {2, 1, 0, 2};
  REQUIRE(domain::estimate_inference_cost(
              inconsistent, domain::CostEstimateUnit::usd)
              .error()
              .reason ==
          domain::CostEstimateUnavailableReason::inconsistent_usage);

  inconsistent.usage = {1, 0, 0, 0};
  inconsistent.usage_observed = false;
  REQUIRE(domain::estimate_inference_cost(
              inconsistent, domain::CostEstimateUnit::usd)
              .error()
              .reason ==
          domain::CostEstimateUnavailableReason::usage_unobserved);
  inconsistent.usage_observed = true;
  inconsistent.pricing_observation.reset();
  REQUIRE(domain::estimate_inference_cost(
              inconsistent, domain::CostEstimateUnit::usd)
              .error()
              .reason ==
          domain::CostEstimateUnavailableReason::pricing_unobserved);

  domain::TextPricing usd_only;
  usd_only.base.input =
      domain::PriceRate{amount("1"), std::nullopt};
  const auto missing_diem =
      usage_record({1, 0, 0, 0}, std::move(usd_only));
  REQUIRE(domain::estimate_inference_cost(
              missing_diem, domain::CostEstimateUnit::venice_diem)
              .error()
              .reason == domain::CostEstimateUnavailableReason::missing_rate);
  auto missing_output = missing_diem;
  missing_output.usage = {0, 1, 0, 0};
  REQUIRE(domain::estimate_inference_cost(
              missing_output, domain::CostEstimateUnit::usd)
              .error()
              .reason == domain::CostEstimateUnavailableReason::missing_rate);

  auto forged = usage_record({1, 0, 0, 0}, pricing());
  forged.pricing_observation->rate_card_digest.value = "forged";
  REQUIRE(domain::estimate_inference_cost(forged,
                                          domain::CostEstimateUnit::usd)
              .error()
              .reason ==
          domain::CostEstimateUnavailableReason::invalid_pricing);
}

TEST_CASE("catalog estimate arithmetic is exact or explicitly unavailable",
          "[pricing][estimate][failure]") {
  domain::TextPricing precise;
  precise.base.input =
      domain::PriceRate{amount("0.000000000000000001"), std::nullopt};
  auto too_precise = usage_record({1, 0, 0, 0}, std::move(precise));
  REQUIRE(domain::estimate_inference_cost(
              too_precise, domain::CostEstimateUnit::usd)
              .error()
              .reason ==
          domain::CostEstimateUnavailableReason::arithmetic_overflow);

  domain::TextPricing large;
  large.base.input = domain::PriceRate{
      amount("18446744073709551615"), std::nullopt};
  auto overflowing = usage_record({2'000'000, 0, 0, 0}, std::move(large));
  REQUIRE(domain::estimate_inference_cost(
              overflowing, domain::CostEstimateUnit::usd)
              .error()
              .reason ==
          domain::CostEstimateUnavailableReason::arithmetic_overflow);
}

TEST_CASE("session catalog estimates expose per-unit partial coverage",
          "[pricing][estimate][projection]") {
  auto available = usage_record({1'000'000, 0, 0, 0}, pricing("1"));
  auto missing = available;
  missing.inference_id = id<domain::InferenceId>("missing");
  missing.pricing_observation.reset();
  const std::vector records{available, missing};

  const auto usd = domain::summarize_cost_estimates(
      records, domain::CostEstimateUnit::usd);
  REQUIRE(usd.subtotal);
  REQUIRE(usd.subtotal->amount().to_string() == "1");
  REQUIRE(usd.estimated_inferences == 1);
  REQUIRE(usd.total_inferences == 2);
  REQUIRE(usd.unavailable ==
          std::vector{domain::CostEstimateFailureCount{
              domain::CostEstimateUnavailableReason::pricing_unobserved, 1}});

  auto zero = usage_record({}, pricing());
  const auto zero_estimate = domain::estimate_inference_cost(
      zero, domain::CostEstimateUnit::usd);
  REQUIRE(zero_estimate);
  REQUIRE(zero_estimate->amount.amount().to_string() == "0");
}

TEST_CASE("session estimate aggregation overflow remains explicit",
          "[pricing][estimate][projection][failure]") {
  domain::TextPricing prices;
  prices.base.input = domain::PriceRate{
      amount("18446744073709551615"), std::nullopt};
  auto first = usage_record({1'000'000, 0, 0, 0}, prices);
  auto second = first;
  second.inference_id = id<domain::InferenceId>("second");
  const std::vector records{first, second};

  const auto summary = domain::summarize_cost_estimates(
      records, domain::CostEstimateUnit::usd);
  REQUIRE(summary.estimated_inferences == 2);
  REQUIRE_FALSE(summary.subtotal);
  REQUIRE(summary.aggregation_failure ==
          domain::CostEstimateUnavailableReason::arithmetic_overflow);
}
