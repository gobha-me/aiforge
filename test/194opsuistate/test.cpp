#include "../159foldergrant/fixture.hpp"
#include <aiforge/runtime/local_source_worker.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {
using namespace aiforge;
using namespace std::chrono_literals;
using folder_grant_test::Gate;
using folder_grant_test::Release;
using folder_grant_test::until;
template <class T> auto id(std::string value) -> T {
  return T::from(std::move(value)).value();
}
auto request() -> runtime::OpsSourcePreparationRequest {
  return {{id<domain::SessionId>("session"),
           1,
           1,
           {id<domain::OpsTargetId>("target"),
            id<domain::OpsConfigurationRevision>("revision"),
            domain::OpsTargetKind::linux_local}},
          std::chrono::steady_clock::now() + 5s};
}
struct State {
  std::shared_ptr<Gate> prepare_gate, destruction_gate;
  std::atomic<unsigned> calls{}, observations{}, destroyed{};
  bool fail{};
};
class Source final : public runtime::OpsObservationSource {
 public:
  explicit Source(std::shared_ptr<State> state) : m_state(std::move(state)) {}
  ~Source() override {
    if (m_state->destruction_gate) m_state->destruction_gate->wait();
    ++m_state->destroyed;
  }
  auto guarantees_bound_read_only_observations() const noexcept
      -> bool override {
    return true;
  }
  auto target_binding() const noexcept
      -> const domain::OpsTargetBinding& override {
    return m_binding;
  }
  auto observe(const domain::OpsObservationRequest&, std::stop_token)
      -> std::expected<domain::OpsObservation,
                       runtime::OpsObservationSourceError> override {
    ++m_state->observations;
    return std::unexpected(runtime::OpsObservationSourceError::unavailable);
  }

 private:
  std::shared_ptr<State> m_state;
  domain::OpsTargetBinding m_binding{
      request().token.selection.target_id,
      request().token.selection.configuration_revision,
      domain::LinuxOpsIdentity{domain::LinuxExecutionScope::container,
                               "12345678-1234-1234-1234-123456789abc", 42, 43}};
};
class Factory final : public runtime::OpsSourcePreparationFactory {
 public:
  std::shared_ptr<State> state{std::make_shared<State>()};
  runtime::OpsSourcePreparationIdentity identity{request().token.selection};
  auto guarantees_owned_read_only_preparation() const noexcept
      -> bool override {
    return true;
  }
  auto preparation_identity() const noexcept
      -> const runtime::OpsSourcePreparationIdentity& override {
    return identity;
  }
  auto prepare(const runtime::OpsSourcePreparationRequest&, std::stop_token)
      -> std::expected<runtime::PreparedOpsSource,
                       runtime::OpsObservationSourceError> override {
    ++state->calls;
    if (state->prepare_gate) state->prepare_gate->wait();
    if (state->fail)
      return std::unexpected(runtime::OpsObservationSourceError::unavailable);
    return runtime::PreparedOpsSource{std::make_shared<Source>(state)};
  }
};
class StalledFolder final : public runtime::LocalSourceGrantFactory {
 public:
  std::shared_ptr<Gate> gate{std::make_shared<Gate>()};
  auto guarantees_pinned_read_only_sources() const noexcept -> bool override {
    return true;
  }
  auto grant(const runtime::LocalFolderGrantRequest&, std::stop_token)
      -> std::expected<runtime::LocalFolderGrantResult,
                       domain::LocalSourceError> override {
    gate->wait();
    return std::unexpected(domain::LocalSourceError{});
  }
};
auto inspect(const runtime::LocalSourceWorker& worker,
             const runtime::OpsSourcePreparationToken& token)
    -> runtime::OpsSourcePreparationState {
  auto result = worker.preparation_state(token);
  REQUIRE(result);
  return *result;
}
} // namespace

