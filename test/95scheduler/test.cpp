#include <aiforge/domain/task_scheduler.hpp>
#include <aiforge/runtime/run_kernel.hpp>
#include <aiforge/testing/scripted_backend.hpp>
#include <aiforge/testing/scripted_child_runner.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace aiforge;
using namespace std::chrono_literals;

template <typename Id> auto id(std::string value) -> Id {
  return Id::from(std::move(value)).value();
}

auto snapshot() -> domain::RepositorySnapshotIdentity {
  return {id<domain::RepositoryId>("repository"),
          {"sha256", "aaaaaaaaaaaaaaaa", 64}};
}

auto attributes() -> domain::RunStarted {
  return {id<domain::SurfaceId>("test"), id<domain::WorkspaceId>("code"),
          id<domain::PermissionProfileId>("ask"), std::nullopt};
}

auto plan_task(std::string name, domain::Effect effect,
               std::string kind = "repository_path", std::string value = "src")
    -> domain::PlanTask {
  const auto task_id = id<domain::PlanTaskId>(std::move(name));
  return {task_id,
          std::nullopt,
          {},
          std::string{"Execute "} + std::string{task_id.value()},
          {"The task produces a bounded result"},
          {effect},
          {{effect, std::move(kind), std::move(value)}}};
}

auto descriptor(const domain::PlanTaskId& task_id, std::string child_id,
                const std::uint32_t attempt = 1) -> domain::ChildRunDescriptor {
  return {id<domain::RunId>("planning-run"),
          id<domain::PlanId>("plan"),
          id<domain::PlanRevisionId>("revision"),
          task_id,
          {id<domain::ContextParcelId>("parcel-" + child_id),
           snapshot(),
           {id<domain::EvidenceId>("context-" + child_id)},
           7,
           2},
          {2, 2, 100, 50, 5s},
          {domain::Effect::write},
          {{domain::Effect::write, "filesystem.root", "/workspace"}},
          attempt};
}

auto task_result(const domain::PlanTaskId& task_id, std::string child_id,
                 const domain::SessionTaskOutcome outcome,
                 const bool retryable = false,
                 domain::ChildRunConsumption consumption = {})
    -> domain::SessionTaskResult {
  return {
      id<domain::PlanId>("plan"),
      id<domain::PlanRevisionId>("revision"),
      task_id,
      id<domain::RunId>(std::move(child_id)),
      outcome,
      consumption,
      {id<domain::EvidenceId>("evidence")},
      {id<domain::ArtifactId>("artifact")},
      outcome == domain::SessionTaskOutcome::completed
          ? std::nullopt
          : std::optional{domain::DomainError{domain::ErrorCode::unavailable,
                                              "bounded failure", retryable}}};
}

auto execution(
    const domain::PlanTask& task,
    std::vector<domain::ProjectedPlanRevision::TaskExecution::Attempt>
        attempts = {}) -> domain::ProjectedPlanRevision::TaskExecution {
  return {task.task_id, std::move(attempts)};
}

auto attempt(const domain::PlanTask& task, std::string child_id,
             std::optional<domain::SessionTaskResult> result = std::nullopt,
             const std::uint32_t number = 1)
    -> domain::ProjectedPlanRevision::TaskExecution::Attempt {
  const auto run_id = id<domain::RunId>(child_id);
  return {run_id, descriptor(task.task_id, std::move(child_id), number),
          std::move(result)};
}

auto projected(
    std::vector<domain::PlanTask> tasks,
    std::vector<domain::ProjectedPlanRevision::TaskExecution> executions)
    -> domain::ProjectedPlanRevision {
  return {{id<domain::PlanId>("plan"),
           id<domain::PlanRevisionId>("revision"),
           std::nullopt,
           "Schedule accepted work",
           snapshot(),
           std::move(tasks),
           {}},
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          {},
          std::nullopt,
          std::move(executions)};
}

