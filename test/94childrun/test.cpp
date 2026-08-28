#include <aiforge/repository/review_receipt.hpp>
#include <aiforge/runtime/run_kernel.hpp>
#include <aiforge/testing/scripted_backend.hpp>
#include <aiforge/testing/scripted_child_runner.hpp>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
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

auto snapshot(std::string fingerprint = "aaaaaaaaaaaaaaaa")
    -> domain::RepositorySnapshotIdentity {
  return {id<domain::RepositoryId>("repository"),
          {"sha256", std::move(fingerprint), 64}};
}

auto attributes() -> domain::RunStarted {
  return {id<domain::SurfaceId>("test"), id<domain::WorkspaceId>("code"),
          id<domain::PermissionProfileId>("observe"), std::nullopt};
}

auto task() -> domain::PlanTask {
  return {id<domain::PlanTaskId>("task"),
          std::nullopt,
          {},
          "Execute one bounded child task",
          {"The child result is replayable"},
          {domain::Effect::read},
          {{domain::Effect::read, "repository_path", "src"}}};
}

auto revision() -> domain::PlanRevision {
  return {id<domain::PlanId>("plan"),
          id<domain::PlanRevisionId>("revision"),
          std::nullopt,
          "Execute accepted work through a child run",
          snapshot(),
          {task()},
          {{id<domain::EvidenceId>("approval-evidence"),
            {"sha256", "bbbbbbbbbbbbbbbb", 16}}}};
}

auto revision_with_dependency() -> domain::PlanRevision {
  auto value = revision();
  auto dependent = task();
  dependent.task_id = id<domain::PlanTaskId>("dependent-task");
  dependent.dependency_task_ids = {id<domain::PlanTaskId>("task")};
  dependent.title = "Run after the first task";
  value.tasks.push_back(std::move(dependent));
  return value;
}

auto environment() -> runtime::PlanApprovalEnvironment {
  return {snapshot(),
          {{id<domain::EvidenceId>("approval-evidence"),
            {"sha256", "bbbbbbbbbbbbbbbb", 16}}}};
}

auto parcel() -> domain::ContextParcel {
  const domain::RepositorySourceIdentity source{
      snapshot(),
      "src/main.cpp",
      {"sha256", "cccccccccccccccc", 13},
      domain::SourceByteRange{0, 13}};
  return {id<domain::ContextParcelId>("parcel"),
          "execute the accepted task",
          domain::TaskPhase::editing,
          snapshot(),
          {{id<domain::EvidenceId>("context-evidence"),
            domain::ExactSourceEvidence{source},
            domain::EvidenceFreshness::current,
            {domain::EvidenceDerivation::observed,
             "filesystem",
             "1",
             domain::EventTimestamp{100ms},
             snapshot(),
             {},
             {},
             std::nullopt},
            {domain::TextBlock{"int main() {}"}},
            13,
            4}}};
}

auto budget() -> domain::ChildRunBudget {
  return {2, 3, 100, 50, 5s};
}

auto scope() -> domain::CapabilityScope {
  return {domain::Effect::read, "filesystem.root", "/config/Projects/aiforge"};
}

auto child_start() -> runtime::ChildRunStart {
  return {id<domain::RunId>("child-run"),
          id<domain::RunId>("planning-run"),
          attributes(),
          id<domain::PlanId>("plan"),
          id<domain::PlanRevisionId>("revision"),
          id<domain::PlanTaskId>("task"),
          parcel(),
          budget(),
          {domain::Effect::read},
          {scope()},
          {domain::Effect::read},
          {scope()},
          1,
          std::nullopt};
}

auto descriptor() -> domain::ChildRunDescriptor {
  return {id<domain::RunId>("planning-run"),
          id<domain::PlanId>("plan"),
          id<domain::PlanRevisionId>("revision"),
          id<domain::PlanTaskId>("task"),
          {id<domain::ContextParcelId>("parcel"),
           snapshot(),
           {id<domain::EvidenceId>("context-evidence")},
           13,
           4},
          budget(),
          {domain::Effect::read},
          {scope()},
          1,
          std::nullopt};
}

auto invocation() -> runtime::ChildRunInvocation {
  return {id<domain::RunId>("child-run"), descriptor(), task(), parcel()};
}

