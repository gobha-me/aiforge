// Spend projection compatibility matrix: schema3 is strictly unpaid and keeps
// typed historical proof plus bounded normalized arguments. Missing proof,
// legacy schema smuggling, spend effects/quotes and malformed normalized data
// fail without advancing the ledger; valid observation lifecycles add no spend.
#include <aiforge/domain/events.hpp>
#include <aiforge/domain/tool_spend.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {
using namespace aiforge::domain;
template <class T> auto id(std::string value) -> T {
  return T::from(std::move(value)).value();
}
auto proposal() -> ToolProposed {
  ToolProposed result{id<InvocationId>("invocation"),
                      "observe_target",
                      {"application/json", "{}"},
                      {Effect::read}};
  result.validated_arguments = result.arguments;
  result.observation_request = OpsObservationRequest{
      id<OpsOwnerId>("owner"),
      id<SessionId>("session"),
      id<OpsRequestId>("request"),
      {id<OpsTargetId>("target"), id<OpsConfigurationRevision>("revision"),
       LinuxOpsIdentity{LinuxExecutionScope::container,
                        "12345678-1234-1234-1234-123456789abc", 42, 43}},
      1,
      OpsObservationOperation::linux_health,
      {},
      1,
      {}};
  return result;
}
auto event(RunEventPayload value, std::uint64_t sequence = 1,
           std::uint32_t schema = 3) -> RunEvent {
  return {{id<EventId>("event-" + std::to_string(sequence)),
           id<RunId>("run"),
           sequence,
           schema,
           EventTimestamp{std::chrono::milliseconds{sequence}},
           {},
           {},
           id<InvocationId>("invocation")},
          std::move(value)};
}
} // namespace

TEST_CASE("spend projection refuses malformed or paid observation proposals",
          "[ops][spend][recovery]") {
  auto value = proposal();
  std::uint32_t schema{3};
  SECTION("schema1 smuggles proof") {
    schema = 1;
  }
  SECTION("schema2 smuggles proof") {
    schema = 2;
  }
  SECTION("unknown schema") {
    schema = 4;
  }
  SECTION("schema3 missing proof") {
    value.observation_request.reset();
  }
  SECTION("missing normalized arguments") {
    value.validated_arguments.reset();
  }
  SECTION("wrong normalized media type") {
    value.validated_arguments->media_type = "text/plain";
  }
  SECTION("empty normalized arguments") {
    value.validated_arguments->data.clear();
  }
  SECTION("oversized normalized arguments") {
    value.validated_arguments->data.assign(16385, 'x');
  }
  SECTION("unsafe normalized text") {
    value.validated_arguments->data = std::string{"x\0y", 3};
  }
  SECTION("malformed historical request") {
    value.observation_request->selection_generation = 0;
  }
  SECTION("spend effect") {
    value.declared_effects.push_back(Effect::spend);
  }
  SECTION("spend quote") {
    value.spend_quote = ToolSpendQuote{
        MonetaryAmount::create("USD", DecimalAmount::from("1").value()).value(),
        ToolSpendEstimateBasis::policy_upper_bound,
        {"sha256", std::string(64, 'a'), 1},
        EventTimestamp::max()};
  }
  SECTION("spend effect with a matching valid quote") {
    value.declared_effects.push_back(Effect::spend);
    value.spend_quote = ToolSpendQuote{
        MonetaryAmount::create("USD", DecimalAmount::from("1").value()).value(),
        ToolSpendEstimateBasis::policy_upper_bound,
        {"sha256", std::string(64, 'a'), 1},
        EventTimestamp::max()};
  }
  ToolSpendLedgerProjection ledger;
  auto rejected = ledger.apply(event(value, 1, schema));
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().code ==
          ToolSpendLedgerErrorCode::invalid_transition);
  // Refusal must not retain either the event ID or its invocation proposal.
  REQUIRE(ledger.apply(event(proposal())));
  REQUIRE(
      ledger.apply(event(ToolStarted{id<InvocationId>("invocation")}, 2, 1)));
  REQUIRE(ledger.records().empty());
}

TEST_CASE("unpaid observation lifecycle preserves spend projection neutrality",
          "[ops][spend][recovery]") {
  ToolSpendLedgerProjection ledger;
  auto value = proposal();
  SECTION("minimal normalized arguments") {
  }
  SECTION("exact normalized byte ceiling") {
    value.validated_arguments->data = "\"" + std::string(16382, 'x') + "\"";
  }
  REQUIRE(ledger.apply(event(value)));
  REQUIRE(
      ledger.apply(event(ToolStarted{id<InvocationId>("invocation")}, 2, 1)));
  REQUIRE(ledger.apply(
      event(ToolResultRecorded{id<InvocationId>("invocation"),
                               {TextBlock{"historical observation"}},
                               id<MessageId>("result")},
            3, 1)));
  REQUIRE(ledger.apply(event(RunCompleted{}, 4, 1)));
  REQUIRE(ledger.records().empty());
}