auto parcel(std::string name) -> domain::ContextParcel {
  const domain::RepositorySourceIdentity source{
      snapshot(),
      "src/" + name + ".cpp",
      {"sha256", "cccccccccccccccc", 7},
      domain::SourceByteRange{0, 7}};
  return {id<domain::ContextParcelId>("parcel-" + name),
          "execute " + name,
          domain::TaskPhase::editing,
          snapshot(),
          {{id<domain::EvidenceId>("context-" + name),
            domain::ExactSourceEvidence{source},
            domain::EvidenceFreshness::current,
            {domain::EvidenceDerivation::observed,
             "filesystem",
             "1",
             domain::EventTimestamp{1ms},
             snapshot(),
             {},
             {},
             std::nullopt},
            {domain::TextBlock{"int x;\n"}},
            7,
            2}}};
}

auto runtime_revision() -> domain::PlanRevision {
  auto first =
      plan_task("first", domain::Effect::write, "repository_path", "src/core");
  auto second =
      plan_task("second", domain::Effect::write, "repository_path", "docs");
  auto overlap = plan_task("overlap", domain::Effect::write, "repository_path",
                           "src/core/file.cpp");
  return {id<domain::PlanId>("plan"),
          id<domain::PlanRevisionId>("revision"),
          std::nullopt,
          "Run independent tasks concurrently",
          snapshot(),
          {std::move(first), std::move(second), std::move(overlap)},
          {}};
}

auto child_start(std::string task_name, std::string child_name,
                 const std::uint32_t attempt_number = 1)
    -> runtime::ChildRunStart {
  return {id<domain::RunId>(child_name),
          id<domain::RunId>("planning-run"),
          attributes(),
          id<domain::PlanId>("plan"),
          id<domain::PlanRevisionId>("revision"),
          id<domain::PlanTaskId>(task_name),
          parcel(child_name),
          {2, 2, 100, 50, 5s},
          {domain::Effect::write},
          {{domain::Effect::write, "filesystem.root", "/workspace"}},
          {domain::Effect::write},
          {{domain::Effect::write, "filesystem.root", "/workspace"}},
          attempt_number};
}

auto invocation(std::string task_name, std::string child_name,
                std::string resource, const std::uint32_t attempt_number = 1)
    -> runtime::ChildRunInvocation {
  auto task = plan_task(std::move(task_name), domain::Effect::write,
                        "repository_path", std::move(resource));
  const auto task_id = task.task_id;
  return {id<domain::RunId>(child_name),
          descriptor(task_id, child_name, attempt_number), std::move(task),
          parcel(std::move(child_name))};
}

auto success() -> runtime::ChildRunResult {
  return {domain::SessionTaskOutcome::completed,
          {1, 1, {10, 5, 0, 0}},
          {id<domain::EvidenceId>("result")},
          {id<domain::ArtifactId>("result")},
          std::nullopt};
}

auto approve(runtime::RunKernel& kernel, const domain::PlanRevision& revision)
    -> void {
  REQUIRE(kernel.start_plan(
      {id<domain::RunId>("planning-run"), attributes(), revision}));
  REQUIRE(kernel.decide_plan(id<domain::RunId>("planning-run"),
                             {revision.plan_id, revision.revision_id,
                              domain::PlanDecision::approved,
                              domain::PlanDecisionSource::user, std::nullopt},
                             {snapshot(), {}}) ==
          runtime::PlanDecisionOutcome::recorded);
}

auto drain_children(runtime::RunKernel& kernel) -> void {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!kernel.active_child_run_ids().empty() &&
         std::chrono::steady_clock::now() < deadline) {
    REQUIRE(kernel.drain());
    std::this_thread::sleep_for(1ms);
  }
  REQUIRE(kernel.active_child_run_ids().empty());
  REQUIRE(kernel.drain());
}

} // namespace

