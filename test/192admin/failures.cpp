#include "../../src/adapters/process_admin_internal.hpp"
#include <aiforge/adapters/sqlite_session_store.hpp>
#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace {
using namespace aiforge;
using adapters::admin_detail::Result;
using Command = cli::AdminCommand;
using Clock = std::chrono::steady_clock;
enum class Refusal { none, admission, publication, cancellation };
struct State {
  std::atomic<unsigned> preparations{}, observations{}, returned{}, destroyed{},
      refused{};
  std::mutex mutex;
  std::condition_variable changed;
  bool release{}, stop_observed{};
  bool stall{}, cancel_outer{};
  std::stop_source stop;
  auto release_source() -> void {
    {
      std::lock_guard lock{mutex};
      release = true;
    }
    changed.notify_all();
  }
};
class Source final : public runtime::OpsObservationSource {
 public:
  Source(runtime::OpsSourcePreparationIdentity identity,
         std::shared_ptr<State> state)
      : m_binding{std::move(identity.target_id),
                  std::move(identity.configuration_revision),
                  domain::LinuxOpsIdentity{
                      domain::LinuxExecutionScope::container,
                      "12345678-1234-1234-1234-123456789abc", 42, 43}},
        m_state(std::move(state)) {}
  ~Source() override {
    {
      std::lock_guard lock{m_state->mutex};
      ++m_state->destroyed;
    }
    m_state->changed.notify_all();
  }
  auto guarantees_bound_read_only_observations() const noexcept
      -> bool override {
    return true;
  }
  auto target_binding() const noexcept
      -> const domain::OpsTargetBinding& override {
    return m_binding;
  }
  auto observe(const domain::OpsObservationRequest& request,
               std::stop_token stop)
      -> std::expected<domain::OpsObservation,
                       runtime::OpsObservationSourceError> override {
    ++m_state->observations;
    std::stop_callback observed{stop, [state = m_state] {
                                  {
                                    std::lock_guard lock{state->mutex};
                                    state->stop_observed = true;
                                  }
                                  state->changed.notify_all();
                                }};
    if (m_state->cancel_outer) m_state->stop.request_stop();
    if (m_state->stall || m_state->cancel_outer) {
      std::unique_lock lock{m_state->mutex};
      // Deliberately ignore cancellation while physical source work is held.
      m_state->changed.wait(lock, [&] {
        return m_state->release ||
               (m_state->cancel_outer && m_state->stop_observed);
      });
    }
    ++m_state->returned;
    // A valid late value must still be discarded after logical cancellation.
    return domain::OpsObservation{
        request,
        domain::EventTimestamp{std::chrono::milliseconds{1000}},
        domain::EventTimestamp{std::chrono::milliseconds{1001}},
        domain::OpsObservationCompleteness::partial,
        {},
        0,
        {},
        domain::LinuxHealthObservation{
            domain::OpsHealthState::unknown, 12,
            domain::LinuxMemoryObservation{domain::OpsMemoryScope::kernel, 1024,
                                           512}}};
  }

 private:
  const domain::OpsTargetBinding m_binding;
  std::shared_ptr<State> m_state;
};
class Factory final : public runtime::OpsSourcePreparationFactory {
 public:
  Factory(runtime::OpsSourcePreparationIdentity identity,
          std::shared_ptr<State> state)
      : m_identity(std::move(identity)), m_state(std::move(state)) {}
  auto guarantees_owned_read_only_preparation() const noexcept
      -> bool override {
    return true;
  }
  auto preparation_identity() const noexcept
      -> const runtime::OpsSourcePreparationIdentity& override {
    return m_identity;
  }
  auto prepare(const runtime::OpsSourcePreparationRequest&, std::stop_token)
      -> std::expected<runtime::PreparedOpsSource,
                       runtime::OpsObservationSourceError> override {
    ++m_state->preparations;
    return runtime::PreparedOpsSource{
        std::make_shared<Source>(m_identity, m_state)};
  }

