#include <aiforge/runtime/run_kernel.hpp>
#include <aiforge/testing/scripted_backend.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace aiforge;

template <typename IdType> auto id(std::string value) -> IdType {
  return IdType::from(std::move(value)).value();
}

auto attributes() -> domain::RunStarted {
  return {id<domain::SurfaceId>("test"), id<domain::WorkspaceId>("code"),
          id<domain::PermissionProfileId>("observe"), std::nullopt};
}

auto revision(std::string revision_id = "revision-1",
              std::optional<std::string> supersedes = std::nullopt)
    -> domain::PlanRevision {
  return {
      id<domain::PlanId>("plan"),
      id<domain::PlanRevisionId>(std::move(revision_id)),
      supersedes ? std::optional{id<domain::PlanRevisionId>(*supersedes)}
                 : std::nullopt,
      "Implement the approved runtime plan",
      domain::RepositorySnapshotIdentity{id<domain::RepositoryId>("repository"),
                                         {"sha256", "aaaaaaaaaaaaaaaa", 64}},
      {{id<domain::PlanTaskId>("contract"),
        std::nullopt,
        {},
        "Define the contract",
        {"Invalid transitions fail closed"},
        {domain::Effect::read},
        {{domain::Effect::read, "repository_path", "include"}}},
       {id<domain::PlanTaskId>("runtime"),
        std::nullopt,
        {id<domain::PlanTaskId>("contract")},
        "Build the runtime",
        {"Replay materializes exactly the accepted tasks"},
        {domain::Effect::write},
        {{domain::Effect::write, "repository_path", "src"}}}},
      {{id<domain::EvidenceId>("evidence"),
        {"sha256", "bbbbbbbbbbbbbbbb", 32}}}};
}

auto environment() -> runtime::PlanApprovalEnvironment {
  return {
      domain::RepositorySnapshotIdentity{id<domain::RepositoryId>("repository"),
                                         {"sha256", "aaaaaaaaaaaaaaaa", 64}},
      {{id<domain::EvidenceId>("evidence"),
        {"sha256", "bbbbbbbbbbbbbbbb", 32}}}};
}

auto decision(const domain::PlanRevision& value,
              const domain::PlanDecision result)
    -> domain::PlanRevisionDecision {
  return {value.plan_id, value.revision_id, result,
          domain::PlanDecisionSource::user, std::nullopt};
}

template <typename Payload>
auto event(const std::uint64_t sequence, Payload payload,
           std::string event_id = {}) -> domain::RunEvent {
  if (event_id.empty()) event_id = "event-" + std::to_string(sequence);
  return {{id<domain::EventId>(std::move(event_id)),
           id<domain::RunId>("planning-run"), sequence, 1,
           domain::EventTimestamp{std::chrono::milliseconds{sequence}},
           std::nullopt, std::nullopt, std::nullopt},
          std::move(payload)};
}

class MemorySessionStore final : public storage::SessionStore {
 public:
  auto create_session(storage::SessionCreate session, std::stop_token)
      -> std::expected<void, storage::SessionStoreError> override {
    if (m_session_id) return std::unexpected(error("session already exists"));
    m_session_id = session.session_id;
    m_created_at = session.created_at;
    return {};
  }

  auto open_session(const domain::SessionId& session_id, std::stop_token)
      -> std::expected<storage::SessionInfo,
                       storage::SessionStoreError> override {
    if (!m_session_id || *m_session_id != session_id) {
      return std::unexpected(error("session was not found",
                                   storage::SessionStoreErrorCode::not_found));
    }
    return storage::SessionInfo{
        session_id, m_created_at,
        m_events.empty() ? m_created_at : m_events.back().metadata.timestamp,
        m_events.empty() ? 0 : m_events.back().metadata.sequence, run_count()};
  }

  auto list_sessions(std::size_t, std::stop_token)
      -> std::expected<std::vector<storage::SessionInfo>,
                       storage::SessionStoreError> override {
    if (!m_session_id) return std::vector<storage::SessionInfo>{};
    return std::vector<storage::SessionInfo>{*open_session(*m_session_id, {})};
  }