TEST_CASE("resource intents fail closed when independence is ambiguous",
          "[scheduler][resources][failure]") {
  const auto read_left =
      plan_task("read-left", domain::Effect::read, "repository_path", "src");
  const auto read_right = plan_task("read-right", domain::Effect::read,
                                    "repository_path", "src/file.cpp");
  REQUIRE_FALSE(domain::task_resource_conflict(read_left, read_right));

  const auto writer =
      plan_task("writer", domain::Effect::write, "repository_path", "src");
  REQUIRE(domain::task_resource_conflict(writer, read_right));
  REQUIRE_FALSE(domain::task_resource_conflict(
      writer,
      plan_task("docs", domain::Effect::write, "repository_path", "docs")));
  REQUIRE(domain::task_resource_conflict(
      writer,
      plan_task("unknown", domain::Effect::write, "future_kind", "src")));
  auto missing = writer;
  missing.resource_intents.clear();
  REQUIRE(domain::task_resource_conflict(missing, read_right));
}

TEST_CASE("scheduler policy rejects zero and unbounded fan-out",
          "[scheduler][policy][failure]") {
  const auto task = plan_task("task", domain::Effect::read);
  const auto value = projected({task}, {execution(task)});
  for (const auto policy : std::vector<domain::TaskSchedulingPolicy>{
           {0, 1}, {17, 1}, {1, 0}, {1, 9}}) {
    const auto result = domain::analyze_task_schedule(value, policy);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code ==
            domain::TaskSchedulingErrorCode::invalid_policy);
  }
}

TEST_CASE("scheduler rejects malformed attempt history",
          "[scheduler][projection][failure]") {
  const auto task = plan_task("task", domain::Effect::write);
  auto wrong_number = attempt(task, "child-1", std::nullopt, 2);
  auto analysis = domain::analyze_task_schedule(
      projected({task}, {execution(task, {wrong_number})}), {1, 2});
  REQUIRE_FALSE(analysis);
  REQUIRE(analysis.error().code ==
          domain::TaskSchedulingErrorCode::invalid_projection);

  auto permanent_failure = task_result(
      task.task_id, "child-1", domain::SessionTaskOutcome::failed, false);
  auto first = attempt(task, "child-1", permanent_failure);
  auto second = attempt(task, "child-2", std::nullopt, 2);
  analysis = domain::analyze_task_schedule(
      projected({task}, {execution(task, {first, second})}), {1, 2});
  REQUIRE_FALSE(analysis);
  REQUIRE(analysis.error().code ==
          domain::TaskSchedulingErrorCode::invalid_projection);
}

TEST_CASE("readiness uses stable dependency and conflict ordering",
          "[scheduler][readiness]") {
  auto first =
      plan_task("first", domain::Effect::write, "repository_path", "src/core");
  auto second =
      plan_task("second", domain::Effect::write, "repository_path", "docs");
  auto overlap = plan_task("overlap", domain::Effect::write, "repository_path",
                           "src/core/file.cpp");
  auto analysis = domain::analyze_task_schedule(
      projected({first, second, overlap},
                {execution(first), execution(second), execution(overlap)}),
      {2, 1});
  REQUIRE(analysis);
  REQUIRE(analysis->dispatchable_task_ids ==
          std::vector{first.task_id, second.task_id});
  REQUIRE(analysis->tasks.back().state ==
          domain::TaskReadinessState::blocked_by_resource);

  second.dependency_task_ids = {first.task_id};
  analysis = domain::analyze_task_schedule(
      projected({first, second}, {execution(first), execution(second)}),
      {2, 1});
  REQUIRE(analysis);
  REQUIRE(analysis->dispatchable_task_ids == std::vector{first.task_id});
  REQUIRE(analysis->tasks.back().state ==
          domain::TaskReadinessState::waiting_for_dependencies);
}

