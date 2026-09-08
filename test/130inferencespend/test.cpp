#include <aiforge/runtime/inference_spend.hpp>

#include <aiforge/domain/tool_spend.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <utility>

namespace {
using namespace aiforge;
using namespace domain;
using Code = runtime::InferenceSpendErrorCode;

template <typename Id> auto id(const std::string& value) -> Id {
  return Id::from(value).value();
}
auto money(const std::string& amount, std::string unit = "USD")
    -> MonetaryAmount {
  return MonetaryAmount::create(std::move(unit),
                                DecimalAmount::from(amount).value())
      .value();
}
auto reported(const std::string& amount, std::string unit = "USD")
    -> ReportedCost {
  return ReportedCost::create({money(amount, std::move(unit))}).value();
}
struct History {
  SessionEventLog log{id<SessionId>("session")};
  auto add(RunEventPayload payload, std::optional<InvocationId> invocation = {})
      -> void {
    const auto sequence = log.last_sequence() + 1;
    const auto schema = std::holds_alternative<ToolProposed>(payload) ? 2U : 1U;
    REQUIRE(log.append({{id<EventId>("event-" + std::to_string(sequence)),
                         id<RunId>("run"),
                         sequence,
                         schema,
                         EventTimestamp{std::chrono::milliseconds{sequence}},
                         {},
                         {},
                         std::move(invocation)},
                        std::move(payload)}));
  }
  auto ceiling(const std::string& amount) -> void {
    add(SessionSpendCeilingSet{SessionSpendCeiling::from(amount).value(),
                               SessionSpendCeilingSource::command_line});
  }
  auto inference(std::optional<ReportedCost> cost = {}, bool complete = true)
      -> void {
    const auto inference_id = id<InferenceId>("inference");
    add(InferenceStarted{inference_id, id<ModelId>("model")});
    if (cost) add(InferenceCostRecorded{inference_id, std::move(*cost)});
    if (complete) add(InferenceFinished{inference_id, FinishReason::stop});
  }
  auto reserve(const std::string& amount) -> void {
    const auto invocation = id<InvocationId>("paid");
    const ToolSpendReservation reservation{
        invocation,
        money(amount),
        ToolSpendEstimateBasis::catalog_estimate,
        {"sha256", std::string(64, 'a'), 17},
        EventTimestamp::max()};
    add(ToolProposed{invocation,
                     "paid",
                     {"application/json", "{}"},
                     {Effect::spend},
                     {},
                     true,
                     {},
                     {},
                     {},
                     ToolSpendQuote{reservation.maximum, reservation.basis,
                                    reservation.evidence_digest,
                                    reservation.valid_until},
                     StructuredDataBlock{"application/json", "{}"}},
        invocation);
    add(ToolSpendReserved{reservation}, invocation);
  }
};
auto check(const History& history) {
  const auto before = history.log.events();
  auto result = runtime::preflight_inference_spend(history.log);
  CHECK(history.log.events() == before);
  return result;
}
} // namespace

TEST_CASE(
    "inference spend preflight rejects malformed ceiling and ledger history",
    "[spend][preflight][failure]") {
  History history;
  history.ceiling("1");
  std::string message = "session spend history is invalid";
  SECTION("orphan usage") {
    history.add(UsageRecorded{id<InferenceId>("missing"), {1, 1, 0, 0}});
  }
  SECTION("orphan tool reservation") {
    const auto invocation = id<InvocationId>("missing");
    history.add(ToolSpendReserved{{invocation,
                                   money("0.1"),
                                   ToolSpendEstimateBasis::catalog_estimate,
                                   {"sha256", std::string(64, 'a'), 17},
                                   EventTimestamp::max()}},
                invocation);
  }
  SECTION("ceiling widening") {
    history.ceiling("2");
    message = "session spend ceiling history is invalid";
  }
  SECTION("invalid ceiling source") {
    history.add(
        SessionSpendCeilingSet{SessionSpendCeiling::from("0.5").value(),
                               static_cast<SessionSpendCeilingSource>(99)});
    message = "session spend ceiling history is invalid";
  }
  const auto result = check(history);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == Code::invalid_history);
  CHECK(result.error().message == message);
  CHECK_FALSE(result.error().summary);
}

TEST_CASE("inference spend preflight refuses unavailable USD accounting",
          "[spend][preflight][failure]") {
  History history;
  history.ceiling("1");
  SECTION("completed without price or reported cost") {
    history.inference();
  }
  SECTION("non USD report") {
    history.inference(reported("0.1", "venice.diem"));
  }
  SECTION("active inference remains unaccounted") {
    history.inference({}, false);
  }
  const auto result = check(history);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == Code::accounting_unavailable);
  CHECK(result.error().message ==
        "session spend accounting is unavailable; refusing another inference");
  REQUIRE(result.error().summary);
  CHECK_FALSE(result.error().summary->accounted);
}