  auto append_events(const domain::SessionId& session_id,
                     std::span<const domain::RunEvent> events, std::stop_token)
      -> std::expected<void, storage::SessionStoreError> override {
    if (m_fail_next_append) {
      m_fail_next_append = false;
      return std::unexpected(error("scripted append failure",
                                   storage::SessionStoreErrorCode::io_failure));
    }
    if (!m_session_id || *m_session_id != session_id || events.empty()) {
      return std::unexpected(error("append is invalid"));
    }
    m_events.insert(m_events.end(), events.begin(), events.end());
    return {};
  }

  auto replay_events(const domain::SessionId& session_id, std::stop_token)
      -> std::expected<std::vector<domain::RunEvent>,
                       storage::SessionStoreError> override {
    if (!m_session_id || *m_session_id != session_id) {
      return std::unexpected(error("session was not found",
                                   storage::SessionStoreErrorCode::not_found));
    }
    return m_events;
  }

  auto fail_next_append() -> void { m_fail_next_append = true; }
  [[nodiscard]] auto events() const noexcept
      -> const std::vector<domain::RunEvent>& {
    return m_events;
  }

 private:
  [[nodiscard]] static auto error(
      std::string message, const storage::SessionStoreErrorCode code =
                               storage::SessionStoreErrorCode::invalid_argument)
      -> storage::SessionStoreError {
    return {code, std::move(message), false};
  }

  [[nodiscard]] auto run_count() const -> std::uint64_t {
    std::uint64_t result{};
    for (const auto& item : m_events) {
      if (std::holds_alternative<domain::RunStarted>(item.payload)) ++result;
    }
    return result;
  }

  std::optional<domain::SessionId> m_session_id;
  domain::EventTimestamp m_created_at{};
  std::vector<domain::RunEvent> m_events;
  bool m_fail_next_append{};
};

auto open_session(MemorySessionStore& store, testing::ScriptedBackend& backend,
                  const runtime::DurableSessionMode mode)
    -> std::unique_ptr<runtime::RunKernel> {
  auto opened = runtime::RunKernel::open_durable(
      {id<domain::SessionId>("session"), mode,
       domain::EventTimestamp{std::chrono::milliseconds{1}}},
      store, backend, nullptr,
      [] { return domain::EventTimestamp{std::chrono::milliseconds{10}}; });
  REQUIRE(opened);
  return std::move(*opened);
}

} // namespace

TEST_CASE("plan projection rejects duplicate materialization and skips futures",
          "[plan][task][failure]") {
  const auto value = revision();
  domain::PlanGraphProjection projection;
  REQUIRE(projection.apply(event(1, domain::PlanRevisionProposed{value})));
  REQUIRE(
      projection.apply(event(2, domain::PlanRevisionDecisionRecorded{decision(
                                    value, domain::PlanDecision::approved)})));
  REQUIRE(projection.apply(event(
      3, domain::SessionTasksMaterialized{value.plan_id, value.revision_id})));
  REQUIRE(projection.active_tasks().size() == value.tasks.size());
  REQUIRE(projection.apply(
      event(4, domain::UnknownEvent{"future.session_task",
                                    {"application/json", "{}"}})));

  const auto duplicate = projection.apply(event(
      5, domain::SessionTasksMaterialized{value.plan_id, value.revision_id}));
  REQUIRE_FALSE(duplicate);
  REQUIRE(duplicate.error().code ==
          domain::PlanGraphErrorCode::invalid_transition);
  REQUIRE(projection.active_tasks().size() == value.tasks.size());
}