TEST_CASE("only retryable unavailability receives a bounded next attempt",
          "[scheduler][retry][failure]") {
  const auto task = plan_task("task", domain::Effect::write);
  auto failed = task_result(task.task_id, "child-1",
                            domain::SessionTaskOutcome::unavailable, true);
  auto analysis = domain::analyze_task_schedule(
      projected({task}, {execution(task, {attempt(task, "child-1", failed)})}),
      {1, 2});
  REQUIRE(analysis);
  REQUIRE(analysis->dispatchable_task_ids == std::vector{task.task_id});
  REQUIRE(analysis->tasks.front().next_attempt == 2);

  analysis = domain::analyze_task_schedule(
      projected({task}, {execution(task, {attempt(task, "child-1", failed)})}),
      {1, 1});
  REQUIRE(analysis);
  REQUIRE(analysis->dispatchable_task_ids.empty());
  REQUIRE(analysis->tasks.front().state == domain::TaskReadinessState::failed);
  REQUIRE(analysis->rollup.state == domain::TaskRollupState::failed);

  failed.error->retryable = false;
  analysis = domain::analyze_task_schedule(
      projected({task}, {execution(task, {attempt(task, "child-1", failed)})}),
      {1, 2});
  REQUIRE(analysis);
  REQUIRE(analysis->dispatchable_task_ids.empty());
  REQUIRE(analysis->tasks.front().state == domain::TaskReadinessState::failed);
}

TEST_CASE("rollup is checked and deduplicated in plan order",
          "[scheduler][rollup]") {
  const auto first = plan_task("first", domain::Effect::write);
  const auto second =
      plan_task("second", domain::Effect::write, "repository_path", "docs");
  const domain::ChildRunConsumption consumption{1, 2, {10, 5, 3, 1}};
  auto first_result =
      task_result(first.task_id, "child-1",
                  domain::SessionTaskOutcome::completed, false, consumption);
  auto second_result =
      task_result(second.task_id, "child-2",
                  domain::SessionTaskOutcome::completed, false, consumption);
  auto analysis = domain::analyze_task_schedule(
      projected(
          {first, second},
          {execution(first, {attempt(first, "child-1", first_result)}),
           execution(second, {attempt(second, "child-2", second_result)})}),
      {2, 1});
  REQUIRE(analysis);
  REQUIRE(analysis->rollup.state == domain::TaskRollupState::completed);
  REQUIRE(analysis->rollup.consumption ==
          domain::ChildRunConsumption{2, 4, {20, 10, 6, 2}});
  REQUIRE(analysis->rollup.evidence_ids.size() == 1);
  REQUIRE(analysis->rollup.artifact_ids.size() == 1);

  first_result.consumption.usage.cached_input_tokens =
      std::numeric_limits<std::uint64_t>::max();
  second_result.consumption.usage.cached_input_tokens = 1;
  analysis = domain::analyze_task_schedule(
      projected(
          {first, second},
          {execution(first, {attempt(first, "child-1", first_result)}),
           execution(second, {attempt(second, "child-2", second_result)})}),
      {2, 1});
  REQUIRE_FALSE(analysis);
  REQUIRE(analysis.error().code ==
          domain::TaskSchedulingErrorCode::resource_exhausted);
}

TEST_CASE("kernel dispatches only independent children within capacity",
          "[scheduler][runtime][concurrency]") {
  auto runner = std::make_shared<testing::ScriptedChildRunner>(
      std::vector{
          testing::ScriptedChildRunExchange{
              invocation("first", "child-first", "src/core"),
              testing::ChildRunStreamScript{{testing::ChildRunWaitForStop{}}}},
          testing::ScriptedChildRunExchange{
              invocation("second", "child-second", "docs"),
              testing::ChildRunStreamScript{{testing::ChildRunWaitForStop{}}}}},
      2);
  testing::ScriptedBackend backend{{}};
  runtime::RunKernel kernel{
      id<domain::SessionId>("session"),  backend, nullptr, {},
      {256, 8U * 1024U * 1024U, {2, 1}}, {},      {},      runner};
  const auto revision = runtime_revision();
  approve(kernel, revision);
  REQUIRE(kernel.dispatch_child(child_start("first", "child-first")));

  const auto conflict =
      kernel.dispatch_child(child_start("overlap", "child-overlap"));
  REQUIRE_FALSE(conflict);
  REQUIRE(conflict.error().code ==
          runtime::RunKernelErrorCode::invalid_child_state);

  auto drifted = child_start("second", "child-drifted");
  drifted.context.target_snapshot.fingerprint.value = "dddddddddddddddd";
  const auto drift = kernel.dispatch_child(std::move(drifted));
  REQUIRE_FALSE(drift);
  REQUIRE(drift.error().code ==
          runtime::RunKernelErrorCode::invalid_child_state);

  REQUIRE(kernel.dispatch_child(child_start("second", "child-second")));
  REQUIRE(kernel.active_child_run_ids() ==
          std::vector{id<domain::RunId>("child-first"),
                      id<domain::RunId>("child-second")});
  REQUIRE_FALSE(kernel.active_run_id());
  REQUIRE(kernel.cancel_run(id<domain::RunId>("child-first"), "test"));
  REQUIRE(kernel.cancel_run(id<domain::RunId>("child-second"), "test"));
  drain_children(kernel);
}