TEST_CASE("Preparation state compares every token field without consuming work",
          "[ops][ui-state]") {
  auto worker = runtime::LocalSourceWorker::create(1).value();
  auto factory = std::make_shared<Factory>();
  const auto input = request();
  CHECK_FALSE(inspect(*worker, input.token).ready);
  CHECK_FALSE(inspect(*worker, input.token).physically_outstanding);
  REQUIRE(worker->submit(factory, input));
  REQUIRE(until([&] { return inspect(*worker, input.token).ready; }));
  auto foreign = input.token;
  SECTION("session") {
    foreign.session_id = id<domain::SessionId>("foreign");
  }
  SECTION("epoch") {
    ++foreign.session_epoch;
  }
  SECTION("request") {
    ++foreign.request_id;
  }
  SECTION("target") {
    foreign.selection.target_id = id<domain::OpsTargetId>("foreign");
  }
  SECTION("revision") {
    foreign.selection.configuration_revision =
        id<domain::OpsConfigurationRevision>("foreign");
  }
  SECTION("kind") {
    foreign.selection.kind = domain::OpsTargetKind::kubernetes;
  }
  CHECK_FALSE(inspect(*worker, foreign).ready);
  CHECK_FALSE(inspect(*worker, foreign).physically_outstanding);
  for (unsigned i{}; i < 3; ++i) {
    const auto state = inspect(*worker, input.token);
    CHECK(state.ready);
    CHECK(state.physically_outstanding);
  }
  CHECK(factory->state->calls == 1);
  CHECK(factory->state->observations == 0);
  auto result = worker->poll(input.token);
  REQUIRE(result);
  REQUIRE(*result);
  REQUIRE((*result)->result);
  CHECK_FALSE(inspect(*worker, input.token).ready);
  REQUIRE(until(
      [&] { return !inspect(*worker, input.token).physically_outstanding; }));
  REQUIRE_FALSE(worker->poll(input.token));
}

TEST_CASE("Failed preparation is ready for one explicit poll",
          "[ops][ui-state]") {
  auto worker = runtime::LocalSourceWorker::create(1).value();
  auto factory = std::make_shared<Factory>();
  factory->state->fail = true;
  const auto input = request();
  REQUIRE(worker->submit(factory, input));
  REQUIRE(until([&] { return inspect(*worker, input.token).ready; }));
  CHECK(inspect(*worker, input.token).physically_outstanding);
  auto result = worker->poll(input.token);
  REQUIRE(result);
  REQUIRE(*result);
  REQUIRE_FALSE((*result)->result);
  CHECK((*result)->result.error() ==
        runtime::OpsObservationSourceError::unavailable);
  REQUIRE(until(
      [&] { return !inspect(*worker, input.token).physically_outstanding; }));
}

TEST_CASE(
    "Cancelled preparation remains physical until its owned producer exits",
    "[ops][ui-state]") {
  auto worker = runtime::LocalSourceWorker::create(1).value();
  auto factory = std::make_shared<Factory>();
  factory->state->prepare_gate = std::make_shared<Gate>();
  const Release release{factory->state->prepare_gate};
  const auto input = request();
  REQUIRE(worker->submit(factory, input));
  REQUIRE(factory->state->prepare_gate->await());
  SECTION("cancel") {
    REQUIRE(worker->cancel(input.token));
  }
  SECTION("invalidate") {
    REQUIRE(worker->invalidate_session(input.token.session_id));
  }
  CHECK_FALSE(inspect(*worker, input.token).ready);
  CHECK(inspect(*worker, input.token).physically_outstanding);
  factory->state->prepare_gate->release();
  REQUIRE(until(
      [&] { return !inspect(*worker, input.token).physically_outstanding; }));
  CHECK(factory->state->observations == 0);
}

TEST_CASE("Claimed expired preparation retires independently of unrelated "
          "stalled work",
          "[ops][ui-state]") {
  auto worker = runtime::LocalSourceWorker::create(2).value();
  auto folder = std::make_shared<StalledFolder>();
  const Release release_folder{folder->gate};
  REQUIRE(worker->submit(folder, folder_grant_test::request(2)));
  REQUIRE(folder->gate->await());
  auto factory = std::make_shared<Factory>();
  factory->state->destruction_gate = std::make_shared<Gate>();
  const Release release_source{factory->state->destruction_gate};
  auto input = request();
  input.token.request_id = 3;
  REQUIRE(worker->submit(factory, input));
  REQUIRE(until([&] { return inspect(*worker, input.token).ready; }));
  std::this_thread::sleep_until(input.deadline);
  // Ready is not a success or freshness claim. Poll owns the original deadline.
  CHECK(inspect(*worker, input.token).ready);
  auto result = worker->poll(input.token);
  REQUIRE(result);
  REQUIRE(*result);
  REQUIRE_FALSE((*result)->result);
  CHECK((*result)->result.error() ==
        runtime::OpsObservationSourceError::timed_out);
  REQUIRE(factory->state->destruction_gate->await());
  CHECK_FALSE(inspect(*worker, input.token).ready);
  CHECK(inspect(*worker, input.token).physically_outstanding);
  CHECK(worker->occupied_slots() == 2);
  factory->state->destruction_gate->release();
  REQUIRE(until(
      [&] { return !inspect(*worker, input.token).physically_outstanding; }));
  CHECK(worker->occupied_slots() == 1);
  CHECK(factory->state->destroyed == 1);
  CHECK(factory->state->observations == 0);
  REQUIRE(worker->cancel(folder_grant_test::request(2).token));
  folder->gate->release();
  REQUIRE(until([&] { return worker->occupied_slots() == 0; }));
}