TEST_CASE("approval materializes exact tasks and later drift invalidates them",
          "[plan][runtime]") {
  testing::ScriptedBackend backend{{}};
  runtime::RunKernel kernel{id<domain::SessionId>("session"), backend};
  const auto value = revision();
  const auto planning_run = id<domain::RunId>("planning-run");

  REQUIRE_FALSE(kernel.decide_plan(
      planning_run, decision(value, domain::PlanDecision::approved),
      environment()));
  REQUIRE(kernel.start_plan({planning_run, attributes(), value}));
  REQUIRE(kernel.pending_plan_decision() ==
          runtime::PendingPlanDecision{planning_run, value.plan_id,
                                       value.revision_id});
  REQUIRE(kernel.projection(planning_run)->status() ==
          domain::RunStatus::awaiting_plan_decision);
  const auto cancellation = kernel.cancel_run(planning_run);
  REQUIRE_FALSE(cancellation);
  REQUIRE(cancellation.error().code ==
          runtime::RunKernelErrorCode::invalid_plan_state);
  REQUIRE(kernel.event_log().events().size() == 2);

  const auto approved = kernel.decide_plan(
      planning_run, decision(value, domain::PlanDecision::approved),
      environment());
  REQUIRE(approved == runtime::PlanDecisionOutcome::recorded);
  REQUIRE_FALSE(kernel.active_run_id());
  const auto active_tasks = kernel.active_session_tasks();
  REQUIRE(active_tasks.size() == value.tasks.size());
  for (std::size_t index = 0; index < value.tasks.size(); ++index) {
    REQUIRE(active_tasks[index] ==
            runtime::ActiveSessionTask{value.plan_id, value.revision_id,
                                       value.tasks[index],
                                       runtime::SessionTaskState::pending,
                                       std::nullopt, std::nullopt});
  }
  REQUIRE(kernel.event_log().events().size() == 5);

  const auto duplicate = kernel.decide_plan(
      planning_run, decision(value, domain::PlanDecision::approved),
      environment());
  REQUIRE(duplicate == runtime::PlanDecisionOutcome::already_recorded);
  REQUIRE(kernel.event_log().events().size() == 5);

  REQUIRE(kernel.revalidate_plan_approval({id<domain::RunId>("unused-run"),
                                           attributes(), value.plan_id,
                                           value.revision_id, environment()}) ==
          runtime::PlanRevalidationOutcome::current);
  REQUIRE(kernel.event_log().events().size() == 5);

  auto stale = environment();
  stale.source_snapshot->fingerprint.value = "cccccccccccccccc";
  stale.evidence.front().digest.value = "dddddddddddddddd";
  REQUIRE(kernel.revalidate_plan_approval(
              {id<domain::RunId>("revalidation-run"), attributes(),
               value.plan_id, value.revision_id, stale}) ==
          runtime::PlanRevalidationOutcome::invalidated);
  REQUIRE(kernel.active_session_tasks().empty());
  const auto* projected = kernel.plan_projection(value.plan_id);
  REQUIRE(projected != nullptr);
  REQUIRE(projected->state() == domain::PlanGraphState::invalidated);
  REQUIRE(projected->current_revision()->invalidation_triggers ==
          std::vector{domain::PlanInvalidationTrigger::source_snapshot_changed,
                      domain::PlanInvalidationTrigger::evidence_changed});
  REQUIRE(kernel.revalidate_plan_approval(
              {id<domain::RunId>("another-unused-run"), attributes(),
               value.plan_id, value.revision_id, stale}) ==
          runtime::PlanRevalidationOutcome::already_invalidated);
  REQUIRE(backend.recorded_requests().empty());
}

TEST_CASE("stale approval records invalidation before any decision",
          "[plan][runtime][failure]") {
  testing::ScriptedBackend backend{{}};
  runtime::RunKernel kernel{id<domain::SessionId>("session"), backend};
  auto first = revision();
  const auto planning_run = id<domain::RunId>("planning-run");
  REQUIRE(kernel.start_plan({planning_run, attributes(), first}));

  auto stale = environment();
  stale.evidence.clear();
  REQUIRE(kernel.decide_plan(
              planning_run, decision(first, domain::PlanDecision::approved),
              stale) == runtime::PlanDecisionOutcome::invalidated);
  REQUIRE(kernel.projection(planning_run)->status() ==
          domain::RunStatus::awaiting_plan_revision);
  REQUIRE(kernel.active_session_tasks().empty());

  auto second = revision("revision-2", "revision-1");
  second.goal = "Revise against current evidence";
  REQUIRE(kernel.revise_plan(planning_run, second));
  REQUIRE(kernel.pending_plan_decision()->revision_id == second.revision_id);
  REQUIRE(kernel.decide_plan(
              planning_run, decision(second, domain::PlanDecision::approved),
              environment()) == runtime::PlanDecisionOutcome::recorded);
  REQUIRE(kernel.active_session_tasks().size() == second.tasks.size());
}