 private:
  const runtime::OpsSourcePreparationIdentity m_identity;
  std::shared_ptr<State> m_state;
};
// Only the selected append is refused. All other storage uses the real SQLite
// adapter so read-only replay checks what actually survived the failed batch.
class Store final : public storage::SessionStore {
 public:
  Store(std::unique_ptr<adapters::SqliteSessionStore> delegate,
        std::shared_ptr<State> state, Refusal refusal)
      : m_delegate(std::move(delegate)), m_state(std::move(state)),
        m_refusal(refusal) {}
  auto create_session(storage::SessionCreate request, std::stop_token stop)
      -> std::expected<void, storage::SessionStoreError> override {
    return m_delegate->create_session(std::move(request), stop);
  }
  auto create_session_with_events(storage::SessionCreate request,
                                  std::span<const domain::RunEvent> events,
                                  std::stop_token stop)
      -> std::expected<void, storage::SessionStoreError> override {
    return m_delegate->create_session_with_events(std::move(request), events,
                                                  stop);
  }
  auto open_session(const domain::SessionId& id, std::stop_token stop)
      -> std::expected<storage::SessionInfo,
                       storage::SessionStoreError> override {
    return m_delegate->open_session(id, stop);
  }
  auto list_sessions(std::size_t limit, std::stop_token stop)
      -> std::expected<std::vector<storage::SessionInfo>,
                       storage::SessionStoreError> override {
    return m_delegate->list_sessions(limit, stop);
  }
  auto append_events(const domain::SessionId& id,
                     std::span<const domain::RunEvent> events,
                     std::stop_token stop)
      -> std::expected<void, storage::SessionStoreError> override {
    const bool refuse = std::ranges::any_of(events, [&](const auto& event) {
      return (m_refusal == Refusal::admission &&
              std::holds_alternative<domain::HumanObservationRequested>(
                  event.payload)) ||
             (m_refusal == Refusal::publication &&
              std::holds_alternative<domain::OpsObservationRecorded>(
                  event.payload)) ||
             (m_refusal == Refusal::cancellation &&
              std::holds_alternative<domain::RunCancelled>(event.payload));
    });
    if (refuse) {
      ++m_state->refused;
      return std::unexpected(
          storage::SessionStoreError{storage::SessionStoreErrorCode::io_failure,
                                     "private append refusal detail", false});
    }
    return m_delegate->append_events(id, events, stop);
  }
  auto replay_events(const domain::SessionId& id, std::stop_token stop)
      -> std::expected<std::vector<domain::RunEvent>,
                       storage::SessionStoreError> override {
    return m_delegate->replay_events(id, stop);
  }

 private:
  std::unique_ptr<adapters::SqliteSessionStore> m_delegate;
  std::shared_ptr<State> m_state;
  Refusal m_refusal;
};
class Dependencies final : public adapters::admin_detail::Dependencies {
 public:
  std::shared_ptr<State> state{std::make_shared<State>()};
  Refusal refusal{Refusal::none};
  Dependencies()
      : m_directory(std::filesystem::temp_directory_path() /
                    ("aiforge-admin-failures-" +
                     std::to_string(std::random_device{}()))) {
    if (!std::filesystem::create_directory(m_directory))
      throw std::runtime_error("test directory was not exclusively created");
  }
  ~Dependencies() override {
    std::error_code ignored;
    std::filesystem::remove_all(m_directory, ignored);
  }
  auto load_catalog() -> Result<config::OpsTargetsConfig> override {
    return config::OpsTargetsConfig{
        {{"local", "This environment", config::LinuxLocalTargetConfig{}}}};
  }
  auto open_store() -> Result<std::unique_ptr<storage::SessionStore>> override {
    auto store = adapters::SqliteSessionStore::open(database());
    if (!store)
      return std::unexpected(cli::CommandFailure{
          cli::CommandFailureKind::runtime, "test storage unavailable"});
    return std::make_unique<Store>(std::move(*store), state, refusal);
  }
  auto factory(runtime::OpsSourcePreparationIdentity identity) -> Result<
      std::shared_ptr<runtime::OpsSourcePreparationFactory>> override {
    return std::make_shared<Factory>(std::move(identity), state);
  }
  auto instance_identity() -> Result<std::string> override {
    return "admin-failure-test";
  }
  auto history() const -> std::vector<domain::RunEvent> {
    auto store =
        adapters::SqliteSessionStore::open_existing_read_only(database());
    REQUIRE(store);
    auto events = (*store)->replay_events(
        domain::SessionId::from("admin-failure-test").value(), {});
    REQUIRE(events);
    return std::move(*events);
  }

 private:
  auto database() const -> std::filesystem::path {
    return m_directory / "sessions.sqlite";
  }
  std::filesystem::path m_directory;
};
template <class Event>
auto count(const std::vector<domain::RunEvent>& events) -> std::size_t {
  return static_cast<std::size_t>(
      std::ranges::count_if(events, [](const auto& event) {
        return std::holds_alternative<Event>(event.payload);
      }));
}
struct Outcome {
  std::mutex mutex;
  std::condition_variable changed;
  std::optional<Result<void>> result;
  std::string output;
};
struct ReleaseSource {
  std::shared_ptr<State> state;
  ~ReleaseSource() {
    state->stop.request_stop();
    state->release_source();
  }
};
} // namespace

TEST_CASE("Admin append refusal preserves only committed SQLite history",
          "[admin]") {
  Dependencies dependencies;
  SECTION("atomic human admission") {
    dependencies.refusal = Refusal::admission;
  }
  SECTION("atomic observation publication") {
    dependencies.refusal = Refusal::publication;
  }
  std::ostringstream output;
  const auto result = adapters::admin_detail::execute(
      {Command::Operation::health}, dependencies, {}, output);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().kind == cli::CommandFailureKind::runtime);
  REQUIRE(result.error().message.find("private append refusal") ==
          std::string::npos);
  REQUIRE(output.str().empty());
  REQUIRE(dependencies.state->preparations == 1);
  REQUIRE(dependencies.state->refused == 1);
  const auto events = dependencies.history();
  REQUIRE(count<domain::OpsObservationRecorded>(events) == 0);
  REQUIRE(count<domain::ToolResultRecorded>(events) == 0);
  REQUIRE(count<domain::RunCompleted>(events) == 0);
  REQUIRE(count<domain::InferenceStarted>(events) == 0);
  if (dependencies.refusal == Refusal::admission) {
    REQUIRE(events.empty());
    REQUIRE(dependencies.state->observations == 0);
  } else {
    REQUIRE(dependencies.state->observations == 1);
    REQUIRE(count<domain::HumanObservationRequested>(events) == 1);
    REQUIRE(count<domain::ToolStarted>(events) == 1);
  }
}

