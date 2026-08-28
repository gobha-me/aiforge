#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <aiforge/domain/run_projection.hpp>
#include <aiforge/domain/usage_ledger.hpp>

namespace {

using namespace aiforge::domain;

template <typename IdType> auto make_id(const std::string& value) -> IdType {
  return IdType::from(value).value();
}

auto amount(const std::string_view text) -> DecimalAmount {
  return DecimalAmount::from(text).value();
}

auto money(std::string unit, const std::string_view value) -> MonetaryAmount {
  return MonetaryAmount::create(std::move(unit), amount(value)).value();
}

auto cost(std::vector<MonetaryAmount> amounts) -> ReportedCost {
  return ReportedCost::create(std::move(amounts)).value();
}

template <typename Payload>
auto event(const std::uint64_t sequence, Payload payload, std::string event_id,
           std::string run_id = "run") -> RunEvent {
  return {{make_id<EventId>(std::move(event_id)),
           make_id<RunId>(std::move(run_id)), sequence, 1,
           EventTimestamp{std::chrono::milliseconds{sequence}}, std::nullopt,
           std::nullopt, std::nullopt},
          std::move(payload)};
}

auto inference(const std::string& id, const std::string& model)
    -> InferenceStarted {
  return {make_id<InferenceId>(id), make_id<ModelId>(model)};
}

auto started() -> RunStarted {
  return {make_id<SurfaceId>("surface"), make_id<WorkspaceId>("chat"),
          make_id<PermissionProfileId>("observe"), std::nullopt};
}

} // namespace

TEST_CASE("decimal money rejects malformed and overflowing values",
          "[cost][failure]") {
  for (const auto* value : {"", "-1", "+1", ".", "1.", "1e", "1x", "nan", "inf",
                            "1e-19", "18446744073709551616"}) {
    CAPTURE(value);
    REQUIRE_FALSE(DecimalAmount::from(value));
  }

  const auto maximum = amount("18446744073709551615");
  const auto overflow = add(maximum, amount("1"));
  REQUIRE_FALSE(overflow);
  REQUIRE(overflow.error().code == MoneyErrorCode::amount_overflow);
  REQUIRE_FALSE(DecimalAmount::from(std::string(129, '1')));
}

TEST_CASE("decimal money canonicalizes exponents and checked sums", "[cost]") {
  REQUIRE(amount("0012.3400").to_string() == "12.34");
  REQUIRE(amount("6.45375e-2").to_string() == "0.0645375");
  REQUIRE(amount("15e1").to_string() == "150");
  REQUIRE(amount("0e-999").to_string() == "0");
  REQUIRE(add(amount("0.9"), amount("0.0645375"))->to_string() == "0.9645375");
}

TEST_CASE("reported costs enforce bounded unique unit identities",
          "[cost][failure]") {
  REQUIRE_FALSE(MonetaryAmount::create("", amount("1")));
  REQUIRE_FALSE(MonetaryAmount::create("1USD", amount("1")));
  REQUIRE_FALSE(MonetaryAmount::create("USD\nforged", amount("1")));
  REQUIRE_FALSE(ReportedCost::create({}));

  std::vector<MonetaryAmount> too_many;
  for (int index = 0; index < 17; ++index) {
    too_many.push_back(money("unit" + std::to_string(index), "1"));
  }
  REQUIRE_FALSE(ReportedCost::create(std::move(too_many)));

  auto duplicate = ReportedCost::create({money("USD", "1"), money("USD", "2")});
  REQUIRE_FALSE(duplicate);
  REQUIRE(duplicate.error().code == MoneyErrorCode::duplicate_unit);
}

TEST_CASE("reported cost sums preserve currencies and stable ordering",
          "[cost]") {
  const auto first = cost({money("venice.diem", "0.04"), money("USD", "0")});
  const auto second =
      cost({money("venice.diem", "0.0245375"), money("credits", "2")});
  const auto total = add(first, second);
  REQUIRE(total);
  REQUIRE(total->amounts().size() == 3);
  REQUIRE(total->amounts()[0].unit() == "USD");
  REQUIRE(total->amounts()[1].unit() == "credits");
  REQUIRE(total->amounts()[2].unit() == "venice.diem");
  REQUIRE(total->amounts()[2].amount().to_string() == "0.0645375");
}