TEST_CASE("kernel defaults to deterministic serial dispatch",
          "[scheduler][runtime][capacity]") {
  auto runner = std::make_shared<testing::ScriptedChildRunner>(
      std::vector{testing::ScriptedChildRunExchange{
          invocation("first", "child-first", "src/core"),
          testing::ChildRunStreamScript{{testing::ChildRunWaitForStop{}}}}},
      2);
  testing::ScriptedBackend backend{{}};
  runtime::RunKernel kernel{id<domain::SessionId>("serial-session"), backend,
                            nullptr, {}, {}, {}, {}, runner};
  const auto revision = runtime_revision();
  approve(kernel, revision);
  REQUIRE(kernel.dispatch_child(child_start("first", "child-first")));
  const auto second =
      kernel.dispatch_child(child_start("second", "child-second"));
  REQUIRE_FALSE(second);
  REQUIRE(second.error().code ==
          runtime::RunKernelErrorCode::run_already_active);
  REQUIRE(kernel.cancel_run(id<domain::RunId>("child-first"), "test"));
  drain_children(kernel);
}

TEST_CASE("kernel records contiguous retry attempts without redispatch",
          "[scheduler][runtime][retry]") {
  auto first_invocation = invocation("first", "child-first", "src/core");
  auto retry_invocation = invocation("first", "child-retry", "src/core", 2);
  auto runner = std::make_shared<testing::ScriptedChildRunner>(
      std::vector{
          testing::ScriptedChildRunExchange{
              first_invocation,
              runtime::ChildRunError{runtime::ChildRunErrorCode::unavailable,
                                     "worker unavailable", true}},
          testing::ScriptedChildRunExchange{
              retry_invocation,
              testing::ChildRunStreamScript{
                  {success(), testing::ChildRunEndOfStream{}}}}},
      1);
  testing::ScriptedBackend backend{{}};
  runtime::RunKernel kernel{
      id<domain::SessionId>("retry-session"), backend, nullptr, {},
      {256, 8U * 1024U * 1024U, {1, 2}},      {},      {},      runner};
  auto revision = runtime_revision();
  revision.tasks.erase(revision.tasks.begin() + 1, revision.tasks.end());
  approve(kernel, revision);
  REQUIRE(kernel.dispatch_child(child_start("first", "child-first")));
  drain_children(kernel);
  REQUIRE(kernel.active_session_tasks().front().state ==
          runtime::SessionTaskState::unavailable);

  const auto duplicate =
      kernel.dispatch_child(child_start("first", "child-duplicate"));
  REQUIRE_FALSE(duplicate);
  REQUIRE(duplicate.error().code ==
          runtime::RunKernelErrorCode::invalid_child_state);

  const auto skipped =
      kernel.dispatch_child(child_start("first", "child-skipped", 3));
  REQUIRE_FALSE(skipped);
  REQUIRE(skipped.error().code ==
          runtime::RunKernelErrorCode::invalid_child_state);

  REQUIRE(kernel.dispatch_child(child_start("first", "child-retry", 2)));
  drain_children(kernel);
  REQUIRE(kernel.active_session_tasks().front().state ==
          runtime::SessionTaskState::completed);
  const auto* plan = kernel.plan_projection(id<domain::PlanId>("plan"));
  REQUIRE(plan != nullptr);
  REQUIRE(
      plan->task_execution(id<domain::PlanTaskId>("first"))->attempts.size() ==
      2);
}