auto success() -> runtime::ChildRunResult {
  return {domain::SessionTaskOutcome::completed,
          {1, 2, {20, 10, 0, 0}},
          {id<domain::EvidenceId>("result-evidence")},
          {id<domain::ArtifactId>("result-artifact")},
          std::nullopt,
          std::nullopt};
}

auto review_participant(std::string actor_name = "reviewer")
    -> domain::ReviewParticipantProvenance {
  return {{std::move(actor_name), "Repository Reviewer"},
          id<domain::RunId>("reviewer-run"),
          std::string{"fake-backend"},
          std::string{"1"},
          id<domain::ModelId>("review-model"),
          std::string{"2026-08-28"}};
}

auto review_draft() -> domain::ReviewReceiptDraft {
  return {id<domain::ReviewReceiptId>("review-receipt"),
          {snapshot(), "candidate-revision"},
          {{id<domain::ReviewRequirementId>("tests"),
            domain::ReviewEvidenceKind::verification,
            "ctest",
            "3.28",
            id<domain::VerificationEvidenceId>("verification"),
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            {"sha256", "dddddddddddddddd", 64},
            {}}},
          review_participant("author")};
}

auto review_parcel() -> domain::ContextParcel {
  auto exact = parcel().items.front();
  exact.evidence_id = id<domain::EvidenceId>("exact-review-source");
  const auto artifact = id<domain::ArtifactId>("diff-artifact");
  domain::ContextParcelItem diff{
      id<domain::EvidenceId>("review-diff"),
      domain::DiffEvidence{snapshot("eeeeeeeeeeeeeeee"), snapshot(), artifact},
      domain::EvidenceFreshness::current,
      {domain::EvidenceDerivation::observed,
       "git-diff",
       "1",
       domain::EventTimestamp{100ms},
       snapshot(),
       {},
       {},
       std::nullopt},
      {domain::ArtifactReferenceBlock{artifact, std::nullopt}},
      64,
      8};
  return {id<domain::ContextParcelId>("review-parcel"),
          "review the exact candidate",
          domain::TaskPhase::review,
          snapshot(),
          {std::move(exact), std::move(diff)}};
}

auto review_start() -> runtime::ChildRunStart {
  auto value = child_start();
  value.context = review_parcel();
  value.review = runtime::ChildRunStart::ReviewRequest{
      review_draft(), review_draft().author->actor};
  return value;
}

auto review_invocation() -> runtime::ChildRunInvocation {
  auto value = invocation();
  value.context = review_parcel();
  value.descriptor.context = {value.context.parcel_id,
                              value.context.target_snapshot,
                              {id<domain::EvidenceId>("exact-review-source"),
                               id<domain::EvidenceId>("review-diff")},
                              77,
                              12};
  value.descriptor.review_receipt_id = review_draft().receipt_id;
  return value;
}

auto approved_review_result() -> runtime::ChildRunResult {
  auto value = success();
  value.review = domain::ReviewChildResult{review_draft().receipt_id,
                                           review_draft().candidate,
                                           review_participant(),
                                           {},
                                           domain::ReviewVerdict::approved};
  return value;
}

auto approve(runtime::RunKernel& kernel,
             const domain::PlanRevision& value = revision()) -> void {
  REQUIRE(kernel.start_plan(
      {id<domain::RunId>("planning-run"), attributes(), value}));
  REQUIRE(kernel.decide_plan(
              id<domain::RunId>("planning-run"),
              {value.plan_id, value.revision_id, domain::PlanDecision::approved,
               domain::PlanDecisionSource::user, std::nullopt},
              environment()) == runtime::PlanDecisionOutcome::recorded);
}

auto drain_until_terminal(runtime::RunKernel& kernel) -> void {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (kernel.active_run_id() &&
         std::chrono::steady_clock::now() < deadline) {
    REQUIRE(kernel.drain());
    std::this_thread::sleep_for(1ms);
  }
  REQUIRE_FALSE(kernel.active_run_id());
  REQUIRE(kernel.drain());
}

