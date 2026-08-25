#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <aiforge/domain/usage_ledger.hpp>

namespace {

using namespace aiforge::domain;

template <typename IdType> auto make_id(const std::string &value) -> IdType {
  return IdType::from(value).value();
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

auto inference(const std::string &id, const std::string &model)
    -> InferenceStarted {
  return {make_id<InferenceId>(id), make_id<ModelId>(model)};
}

auto started() -> RunStarted {
  return {make_id<SurfaceId>("surface"), make_id<WorkspaceId>("chat"),
          make_id<PermissionProfileId>("observe"), std::nullopt};
}

} // namespace

TEST_CASE("usage ledger rejects invalid event identity and order",
          "[usage][failure]") {
  UsageLedgerProjection ledger;

  auto zero_sequence = event(0, UnknownEvent{"future"}, "zero");
  REQUIRE_FALSE(ledger.apply(zero_sequence));
  REQUIRE(ledger.last_sequence() == 0);

  auto zero_schema = event(1, UnknownEvent{"future"}, "schema");
  zero_schema.metadata.schema_version = 0;
  REQUIRE_FALSE(ledger.apply(zero_schema));

  REQUIRE(ledger.apply(event(1, UnknownEvent{"future"}, "one")));
  auto duplicate = ledger.apply(event(2, UnknownEvent{"future"}, "one"));
  REQUIRE_FALSE(duplicate);
  REQUIRE(duplicate.error().code == UsageLedgerErrorCode::duplicate_event_id);

  auto regressing = ledger.apply(event(1, UnknownEvent{"future"}, "two"));
  REQUIRE_FALSE(regressing);
  REQUIRE(regressing.error().code ==
          UsageLedgerErrorCode::non_monotonic_sequence);
  REQUIRE(ledger.last_sequence() == 1);
}

TEST_CASE("usage ledger rejects unrelated and terminal inference events",
          "[usage][failure]") {
  UsageLedgerProjection ledger;
  const auto inference_id = make_id<InferenceId>("inference");

  auto orphan = ledger.apply(
      event(1, UsageRecorded{inference_id, {1, 0, 0, 0}}, "orphan"));
  REQUIRE_FALSE(orphan);
  REQUIRE(orphan.error().code == UsageLedgerErrorCode::unknown_inference);

  REQUIRE(ledger.apply(event(1, inference("inference", "model"), "start")));
  auto duplicate = ledger.apply(
      event(2, inference("inference", "other-model"), "duplicate"));
  REQUIRE_FALSE(duplicate);
  REQUIRE(duplicate.error().code == UsageLedgerErrorCode::duplicate_inference);

  auto wrong_run = ledger.apply(event(
      2, UsageRecorded{inference_id, {1, 0, 0, 0}}, "wrong-run", "other"));
  REQUIRE_FALSE(wrong_run);
  REQUIRE(wrong_run.error().code == UsageLedgerErrorCode::wrong_run);

  REQUIRE(ledger.apply(
      event(2, UsageRecorded{inference_id, {1, 2, 3, 4}}, "usage")));

  auto duplicated_usage = ledger.apply(
      event(3, UsageRecorded{inference_id, {9, 9, 9, 9}}, "usage"));
  REQUIRE_FALSE(duplicated_usage);
  REQUIRE(duplicated_usage.error().code ==
          UsageLedgerErrorCode::duplicate_event_id);
  REQUIRE(ledger.total_usage() == Usage{1, 2, 3, 4});

  auto wrong_terminal =
      ledger.apply(event(3, InferenceFinished{inference_id, FinishReason::stop},
                         "wrong-finish", "other"));
  REQUIRE_FALSE(wrong_terminal);
  REQUIRE(wrong_terminal.error().code == UsageLedgerErrorCode::wrong_run);

  REQUIRE(ledger.apply(
      event(3, InferenceFinished{inference_id, FinishReason::stop}, "finish")));

  auto after_terminal = ledger.apply(
      event(4, UsageRecorded{inference_id, {1, 0, 0, 0}}, "late-usage"));
  REQUIRE_FALSE(after_terminal);
  REQUIRE(after_terminal.error().code ==
          UsageLedgerErrorCode::invalid_transition);

  auto duplicate_terminal = ledger.apply(
      event(4, InferenceCancelled{inference_id, std::nullopt}, "late-cancel"));
  REQUIRE_FALSE(duplicate_terminal);
  REQUIRE(duplicate_terminal.error().code ==
          UsageLedgerErrorCode::invalid_transition);
  REQUIRE(ledger.last_sequence() == 3);
}

TEST_CASE("usage ledger overflow does not partially mutate records or totals",
          "[usage][failure]") {
  UsageLedgerProjection ledger;
  const auto first = make_id<InferenceId>("first");
  const auto second = make_id<InferenceId>("second");

  REQUIRE(ledger.apply(event(1, inference("first", "model"), "start-first")));
  REQUIRE(ledger.apply(
      event(2,
            UsageRecorded{first,
                          {std::numeric_limits<std::uint64_t>::max(), 1, 2, 3}},
            "usage-first")));
  REQUIRE(ledger.apply(
      event(3, InferenceFinished{first, FinishReason::stop}, "finish-first")));
  REQUIRE(ledger.apply(event(4, inference("second", "model"), "start-second")));

  const auto failed = ledger.apply(
      event(5, UsageRecorded{second, {1, 4, 5, 6}}, "usage-second"));
  REQUIRE_FALSE(failed);
  REQUIRE(failed.error().code == UsageLedgerErrorCode::usage_overflow);
  REQUIRE(ledger.last_sequence() == 4);
  REQUIRE(ledger.records()[1].usage == Usage{});
  REQUIRE(ledger.total_usage() ==
          Usage{std::numeric_limits<std::uint64_t>::max(), 1, 2, 3});
}

TEST_CASE(
    "usage ledger replays interleaved inference outcomes deterministically",
    "[usage][replay]") {
  const auto first = make_id<InferenceId>("first");
  const auto second = make_id<InferenceId>("second");
  const DomainError failure{ErrorCode::backend, "redacted", true};
  const std::vector<RunEvent> events{
      event(1, started(), "run-a-start", "run-a"),
      event(2, inference("first", "model-a"), "first-start", "run-a"),
      event(3, UsageRecorded{first, {2, 1, 1, 0}}, "first-usage-1", "run-a"),
      event(4, started(), "run-b-start", "run-b"),
      event(5, inference("second", "model-b"), "second-start", "run-b"),
      event(6, UsageRecorded{second, {4, 2, 0, 3}}, "second-usage", "run-b"),
      event(7, InferenceFailed{second, failure}, "second-failed", "run-b"),
      event(8, UnknownEvent{"future.usage.fact"}, "future", "run-a"),
      event(9, UsageRecorded{first, {3, 2, 2, 1}}, "first-usage-2", "run-a"),
      event(10, InferenceFinished{first, FinishReason::stop}, "first-finish",
            "run-a"),
  };

  UsageLedgerProjection ledger;
  UsageLedgerProjection replayed;
  for (const auto &item : events) {
    REQUIRE(ledger.apply(item));
    REQUIRE(replayed.apply(item));
  }

  REQUIRE(ledger.records() == replayed.records());
  REQUIRE(ledger.total_usage() == replayed.total_usage());
  REQUIRE(ledger.last_sequence() == 10);
  REQUIRE(ledger.records().size() == 2);
  REQUIRE(ledger.records()[0].run_id == make_id<RunId>("run-a"));
  REQUIRE(ledger.records()[0].model_id == make_id<ModelId>("model-a"));
  REQUIRE(ledger.records()[0].started_at ==
          EventTimestamp{std::chrono::milliseconds{2}});
  REQUIRE(ledger.records()[0].ended_at ==
          EventTimestamp{std::chrono::milliseconds{10}});
  REQUIRE(ledger.records()[0].status == InferenceUsageStatus::completed);
  REQUIRE(ledger.records()[0].usage == Usage{5, 3, 3, 1});
  REQUIRE(ledger.records()[1].status == InferenceUsageStatus::failed);
  REQUIRE(ledger.records()[1].usage == Usage{4, 2, 0, 3});
  REQUIRE(ledger.total_usage() == Usage{9, 5, 3, 4});
}

TEST_CASE("usage ledger preserves cancelled and active partial usage",
          "[usage][cancel]") {
  UsageLedgerProjection ledger;
  const auto cancelled = make_id<InferenceId>("cancelled");
  const auto active = make_id<InferenceId>("active");

  REQUIRE(ledger.apply(
      event(1, inference("cancelled", "model"), "cancelled-start")));
  REQUIRE(ledger.apply(
      event(2, UsageRecorded{cancelled, {3, 1, 0, 0}}, "cancelled-usage")));
  REQUIRE(ledger.apply(
      event(3, InferenceCancelled{cancelled, std::string{"user request"}},
            "cancelled-end")));
  REQUIRE(ledger.apply(event(4, inference("active", "model"), "active-start")));
  REQUIRE(ledger.apply(
      event(5, UsageRecorded{active, {2, 0, 1, 0}}, "active-usage")));

  REQUIRE(ledger.records()[0].status == InferenceUsageStatus::cancelled);
  REQUIRE(ledger.records()[0].ended_at.has_value());
  REQUIRE(ledger.records()[1].status == InferenceUsageStatus::active);
  REQUIRE_FALSE(ledger.records()[1].ended_at.has_value());
  REQUIRE(ledger.total_usage() == Usage{5, 1, 1, 0});
}
