#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <aiforge/domain/usage_ledger.hpp>

namespace {

using namespace aiforge::domain;

template <typename IdType> auto id(const std::string& value) -> IdType {
  return IdType::from(value).value();
}

auto amount(const std::string& value) -> DecimalAmount {
  return DecimalAmount::from(value).value();
}

auto ceiling(const std::string& value) -> SessionSpendCeiling {
  return SessionSpendCeiling::from(value).value();
}

auto money(std::string unit, const std::string& value) -> MonetaryAmount {
  return MonetaryAmount::create(std::move(unit), amount(value)).value();
}

auto reported(std::vector<MonetaryAmount> values) -> ReportedCost {
  return ReportedCost::create(std::move(values)).value();
}

auto observation() -> PricingObservation {
  TextPricing pricing;
  pricing.base.input = PriceRate{amount("1"), std::nullopt};
  return make_pricing_observation(
             id<ModelId>("model"), "test.catalog", std::nullopt,
             EventTimestamp{std::chrono::milliseconds{1}},
             PricingCatalogOrigin::live, std::move(pricing))
      .value();
}

auto record(const std::string& inference, Usage usage,
            std::optional<ReportedCost> cost,
            std::optional<PricingObservation> pricing) -> InferenceUsageRecord {
  return {id<RunId>("run-" + inference),
          id<InferenceId>(inference),
          id<ModelId>("model"),
          EventTimestamp{},
          EventTimestamp{std::chrono::milliseconds{2}},
          InferenceUsageStatus::completed,
          usage,
          true,
          std::move(cost),
          std::move(pricing)};
}

template <typename Payload>
auto event(const std::uint64_t sequence, Payload payload, std::string event_id)
    -> RunEvent {
  return {{id<EventId>(std::move(event_id)), id<RunId>("policy"), sequence, 1,
           EventTimestamp{std::chrono::milliseconds{sequence}}, std::nullopt,
           std::nullopt, std::nullopt},
          std::move(payload)};
}

} // namespace

TEST_CASE("session spend ceilings reject ambiguous or unsafe amounts",
          "[spend][failure]") {
  for (const auto* value : {"", "0", "-1", "+1", ".1", "1.", "1e2", "0.0000001",
                            "1.0000000", "18446744073709551616"}) {
    CAPTURE(value);
    REQUIRE_FALSE(SessionSpendCeiling::from(value));
  }

  REQUIRE(compare(amount("1.000001"), amount("1.0000009")) ==
          std::strong_ordering::greater);
  REQUIRE(subtract(amount("1"), amount("0.000001"))->to_string() == "0.999999");
  const auto negative = subtract(amount("0.9"), amount("1"));
  REQUIRE_FALSE(negative);
  REQUIRE(negative.error().code == MoneyErrorCode::negative_result);
}

TEST_CASE("session spend fails closed when any inference lacks USD accounting",
          "[spend][failure]") {
  const std::vector<InferenceUsageRecord> records{
      record("reported", {}, reported({money("USD", "0.4")}), std::nullopt),
      record("unknown", {}, reported({money("venice.diem", "2")}),
             std::nullopt)};

  const auto summary = summarize_session_spend(records, ceiling("2"));
  REQUIRE_FALSE(summary.accounted);
  REQUIRE_FALSE(summary.remaining);
  REQUIRE(summary.reported_inferences == 1);
  REQUIRE(summary.estimated_inferences == 0);
  REQUIRE(summary.unavailable ==
          std::vector<CostEstimateFailureCount>{
              {CostEstimateUnavailableReason::pricing_unobserved, 1}});
}

TEST_CASE("reported USD wins and catalog USD fills only missing actuals",
          "[spend]") {
  const std::vector<InferenceUsageRecord> records{
      record("actual", {9'000'000, 0, 0, 0},
             reported({money("USD", "0.4"), money("venice.diem", "8")}),
             observation()),
      record("estimate", {1'000'000, 0, 0, 0},
             reported({money("venice.diem", "1")}), observation())};

  const auto open = summarize_session_spend(records, ceiling("2"));
  REQUIRE(open.accounted->amount().to_string() == "1.4");
  REQUIRE(open.remaining->amount().to_string() == "0.6");
  REQUIRE(open.reported_inferences == 1);
  REQUIRE(open.estimated_inferences == 1);
  REQUIRE_FALSE(open.reached);

  const auto crossed = summarize_session_spend(records, ceiling("1.4"));
  REQUIRE(crossed.reached);
  REQUIRE(crossed.remaining->amount().to_string() == "0");
}

TEST_CASE("spend ceiling projection rejects widening and bad event order",
          "[spend][projection][failure]") {
  SessionSpendCeilingProjection projection;
  REQUIRE(projection.apply(
      event(1,
            SessionSpendCeilingSet{ceiling("10"),
                                   SessionSpendCeilingSource::command_line},
            "set-10")));
  REQUIRE(projection.apply(
      event(2,
            SessionSpendCeilingSet{ceiling("10"),
                                   SessionSpendCeilingSource::command_line},
            "set-10-again")));
  REQUIRE(projection.apply(
      event(3,
            SessionSpendCeilingSet{ceiling("5"),
                                   SessionSpendCeilingSource::command_line},
            "set-5")));
  REQUIRE(projection.ceiling()->amount().to_string() == "5");

  const auto widened = projection.apply(
      event(4,
            SessionSpendCeilingSet{ceiling("6"),
                                   SessionSpendCeilingSource::command_line},
            "set-6"));
  REQUIRE_FALSE(widened);
  REQUIRE(widened.error().code == UsageLedgerErrorCode::ceiling_widening);
  REQUIRE(projection.last_sequence() == 3);

  const auto duplicate =
      projection.apply(event(4, UnknownEvent{"future"}, "set-5"));
  REQUIRE_FALSE(duplicate);
  REQUIRE(duplicate.error().code == UsageLedgerErrorCode::duplicate_event_id);

  const auto regressing =
      projection.apply(event(2, UnknownEvent{"future"}, "regressing"));
  REQUIRE_FALSE(regressing);
  REQUIRE(regressing.error().code ==
          UsageLedgerErrorCode::non_monotonic_sequence);
}