class MemorySessionStore final : public storage::SessionStore {
 public:
  auto create_session(storage::SessionCreate session, std::stop_token)
      -> std::expected<void, storage::SessionStoreError> override {
    m_session = session;
    return {};
  }
  auto open_session(const domain::SessionId& session_id, std::stop_token)
      -> std::expected<storage::SessionInfo,
                       storage::SessionStoreError> override {
    if (!m_session || m_session->session_id != session_id)
      return std::unexpected(error());
    return storage::SessionInfo{
        session_id, m_session->created_at,
        m_events.empty() ? m_session->created_at
                         : m_events.back().metadata.timestamp,
        m_events.empty() ? 0 : m_events.back().metadata.sequence, 2};
  }
  auto list_sessions(std::size_t, std::stop_token)
      -> std::expected<std::vector<storage::SessionInfo>,
                       storage::SessionStoreError> override {
    return std::vector<storage::SessionInfo>{};
  }
  auto append_events(const domain::SessionId&,
                     std::span<const domain::RunEvent> events, std::stop_token)
      -> std::expected<void, storage::SessionStoreError> override {
    m_events.insert(m_events.end(), events.begin(), events.end());
    return {};
  }
  auto replay_events(const domain::SessionId&, std::stop_token)
      -> std::expected<std::vector<domain::RunEvent>,
                       storage::SessionStoreError> override {
    return m_events;
  }

 private:
  [[nodiscard]] static auto error() -> storage::SessionStoreError {
    return {storage::SessionStoreErrorCode::not_found, "not found", false};
  }
  std::optional<storage::SessionCreate> m_session;
  std::vector<domain::RunEvent> m_events;
};

} // namespace

TEST_CASE("child-run contracts reject invalid limits and results",
          "[child-run][contract][failure]") {
  auto value = descriptor();
  REQUIRE(domain::validate_child_run_descriptor(value));

  value.budget.maximum_inferences = 0;
  REQUIRE_FALSE(domain::validate_child_run_descriptor(value));

  value = descriptor();
  value.context.evidence_ids.clear();
  REQUIRE_FALSE(domain::validate_child_run_descriptor(value));

  value = descriptor();
  value.context.represented_bytes = 256U * 1024U * 1024U + 1;
  REQUIRE_FALSE(domain::validate_child_run_descriptor(value));

  value = descriptor();
  value.capability_scopes.front().effect = domain::Effect::write;
  REQUIRE_FALSE(domain::validate_child_run_descriptor(value));

  domain::SessionTaskResult result{value.plan_id,
                                   value.revision_id,
                                   value.task_id,
                                   id<domain::RunId>("child-run"),
                                   domain::SessionTaskOutcome::completed,
                                   {3, 0, {}},
                                   {},
                                   {},
                                   std::nullopt};
  REQUIRE_FALSE(domain::validate_session_task_result(result, budget()));
  result.consumption.inference_count = 1;
  result.error =
      domain::DomainError{domain::ErrorCode::unavailable, "unexpected", false};
  REQUIRE_FALSE(domain::validate_session_task_result(result, budget()));
}

TEST_CASE("dispatch fails before execution for unavailable or widened children",
          "[child-run][dispatch][failure]") {
  testing::ScriptedBackend backend{{}};
  runtime::RunKernel unavailable{id<domain::SessionId>("session"), backend};
  approve(unavailable);
  auto result = unavailable.dispatch_child(child_start());
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          runtime::RunKernelErrorCode::child_runner_unavailable);
  REQUIRE(unavailable.active_session_tasks().front().state ==
          runtime::SessionTaskState::pending);

  auto runner = std::make_shared<testing::ScriptedChildRunner>(
      std::vector<testing::ScriptedChildRunExchange>{});
  runtime::RunKernel widened{id<domain::SessionId>("session-2"),
                             backend,
                             nullptr,
                             {},
                             {},
                             {},
                             {},
                             runner};
  approve(widened);
  auto request = child_start();
  request.requested_effects = {domain::Effect::write};
  request.requested_scopes = {
      {domain::Effect::write, "filesystem.root", "/config/Projects/aiforge"}};
  result = widened.dispatch_child(std::move(request));
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          runtime::RunKernelErrorCode::policy_scope_widening);
  REQUIRE(runner->recorded_invocations().empty());
}

