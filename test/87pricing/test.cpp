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
