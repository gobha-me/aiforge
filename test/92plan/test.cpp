#include <aiforge/domain/plan_projection.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace aiforge;

template <typename Id> auto id(std::string value) -> Id {
  auto parsed = Id::from(std::move(value));
  REQUIRE(parsed);
  return std::move(*parsed);
}

auto revision(std::string revision_id = "revision-1",
              std::optional<std::string> supersedes = std::nullopt)
    -> domain::PlanRevision {
  return {
      id<domain::PlanId>("plan"),
      id<domain::PlanRevisionId>(std::move(revision_id)),
      supersedes ? std::optional{id<domain::PlanRevisionId>(*supersedes)}
                 : std::nullopt,
      "Land a replayable plan graph",
      domain::RepositorySnapshotIdentity{id<domain::RepositoryId>("repository"),
                                         {"sha256", "aaaaaaaaaaaaaaaa", 0}},
      {{id<domain::PlanTaskId>("contract"),
        std::nullopt,
        {},
        "Define the contract",
        {"Invalid graphs are rejected"},
        {domain::Effect::read},
        {{domain::Effect::read, "repository_path", "include"}}},
       {id<domain::PlanTaskId>("projection"),
        id<domain::PlanTaskId>("contract"),
        {id<domain::PlanTaskId>("contract")},
        "Build the projection",
        {"Replay is deterministic"},
        {domain::Effect::write},
        {{domain::Effect::write, "repository_path", "src"}}}},
      {}};
}

auto decision(const domain::PlanRevision &value,
              const domain::PlanDecision result,
              std::optional<std::string> reason = std::nullopt)
    -> domain::PlanRevisionDecision {
  return {value.plan_id, value.revision_id, result,
          domain::PlanDecisionSource::user, std::move(reason)};
}

template <typename Payload>
auto event(const std::uint64_t sequence, Payload payload,
           std::string event_id = {}) -> domain::RunEvent {
  if (event_id.empty())
    event_id = "event-" + std::to_string(sequence);
  return {{id<domain::EventId>(std::move(event_id)),
           id<domain::RunId>("planning-run"), sequence, 1,
           domain::EventTimestamp{std::chrono::milliseconds{1000 + sequence}},
           std::nullopt, std::nullopt, std::nullopt},
          std::move(payload)};
}

} // namespace

TEST_CASE("plan validation rejects malformed and unbounded task graphs",
          "[plan][validation][failure]") {
  auto value = revision();
  REQUIRE(domain::validate_plan_revision(value));

  domain::PlanGraphLimits limits;
  limits.maximum_tasks = 0;
  auto result = domain::validate_plan_revision(value, limits);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == domain::PlanGraphErrorCode::invalid_limits);

  value = revision();
  value.tasks.front().acceptance_criteria.clear();
  result = domain::validate_plan_revision(value);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == domain::PlanGraphErrorCode::invalid_task);

  value = revision();
  value.tasks.back().task_id = value.tasks.front().task_id;
  result = domain::validate_plan_revision(value);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          domain::PlanGraphErrorCode::duplicate_identity);

  value = revision();
  value.tasks.back().dependency_task_ids = {id<domain::PlanTaskId>("missing")};
  result = domain::validate_plan_revision(value);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == domain::PlanGraphErrorCode::unknown_reference);

  value = revision();
  value.tasks.front().parent_task_id = value.tasks.back().task_id;
  result = domain::validate_plan_revision(value);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == domain::PlanGraphErrorCode::cyclic_graph);

  value = revision();
  value.tasks.front().dependency_task_ids = {value.tasks.back().task_id};
  result = domain::validate_plan_revision(value);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == domain::PlanGraphErrorCode::cyclic_graph);

  value = revision();
  value.tasks.front().resource_intents.front().effect = domain::Effect::write;
  result = domain::validate_plan_revision(value);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == domain::PlanGraphErrorCode::invalid_task);

  value = revision();
  value.evidence = {
      {id<domain::EvidenceId>("evidence"),
       {"sha256", "aaaaaaaaaaaaaaaa", 16}},
      {id<domain::EvidenceId>("evidence"),
       {"sha256", "bbbbbbbbbbbbbbbb", 16}}};
  result = domain::validate_plan_revision(value);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == domain::PlanGraphErrorCode::invalid_plan);

  value = revision();
  limits = {};
  limits.maximum_text_bytes = 64;
  limits.maximum_total_text_bytes = 64;
  result = domain::validate_plan_revision(value, limits);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          domain::PlanGraphErrorCode::resource_exhausted);
}