TEST_CASE("review child dispatch requires bounded read-only review evidence",
          "[child-run][review][failure]") {
  testing::ScriptedBackend backend{{}};
  auto runner = std::make_shared<testing::ScriptedChildRunner>(
      std::vector<testing::ScriptedChildRunExchange>{});
  runtime::RunKernel kernel{id<domain::SessionId>("review-failure-session"),
                            backend,
                            nullptr,
                            {},
                            {},
                            {},
                            {},
                            runner};
  approve(kernel);

  auto request = review_start();
  request.context.phase = domain::TaskPhase::editing;
  REQUIRE_FALSE(kernel.dispatch_child(request));
  request = review_start();
  request.context.items.pop_back();
  REQUIRE_FALSE(kernel.dispatch_child(request));
  request = review_start();
  request.requested_effects = {domain::Effect::write};
  REQUIRE_FALSE(kernel.dispatch_child(request));
  REQUIRE(runner->recorded_invocations().empty());
}

TEST_CASE("review child validates structured findings against its receipt",
          "[child-run][review][contract][failure]") {
  auto result = domain::ReviewChildResult{
      review_draft().receipt_id,
      review_draft().candidate,
      review_participant(),
      {{id<domain::ReviewFindingId>("finding"),
        "The candidate has a reproducible defect",
        id<domain::VerificationEvidenceId>("verification"),
        {id<domain::ArtifactId>("result-artifact")},
        domain::ReviewFindingSeverity::high,
        std::nullopt,
        {}}},
      domain::ReviewVerdict::changes_requested};
  REQUIRE(repository::validate_review_child_result(
      result, review_draft(), {},
      std::vector{id<domain::ArtifactId>("result-artifact")}));
  result.verdict = domain::ReviewVerdict::approved;
  REQUIRE_FALSE(repository::validate_review_child_result(
      result, review_draft(), {},
      std::vector{id<domain::ArtifactId>("result-artifact")}));
  result.verdict = domain::ReviewVerdict::changes_requested;
  result.findings.front().verification_evidence_id =
      id<domain::VerificationEvidenceId>("unknown");
  REQUIRE_FALSE(repository::validate_review_child_result(
      result, review_draft(), {},
      std::vector{id<domain::ArtifactId>("result-artifact")}));
}

TEST_CASE("review child records one replayable receipt and verdict",
          "[child-run][review][runtime]") {
  auto runner = std::make_shared<testing::ScriptedChildRunner>(
      std::vector{testing::ScriptedChildRunExchange{
          review_invocation(),
          testing::ChildRunStreamScript{
              {approved_review_result(), testing::ChildRunEndOfStream{}}}}});
  testing::ScriptedBackend backend{{}};
  runtime::RunKernel kernel{id<domain::SessionId>("review-session"),
                            backend,
                            nullptr,
                            {},
                            {},
                            {},
                            {},
                            runner};
  approve(kernel);
  REQUIRE(kernel.dispatch_child(review_start()));
  drain_until_terminal(kernel);
  REQUIRE(kernel.active_session_tasks().front().state ==
          runtime::SessionTaskState::completed);

  const auto& events = kernel.event_log().events();
  REQUIRE(std::ranges::count_if(events, [](const auto& event) {
            return std::holds_alternative<domain::ReviewReceiptDrafted>(
                event.payload);
          }) == 1);
  REQUIRE(std::ranges::count_if(events, [](const auto& event) {
            return std::holds_alternative<domain::ReviewVerdictRecorded>(
                event.payload);
          }) == 1);
  const auto created = std::ranges::find_if(events, [](const auto& event) {
    return std::holds_alternative<domain::ChildRunCreated>(event.payload);
  });
  REQUIRE(created != events.end());
  REQUIRE(created->metadata.schema_version == 4);

  const auto projection = repository::ReviewReceiptProjection::rebuild(events);
  REQUIRE(projection);
  REQUIRE(projection->state() == repository::ReviewReceiptState::approved);
  REQUIRE(projection->verdicts().front().reviewer_provenance ==
          review_participant());
}