TEST_CASE("Admin logical deadline returns before stalled source retirement",
          "[admin]") {
  auto dependencies = std::make_shared<Dependencies>();
  dependencies->state->stall = true;
  auto outcome = std::make_shared<Outcome>();
  const auto started = Clock::now();
  // The thread owns every object it accesses. The following release guard runs
  // before its join on assertion failure, so no test gate strands source work.
  std::jthread runner{[dependencies, outcome] {
    std::ostringstream output;
    auto result = adapters::admin_detail::execute(
        {Command::Operation::health}, *dependencies,
        dependencies->state->stop.get_token(), output);
    {
      std::lock_guard lock{outcome->mutex};
      outcome->result = std::move(result);
      outcome->output = output.str();
    }
    outcome->changed.notify_all();
  }};
  ReleaseSource release{dependencies->state};
  {
    std::unique_lock lock{outcome->mutex};
    REQUIRE(outcome->changed.wait_for(lock, std::chrono::seconds{12}, [&] {
      return outcome->result.has_value();
    }));
    REQUIRE_FALSE(*outcome->result);
    REQUIRE(outcome->result->error().kind == cli::CommandFailureKind::runtime);
    REQUIRE(outcome->output.empty());
  }
  REQUIRE(Clock::now() - started >= std::chrono::seconds{4});
  REQUIRE(dependencies->state->preparations == 1);
  REQUIRE(dependencies->state->observations == 1);
  REQUIRE(dependencies->state->returned == 0);
  REQUIRE(dependencies->state->destroyed == 0);
  {
    std::unique_lock lock{dependencies->state->mutex};
    REQUIRE_FALSE(dependencies->state->release);
    REQUIRE(dependencies->state->changed.wait_for(
        lock, std::chrono::seconds{3},
        [&] { return dependencies->state->stop_observed; }));
    REQUIRE_FALSE(dependencies->state->release);
    REQUIRE(dependencies->state->returned == 0);
  }
  const auto before_release = dependencies->history();
  REQUIRE(count<domain::OpsObservationRecorded>(before_release) == 0);
  REQUIRE(count<domain::ToolResultRecorded>(before_release) == 0);
  REQUIRE(count<domain::RunCompleted>(before_release) == 0);
  REQUIRE(count<domain::InferenceStarted>(before_release) == 0);
  REQUIRE(count<domain::RunCancelled>(before_release) +
              count<domain::RunFailed>(before_release) ==
          1);
  dependencies->state->release_source();
  {
    std::unique_lock lock{dependencies->state->mutex};
    REQUIRE(dependencies->state->changed.wait_for(
        lock, std::chrono::seconds{3},
        [&] { return dependencies->state->destroyed.load() == 1; }));
  }
  REQUIRE(dependencies->state->returned == 1);
  const auto after_release = dependencies->history();
  REQUIRE(after_release.size() == before_release.size());
  REQUIRE(count<domain::OpsObservationRecorded>(after_release) == 0);
  REQUIRE(dependencies->state->observations == 1);
}

TEST_CASE("Admin cancellation append refusal remains a runtime failure",
          "[admin]") {
  Dependencies dependencies;
  dependencies.refusal = Refusal::cancellation;
  dependencies.state->cancel_outer = true;
  ReleaseSource release{dependencies.state};
  std::ostringstream output;
  const auto result = adapters::admin_detail::execute(
      {Command::Operation::health}, dependencies,
      dependencies.state->stop.get_token(), output);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().kind == cli::CommandFailureKind::runtime);
  REQUIRE(result.error().message ==
          "Admin cancellation could not be committed");
  REQUIRE(output.str().empty());
  REQUIRE(dependencies.state->preparations == 1);
  REQUIRE(dependencies.state->observations == 1);
  REQUIRE(dependencies.state->refused >= 1);
  {
    std::unique_lock lock{dependencies.state->mutex};
    REQUIRE(dependencies.state->changed.wait_for(
        lock, std::chrono::seconds{3}, [&] {
          return dependencies.state->stop_observed &&
                 dependencies.state->destroyed.load() == 1;
        }));
  }
  REQUIRE(dependencies.state->returned == 1);
  const auto events = dependencies.history();
  REQUIRE(count<domain::HumanObservationRequested>(events) == 1);
  REQUIRE(count<domain::ToolStarted>(events) == 1);
  REQUIRE(count<domain::RunCancelled>(events) == 0);
  REQUIRE(count<domain::OpsObservationRecorded>(events) == 0);
  REQUIRE(count<domain::ToolResultRecorded>(events) == 0);
  REQUIRE(count<domain::RunCompleted>(events) == 0);
  REQUIRE(count<domain::InferenceStarted>(events) == 0);
}