TEST_CASE(
    "plan projection records revision decisions and superseding revisions",
    "[plan][projection]") {
  const auto first = revision();
  auto second = revision("revision-2", "revision-1");
  second.goal = "Apply the requested revision";
  const std::vector<domain::RunEvent> events{
      event(1, domain::UnknownEvent{"future.plan.metadata"}),
      event(2, domain::PlanRevisionProposed{first}),
      event(3, domain::PlanRevisionDecisionRecorded{decision(
                   first, domain::PlanDecision::revision_requested,
                   "tighten acceptance")}),
      event(4, domain::PlanRevisionProposed{second}),
      event(5, domain::PlanRevisionDecisionRecorded{
                   decision(second, domain::PlanDecision::approved)})};

  auto projection = domain::PlanGraphProjection::rebuild(events);
  REQUIRE(projection);
  REQUIRE(projection->plan_id() == first.plan_id);
  REQUIRE(projection->revisions().size() == 2);
  REQUIRE(projection->current_revision() != nullptr);
  REQUIRE(projection->current_revision()->revision == second);
  REQUIRE(projection->state() == domain::PlanGraphState::approved);
  REQUIRE(projection->last_sequence() == 5);

  domain::PlanGraphProjection replayed;
  for (const auto &item : events)
    REQUIRE(replayed.apply(item));
  REQUIRE(replayed.revisions() == projection->revisions());
  REQUIRE(replayed.state() == projection->state());
}

TEST_CASE(
    "plan projection rejects stale decisions and malformed revision chains",
    "[plan][projection][failure]") {
  const auto first = revision();
  const auto second = revision("revision-2", "revision-1");
  domain::PlanGraphProjection projection;

  auto linked_first = first;
  linked_first.supersedes_revision_id = id<domain::PlanRevisionId>("missing");
  auto result = projection.apply(
      event(1, domain::PlanRevisionProposed{linked_first}, "bad-first"));
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          domain::PlanGraphErrorCode::invalid_transition);
  REQUIRE(projection.last_sequence() == 0);

  REQUIRE(projection.apply(event(1, domain::PlanRevisionProposed{first})));
  auto unlinked = second;
  unlinked.supersedes_revision_id = id<domain::PlanRevisionId>("other");
  result = projection.apply(
      event(2, domain::PlanRevisionProposed{unlinked}, "unlinked"));
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          domain::PlanGraphErrorCode::invalid_transition);
  REQUIRE(projection.last_sequence() == 1);

  REQUIRE(projection.apply(event(2, domain::PlanRevisionProposed{second})));
  result = projection.apply(event(3,
                                  domain::PlanRevisionDecisionRecorded{decision(
                                      first, domain::PlanDecision::approved)},
                                  "stale-decision"));
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == domain::PlanGraphErrorCode::wrong_revision);
  REQUIRE(projection.last_sequence() == 2);

  REQUIRE(
      projection.apply(event(3, domain::PlanRevisionDecisionRecorded{decision(
                                    second, domain::PlanDecision::approved)})));
  result = projection.apply(event(4,
                                  domain::PlanRevisionDecisionRecorded{decision(
                                      second, domain::PlanDecision::rejected)},
                                  "duplicate-decision"));
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          domain::PlanGraphErrorCode::invalid_transition);
  REQUIRE(projection.last_sequence() == 3);

  auto regressing = event(3, domain::UnknownEvent{"future"}, "regressing");
  result = projection.apply(regressing);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          domain::PlanGraphErrorCode::non_monotonic_sequence);

  auto duplicate = event(5, domain::UnknownEvent{"future"}, "event-3");
  result = projection.apply(duplicate);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == domain::PlanGraphErrorCode::duplicate_event);
}

TEST_CASE("plan decisions reject unknown enum values and bounded reasons",
          "[plan][decision][failure]") {
  auto value = revision();
  auto recorded = decision(value, domain::PlanDecision::approved);
  REQUIRE(domain::validate_plan_decision(recorded));

  recorded.decision = static_cast<domain::PlanDecision>(999);
  auto result = domain::validate_plan_decision(recorded);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == domain::PlanGraphErrorCode::invalid_decision);

  recorded = decision(value, domain::PlanDecision::rejected,
                      std::string(16U * 1024U + 1U, 'x'));
  result = domain::validate_plan_decision(recorded);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == domain::PlanGraphErrorCode::invalid_decision);

  domain::PlanRevisionInvalidation invalidation{
      value.plan_id, value.revision_id, {}};
  result = domain::validate_plan_invalidation(invalidation);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          domain::PlanGraphErrorCode::invalid_transition);
}