TEST_CASE("malformed review output records no partial receipt facts",
          "[child-run][review][protocol][failure]") {
  auto malformed = approved_review_result();
  malformed.review->findings.push_back(
      {id<domain::ReviewFindingId>("finding"),
       "An approved result cannot contain findings",
       id<domain::VerificationEvidenceId>("verification"),
       {id<domain::ArtifactId>("result-artifact")},
       domain::ReviewFindingSeverity::high,
       std::nullopt,
       {}});
  auto runner = std::make_shared<testing::ScriptedChildRunner>(
      std::vector{testing::ScriptedChildRunExchange{
          review_invocation(),
          testing::ChildRunStreamScript{
              {malformed, testing::ChildRunEndOfStream{}}}}});
  testing::ScriptedBackend backend{{}};
  runtime::RunKernel kernel{id<domain::SessionId>("malformed-review-session"),
                            backend,
                            nullptr,
                            {},
                            {},
                            {},
                            {},
                            runner};
  approve(kernel);
  REQUIRE(kernel.dispatch_child(review_start()));
  drain_until_terminal(kernel);
  REQUIRE(kernel.active_session_tasks().front().state ==
          runtime::SessionTaskState::failed);

  const auto& events = kernel.event_log().events();
  REQUIRE(std::ranges::none_of(events, [](const auto& event) {
    return std::holds_alternative<domain::ReviewFindingOpened>(event.payload) ||
           std::holds_alternative<domain::ReviewVerdictRecorded>(event.payload);
  }));
}

TEST_CASE("review retry reuses one receipt",
          "[child-run][review][retry][runtime]") {
  auto retry_invocation = review_invocation();
  retry_invocation.child_run_id = id<domain::RunId>("reviewer-retry");
  retry_invocation.descriptor.attempt = 2;
  auto runner = std::make_shared<testing::ScriptedChildRunner>(std::vector{
      testing::ScriptedChildRunExchange{
          review_invocation(),
          runtime::ChildRunError{runtime::ChildRunErrorCode::unavailable,
                                 "worker unavailable", true}},
      testing::ScriptedChildRunExchange{
          retry_invocation,
          testing::ChildRunStreamScript{
              {approved_review_result(), testing::ChildRunEndOfStream{}}}}});
  testing::ScriptedBackend backend{{}};
  runtime::RunKernel kernel{id<domain::SessionId>("review-retry-session"),
                            backend,
                            nullptr,
                            {},
                            {256, 8U * 1024U * 1024U, {1, 2}},
                            {},
                            {},
                            runner};
  approve(kernel);
  REQUIRE(kernel.dispatch_child(review_start()));
  drain_until_terminal(kernel);
  REQUIRE(kernel.active_session_tasks().front().state ==
          runtime::SessionTaskState::unavailable);

  auto retry = review_start();
  retry.child_run_id = id<domain::RunId>("reviewer-retry");
  retry.attempt = 2;
  REQUIRE(kernel.dispatch_child(std::move(retry)));
  drain_until_terminal(kernel);
  REQUIRE(kernel.active_session_tasks().front().state ==
          runtime::SessionTaskState::completed);

  const auto& events = kernel.event_log().events();
  REQUIRE(std::ranges::count_if(events, [](const auto& event) {
            return std::holds_alternative<domain::ReviewReceiptDrafted>(
                event.payload);
          }) == 1);
  REQUIRE(std::ranges::count_if(events, [](const auto& event) {
            return std::holds_alternative<domain::ReviewRequested>(
                event.payload);
          }) == 1);
  REQUIRE(std::ranges::count_if(events, [](const auto& event) {
            return std::holds_alternative<domain::ReviewVerdictRecorded>(
                event.payload);
          }) == 1);
}

TEST_CASE("dispatch rejects unknown parents, tasks, and missing context",
          "[child-run][dispatch][failure]") {
  testing::ScriptedBackend backend{{}};
  auto runner = std::make_shared<testing::ScriptedChildRunner>(
      std::vector<testing::ScriptedChildRunExchange>{});
  runtime::RunKernel kernel{id<domain::SessionId>("session"),
                            backend,
                            nullptr,
                            {},
                            {},
                            {},
                            {},
                            runner};
  approve(kernel);

  auto request = child_start();
  request.parent_run_id = id<domain::RunId>("failed-parent");
  auto result = kernel.dispatch_child(request);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          runtime::RunKernelErrorCode::invalid_child_state);

  request = child_start();
  request.task_id = id<domain::PlanTaskId>("unknown-task");
  result = kernel.dispatch_child(request);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          runtime::RunKernelErrorCode::invalid_child_state);

  request = child_start();
  request.context.items.clear();
  result = kernel.dispatch_child(request);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          runtime::RunKernelErrorCode::invalid_child_state);
  REQUIRE(runner->recorded_invocations().empty());
}

