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
  std::mutex mutex;
  std::vector<domain::OpsObservationRequest> requests;
  bool preparation_failure{}, source_failure{}, wrong_source{};
  bool missing_log_identity{};
  bool cancel_preparation{}, cancel_observation{};
  std::optional<domain::OpsObservationOperation> cancel_operation;
  std::stop_source stop;
};
class Source final : public runtime::OpsObservationSource {
 public:
  Source(runtime::OpsSourcePreparationIdentity identity,
         std::shared_ptr<State> state)
      : m_binding{std::move(identity.target_id),
                  std::move(identity.configuration_revision),
                  identity.kind == domain::OpsTargetKind::kubernetes
                      ? domain::OpsTargetIdentity{domain::KubernetesOpsIdentity{
                            "chosen",
                            "default",
                            {"127.0.0.1", 6443},
                            "sha256:fixture"}}
                      : domain::OpsTargetIdentity{domain::LinuxOpsIdentity{
                            domain::LinuxExecutionScope::container,
                            "12345678-1234-1234-1234-123456789abc", 42, 43}}},
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
    {
      const std::lock_guard lock{m_state->mutex};
      m_state->requests.push_back(request);
    }
    if (m_state->cancel_observation ||
        m_state->cancel_operation == request.operation) {
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
      auto identity = std::get<domain::LinuxServiceIdentity>(request.resource);
      if (!m_state->missing_log_identity)
        identity.invocation_id =
            domain::OpsResourceUid::from("service-invocation").value();
      result.payload = domain::LinuxServiceObservation{
          std::move(identity), domain::OpsServiceState::active,
          domain::OpsObservationReason::none, 0, 2};
    } else if (request.operation ==
               domain::OpsObservationOperation::kubernetes_workloads) {
      result.source_version = "rv-workloads";
      result.payload = domain::KubernetesWorkloadsObservation{
          {{{domain::OpsWorkloadKind::pod, "default", "broken-pod",
             domain::OpsResourceUid::from("pod-uid").value()},
            domain::OpsHealthState::unhealthy,
            1,
            0,
            1}}};
    } else if (request.operation ==
               domain::OpsObservationOperation::kubernetes_pod_health) {
      result.source_version = "rv-pod";
      result.payload = domain::KubernetesPodObservation{
          std::get<domain::KubernetesPodIdentity>(request.resource),
          domain::OpsPodPhase::failed,
          {{"app",
            m_state->missing_log_identity
                ? std::nullopt
                : std::optional<std::string>{"containerd://app"},
            domain::OpsContainerState::terminated,
            domain::OpsReadiness::not_ready,
            domain::OpsObservationReason::failed_exit, 4, 7}}};
    } else if (request.operation ==
               domain::OpsObservationOperation::kubernetes_events) {
      result.source_version = "rv-events";
      const auto regarding =
          std::holds_alternative<domain::KubernetesPodIdentity>(
              request.resource)
              ? std::get<domain::KubernetesPodIdentity>(request.resource)
              : domain::KubernetesPodIdentity{
                    "default",
                    "broken-pod",
                    domain::OpsResourceUid::from("pod-uid").value(),
                    {}};
      result.payload = domain::KubernetesEventsObservation{
          {{domain::OpsResourceUid::from("event-uid").value(),
            {domain::OpsWorkloadKind::pod, regarding.namespace_name,
             regarding.name, regarding.uid},
            domain::OpsEventSeverity::warning,
            domain::OpsObservationReason::failed_exit,
            {},
            {},
            2}}};
    } else if (request.operation ==
                   domain::OpsObservationOperation::linux_service_logs ||
               request.operation ==
                   domain::OpsObservationOperation::kubernetes_pod_logs) {
      result.payload = domain::OpsLogObservation{
          request.resource,
          {{domain::EventTimestamp{std::chrono::milliseconds{1000}},
            "bounded log line"}}};
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
    std::error_code error;
    std::filesystem::permissions(directory, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, error);
    if (error) {
      std::error_code ignored;
      std::filesystem::remove(directory, ignored);
      throw std::runtime_error("test directory permissions could not be set");
    }
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
  auto factory(const config::OpsTargetConfig& target,
               domain::OpsConfigurationRevision revision)
      -> Result<
          std::shared_ptr<runtime::OpsSourcePreparationFactory>> override {
    ++factory_calls;
    auto target_id = domain::OpsTargetId::from(target.id);
    REQUIRE(target_id);
    runtime::OpsSourcePreparationIdentity identity{
        std::move(*target_id), std::move(revision),
        std::holds_alternative<config::LinuxLocalTargetConfig>(target.source)
            ? domain::OpsTargetKind::linux_local
            : domain::OpsTargetKind::kubernetes};
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
template <class Event>
auto count_manual_run(const std::vector<domain::RunEvent>& events)
    -> std::size_t {
  const auto requested = std::ranges::find_if(events, [](const auto& event) {
    return std::holds_alternative<domain::HumanObservationRequested>(
        event.payload);
  });
  if (requested == events.end()) return 0;
  return static_cast<std::size_t>(
      std::ranges::count_if(events, [&](const auto& event) {
        return event.metadata.run_id == requested->metadata.run_id &&
               std::holds_alternative<Event>(event.payload);
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
  REQUIRE(count_manual_run<domain::RunCompleted>(deps.history()) == 1);
  REQUIRE(count<domain::OpsTargetSelected>(deps.history()) == 1);
  REQUIRE(count<domain::RunCompleted>(deps.history()) == 2);
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
  REQUIRE(output.str().find("last_success") != std::string::npos);
  const auto events = deps.history();
  REQUIRE(count<domain::HumanObservationRequested>(events) == 1);
  REQUIRE(count<domain::OpsObservationRecorded>(events) == 1);
  REQUIRE(count_manual_run<domain::RunCompleted>(events) == 1);
  REQUIRE(count<domain::OpsTargetSelected>(events) == 1);
  REQUIRE(count<domain::RunCompleted>(events) == 2);
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

TEST_CASE(
    "Admin Kubernetes commands retain context namespace and exact Pod identity",
    "[admin][kubernetes]") {
  for (auto request :
       {Command::Request{Command::Operation::workloads, "cluster"},
        Command::Request{Command::Operation::events, "cluster"},
        Command::Request{Command::Operation::pod,
                         "cluster",
                         {},
                         "broken-pod",
                         domain::OpsResourceUid::from("pod-uid").value()},
        Command::Request{Command::Operation::events,
                         "cluster",
                         {},
                         "broken-pod",
                         domain::OpsResourceUid::from("pod-uid").value()}}) {
    Dependencies deps;
    std::ostringstream output;
    REQUIRE(execute(request, deps, {}, output));
    CHECK(output.str().find("context_name=\"chosen\"") != std::string::npos);
    CHECK(output.str().find("namespace_name=\"default\"") != std::string::npos);
    CHECK(output.str().find("completed_at_ms=1001") != std::string::npos);
    CHECK(output.str().find("last_success") != std::string::npos);
    if (request.pod) {
      CHECK(output.str().find("broken-pod") != std::string::npos);
      CHECK(output.str().find("pod-uid") != std::string::npos);
    }
    CHECK(deps.state->preparations == 1);
    CHECK(deps.state->observations == 1);
  }
}

TEST_CASE("Admin refuses target-kind and partial Pod identity mismatch before "
          "preparation",
          "[admin][kubernetes][failure]") {
  Dependencies deps;
  for (auto request :
       {Command::Request{Command::Operation::workloads, "local"},
        Command::Request{Command::Operation::health, "cluster"},
        Command::Request{Command::Operation::pod, "cluster"},
        Command::Request{
            Command::Operation::events, "cluster", {}, "broken-pod"}}) {
    std::ostringstream output;
    const auto result = execute(request, deps, {}, output);
    REQUIRE_FALSE(result);
    CHECK(result.error().kind == cli::CommandFailureKind::usage);
    CHECK(output.str().empty());
  }
  CHECK(deps.factory_calls == 0);
  CHECK(deps.state->preparations == 0);
  CHECK(deps.state->observations == 0);
}

TEST_CASE("One-shot service logs prove invocation before one allowed log read",
          "[admin][logs]") {
  Dependencies deps;
  Command::Request request{Command::Operation::service_logs};
  request.target = "local";
  request.unit = "Selected.service";
  request.allow_log_text = true;
  std::ostringstream output;
  REQUIRE(execute(request, deps, {}, output));
  REQUIRE(deps.state->observations == 2);
  REQUIRE(deps.state->requests.size() == 2);
  CHECK(deps.state->requests[0].operation ==
        domain::OpsObservationOperation::linux_service_health);
  CHECK(deps.state->requests[1].operation ==
        domain::OpsObservationOperation::linux_service_logs);
  const auto& source =
      std::get<domain::LinuxServiceIdentity>(deps.state->requests[1].resource);
  CHECK(source.unit_name == "Selected.service");
  REQUIRE(source.invocation_id);
  CHECK(source.invocation_id->value() == "service-invocation");
  CHECK(deps.state->requests[1].selection_generation == 2);
  CHECK(deps.state->requests[1].log_policy_revision == 2);
  CHECK(output.str().find("bounded log line") != std::string::npos);
  CHECK(output.str().find("explicitly allowed") != std::string::npos);
}

TEST_CASE("One-shot Pod logs prove exact container runtime before log read",
          "[admin][kubernetes][logs]") {
  Dependencies deps;
  Command::Request request{Command::Operation::pod_logs};
  request.target = "cluster";
  request.pod = "broken-pod";
  request.pod_uid = domain::OpsResourceUid::from("pod-uid").value();
  request.container = "app";
  request.allow_log_text = true;
  std::ostringstream output;
  REQUIRE(execute(request, deps, {}, output));
  REQUIRE(deps.state->observations == 2);
  REQUIRE(deps.state->requests.size() == 2);
  CHECK(deps.state->requests[0].operation ==
        domain::OpsObservationOperation::kubernetes_pod_health);
  CHECK(deps.state->requests[1].operation ==
        domain::OpsObservationOperation::kubernetes_pod_logs);
  const auto& source =
      std::get<domain::KubernetesPodIdentity>(deps.state->requests[1].resource);
  REQUIRE(source.container);
  CHECK(source.container->name == "app");
  CHECK(source.container->runtime_identity == "containerd://app");
  CHECK(output.str().find("bounded log line") != std::string::npos);
}

TEST_CASE("One-shot logs refuse absent consent before catalog or source work",
          "[admin][logs][failure]") {
  for (const auto operation :
       {Command::Operation::service_logs, Command::Operation::pod_logs}) {
    Dependencies deps;
    Command::Request request{operation};
    request.unit = operation == Command::Operation::service_logs
                       ? std::optional<std::string>{"Selected.service"}
                       : std::nullopt;
    if (operation == Command::Operation::pod_logs) {
      request.target = "cluster";
      request.pod = "broken-pod";
      request.pod_uid = domain::OpsResourceUid::from("pod-uid").value();
      request.container = "app";
    }
    std::ostringstream output;
    const auto result = execute(request, deps, {}, output);
    REQUIRE_FALSE(result);
    CHECK(result.error().kind == cli::CommandFailureKind::usage);
    CHECK(deps.catalog_calls == 0);
    CHECK(deps.store_calls == 0);
    CHECK(deps.state->observations == 0);
  }
}

TEST_CASE("One-shot logs fail closed when health lacks runtime identity",
          "[admin][logs][failure]") {
  for (const auto operation :
       {Command::Operation::service_logs, Command::Operation::pod_logs}) {
    Dependencies deps;
    deps.state->missing_log_identity = true;
    Command::Request request{operation};
    request.allow_log_text = true;
    if (operation == Command::Operation::service_logs) {
      request.unit = "Selected.service";
    } else {
      request.target = "cluster";
      request.pod = "broken-pod";
      request.pod_uid = domain::OpsResourceUid::from("pod-uid").value();
      request.container = "app";
    }
    std::ostringstream output;
    REQUIRE_FALSE(execute(request, deps, {}, output));
    CHECK(deps.state->observations == 1);
    CHECK(output.str().empty());
  }
}

TEST_CASE("One-shot log cancellation publishes only the completed health proof",
          "[admin][logs][failure][cancellation]") {
  Dependencies deps;
  deps.state->cancel_operation =
      domain::OpsObservationOperation::linux_service_logs;
  Command::Request request{Command::Operation::service_logs};
  request.unit = "Selected.service";
  request.allow_log_text = true;
  std::ostringstream output;
  REQUIRE_FALSE(execute(request, deps, deps.state->stop.get_token(), output));
  CHECK(deps.state->observations == 2);
  CHECK(output.str().empty());
  const auto events = deps.history();
  CHECK(count<domain::OpsObservationRecorded>(events) == 1);
  CHECK(count<domain::InferenceStarted>(events) == 0);
}
