#include <aiforge/domain/project_backlog_projection.hpp>
#include <aiforge/runtime/plan_task_controller.hpp>
#include <aiforge/testing/scripted_backend.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace aiforge;

template <typename Id>
auto id(std::string value) -> Id {
  return Id::from(std::move(value)).value();
}

auto attributes() -> domain::RunStarted {
  return {id<domain::SurfaceId>("test"), id<domain::WorkspaceId>("workspace"),
          id<domain::PermissionProfileId>("observe"), std::nullopt};
}

auto task(std::string task_id = "task") -> domain::PlanTask {
  return {id<domain::PlanTaskId>(std::move(task_id)),
          std::nullopt,
          {},
          "Implement durable plan task controls",
          {"Invalid transitions fail closed"},
          {domain::Effect::write},
          {{domain::Effect::write, "repository_path", "src"}}};
}

auto revision() -> domain::PlanRevision {
  auto first = task("contract");
  first.intended_effects = {domain::Effect::read};
  first.resource_intents = {
      {domain::Effect::read, "repository_path", "include"}};
  auto second = task("runtime");
  second.dependency_task_ids = {first.task_id};
  return {
      id<domain::PlanId>("plan"),
      id<domain::PlanRevisionId>("revision"),
      std::nullopt,
      "Implement durable plan task controls",
      domain::RepositorySnapshotIdentity{id<domain::RepositoryId>("repository"),
                                         {"sha256", "aaaaaaaaaaaaaaaa", 64}},
      {std::move(first), std::move(second)},
      {}};
}

auto backlog_item(std::string item_id = "backlog-item")
    -> domain::ProjectBacklogItem {
  auto value = task();
  return {id<domain::ProjectBacklogItemId>(std::move(item_id)),
          id<domain::RepositoryId>("repository"),
          {id<domain::SessionId>("session"), id<domain::PlanId>("plan"),
           id<domain::PlanRevisionId>("revision"), value.task_id},
          std::move(value),
          domain::ProjectBacklogDecisionSource::user};
}

template <typename Payload>
auto event(const std::uint64_t sequence, Payload payload,
           std::string event_id = {}) -> domain::RunEvent {
  if (event_id.empty()) event_id = "event-" + std::to_string(sequence);
  return {{id<domain::EventId>(std::move(event_id)),
           id<domain::RunId>("control-run"), sequence, 1,
           domain::EventTimestamp{std::chrono::milliseconds{sequence}},
           std::nullopt, std::nullopt, std::nullopt},
          std::move(payload)};
}

auto approve(runtime::RunKernel& kernel, const domain::PlanRevision& value)
    -> void {
  const auto run_id = id<domain::RunId>("planning-run");
  REQUIRE(kernel.start_plan({run_id, attributes(), value}));
  REQUIRE(kernel.decide_plan(run_id,
                             {value.plan_id, value.revision_id,
                              domain::PlanDecision::approved,
                              domain::PlanDecisionSource::user, std::nullopt},
                             {value.source_snapshot, {}}));
}

} // namespace

TEST_CASE("project backlog validation rejects malformed items and status",
          "[plan][tasks][failure]") {
  auto item = backlog_item();
  REQUIRE(domain::validate_project_backlog_item(item));

  item.origin.task_id = id<domain::PlanTaskId>("other");
  auto result = domain::validate_project_backlog_item(item);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == domain::ProjectBacklogErrorCode::invalid_item);

  item = backlog_item();
  item.task.dependency_task_ids = {item.task.task_id};
  result = domain::validate_project_backlog_item(item);
  REQUIRE_FALSE(result);

  domain::ProjectBacklogStatusChange status{
      item.item_id,
      item.repository_id,
      domain::ProjectBacklogItemStatus::resolved,
      domain::ProjectBacklogDecisionSource::user,
      std::nullopt,
      id<domain::EventId>("promotion")};
  auto changed = domain::validate_project_backlog_status_change(status);
  REQUIRE_FALSE(changed);
  REQUIRE(changed.error().code ==
          domain::ProjectBacklogErrorCode::invalid_item);

  status.reason = std::string(16U * 1024U + 1U, 'x');
  changed = domain::validate_project_backlog_status_change(status);
  REQUIRE_FALSE(changed);
}