TEST_CASE("dispatch rejects a non-ready task and a failed plan parent",
          "[child-run][dispatch][parent][failure]") {
  testing::ScriptedBackend backend{{}};
  auto runner = std::make_shared<testing::ScriptedChildRunner>(
      std::vector<testing::ScriptedChildRunExchange>{});

  runtime::RunKernel dependencies{id<domain::SessionId>("dependencies"),
                                  backend,
                                  nullptr,
                                  {},
                                  {},
                                  {},
                                  {},
                                  runner};
  approve(dependencies, revision_with_dependency());
  auto request = child_start();
  request.task_id = id<domain::PlanTaskId>("dependent-task");
  auto result = dependencies.dispatch_child(request);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          runtime::RunKernelErrorCode::invalid_child_state);

  runtime::RunKernel seed{id<domain::SessionId>("seed"), backend};
  approve(seed);
  auto failed_events = seed.event_log().events();
  REQUIRE(std::holds_alternative<domain::RunCompleted>(
      failed_events.back().payload));
  failed_events.back().payload = domain::RunFailed{
      {domain::ErrorCode::unavailable, "planning failed", false}};

  MemorySessionStore store;
  const auto session_id = id<domain::SessionId>("failed-parent-session");
  REQUIRE(store.create_session({session_id, domain::EventTimestamp{1ms}}, {}));
  REQUIRE(store.append_events(session_id, failed_events, {}));
  auto replayed = runtime::RunKernel::open_durable(
      {session_id, runtime::DurableSessionMode::resume,
       domain::EventTimestamp{1ms}},
      store, backend, nullptr, {}, {}, {}, {}, runner);
  REQUIRE(replayed);
  result = (*replayed)->dispatch_child(child_start());
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          runtime::RunKernelErrorCode::invalid_child_state);
  REQUIRE(runner->recorded_invocations().empty());
}

TEST_CASE("accepted task executes once and records a bounded terminal result",
          "[child-run][runtime]") {
  auto runner = std::make_shared<testing::ScriptedChildRunner>(
      std::vector{testing::ScriptedChildRunExchange{
          invocation(), testing::ChildRunStreamScript{
                            {success(), testing::ChildRunEndOfStream{}}}}});
  testing::ScriptedBackend backend{{}};
  runtime::RunKernel kernel{id<domain::SessionId>("session"),
                            backend,
                            nullptr,
                            {},
                            {},
                            {},
                            {},
                            runner};
  approve(kernel);
  REQUIRE(kernel.dispatch_child(child_start()));
  REQUIRE(kernel.active_session_tasks().front().state ==
          runtime::SessionTaskState::dispatched);
  drain_until_terminal(kernel);

  const auto projected = kernel.active_session_tasks().front();
  REQUIRE(projected.state == runtime::SessionTaskState::completed);
  REQUIRE(projected.child_run_id == id<domain::RunId>("child-run"));
  REQUIRE(projected.result);
  REQUIRE(projected.result->evidence_ids ==
          std::vector{id<domain::EvidenceId>("result-evidence")});
  REQUIRE(projected.result->artifact_ids ==
          std::vector{id<domain::ArtifactId>("result-artifact")});
  REQUIRE(runner->recorded_invocations() == std::vector{invocation()});

  const auto& events = kernel.event_log().events();
  const auto created = std::ranges::find_if(events, [](const auto& event) {
    return std::holds_alternative<domain::ChildRunCreated>(event.payload);
  });
  REQUIRE(created != events.end());
  REQUIRE(created->metadata.schema_version == 3);
  REQUIRE(created->metadata.parent_run_id == id<domain::RunId>("planning-run"));
}

TEST_CASE("duplicate terminal results fail closed as one task result",
          "[child-run][protocol][failure]") {
  auto runner = std::make_shared<testing::ScriptedChildRunner>(
      std::vector{testing::ScriptedChildRunExchange{
          invocation(),
          testing::ChildRunStreamScript{
              {success(), success(), testing::ChildRunEndOfStream{}}}}});
  testing::ScriptedBackend backend{{}};
  runtime::RunKernel kernel{id<domain::SessionId>("session"),
                            backend,
                            nullptr,
                            {},
                            {},
                            {},
                            {},
                            runner};
  approve(kernel);
  REQUIRE(kernel.dispatch_child(child_start()));
  drain_until_terminal(kernel);
  const auto projected = kernel.active_session_tasks().front();
  REQUIRE(projected.state == runtime::SessionTaskState::failed);
  REQUIRE(projected.result);
  REQUIRE(projected.result->error->message == "child runner protocol failure");
}