TEST_CASE("inference spend preflight refuses a reached or exceeded ceiling",
          "[spend][preflight][failure]") {
  History history;
  history.ceiling("1");
  std::string cost = "1";
  SECTION("exact ceiling") {
  }
  SECTION("exceeded ceiling") {
    cost = "1.25";
  }
  history.inference(reported(cost));
  const auto result = check(history);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == Code::ceiling_reached);
  REQUIRE(result.error().summary);
  REQUIRE(result.error().summary->accounted);
  CHECK(result.error().summary->accounted->amount().to_string() == cost);
  CHECK(result.error().summary->ceiling.amount().to_string() == "1");
  CHECK(result.error().summary->reached);
}

TEST_CASE("inference spend preflight counts outstanding and uncertain tool "
          "reservations",
          "[spend][preflight][tools]") {
  History history;
  history.ceiling("1");
  history.inference(reported("0.4"));
  history.reserve("0.6");
  const auto invocation = id<InvocationId>("paid");
  bool released = false;
  bool reconciled = false;
  SECTION("outstanding reservation") {
  }
  SECTION("uncertain provider outcome") {
    history.add(ToolStarted{invocation}, invocation);
    history.add(
        ToolSpendReconciliationRequired{
            invocation,
            ToolSpendReconciliationReason::provider_cost_unavailable},
        invocation);
    reconciled = true;
  }
  SECTION("released reservation restores capacity") {
    history.add(ToolSpendReleased{invocation}, invocation);
    released = true;
  }
  const auto result = check(history);
  if (released) {
    REQUIRE(result);
    REQUIRE(*result);
    CHECK((*result)->accounted->amount().to_string() == "0.4");
    CHECK((*result)->tool_released == 1);
  } else {
    REQUIRE_FALSE(result);
    CHECK(result.error().code == Code::ceiling_reached);
    REQUIRE(result.error().summary);
    const auto& summary = *result.error().summary;
    REQUIRE(summary.accounted);
    CHECK(summary.accounted->amount().to_string() == "1");
    REQUIRE(summary.tool_accounted);
    CHECK(summary.tool_accounted->amount().to_string() == "0.6");
    if (reconciled) {
      CHECK(summary.tool_reconciliation_required == 1);
      REQUIRE(summary.tool_reconciliation_maximum);
      CHECK(summary.tool_reconciliation_maximum->amount().to_string() == "0.6");
    } else {
      CHECK(summary.tool_reserved == 1);
      REQUIRE(summary.tool_reserved_maximum);
      CHECK(summary.tool_reserved_maximum->amount().to_string() == "0.6");
    }
  }
}

TEST_CASE("inference spend preflight refuses combined accounting overflow",
          "[spend][preflight][failure]") {
  History history;
  const std::string maximum = "18446744073709551615";
  history.ceiling(maximum);
  history.inference(reported(maximum));
  history.reserve("1");
  const auto result = check(history);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == Code::accounting_unavailable);
  CHECK_FALSE(result.error().summary);
}

TEST_CASE("inference spend preflight retains the no ceiling fast path",
          "[spend][preflight]") {
  History history;
  SECTION("empty session") {
  }
  SECTION("unused malformed ledger is not projected") {
    history.add(UsageRecorded{id<InferenceId>("missing"), {1, 1, 0, 0}});
  }
  SECTION("unknown cost without a ceiling") {
    history.inference();
  }
  const auto result = check(history);
  REQUIRE(result);
  CHECK_FALSE(*result);
}

TEST_CASE("inference spend preflight reports zero for a capped empty session",
          "[spend][preflight]") {
  History history;
  history.ceiling("1");
  const auto result = check(history);
  REQUIRE(result);
  REQUIRE(*result);
  REQUIRE((*result)->accounted);
  CHECK((*result)->accounted->amount().to_string() == "0");
  CHECK((*result)->remaining->amount().to_string() == "1");
  CHECK_FALSE((*result)->reached);
}

TEST_CASE("inference spend preflight allows accounted spend below its ceiling",
          "[spend][preflight]") {
  History history;
  history.ceiling("1");
  history.inference(reported("0.25"));
  const auto result = check(history);
  REQUIRE(result);
  REQUIRE(*result);
  REQUIRE((*result)->accounted);
  CHECK((*result)->accounted->amount().to_string() == "0.25");
  CHECK((*result)->remaining->amount().to_string() == "0.75");
  CHECK((*result)->reported_inferences == 1);
  CHECK_FALSE((*result)->reached);
}
