#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <aiforge/domain/events.hpp>
#include <aiforge/domain/tool_spend.hpp>
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

auto digest(const char value = 'a') -> ContentDigest {
  return {"sha256", std::string(64, value), 17};
}

auto expiry() -> EventTimestamp {
  return EventTimestamp::max();
}

auto reservation(const std::string& invocation, const std::string& maximum,
                 const ToolSpendEstimateBasis basis =
                     ToolSpendEstimateBasis::catalog_estimate)
    -> ToolSpendReservation {
  return {id<InvocationId>(invocation), money("USD", maximum), basis, digest(),
          expiry()};
}

auto proposal(const std::string& invocation, const std::string& maximum,
              const ToolSpendEstimateBasis basis =
                  ToolSpendEstimateBasis::catalog_estimate) -> ToolProposed {
  const auto quoted = reservation(invocation, maximum, basis);
  return {quoted.invocation_id,
          "paid",
          {"application/json", "{}"},
          {Effect::spend},
          std::nullopt,
          true,
          {},
          {},
          std::nullopt,
          ToolSpendQuote{quoted.maximum, quoted.basis, quoted.evidence_digest,
                         quoted.valid_until},
          StructuredDataBlock{"application/json", "{}"}};
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

template <typename Payload>
auto spend_event(const std::uint64_t sequence, Payload payload,
                 const std::string& event_id, const std::string& invocation,
                 const std::string& run = "paid-run") -> RunEvent {
  std::uint32_t schema_version{1};
  if constexpr (std::is_same_v<Payload, ToolProposed>) {
    if (payload.spend_quote) schema_version = 2;
  }
  return {{id<EventId>(event_id), id<RunId>(run), sequence, schema_version,
           EventTimestamp{std::chrono::milliseconds{sequence}}, std::nullopt,
           std::nullopt, id<InvocationId>(invocation)},
          std::move(payload)};
}

TEST_CASE("paid tool spend lifecycle remains bound to its proposal run",
          "[spend][tool][replay][failure]") {
  ToolSpendLedgerProjection ledger;
  REQUIRE(ledger.apply(
      spend_event(1, proposal("paid", "1"), "proposal", "paid", "run-a")));

  auto cross_run_reservation =
      ledger.apply(spend_event(2, ToolSpendReserved{reservation("paid", "1")},
                               "reserve-b", "paid", "run-b"));
  REQUIRE_FALSE(cross_run_reservation);
  CHECK(cross_run_reservation.error().code ==
        ToolSpendLedgerErrorCode::wrong_run);
  CHECK(ledger.last_sequence() == 1);

  REQUIRE(
      ledger.apply(spend_event(2, ToolSpendReserved{reservation("paid", "1")},
                               "reserve-a", "paid", "run-a")));
  auto cross_run_start = ledger.apply(spend_event(
      3, ToolStarted{id<InvocationId>("paid")}, "start-b", "paid", "run-b"));
  REQUIRE_FALSE(cross_run_start);
  CHECK(cross_run_start.error().code == ToolSpendLedgerErrorCode::wrong_run);

  REQUIRE(
      ledger.apply(spend_event(3, ToolSpendReleased{id<InvocationId>("paid")},
                               "release-a", "paid", "run-a")));
  auto start_after_release = ledger.apply(spend_event(
      4, ToolStarted{id<InvocationId>("paid")}, "late-start", "paid", "run-a"));
  REQUIRE_FALSE(start_after_release);
  CHECK(start_after_release.error().code ==
        ToolSpendLedgerErrorCode::invalid_transition);
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

TEST_CASE("paid tool reservations conservatively account every terminal state",
          "[spend][tool]") {
  ToolSpendLedgerProjection ledger;
  REQUIRE(ledger.apply(
      spend_event(1, proposal("released", "0.25"), "propose-1", "released")));
  REQUIRE(ledger.apply(
      spend_event(2, ToolSpendReserved{reservation("released", "0.25")},
                  "reserve-1", "released")));
  REQUIRE(ledger.apply(
      spend_event(3, ToolSpendReleased{id<InvocationId>("released")},
                  "release-1", "released")));
  REQUIRE(ledger.apply(
      spend_event(4, proposal("finalized", "0.8"), "propose-2", "finalized")));
  REQUIRE(ledger.apply(
      spend_event(5, ToolSpendReserved{reservation("finalized", "0.8")},
                  "reserve-2", "finalized")));
  REQUIRE(ledger.apply(spend_event(
      6, ToolStarted{id<InvocationId>("finalized")}, "start-2", "finalized")));
  REQUIRE(ledger.apply(spend_event(
      7,
      ToolSpendFinalized{ToolSpendFinalization{
          id<InvocationId>("finalized"), money("USD", "0.6"),
          ToolSpendFinalizationBasis::provider_reported, digest('b')}},
      "finalize-2", "finalized")));
  REQUIRE(ledger.apply(
      spend_event(8, proposal("unknown", "0.4"), "propose-3", "unknown")));
  REQUIRE(ledger.apply(
      spend_event(9, ToolSpendReserved{reservation("unknown", "0.4")},
                  "reserve-3", "unknown")));
  REQUIRE(ledger.apply(spend_event(10, ToolStarted{id<InvocationId>("unknown")},
                                   "start-3", "unknown")));
  REQUIRE(ledger.apply(
      spend_event(11,
                  ToolSpendReconciliationRequired{
                      id<InvocationId>("unknown"),
                      ToolSpendReconciliationReason::provider_cost_unavailable},
                  "reconcile-3", "unknown")));
  REQUIRE(ledger.apply(
      spend_event(12, proposal("active", "0.3"), "propose-4", "active")));
  REQUIRE(ledger.apply(
      spend_event(13, ToolSpendReserved{reservation("active", "0.3")},
                  "reserve-4", "active")));

  const auto summary = summarize_tool_spend(ledger, ceiling("2"));
  REQUIRE(summary);
  CHECK(summary->accounted.amount().to_string() == "1.3");
  CHECK(summary->remaining.amount().to_string() == "0.7");
  CHECK(summary->reservations == 4);
  CHECK(summary->released == 1);
  CHECK(summary->finalized == 1);
  CHECK(summary->reconciliation_required == 1);
  CHECK(summary->reserved_maximum.amount().to_string() == "0.3");
  CHECK(summary->reconciliation_maximum.amount().to_string() == "0.4");
  CHECK(summary->provider_reported_amount.amount().to_string() == "0.6");
  CHECK(summary->catalog_estimate_amount.amount().to_string() == "0");
  CHECK(summary->policy_upper_bound_amount.amount().to_string() == "0");
  CHECK_FALSE(summary->reached);

  const std::vector<InferenceUsageRecord> inference{
      record("inference", {}, reported({money("USD", "0.4")}), std::nullopt)};
  const auto combined =
      summarize_combined_session_spend(inference, ledger, ceiling("2"));
  REQUIRE(combined);
  REQUIRE(combined->accounted);
  REQUIRE(combined->inference_accounted);
  REQUIRE(combined->tool_accounted);
  CHECK(combined->accounted->amount().to_string() == "1.7");
  CHECK(combined->remaining->amount().to_string() == "0.3");
  CHECK(combined->inference_accounted->amount().to_string() == "0.4");
  CHECK(combined->tool_accounted->amount().to_string() == "1.3");
  CHECK(combined->tool_reservations == 4);
  CHECK(combined->tool_reserved == 1);
  CHECK(combined->tool_provider_reported == 1);
  CHECK(combined->tool_reconciliation_required == 1);
  REQUIRE(combined->tool_reserved_maximum);
  REQUIRE(combined->tool_reconciliation_maximum);
  REQUIRE(combined->tool_provider_reported_amount);
  CHECK(combined->tool_reserved_maximum->amount().to_string() == "0.3");
  CHECK(combined->tool_reconciliation_maximum->amount().to_string() == "0.4");
  CHECK(combined->tool_provider_reported_amount->amount().to_string() == "0.6");
}

TEST_CASE("tool spend preserves estimate bases and combines across the ceiling",
          "[spend][tool][summary]") {
  ToolSpendLedgerProjection ledger;
  REQUIRE(ledger.apply(spend_event(
      1, proposal("catalog", "0.4", ToolSpendEstimateBasis::catalog_estimate),
      "catalog-proposed", "catalog")));
  REQUIRE(ledger.apply(spend_event(
      2,
      ToolSpendReserved{reservation("catalog", "0.4",
                                    ToolSpendEstimateBasis::catalog_estimate)},
      "catalog-reserved", "catalog")));
  REQUIRE(ledger.apply(spend_event(3, ToolStarted{id<InvocationId>("catalog")},
                                   "catalog-started", "catalog")));
  REQUIRE(ledger.apply(spend_event(
      4,
      ToolSpendFinalized{ToolSpendFinalization{
          id<InvocationId>("catalog"), money("USD", "0.25"),
          ToolSpendFinalizationBasis::catalog_estimate, std::nullopt}},
      "catalog-finalized", "catalog")));

  REQUIRE(ledger.apply(spend_event(
      5, proposal("policy", "0.6", ToolSpendEstimateBasis::policy_upper_bound),
      "policy-proposed", "policy")));
  REQUIRE(ledger.apply(spend_event(
      6,
      ToolSpendReserved{reservation(
          "policy", "0.6", ToolSpendEstimateBasis::policy_upper_bound)},
      "policy-reserved", "policy")));
  REQUIRE(ledger.apply(spend_event(7, ToolStarted{id<InvocationId>("policy")},
                                   "policy-started", "policy")));
  REQUIRE(ledger.apply(spend_event(
      8,
      ToolSpendFinalized{ToolSpendFinalization{
          id<InvocationId>("policy"), money("USD", "0.5"),
          ToolSpendFinalizationBasis::policy_upper_bound, std::nullopt}},
      "policy-finalized", "policy")));

  REQUIRE(ledger.apply(spend_event(9, proposal("reserved", "0.3"),
                                   "reserved-proposed", "reserved")));
  REQUIRE(ledger.apply(
      spend_event(10, ToolSpendReserved{reservation("reserved", "0.3")},
                  "reserved-active", "reserved")));

  REQUIRE(ledger.apply(
      spend_event(11,
                  proposal("reconciliation", "0.4",
                           ToolSpendEstimateBasis::policy_upper_bound),
                  "reconciliation-proposed", "reconciliation")));
  REQUIRE(ledger.apply(spend_event(
      12,
      ToolSpendReserved{reservation(
          "reconciliation", "0.4", ToolSpendEstimateBasis::policy_upper_bound)},
      "reconciliation-reserved", "reconciliation")));
  REQUIRE(ledger.apply(
      spend_event(13, ToolStarted{id<InvocationId>("reconciliation")},
                  "reconciliation-started", "reconciliation")));
  REQUIRE(ledger.apply(
      spend_event(14,
                  ToolSpendReconciliationRequired{
                      id<InvocationId>("reconciliation"),
                      ToolSpendReconciliationReason::provider_cost_unavailable},
                  "reconciliation-required", "reconciliation")));

  REQUIRE(ledger.apply(spend_event(15, proposal("released", "0.2"),
                                   "released-proposed", "released")));
  REQUIRE(ledger.apply(
      spend_event(16, ToolSpendReserved{reservation("released", "0.2")},
                  "released-reserved", "released")));
  REQUIRE(ledger.apply(
      spend_event(17, ToolSpendReleased{id<InvocationId>("released")},
                  "released-terminal", "released")));

  const auto tools = summarize_tool_spend(ledger, ceiling("2"));
  REQUIRE(tools);
  CHECK(tools->accounted.amount().to_string() == "1.45");
  CHECK(tools->reserved_maximum.amount().to_string() == "0.3");
  CHECK(tools->reconciliation_maximum.amount().to_string() == "0.4");
  CHECK(tools->catalog_estimate_amount.amount().to_string() == "0.25");
  CHECK(tools->policy_upper_bound_amount.amount().to_string() == "0.5");
  CHECK(tools->provider_reported_amount.amount().to_string() == "0");
  CHECK(tools->catalog_estimate == 1);
  CHECK(tools->policy_upper_bound == 1);
  CHECK(tools->reserved == 1);
  CHECK(tools->reconciliation_required == 1);
  CHECK(tools->released == 1);

  const std::vector<InferenceUsageRecord> inference{
      record("combined", {}, reported({money("USD", "0.55")}), std::nullopt)};
  const auto open =
      summarize_combined_session_spend(inference, ledger, ceiling("2.1"));
  REQUIRE(open);
  REQUIRE(open->accounted);
  REQUIRE(open->remaining);
  REQUIRE(open->tool_reserved_maximum);
  REQUIRE(open->tool_reconciliation_maximum);
  REQUIRE(open->tool_catalog_estimate_amount);
  REQUIRE(open->tool_policy_upper_bound_amount);
  CHECK(open->accounted->amount().to_string() == "2");
  CHECK(open->remaining->amount().to_string() == "0.1");
  CHECK(open->tool_reserved_maximum->amount().to_string() == "0.3");
  CHECK(open->tool_reconciliation_maximum->amount().to_string() == "0.4");
  CHECK(open->tool_catalog_estimate_amount->amount().to_string() == "0.25");
  CHECK(open->tool_policy_upper_bound_amount->amount().to_string() == "0.5");
  CHECK_FALSE(open->reached);

  const auto reached =
      summarize_combined_session_spend(inference, ledger, ceiling("2"));
  REQUIRE(reached);
  REQUIRE(reached->accounted);
  REQUIRE(reached->remaining);
  CHECK(reached->accounted->amount().to_string() == "2");
  CHECK(reached->remaining->amount().to_string() == "0");
  CHECK(reached->reached);

  const auto exceeded =
      summarize_combined_session_spend(inference, ledger, ceiling("1.9"));
  REQUIRE(exceeded);
  REQUIRE(exceeded->accounted);
  REQUIRE(exceeded->remaining);
  CHECK(exceeded->accounted->amount().to_string() == "2");
  CHECK(exceeded->remaining->amount().to_string() == "0");
  CHECK(exceeded->reached);
}

TEST_CASE("combined spend reports checked arithmetic overflow",
          "[spend][tool][summary][overflow][failure]") {
  ToolSpendLedgerProjection ledger;
  REQUIRE(
      ledger.apply(spend_event(1, proposal("paid", "1"), "proposal", "paid")));
  REQUIRE(ledger.apply(spend_event(
      2, ToolSpendReserved{reservation("paid", "1")}, "reservation", "paid")));
  REQUIRE(ledger.apply(spend_event(3, ToolStarted{id<InvocationId>("paid")},
                                   "started", "paid")));
  REQUIRE(ledger.apply(spend_event(
      4,
      ToolSpendFinalized{ToolSpendFinalization{
          id<InvocationId>("paid"), money("USD", "1"),
          ToolSpendFinalizationBasis::catalog_estimate, std::nullopt}},
      "finalized", "paid")));

  constexpr auto maximum = "18446744073709551615";
  const std::vector<InferenceUsageRecord> inference{
      record("maximum", {}, reported({money("USD", maximum)}), std::nullopt)};
  const auto combined =
      summarize_combined_session_spend(inference, ledger, ceiling(maximum));
  REQUIRE_FALSE(combined);
  CHECK(combined.error().code == ToolSpendLedgerErrorCode::amount_overflow);
}

TEST_CASE("released reservations restore their full maximum",
          "[spend][tool][summary]") {
  ToolSpendLedgerProjection ledger;
  REQUIRE(ledger.apply(
      spend_event(1, proposal("released", "0.9"), "proposal", "released")));
  REQUIRE(ledger.apply(
      spend_event(2, ToolSpendReserved{reservation("released", "0.9")},
                  "reservation", "released")));

  const auto reserved = summarize_tool_spend(ledger, ceiling("1"));
  REQUIRE(reserved);
  CHECK(reserved->accounted.amount().to_string() == "0.9");
  CHECK(reserved->reserved_maximum.amount().to_string() == "0.9");
  CHECK(reserved->remaining.amount().to_string() == "0.1");
  CHECK(reserved->reserved == 1);
  CHECK(reserved->released == 0);

  REQUIRE(ledger.apply(
      spend_event(3, ToolSpendReleased{id<InvocationId>("released")},
                  "released", "released")));
  const auto released = summarize_tool_spend(ledger, ceiling("1"));
  REQUIRE(released);
  CHECK(released->accounted.amount().to_string() == "0");
  CHECK(released->reserved_maximum.amount().to_string() == "0");
  CHECK(released->remaining.amount().to_string() == "1");
  CHECK(released->reserved == 0);
  CHECK(released->released == 1);
}

TEST_CASE("paid tool ledger rejects invalid amounts evidence and transitions",
          "[spend][tool][failure]") {
  auto valid = reservation("paid", "1");
  for (auto invalid :
       {ToolSpendReservation{id<InvocationId>("zero"), money("USD", "0"),
                             ToolSpendEstimateBasis::catalog_estimate, digest(),
                             expiry()},
        ToolSpendReservation{id<InvocationId>("unit"), money("EUR", "1"),
                             ToolSpendEstimateBasis::catalog_estimate, digest(),
                             expiry()},
        ToolSpendReservation{
            id<InvocationId>("precision"), money("USD", "0.0000001"),
            ToolSpendEstimateBasis::catalog_estimate, digest(), expiry()},
        ToolSpendReservation{id<InvocationId>("digest"), money("USD", "1"),
                             ToolSpendEstimateBasis::catalog_estimate,
                             ContentDigest{"sha256", "bad", 1}, expiry()},
        ToolSpendReservation{id<InvocationId>("basis"), money("USD", "1"),
                             static_cast<ToolSpendEstimateBasis>(100), digest(),
                             expiry()}}) {
    CHECK_FALSE(valid_tool_spend_reservation(invalid));
  }

  CHECK_FALSE(valid_tool_spend_finalization(
      {valid.invocation_id, money("USD", "1.1"),
       ToolSpendFinalizationBasis::policy_upper_bound, std::nullopt},
      valid));
  CHECK_FALSE(valid_tool_spend_finalization(
      {valid.invocation_id, money("USD", "0.5"),
       ToolSpendFinalizationBasis::policy_upper_bound, std::nullopt},
      valid));
  CHECK_FALSE(valid_tool_spend_finalization(
      {valid.invocation_id, money("USD", "1"),
       ToolSpendFinalizationBasis::provider_reported, std::nullopt},
      valid));
  CHECK_FALSE(valid_tool_spend_finalization(
      {valid.invocation_id, money("USD", "1"),
       ToolSpendFinalizationBasis::catalog_estimate, digest()},
      valid));
  CHECK_FALSE(valid_tool_spend_finalization(
      {valid.invocation_id, money("USD", "1"),
       static_cast<ToolSpendFinalizationBasis>(100), std::nullopt},
      valid));
  CHECK_FALSE(valid_tool_spend_reconciliation_reason(
      static_cast<ToolSpendReconciliationReason>(100)));

  ToolSpendLedgerProjection ledger;
  auto missing = ledger.apply(spend_event(
      1, ToolSpendReleased{id<InvocationId>("missing")}, "missing", "missing"));
  REQUIRE_FALSE(missing);
  CHECK(missing.error().code == ToolSpendLedgerErrorCode::unknown_reservation);

  REQUIRE(
      ledger.apply(spend_event(1, proposal("paid", "1"), "propose", "paid")));
  REQUIRE(ledger.apply(
      spend_event(2, ToolSpendReserved{valid}, "reserve", "paid")));
  auto duplicate = ledger.apply(
      spend_event(3, ToolSpendReserved{valid}, "duplicate", "paid"));
  REQUIRE_FALSE(duplicate);
  CHECK(duplicate.error().code ==
        ToolSpendLedgerErrorCode::duplicate_reservation);
  REQUIRE(ledger.apply(
      spend_event(3, ToolStarted{valid.invocation_id}, "start", "paid")));
  REQUIRE(ledger.apply(
      spend_event(4,
                  ToolSpendReconciliationRequired{
                      valid.invocation_id,
                      ToolSpendReconciliationReason::transport_outcome_unknown},
                  "reconcile", "paid")));
  auto terminal = ledger.apply(spend_event(
      5, ToolSpendReleased{valid.invocation_id}, "second-terminal", "paid"));
  REQUIRE_FALSE(terminal);
  CHECK(terminal.error().code == ToolSpendLedgerErrorCode::invalid_transition);

  REQUIRE(ledger.apply(
      spend_event(5, proposal("other", "0.1"), "propose-other", "other")));
  auto wrong_metadata =
      spend_event(6, ToolSpendReserved{reservation("other", "0.1")},
                  "wrong-meta", "different");
  auto wrong = ledger.apply(wrong_metadata);
  REQUIRE_FALSE(wrong);
  CHECK(wrong.error().code == ToolSpendLedgerErrorCode::invalid_reservation);
}

TEST_CASE("paid tool spend must terminate before the ordinary tool result",
          "[spend][tool][ordering][failure]") {
  ToolSpendLedgerProjection ledger;
  const auto paid = reservation("paid", "1");
  REQUIRE(
      ledger.apply(spend_event(1, proposal("paid", "1"), "proposal", "paid")));
  REQUIRE(ledger.apply(
      spend_event(2, ToolSpendReserved{paid}, "reservation", "paid")));
  REQUIRE(ledger.apply(
      spend_event(3, ToolStarted{paid.invocation_id}, "started", "paid")));

  auto early_result = ledger.apply(
      spend_event(4, ToolResultRecorded{paid.invocation_id, {}, std::nullopt},
                  "early-result", "paid"));
  REQUIRE_FALSE(early_result);
  CHECK(early_result.error().code ==
        ToolSpendLedgerErrorCode::invalid_transition);

  REQUIRE(ledger.apply(spend_event(
      4,
      ToolSpendFinalized{ToolSpendFinalization{
          paid.invocation_id, money("USD", "0.5"),
          ToolSpendFinalizationBasis::catalog_estimate, std::nullopt}},
      "finalized", "paid")));
  REQUIRE(ledger.apply(
      spend_event(5, ToolResultRecorded{paid.invocation_id, {}, std::nullopt},
                  "result", "paid")));

  auto duplicate = ledger.apply(
      spend_event(6,
                  ToolErrored{paid.invocation_id,
                              {ErrorCode::invalid_state, "failed", false},
                              std::nullopt},
                  "duplicate-terminal", "paid"));
  REQUIRE_FALSE(duplicate);
  CHECK(duplicate.error().code == ToolSpendLedgerErrorCode::invalid_transition);

  REQUIRE(ledger.apply(
      spend_event(6, UnknownEvent{"future.spend"}, "future", "paid")));
}