TEST_CASE("rejected plans complete without session tasks",
          "[plan][runtime][rejected]") {
  testing::ScriptedBackend backend{{}};
  runtime::RunKernel kernel{id<domain::SessionId>("session"), backend};
  const auto value = revision();
  const auto planning_run = id<domain::RunId>("planning-run");
  REQUIRE(kernel.start_plan({planning_run, attributes(), value}));

  const auto rejected = kernel.decide_plan(
      planning_run, decision(value, domain::PlanDecision::rejected));
  REQUIRE(rejected == runtime::PlanDecisionOutcome::recorded);
  REQUIRE_FALSE(kernel.active_run_id());
  REQUIRE(kernel.active_session_tasks().empty());
  REQUIRE(kernel.plan_projection(value.plan_id)->state() ==
          domain::PlanGraphState::rejected);
  REQUIRE(kernel.decide_plan(planning_run,
                             decision(value, domain::PlanDecision::rejected)) ==
          runtime::PlanDecisionOutcome::already_recorded);
}

TEST_CASE("pending plan revisions resume without execution",
          "[plan][runtime][replay]") {
  MemorySessionStore store;
  testing::ScriptedBackend backend{{}};
  const auto first = revision();
  const auto planning_run = id<domain::RunId>("planning-run");

  {
    auto kernel =
        open_session(store, backend, runtime::DurableSessionMode::create);
    REQUIRE(kernel->start_plan({planning_run, attributes(), first}));
  }
  {
    auto kernel =
        open_session(store, backend, runtime::DurableSessionMode::resume);
    REQUIRE(kernel->pending_plan_decision());
    REQUIRE(kernel->decide_plan(
                planning_run,
                decision(first, domain::PlanDecision::revision_requested)) ==
            runtime::PlanDecisionOutcome::recorded);
    REQUIRE(kernel->projection(planning_run)->status() ==
            domain::RunStatus::awaiting_plan_revision);
  }
  const auto second = revision("revision-2", "revision-1");
  {
    auto kernel =
        open_session(store, backend, runtime::DurableSessionMode::resume);
    REQUIRE(kernel->active_run_id() == planning_run);
    REQUIRE_FALSE(kernel->pending_plan_decision());
    REQUIRE(kernel->revise_plan(planning_run, second));
    REQUIRE(kernel->pending_plan_decision());
    REQUIRE(kernel->decide_plan(
                planning_run, decision(second, domain::PlanDecision::approved),
                environment()) == runtime::PlanDecisionOutcome::recorded);
  }
  {
    auto kernel =
        open_session(store, backend, runtime::DurableSessionMode::resume);
    REQUIRE_FALSE(kernel->active_run_id());
    REQUIRE(kernel->active_session_tasks().size() == second.tasks.size());
  }
  REQUIRE(backend.recorded_requests().empty());
}

TEST_CASE("approval persistence failure materializes no partial tasks",
          "[plan][runtime][storage][failure]") {
  MemorySessionStore store;
  testing::ScriptedBackend backend{{}};
  const auto value = revision();
  const auto planning_run = id<domain::RunId>("planning-run");
  auto kernel =
      open_session(store, backend, runtime::DurableSessionMode::create);
  REQUIRE(kernel->start_plan({planning_run, attributes(), value}));
  REQUIRE(store.events().size() == 2);

  store.fail_next_append();
  const auto failed = kernel->decide_plan(
      planning_run, decision(value, domain::PlanDecision::approved),
      environment());
  REQUIRE_FALSE(failed);
  REQUIRE(failed.error().code == runtime::RunKernelErrorCode::storage_failure);
  REQUIRE(store.events().size() == 2);
  REQUIRE(kernel->plan_projection(value.plan_id)->state() ==
          domain::PlanGraphState::proposed);

  kernel.reset();
  kernel = open_session(store, backend, runtime::DurableSessionMode::resume);
  REQUIRE(kernel->decide_plan(
              planning_run, decision(value, domain::PlanDecision::approved),
              environment()) == runtime::PlanDecisionOutcome::recorded);
  REQUIRE(kernel->active_session_tasks().size() == value.tasks.size());
}