TEST_CASE("malformed and over-budget terminal results fail closed",
          "[child-run][budget][protocol][failure]") {
  SECTION("a completed result cannot carry an error") {
    auto malformed = success();
    malformed.error =
        domain::DomainError{domain::ErrorCode::backend, "unexpected", false};
    auto runner = std::make_shared<testing::ScriptedChildRunner>(
        std::vector{testing::ScriptedChildRunExchange{
            invocation(), testing::ChildRunStreamScript{
                              {malformed, testing::ChildRunEndOfStream{}}}}});
    testing::ScriptedBackend backend{{}};
    runtime::RunKernel kernel{id<domain::SessionId>("malformed-session"),
                              backend,
                              nullptr,
                              {},
                              {},
                              {},
                              {},
                              runner};
    approve(kernel);
    REQUIRE(kernel.dispatch_child(child_start()));
    drain_until_terminal(kernel);
    REQUIRE(kernel.active_session_tasks().front().state ==
            runtime::SessionTaskState::failed);
  }

  SECTION("observed usage cannot exceed the dispatch budget") {
    auto excessive = success();
    excessive.consumption.usage.input_tokens =
        budget().maximum_input_tokens + 1;
    auto runner = std::make_shared<testing::ScriptedChildRunner>(
        std::vector{testing::ScriptedChildRunExchange{
            invocation(), testing::ChildRunStreamScript{
                              {excessive, testing::ChildRunEndOfStream{}}}}});
    testing::ScriptedBackend backend{{}};
    runtime::RunKernel kernel{id<domain::SessionId>("budget-session"),
                              backend,
                              nullptr,
                              {},
                              {},
                              {},
                              {},
                              runner};
    approve(kernel);
    REQUIRE(kernel.dispatch_child(child_start()));
    drain_until_terminal(kernel);
    const auto projected = kernel.active_session_tasks().front();
    REQUIRE(projected.state == runtime::SessionTaskState::budget_exhausted);
    REQUIRE(projected.result);
    REQUIRE(projected.result->consumption == domain::ChildRunConsumption{});
  }

  SECTION("child result errors are redacted before persistence") {
    auto failed = success();
    failed.outcome = domain::SessionTaskOutcome::failed;
    failed.error = domain::DomainError{domain::ErrorCode::backend,
                                       "provider-secret-value", false};
    auto runner = std::make_shared<testing::ScriptedChildRunner>(
        std::vector{testing::ScriptedChildRunExchange{
            invocation(), testing::ChildRunStreamScript{
                              {failed, testing::ChildRunEndOfStream{}}}}});
    testing::ScriptedBackend backend{{}};
    runtime::RunKernel kernel{id<domain::SessionId>("redaction-session"),
                              backend,
                              nullptr,
                              {},
                              {},
                              {},
                              {},
                              runner};
    approve(kernel);
    REQUIRE(kernel.dispatch_child(child_start()));
    drain_until_terminal(kernel);
    const auto projected = kernel.active_session_tasks().front();
    REQUIRE(projected.state == runtime::SessionTaskState::failed);
    REQUIRE(projected.result);
    REQUIRE(projected.result->error->message == "child run failed");
  }
}

TEST_CASE("cancellation wins a race with a late child result",
          "[child-run][cancel][race][failure]") {
  auto runner = std::make_shared<testing::ScriptedChildRunner>(
      std::vector{testing::ScriptedChildRunExchange{
          invocation(), testing::ChildRunStreamScript{
                            {testing::ChildRunResultAfterStop{success()},
                             testing::ChildRunEndOfStream{}}}}});
  testing::ScriptedBackend backend{{}};
  runtime::RunKernel kernel{id<domain::SessionId>("session"),
                            backend,
                            nullptr,
                            {},
                            {},
                            {},
                            {},
                            runner};
  approve(kernel);
  REQUIRE(kernel.dispatch_child(child_start()));
  REQUIRE(kernel.cancel_run(id<domain::RunId>("child-run"), "user request"));
  drain_until_terminal(kernel);
  const auto projected = kernel.active_session_tasks().front();
  REQUIRE(projected.state == runtime::SessionTaskState::cancelled);
  REQUIRE(projected.result);
  REQUIRE(projected.result->outcome == domain::SessionTaskOutcome::cancelled);
}

