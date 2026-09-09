#include "../../src/adapters/process_admin_internal.hpp"
#include <aiforge/adapters/sqlite_session_store.hpp>
#include <aiforge/domain/events.hpp>
#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>

namespace {
using namespace aiforge;
using adapters::admin_detail::execute;
using adapters::admin_detail::Result;
using Command = cli::AdminCommand;
struct State {
  std::atomic<unsigned> preparations{}, observations{};
  bool preparation_failure{}, source_failure{}, wrong_source{};
  bool cancel_preparation{}, cancel_observation{};
  std::stop_source stop;
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
    if (m_state->cancel_observation) {
      m_state->stop.request_stop();
      std::mutex mutex;
      std::unique_lock lock{mutex};
      std::condition_variable_any changed;
      changed.wait(lock, stop, [] { return false; });
      return std::unexpected(runtime::OpsObservationSourceError::cancelled);
    }
    if (m_state->source_failure)
      return std::unexpected(
          runtime::OpsObservationSourceError::permission_denied);
    domain::OpsObservation result{
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
    if (request.operation == domain::OpsObservationOperation::linux_services) {
      result.payload = domain::LinuxServicesObservation{
          {{{"example.service", {}},
            domain::OpsServiceState::failed,
            domain::OpsObservationReason::failed_exit,
            7,
            3}}};
    } else if (request.operation ==
               domain::OpsObservationOperation::linux_service_health) {
      result.payload = domain::LinuxServiceObservation{
          std::get<domain::LinuxServiceIdentity>(request.resource),
          domain::OpsServiceState::active, domain::OpsObservationReason::none,
          0, 2};
    }
    return result;
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
    if (m_state->cancel_preparation) m_state->stop.request_stop();
    if (m_state->preparation_failure)
      return std::unexpected(runtime::OpsObservationSourceError::unavailable);
    auto identity = m_identity;
    if (m_state->wrong_source)
      identity.target_id = domain::OpsTargetId::from("foreign").value();
    return runtime::PreparedOpsSource{
        std::make_shared<Source>(std::move(identity), m_state)};
  }

