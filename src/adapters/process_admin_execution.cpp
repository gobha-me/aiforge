#include "process_admin_internal.hpp"
#include <aiforge/adapters/ops_observation_json.hpp>
#include <aiforge/detail/admin_input.hpp>
#include <aiforge/runtime/local_source_worker.hpp>
#include <aiforge/runtime/ops_log_consent.hpp>
#include <aiforge/runtime/ops_observation_history.hpp>
#include <aiforge/surfaces/ops_session.hpp>
#include <algorithm>
#include <chrono>
#include <nlohmann/json.hpp>
#include <ostream>
#include <thread>

namespace aiforge::adapters::admin_detail {
namespace {
using Command = cli::AdminCommand;
using Clock = std::chrono::steady_clock;
auto failure(std::string message,
             cli::CommandFailureKind kind = cli::CommandFailureKind::runtime)
    -> std::unexpected<cli::CommandFailure> {
  return std::unexpected(cli::CommandFailure{kind, std::move(message)});
}
auto stopped(std::stop_token stop, Clock::time_point deadline) -> Result<void> {
  if (stop.stop_requested())
    return failure("Admin observation cancelled",
                   cli::CommandFailureKind::cancelled);
  if (Clock::now() >= deadline)
    return failure("Admin observation deadline expired");
  return {};
}
auto timestamp() -> domain::EventTimestamp {
  return std::chrono::time_point_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now());
}
struct InvocationIdentity {
  domain::SessionId session;
  domain::OpsOwnerId owner;
  domain::OpsConfigurationRevision revision;
  domain::OpsTargetId target;
};
auto invocation_identity(Dependencies& dependencies,
                         const config::OpsTargetConfig& target)
    -> Result<InvocationIdentity> {
  auto instance = dependencies.instance_identity();
  if (!instance || instance->size() > 96)
    return failure("Admin instance identity is unavailable");
  auto session = domain::SessionId::from(*instance);
  auto owner = domain::OpsOwnerId::from(*instance);
  auto revision =
      domain::OpsConfigurationRevision::from(*instance + "-configuration");
  auto target_id = domain::OpsTargetId::from(target.id);
  if (!session || !owner || !revision || !target_id)
    return failure("Admin instance identity is invalid");
  return InvocationIdentity{*session, *owner, *revision, *target_id};
}
auto operation(const Command::Request& request)
    -> Result<domain::OpsObservationOperation> {
  if (request.format != Command::OutputFormat::text &&
      request.format != Command::OutputFormat::json)
    return failure("Invalid Admin output format",
                   cli::CommandFailureKind::usage);
  const bool service = request.operation == Command::Operation::service ||
                       request.operation == Command::Operation::service_logs;
  if (service != request.unit.has_value() ||
      (request.unit && !detail::valid_admin_service(*request.unit)))
    return failure("Admin service requires a canonical .service unit",
                   cli::CommandFailureKind::usage);
  const bool pod_required = request.operation == Command::Operation::pod ||
                            request.operation == Command::Operation::pod_logs;
  const bool pod_allowed =
      pod_required || request.operation == Command::Operation::events;
  if (request.pod.has_value() != request.pod_uid.has_value() ||
      (pod_required && !request.pod) || (!pod_allowed && request.pod) ||
      (request.pod &&
       (!detail::valid_admin_pod(*request.pod) ||
        !detail::valid_admin_resource_uid(request.pod_uid->value()))))
    return failure("Admin Pod requires an exact name and UID",
                   cli::CommandFailureKind::usage);
  const bool logs = request.operation == Command::Operation::service_logs ||
                    request.operation == Command::Operation::pod_logs;
  if (request.allow_log_text != logs ||
      (request.operation == Command::Operation::pod_logs) !=
          request.container.has_value() ||
      (request.container && !detail::valid_admin_container(*request.container)))
    return failure("Admin log reads require explicit allowed exact source",
                   cli::CommandFailureKind::usage);
  switch (request.operation) {
    case Command::Operation::targets:
    case Command::Operation::health:
      return domain::OpsObservationOperation::linux_health;
    case Command::Operation::services:
      return domain::OpsObservationOperation::linux_services;
    case Command::Operation::service:
      return domain::OpsObservationOperation::linux_service_health;
    case Command::Operation::service_logs:
      return domain::OpsObservationOperation::linux_service_logs;
    case Command::Operation::workloads:
      return domain::OpsObservationOperation::kubernetes_workloads;
    case Command::Operation::pod:
      return domain::OpsObservationOperation::kubernetes_pod_health;
    case Command::Operation::events:
      return domain::OpsObservationOperation::kubernetes_events;
    case Command::Operation::pod_logs:
      return domain::OpsObservationOperation::kubernetes_pod_logs;
  }
  return failure("Invalid Admin operation", cli::CommandFailureKind::usage);
}
auto write(std::ostream& output, std::string_view text) -> Result<void> {
  output << text;
  output.flush();
  if (!output)
    return failure("Admin output failed; committed observations remain in "
                   "session storage");
  return {};
}
auto list_targets(const config::OpsTargetsConfig& catalog,
                  Command::OutputFormat format, std::ostream& output)
    -> Result<void> {
  nlohmann::json rows = nlohmann::json::array();
  std::string text;
  for (const auto& target : catalog.targets) {
    const bool local =
        std::holds_alternative<config::LinuxLocalTargetConfig>(target.source);
    const std::string_view kind = local ? "linux_local" : "kubernetes_static";
    const std::string_view capability =
        local ? "health, services, service, "
                "service-logs"
#if defined(__linux__)
              : "workloads, pod, events, pod-logs";
#else
              : "unavailable";
#endif
    if (format == Command::OutputFormat::json)
      rows.push_back({{"id", target.id},
                      {"display_name", target.display_name},
                      {"source", kind},
                      {"operations", capability}});
    else
      text += target.id + "\t" + target.display_name + "\t" +
              std::string{kind} + "\t" + std::string{capability} + '\n';
  }
  if (format == Command::OutputFormat::json)
    text = nlohmann::json{{"schema", "aiforge.admin.targets.v1"},
                          {"targets", std::move(rows)}}
               .dump() +
           '\n';
  return write(output, text);
}
// Declaration order keeps the durable store and source graph alive past kernel
// teardown. Only this standalone invocation owns the worker and issuer.
struct Execution {
  std::shared_ptr<runtime::LocalSourceWorker> worker;
  std::shared_ptr<runtime::OpsObservationBroker> broker;
  std::unique_ptr<storage::SessionStore> store;
  std::shared_ptr<runtime::OpsObservationSource> source;
  std::unique_ptr<surfaces::OpsSession> session;
  std::optional<runtime::OpsSessionLogConsent> consent;
  std::optional<runtime::OpsSourcePreparationToken> preparation;
  ~Execution() {
    if (consent) consent->revoke();
    if (preparation) {
      const auto cancelled = worker->cancel(*preparation);
      static_cast<void>(cancelled);
    }
    if (session) {
      const auto closed = session->close();
      static_cast<void>(closed);
    }
    session.reset();
    if (broker) {
      const auto closed = broker->close();
      static_cast<void>(closed);
    }
  }
};
auto prepare(
    Execution& execution,
    const std::shared_ptr<runtime::OpsSourcePreparationFactory>& factory,
    runtime::OpsSourcePreparationToken token, Clock::time_point deadline,
    std::stop_token stop) -> Result<void> {
  auto submitted = execution.worker->submit(factory, {token, deadline});
  if (!submitted)
    return failure("Admin source preparation could not be admitted");
  execution.preparation = token;
  while (true) {
    auto active = stopped(stop, deadline);
    if (!active) return active;
    auto polled = execution.worker->poll(token);
    if (!polled) return failure("Admin source preparation failed");
    if (*polled) {
      if (!(**polled).result)
        return failure(
            "Admin source is unavailable or changed during preparation");
      execution.source = std::move((**polled).result->source);
      execution.preparation.reset();
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  // Successful claim precedes physical retirement. This worker is exclusively
  // owned here; a shared TUI worker must not wait for unrelated jobs this way.
  while (execution.worker->occupied_slots() != 0) {
    auto active = stopped(stop, deadline);
    if (!active) return active;
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  return stopped(stop, deadline);
}
auto current_result(const surfaces::ManualOpsInspection& view,
                    const surfaces::ObservationSubmission& submitted)
    -> Result<const surfaces::CommittedOpsObservation*> {
  const auto& current = view.projection.current;
  if (!current || current->submission != submitted)
    return failure("Admin current observation identity is unavailable");
  if (current->status == domain::RunStatus::failed ||
      current->status == domain::RunStatus::cancelled)
    return failure("Admin observation failed; verify target identity and "
                   "read permissions");
  if (current->status == domain::RunStatus::completed) {
    const auto& committed = view.projection.latest_success;
    if (!committed || committed->submission != submitted ||
        !current->observation_event_id ||
        *current->observation_event_id != committed->observation_event_id)
      return failure("Admin observation has no committed evidence");
    return &*committed;
  }
  return nullptr;
}
auto collect(Execution& execution, runtime::OpsObservationIntent intent,
             Clock::time_point deadline, std::stop_token stop)
    -> Result<surfaces::CommittedOpsObservation> {
  auto active = stopped(stop, deadline);
  if (!active) return std::unexpected(active.error());
  intent.limits.timeout = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - Clock::now());
  if (intent.limits.timeout.count() <= 0)
    return failure("Admin observation deadline expired");
  auto submitted = execution.session->submit_observation(std::move(intent));
  if (!submitted) return failure("Admin observation could not be admitted");
  while (true) {
    active = stopped(stop, deadline);
    if (!active) {
      auto cancelled = execution.session->cancel_observation(submitted->run_id);
      if (!cancelled)
        return failure("Admin cancellation could not be committed");
      return std::unexpected(active.error());
    }
    auto pumped = execution.session->pump_observations();
    if (!pumped) return failure("Admin observation could not be committed");
    const auto& view = execution.session->inspect_observations();
    if (view.approval) {
      auto cancelled = execution.session->cancel_observation(submitted->run_id);
      if (!cancelled)
        return failure("Admin cancellation could not be committed");
      return failure("Admin observation requires interactive approval; no "
                     "approval was granted");
    }
    auto committed = current_result(view, *submitted);
    if (!committed) return std::unexpected(committed.error());
    if (*committed != nullptr) return **committed;
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
}
auto render(const surfaces::CommittedOpsObservation& value,
            const config::OpsTargetConfig& target, Command::OutputFormat format,
            std::ostream& output) -> Result<void> {
  if (format == Command::OutputFormat::json) {
    auto encoded = encode_ops_observation(value.observation);
    if (!encoded) return failure("Admin observation encoding failed");
    // Codec JSON is already strict/bounded; wrap without parsing/copying a DOM.
    const auto label = nlohmann::json(target.display_name).dump();
    auto envelope = std::string{
        R"({"schema":"aiforge.admin.observation.v1","display_name":)"};
    envelope += label;
    envelope += R"(,"freshness":"last_success","observation":)";
    envelope += *encoded;
    envelope += "}\n";
    return write(output, envelope);
  }
  auto content = runtime::format_ops_observation_content(value.observation);
  if (!content || content->size() != 1)
    return failure("Admin observation formatting failed");
  const auto* text = std::get_if<domain::TextBlock>(&content->front());
  if (text == nullptr) return failure("Admin observation formatting failed");
  const auto logs = value.observation.request.operation ==
                        domain::OpsObservationOperation::linux_service_logs ||
                    value.observation.request.operation ==
                        domain::OpsObservationOperation::kubernetes_pod_logs;
  return write(output, "Target: " + target.display_name + " (" + target.id +
                           ")\nFreshness: last_success\n"
                           "Mode: Observe; bounded native reads; logs " +
                           (logs ? "explicitly allowed\n" : "disabled\n") +
                           text->text);
}
auto exact_log_source(const Command::Request& request,
                      const domain::OpsObservation& proof)
    -> Result<domain::OpsResourceIdentity> {
  if (request.operation == Command::Operation::service_logs) {
    const auto* service =
        std::get_if<domain::LinuxServiceObservation>(&proof.payload);
    const auto* requested =
        std::get_if<domain::LinuxServiceIdentity>(&proof.request.resource);
    if (proof.request.operation !=
            domain::OpsObservationOperation::linux_service_health ||
        service == nullptr || requested == nullptr || !request.unit ||
        !service->identity.invocation_id ||
        requested->unit_name != *request.unit || requested->invocation_id ||
        service->identity.unit_name != *request.unit)
      return failure("Admin service invocation identity is unavailable");
    return service->identity;
  }
  const auto* pod =
      std::get_if<domain::KubernetesPodObservation>(&proof.payload);
  const auto* requested =
      std::get_if<domain::KubernetesPodIdentity>(&proof.request.resource);
  if (proof.request.operation !=
          domain::OpsObservationOperation::kubernetes_pod_health ||
      pod == nullptr || requested == nullptr || !request.pod ||
      !request.pod_uid || requested->container || pod->identity != *requested ||
      !request.container || pod->identity.name != *request.pod ||
      pod->identity.uid != *request.pod_uid)
    return failure("Admin Pod identity is unavailable");
  const auto found =
      std::ranges::find(pod->containers, *request.container,
                        &domain::KubernetesContainerObservation::name);
  if (found == pod->containers.end() || !found->runtime_identity)
    return failure("Admin container runtime identity is unavailable");
  auto source = pod->identity;
  source.container = domain::KubernetesContainerIdentity{
      found->name, *found->runtime_identity};
  return source;
}
[[nodiscard]] auto is_log_operation(domain::OpsObservationOperation operation)
    -> bool {
  return operation == domain::OpsObservationOperation::linux_service_logs ||
         operation == domain::OpsObservationOperation::kubernetes_pod_logs;
}
[[nodiscard]] auto log_proof_operation(
    domain::OpsObservationOperation operation)
    -> domain::OpsObservationOperation {
  return operation == domain::OpsObservationOperation::linux_service_logs
             ? domain::OpsObservationOperation::linux_service_health
             : domain::OpsObservationOperation::kubernetes_pod_health;
}
auto start_execution(Execution& execution, const InvocationIdentity& identity,
                     Dependencies& dependencies)
    -> Result<std::shared_ptr<runtime::OpsObservationEndpoint>> {
  auto worker = runtime::LocalSourceWorker::create(1);
  if (!worker) return failure("Admin worker is unavailable");
  execution.worker = std::move(*worker);
  auto broker = runtime::OpsObservationBroker::create(execution.worker);
  if (!broker) return failure("Admin broker is unavailable");
  execution.broker = std::move(*broker);
  runtime::ApplicationLaunchContextConfiguration configuration;
  configuration.approval_mode = runtime::ApprovalMode::allow_all;
  auto launch = runtime::make_application_launch_context(configuration);
  if (!launch) return failure("Admin Observe policy is unavailable");
  auto store = dependencies.open_store();
  if (!store) return std::unexpected(store.error());
  execution.store = std::move(*store);
  if (!execution.store) return failure("Admin storage is unavailable");
  auto session = surfaces::OpsSession::open(
      {identity.session, timestamp(),
       domain::SurfaceId::from("admin-cli").value(),
       domain::WorkspaceId::from("ops").value()},
      *execution.store,
      {{domain::PermissionProfileId::from("observe").value(), *launch, {}},
       execution.broker,
       [suffix = std::uint64_t{}]() mutable { return ++suffix; }});
  if (!session) return failure("Admin durable session could not be created");
  execution.session = std::move(*session);
  auto endpoint = execution.broker->activate_session(identity.session);
  if (!endpoint) return failure("Admin session activation failed");
  return *endpoint;
}
auto prepare_source(Execution& execution, const InvocationIdentity& identity,
                    const config::OpsTargetConfig& target,
                    Dependencies& dependencies, Clock::time_point deadline,
                    std::stop_token stop) -> Result<void> {
  auto request_id = execution.worker->allocate_request_id();
  if (!request_id) return failure("Admin preparation identity exhausted");
  const auto kind =
      std::holds_alternative<config::LinuxLocalTargetConfig>(target.source)
          ? domain::OpsTargetKind::linux_local
          : domain::OpsTargetKind::kubernetes;
  runtime::OpsSourcePreparationIdentity selection{identity.target,
                                                  identity.revision, kind};
  auto factory = dependencies.factory(target, identity.revision);
  if (!factory) return std::unexpected(factory.error());
  return prepare(execution, *factory,
                 {identity.session, 1, *request_id, selection}, deadline, stop);
}
auto initial_authority_spec(const InvocationIdentity& identity,
                            const Execution& execution,
                            domain::OpsObservationOperation selected)
    -> domain::OpsObservationAuthoritySpec {
  std::vector<domain::OpsObservationOperation> operations{selected};
  if (is_log_operation(selected))
    operations.insert(operations.begin(), log_proof_operation(selected));
  return {identity.owner,
          identity.session,
          execution.source->target_binding(),
          1,
          std::move(operations),
          {},
          {}};
}
auto bind_initial_authority(
    Execution& execution, const InvocationIdentity& identity,
    domain::OpsObservationOperation selected,
    const std::shared_ptr<runtime::OpsObservationEndpoint>& endpoint)
    -> Result<void> {
  auto spec = initial_authority_spec(identity, execution, selected);
  std::expected<domain::OpsObservationAuthority, domain::OpsTargetError>
      authority = domain::OpsObservationAuthority::create(spec);
  if (is_log_operation(selected)) {
    auto consent = runtime::OpsSessionLogConsent::start(*endpoint, spec);
    if (!consent) return failure("Admin log consent is unavailable");
    execution.consent.emplace(std::move(*consent));
    authority = execution.consent->authority();
  }
  if (!authority) return failure("Admin source authority is unavailable");
  auto bound = execution.session->bind_observation(*authority, execution.source,
                                                   endpoint);
  if (!bound) return failure("Admin source binding failed");
  return {};
}
auto make_intent(const Command::Request& request, const Execution& execution,
                 const InvocationIdentity& identity,
                 domain::OpsObservationOperation operation)
    -> Result<runtime::OpsObservationIntent> {
  runtime::OpsObservationIntent intent{identity.target, 1, operation};
  if (request.unit)
    intent.resource = domain::LinuxServiceIdentity{*request.unit};
  if (request.pod) {
    const auto* source_identity = std::get_if<domain::KubernetesOpsIdentity>(
        &execution.source->target_binding().identity);
    if (source_identity == nullptr || !request.pod_uid)
      return failure("Admin Pod identity is unavailable");
    intent.resource = domain::KubernetesPodIdentity{
        source_identity->namespace_name, *request.pod, *request.pod_uid, {}};
  }
  return intent;
}
auto collect_log(
    const Command::Request& request, Execution& execution,
    runtime::OpsSessionLogConsent& consent, const InvocationIdentity& identity,
    domain::OpsObservationOperation selected,
    const std::shared_ptr<runtime::OpsObservationEndpoint>& endpoint,
    const surfaces::CommittedOpsObservation& proof, Clock::time_point deadline,
    std::stop_token stop) -> Result<surfaces::CommittedOpsObservation> {
  auto source = exact_log_source(request, proof.observation);
  if (!source) return std::unexpected(source.error());
  const auto current = consent.authority();
  if (!current) return failure("Admin log consent is unavailable");
  const auto& current_spec = current->specification();
  auto enabled =
      consent.apply({identity.session, execution.source->target_binding(),
                     current_spec.selection_generation,
                     current_spec.logs.revision, *source, true});
  if (!enabled) return failure("Admin log consent could not be enabled");
  auto bound =
      execution.session->bind_observation(*enabled, execution.source, endpoint);
  if (!bound) {
    consent.revoke();
    return failure("Admin log source binding failed");
  }
  runtime::OpsObservationIntent intent{
      identity.target,
      enabled->specification().selection_generation,
      selected,
      std::move(*source),
      {}};
  return collect(execution, std::move(intent), deadline, stop);
}
auto observe(const Command::Request& request,
             const config::OpsTargetConfig& target,
             domain::OpsObservationOperation selected,
             Dependencies& dependencies, std::stop_token stop,
             std::ostream& output) -> Result<void> {
  auto identity = invocation_identity(dependencies, target);
  if (!identity) return std::unexpected(identity.error());
  Execution execution;
  auto endpoint = start_execution(execution, *identity, dependencies);
  if (!endpoint) return std::unexpected(endpoint.error());
  const auto deadline = Clock::now() + std::chrono::seconds{5};
  auto prepared = prepare_source(execution, *identity, target, dependencies,
                                 deadline, stop);
  if (!prepared) return prepared;
  auto bound =
      bind_initial_authority(execution, *identity, selected, *endpoint);
  if (!bound) return bound;
  const bool logs = is_log_operation(selected);
  auto intent = make_intent(request, execution, *identity,
                            logs ? log_proof_operation(selected) : selected);
  if (!intent) return std::unexpected(intent.error());
  auto result = collect(execution, std::move(*intent), deadline, stop);
  if (!result) return std::unexpected(result.error());
  if (logs) {
    if (!execution.consent.has_value())
      return failure("Admin log consent is unavailable");
    result =
        collect_log(request, execution, execution.consent.value(), *identity,
                    selected, *endpoint, *result, deadline, stop);
    if (!result) return std::unexpected(result.error());
  }
  return render(*result, target, request.format, output);
}
} // namespace

auto execute(const Command::Request& request, Dependencies& dependencies,
             std::stop_token stop, std::ostream& output) -> Result<void> {
  try {
    auto selected = operation(request);
    if (!selected) return std::unexpected(selected.error());
    if (stop.stop_requested())
      return failure("Admin observation cancelled",
                     cli::CommandFailureKind::cancelled);
    auto catalog = dependencies.load_catalog();
    if (!catalog) return std::unexpected(catalog.error());
    if (request.operation == Command::Operation::targets) {
      if (request.target != "local")
        return failure("Admin targets does not select a target",
                       cli::CommandFailureKind::usage);
      return list_targets(*catalog, request.format, output);
    }
    const auto found = std::ranges::find(catalog->targets, request.target,
                                         &config::OpsTargetConfig::id);
    if (found == catalog->targets.end())
      return failure("Unknown Admin target; use admin targets",
                     cli::CommandFailureKind::usage);
    const bool local =
        std::holds_alternative<config::LinuxLocalTargetConfig>(found->source);
    const bool linux_operation =
        request.operation == Command::Operation::health ||
        request.operation == Command::Operation::services ||
        request.operation == Command::Operation::service ||
        request.operation == Command::Operation::service_logs;
    if (local != linux_operation)
      return failure(local
                         ? "This Admin operation requires a Kubernetes target"
                         : "This Admin operation requires a Linux-local target",
                     cli::CommandFailureKind::usage);
    return observe(request, *found, *selected, dependencies, stop, output);
  } catch (...) {
    return std::unexpected(
        cli::CommandFailure{cli::CommandFailureKind::runtime, {}});
  }
}
} // namespace aiforge::adapters::admin_detail