TEST_CASE("deadline expiry records one result and ignores a late success",
          "[child-run][deadline][race][failure]") {
  auto expected = invocation();
  expected.descriptor.budget.timeout = 5ms;
  auto runner = std::make_shared<testing::ScriptedChildRunner>(
      std::vector{testing::ScriptedChildRunExchange{
          expected, testing::ChildRunStreamScript{
                        {testing::ChildRunResultAfterStop{success()},
                         testing::ChildRunEndOfStream{}}}}});
  testing::ScriptedBackend backend{{}};
  runtime::RunKernel kernel{id<domain::SessionId>("session"),
                            backend,
                            nullptr,
                            {},
                            {},
                            {},
                            {},
                            runner};
  approve(kernel);
  auto request = child_start();
  request.budget.timeout = 5ms;
  REQUIRE(kernel.dispatch_child(std::move(request)));
  drain_until_terminal(kernel);
  const auto projected = kernel.active_session_tasks().front();
  REQUIRE(projected.state == runtime::SessionTaskState::timed_out);
  REQUIRE(
      std::ranges::count_if(kernel.event_log().events(), [](const auto& event) {
        return std::holds_alternative<domain::SessionTaskResultRecorded>(
            event.payload);
      }) == 1);
}

TEST_CASE("durable replay rebuilds child results without redispatch",
          "[child-run][replay]") {
  MemorySessionStore store;
  testing::ScriptedBackend backend{{}};
  auto runner = std::make_shared<testing::ScriptedChildRunner>(
      std::vector{testing::ScriptedChildRunExchange{
          invocation(), testing::ChildRunStreamScript{
                            {success(), testing::ChildRunEndOfStream{}}}}});
  {
    auto kernel = runtime::RunKernel::open_durable(
        {id<domain::SessionId>("session"), runtime::DurableSessionMode::create,
         domain::EventTimestamp{1ms}},
        store, backend, nullptr, {}, {}, {}, {}, runner);
    REQUIRE(kernel);
    approve(**kernel);
    REQUIRE((*kernel)->dispatch_child(child_start()));
    drain_until_terminal(**kernel);
  }
  const auto calls = runner->recorded_invocations().size();
  auto replay_runner = std::make_shared<testing::ScriptedChildRunner>(
      std::vector<testing::ScriptedChildRunExchange>{});
  auto replayed = runtime::RunKernel::open_durable(
      {id<domain::SessionId>("session"), runtime::DurableSessionMode::resume,
       domain::EventTimestamp{1ms}},
      store, backend, nullptr, {}, {}, {}, {}, replay_runner);
  REQUIRE(replayed);
  REQUIRE((*replayed)->active_session_tasks().front().state ==
          runtime::SessionTaskState::completed);
  REQUIRE(replay_runner->recorded_invocations().empty());
  REQUIRE(calls == 1);
}

TEST_CASE("resume preserves an interrupted dispatch without redispatch",
          "[child-run][replay][failure]") {
  MemorySessionStore store;
  testing::ScriptedBackend backend{{}};
  auto runner = std::make_shared<testing::ScriptedChildRunner>(
      std::vector{testing::ScriptedChildRunExchange{
          invocation(),
          testing::ChildRunStreamScript{{testing::ChildRunWaitForStop{}}}}});
  {
    auto kernel = runtime::RunKernel::open_durable(
        {id<domain::SessionId>("session"), runtime::DurableSessionMode::create,
         domain::EventTimestamp{1ms}},
        store, backend, nullptr, {}, {}, {}, {}, runner);
    REQUIRE(kernel);
    approve(**kernel);
    REQUIRE((*kernel)->dispatch_child(child_start()));
  }

  auto replay_runner = std::make_shared<testing::ScriptedChildRunner>(
      std::vector<testing::ScriptedChildRunExchange>{});
  auto replayed = runtime::RunKernel::open_durable(
      {id<domain::SessionId>("session"), runtime::DurableSessionMode::resume,
       domain::EventTimestamp{1ms}},
      store, backend, nullptr, {}, {}, {}, {}, replay_runner);
  REQUIRE(replayed);
  const auto projected = (*replayed)->active_session_tasks().front();
  REQUIRE(projected.state == runtime::SessionTaskState::dispatched);
  REQUIRE_FALSE((*replayed)->active_run_id());
  REQUIRE(replay_runner->recorded_invocations().empty());
}