 private:
  const runtime::OpsSourcePreparationIdentity m_identity;
  std::shared_ptr<State> m_state;
};
class Dependencies final : public adapters::admin_detail::Dependencies {
 public:
  std::shared_ptr<State> state{std::make_shared<State>()};
  config::OpsTargetsConfig catalog{
      {{"local", "This environment", config::LinuxLocalTargetConfig{}},
       {"cluster", "Private cluster",
        config::StaticKubernetesTargetConfig{"/absent/config", "chosen",
                                             "default"}}}};
  unsigned catalog_calls{}, store_calls{}, factory_calls{};
  bool fail_catalog{}, fail_store{}, duplicate_session{};
  std::filesystem::path directory{
      std::filesystem::temp_directory_path() /
      ("aiforge-admin-test-" + std::to_string(std::random_device{}()))};
  Dependencies() {
    if (!std::filesystem::create_directory(directory))
      throw std::runtime_error("test directory was not exclusively created");
  }
  ~Dependencies() override {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
  auto load_catalog() -> Result<config::OpsTargetsConfig> override {
    ++catalog_calls;
    if (fail_catalog) return refused();
    return catalog;
  }
  auto open_store() -> Result<std::unique_ptr<storage::SessionStore>> override {
    ++store_calls;
    if (fail_store) return refused();
    auto store =
        adapters::SqliteSessionStore::open(directory / "sessions.sqlite");
    REQUIRE(store);
    if (duplicate_session)
      REQUIRE((*store)->create_session(
          {domain::SessionId::from("test-admin").value(), {}}, {}));
    return std::move(*store);
  }
  auto factory(runtime::OpsSourcePreparationIdentity identity) -> Result<
      std::shared_ptr<runtime::OpsSourcePreparationFactory>> override {
    ++factory_calls;
    return std::make_shared<Factory>(std::move(identity), state);
  }
  auto instance_identity() -> Result<std::string> override {
    return "test-admin";
  }
  auto history() -> std::vector<domain::RunEvent> {
    auto store = adapters::SqliteSessionStore::open_existing_read_only(
        directory / "sessions.sqlite");
    REQUIRE(store);
    auto events = (*store)->replay_events(
        domain::SessionId::from("test-admin").value(), {});
    REQUIRE(events);
    return std::move(*events);
  }

 private:
  static auto refused() -> std::unexpected<cli::CommandFailure> {
    return std::unexpected(cli::CommandFailure{cli::CommandFailureKind::runtime,
                                               "fixed test refusal"});
  }
};
template <class Event>
auto count(const std::vector<domain::RunEvent>& events) -> std::size_t {
  return static_cast<std::size_t>(
      std::ranges::count_if(events, [](const auto& event) {
        return std::holds_alternative<Event>(event.payload);
      }));
}
} // namespace

TEST_CASE("Admin invalid selection and catalog never open storage or prepare a "
          "source",
          "[admin]") {
  Dependencies deps;
  Command::Request request{Command::Operation::health};
  SECTION("unknown") {
    request.target = "missing";
  }
  SECTION("wrong target kind") {
    request.target = "cluster";
  }
  SECTION("malformed catalog") {
    deps.fail_catalog = true;
  }
  SECTION("invalid operation") {
    request.operation = static_cast<Command::Operation>(99);
  }
  SECTION("unit on health") {
    request.unit = "example.service";
  }
  SECTION("invalid service name") {
    request.operation = Command::Operation::service;
    request.unit = "../../etc/passwd";
  }
  SECTION("unknown output") {
    request.format = static_cast<Command::OutputFormat>(99);
  }
  SECTION("cancelled") {
    deps.state->stop.request_stop();
  }
  std::ostringstream output;
  REQUIRE_FALSE(execute(request, deps, deps.state->stop.get_token(), output));
  REQUIRE(deps.store_calls == 0);
  REQUIRE(deps.factory_calls == 0);
  REQUIRE(deps.state->preparations == 0);
  REQUIRE(deps.state->observations == 0);
  REQUIRE(output.str().empty());
}
TEST_CASE("Admin target listing performs metadata work only", "[admin]") {
  Dependencies deps;
  Command::Request request{Command::Operation::targets};
  SECTION("text") {
  }
  SECTION("json") {
    request.format = Command::OutputFormat::json;
  }
  std::ostringstream output;
  REQUIRE(execute(request, deps, {}, output));
  REQUIRE(output.str().find("This environment") != std::string::npos);
  REQUIRE(output.str().find("Private cluster") != std::string::npos);
  REQUIRE(output.str().find("/absent/config") == std::string::npos);
  REQUIRE(deps.catalog_calls == 1);
  REQUIRE(deps.store_calls == 0);
  REQUIRE(deps.factory_calls == 0);
  REQUIRE(deps.state->preparations == 0);
  REQUIRE(deps.state->observations == 0);
}
TEST_CASE("Admin construction and preparation failures never collect",
          "[admin]") {
  Dependencies deps;
  SECTION("store open") {
    deps.fail_store = true;
  }
  SECTION("duplicate durable session") {
    deps.duplicate_session = true;
  }
  SECTION("source preparation") {
    deps.state->preparation_failure = true;
  }
  SECTION("wrong prepared source") {
    deps.state->wrong_source = true;
  }
  SECTION("cancel during preparation") {
    deps.state->cancel_preparation = true;
  }
  std::ostringstream output;
  REQUIRE_FALSE(execute({Command::Operation::health}, deps,
                        deps.state->stop.get_token(), output));
  REQUIRE(deps.state->observations == 0);
  REQUIRE(output.str().empty());
}
TEST_CASE("Admin source failure and cancellation cannot report success",
          "[admin]") {
  Dependencies deps;
  SECTION("denied source") {
    deps.state->source_failure = true;
  }
  SECTION("cancel in source") {
    deps.state->cancel_observation = true;
  }
  std::ostringstream output;
  REQUIRE_FALSE(execute({Command::Operation::health}, deps,
                        deps.state->stop.get_token(), output));
  REQUIRE(deps.state->observations == 1);
  REQUIRE(output.str().empty());
  const auto events = deps.history();
  REQUIRE(count<domain::InferenceStarted>(events) == 0);
  REQUIRE(count<domain::OpsObservationRecorded>(events) == 0);
}
TEST_CASE("Admin output refusal preserves one committed read without retry",
          "[admin]") {
  Dependencies deps;
  std::ostringstream output;
  output.setstate(std::ios::badbit);
  REQUIRE_FALSE(execute({Command::Operation::health}, deps, {}, output));
  REQUIRE(deps.state->observations == 1);
  REQUIRE(count<domain::OpsObservationRecorded>(deps.history()) == 1);
  REQUIRE(count<domain::RunCompleted>(deps.history()) == 1);
}
TEST_CASE("Admin capacity-one preparation hands off to one durable native "
          "observation",
          "[admin]") {
  Dependencies deps;
  Command::Request request{Command::Operation::health};
  SECTION("text") {
  }
  SECTION("json") {
    request.format = Command::OutputFormat::json;
  }
  std::ostringstream output;
  REQUIRE(execute(request, deps, {}, output));
  REQUIRE(deps.state->preparations == 1);
  REQUIRE(deps.state->observations == 1);
  REQUIRE(output.str().find("512") != std::string::npos);
  REQUIRE(output.str().find("partial") != std::string::npos);
  REQUIRE(output.str().find("container") != std::string::npos);
  const auto events = deps.history();
  REQUIRE(count<domain::HumanObservationRequested>(events) == 1);
  REQUIRE(count<domain::OpsObservationRecorded>(events) == 1);
  REQUIRE(count<domain::RunCompleted>(events) == 1);
  REQUIRE(count<domain::InferenceStarted>(events) == 0);
}

TEST_CASE("Admin service commands retain selected operation and exact unit",
          "[admin]") {
  Dependencies deps;
  Command::Request request{Command::Operation::services};
  SECTION("loaded services") {
  }
  SECTION("exact service") {
    request.operation = Command::Operation::service;
    request.unit = "Selected@instance.service";
  }
  std::ostringstream output;
  REQUIRE(execute(request, deps, {}, output));
  REQUIRE(deps.state->observations == 1);
  const auto events = deps.history();
  REQUIRE(count<domain::HumanObservationRequested>(events) == 1);
  REQUIRE(count<domain::OpsObservationRecorded>(events) == 1);
  REQUIRE(count<domain::InferenceStarted>(events) == 0);
  REQUIRE(output.str().find(request.unit ? *request.unit : "example.service") !=
          std::string::npos);
  REQUIRE(output.str().find(request.unit
                                ? "linux_service_health"
                                : "linux_services") != std::string::npos);
  if (request.unit)
    REQUIRE(output.str().find("linux_services\n") == std::string::npos);
}
