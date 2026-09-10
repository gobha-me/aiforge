#pragma once
#include "../181opsobservationkernel/fixture.hpp"
#include <aiforge/surfaces/ops_session.hpp>
#include <set>

namespace ops_session_test {
using namespace observation_kernel_test;
using namespace aiforge::surfaces;
class Store final : public storage::SessionStore {
 public:
  observation_kernel_test::Store delegate;
  std::set<SessionId> sessions;
  unsigned creates{}, opens{}, replays{};
  auto create_session(storage::SessionCreate request, std::stop_token)
      -> std::expected<void, storage::SessionStoreError> override {
    ++creates;
    if (!sessions.insert(request.session_id).second)
      return std::unexpected(storage::SessionStoreError{
          storage::SessionStoreErrorCode::already_exists, "private store text",
          false});
    return {};
  }
  auto open_session(const SessionId& session, std::stop_token stop)
      -> std::expected<storage::SessionInfo,
                       storage::SessionStoreError> override {
    ++opens;
    return delegate.open_session(session, stop);
  }
  auto list_sessions(std::size_t limit, std::stop_token stop)
      -> std::expected<std::vector<storage::SessionInfo>,
                       storage::SessionStoreError> override {
    return delegate.list_sessions(limit, stop);
  }
  auto replay_events(const SessionId& session, std::stop_token stop)
      -> std::expected<std::vector<RunEvent>,
                       storage::SessionStoreError> override {
    ++replays;
    return delegate.replay_events(session, stop);
  }
  auto append_events(const SessionId& session, std::span<const RunEvent> events,
                     std::stop_token stop)
      -> std::expected<void, storage::SessionStoreError> override {
    return delegate.append_events(session, events, stop);
  }
};
struct Fixture {
  OpsObservationAuthoritySpec specification{spec()};
  std::shared_ptr<runtime::LocalSourceWorker> worker{
      runtime::LocalSourceWorker::create(1).value()};
  std::shared_ptr<runtime::OpsObservationBroker> broker{
      runtime::OpsObservationBroker::create(worker).value()};
  std::shared_ptr<runtime::OpsObservationEndpoint> endpoint;
  std::shared_ptr<Source> source{std::make_shared<Source>()};
  Store store;
  std::uint64_t suffix{};
  std::unique_ptr<OpsSession> session;
  auto request() const -> OpsSessionOpen {
    return {specification.session_id,
            {},
            id<SurfaceId>("admin"),
            id<WorkspaceId>("ops")};
  }
  auto dependencies(
      runtime::ApprovalMode mode = runtime::ApprovalMode::allow_all)
      -> OpsSessionDependencies {
    runtime::ApplicationLaunchContextConfiguration config;
    config.approval_mode = mode;
    auto launch = runtime::make_application_launch_context(config);
    REQUIRE(launch);
    return {{id<PermissionProfileId>("observe"), *launch, {}}, broker, [this] {
              return ++suffix;
            }};
  }
  auto open(runtime::ApprovalMode mode = runtime::ApprovalMode::allow_all)
      -> void {
    auto opened = OpsSession::open(request(), store, dependencies(mode));
    REQUIRE(opened);
    session = std::move(*opened);
  }
  auto bind() -> void {
    endpoint = broker->activate_session(specification.session_id).value();
    REQUIRE(session->bind_observation(
        OpsObservationAuthority::create(specification).value(), source,
        endpoint));
  }
  auto intent() const -> runtime::OpsObservationIntent {
    return {specification.target.target_id,
            specification.selection_generation,
            OpsObservationOperation::linux_health,
            {},
            specification.limits};
  }
  auto until(const std::function<bool()>& condition, bool allow_failure = false)
      -> void {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!condition() && std::chrono::steady_clock::now() < deadline) {
      auto pumped = session->pump_observations();
      if (!allow_failure) REQUIRE(pumped);
      std::this_thread::yield();
    }
    REQUIRE(condition());
  }
  auto idle(bool allow_failure = false) -> void {
    until([&] { return !session->inspect_observations().busy; }, allow_failure);
  }
  auto history() const -> SessionEventLog {
    SessionEventLog log{specification.session_id};
    for (const auto& event : store.delegate.history)
      REQUIRE(log.append(event));
    return log;
  }
  ~Fixture() {
    session.reset();
    static_cast<void>(broker->close());
  }
};
} // namespace ops_session_test