TEST_CASE("project backlog replay is repository scoped and compare-and-set",
          "[plan][tasks][projection][failure]") {
  const auto item = backlog_item();
  domain::ProjectBacklogProjection projection{item.repository_id};
  REQUIRE(projection.apply(
      item.origin.session_id,
      event(1, domain::UnknownEvent{"future.project_task"}, "future")));
  REQUIRE(projection.apply(
      item.origin.session_id,
      event(2, domain::ProjectBacklogItemPromoted{item}, "promotion")));
  REQUIRE(projection.items().size() == 1);
  REQUIRE(projection.items().front().status_event_id ==
          id<domain::EventId>("promotion"));

  const auto duplicate = projection.apply(
      item.origin.session_id,
      event(3, domain::ProjectBacklogItemPromoted{item}, "duplicate"));
  REQUIRE_FALSE(duplicate);
  REQUIRE(duplicate.error().code ==
          domain::ProjectBacklogErrorCode::duplicate_identity);

  domain::ProjectBacklogStatusChange change{
      item.item_id,
      item.repository_id,
      domain::ProjectBacklogItemStatus::resolved,
      domain::ProjectBacklogDecisionSource::user,
      "completed elsewhere",
      id<domain::EventId>("stale")};
  auto changed = projection.apply(
      item.origin.session_id,
      event(3, domain::ProjectBacklogItemStatusChanged{change}, "status"));
  REQUIRE_FALSE(changed);
  REQUIRE(changed.error().code ==
          domain::ProjectBacklogErrorCode::stale_status);
  REQUIRE(projection.items().front().status ==
          domain::ProjectBacklogItemStatus::open);

  change.expected_status_event_id = id<domain::EventId>("promotion");
  changed = projection.apply(
      item.origin.session_id,
      event(3, domain::ProjectBacklogItemStatusChanged{change}, "status"));
  REQUIRE(changed);
  REQUIRE(projection.items().front().status ==
          domain::ProjectBacklogItemStatus::resolved);

  auto other = backlog_item("other-item");
  other.repository_id = id<domain::RepositoryId>("other-repository");
  REQUIRE(projection.apply(
      other.origin.session_id,
      event(4, domain::ProjectBacklogItemPromoted{other}, "other-promotion")));
  REQUIRE(projection.items().size() == 1);
}

TEST_CASE("runtime promotes exact unresolved tasks with atomic status facts",
          "[plan][tasks][runtime]") {
  testing::ScriptedBackend backend{{}};
  runtime::RunKernel kernel{id<domain::SessionId>("session"), backend};
  const auto value = revision();
  approve(kernel, value);

  auto item = backlog_item();
  item.task = value.tasks.back();
  item.origin.task_id = item.task.task_id;
  REQUIRE(kernel.promote_project_task(
      {id<domain::RunId>("promotion-run"), attributes(), item}));
  const auto after_promotion = kernel.event_log().events().size();
  REQUIRE(after_promotion == 8);
  auto projected = kernel.project_backlog(item.repository_id);
  REQUIRE(projected);
  REQUIRE(projected->items().size() == 1);
  const auto status_event_id = projected->items().front().status_event_id;

  auto duplicate = item;
  duplicate.item_id = id<domain::ProjectBacklogItemId>("duplicate-item");
  REQUIRE_FALSE(kernel.promote_project_task(
      {id<domain::RunId>("duplicate-run"), attributes(), duplicate}));
  REQUIRE(kernel.event_log().events().size() == after_promotion);

  domain::ProjectBacklogStatusChange status{
      item.item_id,
      item.repository_id,
      domain::ProjectBacklogItemStatus::resolved,
      domain::ProjectBacklogDecisionSource::user,
      "implemented",
      id<domain::EventId>("stale")};
  REQUIRE_FALSE(kernel.update_project_task_status(
      {id<domain::RunId>("stale-run"), attributes(), status}));
  REQUIRE(kernel.event_log().events().size() == after_promotion);

  status.expected_status_event_id = status_event_id;
  REQUIRE(kernel.update_project_task_status(
      {id<domain::RunId>("status-run"), attributes(), status}));
  REQUIRE(kernel.event_log().events().size() == after_promotion + 3);
  projected = kernel.project_backlog(item.repository_id);
  REQUIRE(projected->items().front().status ==
          domain::ProjectBacklogItemStatus::resolved);
  REQUIRE(projected->items().front().status_reason == "implemented");
}

TEST_CASE("plan-task controller schedules proposals and requires revise reason",
          "[plan][tasks][controller]") {
  testing::ScriptedBackend backend{{}};
  runtime::RunKernel kernel{id<domain::SessionId>("session"), backend};
  runtime::PlanTaskController controller{kernel, nullptr};
  const auto value = revision();
  REQUIRE(kernel.start_plan(
      {id<domain::RunId>("planning-run"), attributes(), value}));

  const auto state = controller.inspect();
  REQUIRE(state);
  REQUIRE(state->pending_decision);
  REQUIRE(state->schedule);
  REQUIRE(state->schedule->tasks.size() == value.tasks.size());

  const auto rejected =
      controller.decide(id<domain::RunId>("planning-run"),
                        {value.plan_id, value.revision_id,
                         domain::PlanDecision::revision_requested,
                         domain::PlanDecisionSource::user, std::nullopt});
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().code ==
          runtime::PlanTaskControllerErrorCode::invalid_request);
  REQUIRE(kernel.pending_plan_decision());
}