TEST_CASE("usage ledger rejects unrelated duplicate and late costs",
          "[cost][ledger][failure]") {
  UsageLedgerProjection ledger;
  const auto inference_id = make_id<InferenceId>("inference");
  const auto reported = cost({money("USD", "1")});

  auto orphan = ledger.apply(
      event(1, InferenceCostRecorded{inference_id, reported}, "orphan"));
  REQUIRE_FALSE(orphan);
  REQUIRE(orphan.error().code == UsageLedgerErrorCode::unknown_inference);

  REQUIRE(ledger.apply(event(1, inference("inference", "model"), "start")));
  auto wrong_run = ledger.apply(event(
      2, InferenceCostRecorded{inference_id, reported}, "wrong", "other"));
  REQUIRE_FALSE(wrong_run);
  REQUIRE(wrong_run.error().code == UsageLedgerErrorCode::wrong_run);

  REQUIRE(ledger.apply(
      event(2, InferenceCostRecorded{inference_id, reported}, "cost")));
  auto duplicate = ledger.apply(
      event(3, InferenceCostRecorded{inference_id, reported}, "duplicate"));
  REQUIRE_FALSE(duplicate);
  REQUIRE(duplicate.error().code == UsageLedgerErrorCode::invalid_transition);
  REQUIRE(ledger.total_reported_cost() == reported);

  REQUIRE(ledger.apply(
      event(3, InferenceFinished{inference_id, FinishReason::stop}, "finish")));
  auto late = ledger.apply(
      event(4, InferenceCostRecorded{inference_id, reported}, "late"));
  REQUIRE_FALSE(late);
  REQUIRE(late.error().code == UsageLedgerErrorCode::invalid_transition);
  REQUIRE(ledger.last_sequence() == 3);
}

TEST_CASE("cost ledger overflow leaves records and totals unchanged",
          "[cost][ledger][failure]") {
  UsageLedgerProjection ledger;
  const auto first = make_id<InferenceId>("first");
  const auto second = make_id<InferenceId>("second");
  const auto maximum = cost({money("USD", "18446744073709551615")});
  const auto one = cost({money("USD", "1")});

  REQUIRE(ledger.apply(event(1, inference("first", "model"), "start-1")));
  REQUIRE(
      ledger.apply(event(2, InferenceCostRecorded{first, maximum}, "cost-1")));
  REQUIRE(ledger.apply(
      event(3, InferenceFinished{first, FinishReason::stop}, "finish-1")));
  REQUIRE(ledger.apply(event(4, inference("second", "model"), "start-2")));

  const auto overflow =
      ledger.apply(event(5, InferenceCostRecorded{second, one}, "cost-2"));
  REQUIRE_FALSE(overflow);
  REQUIRE(overflow.error().code == UsageLedgerErrorCode::cost_overflow);
  REQUIRE_FALSE(ledger.records()[1].reported_cost);
  REQUIRE(ledger.total_reported_cost() == maximum);
  REQUIRE(ledger.last_sequence() == 4);
}

TEST_CASE("run and session projections replay reported costs deterministically",
          "[cost][replay]") {
  const auto first = make_id<InferenceId>("first");
  const auto second = make_id<InferenceId>("second");
  const auto first_cost =
      cost({money("USD", "0"), money("venice.diem", "0.04")});
  const auto second_cost = cost({money("venice.diem", "0.0245375")});
  const std::vector<RunEvent> events{
      event(1, started(), "run-start"),
      event(2, inference("first", "model-a"), "first-start"),
      event(3, InferenceCostRecorded{first, first_cost}, "first-cost"),
      event(4, InferenceFailed{first, {ErrorCode::backend, "redacted", true}},
            "first-failed"),
      event(5, inference("second", "model-b"), "second-start"),
      event(6, InferenceCostRecorded{second, second_cost}, "second-cost"),
      event(7, InferenceCancelled{second, std::string{"cancelled"}},
            "second-cancelled"),
      event(8, RunCancelled{std::string{"cancelled"}}, "run-cancelled"),
  };

  RunProjection run;
  RunProjection replayed_run;
  UsageLedgerProjection ledger;
  UsageLedgerProjection replayed_ledger;
  for (const auto& item : events) {
    REQUIRE(run.apply(item));
    REQUIRE(replayed_run.apply(item));
    REQUIRE(ledger.apply(item));
    REQUIRE(replayed_ledger.apply(item));
  }

  REQUIRE(run.reported_cost() == replayed_run.reported_cost());
  REQUIRE(ledger.records() == replayed_ledger.records());
  REQUIRE(ledger.total_reported_cost() ==
          replayed_ledger.total_reported_cost());
  REQUIRE(run.reported_cost()->amounts().back().amount().to_string() ==
          "0.0645375");
  REQUIRE(ledger.records()[0].status == InferenceUsageStatus::failed);
  REQUIRE(ledger.records()[1].status == InferenceUsageStatus::cancelled);
}
