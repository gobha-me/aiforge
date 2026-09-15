#include "process_admin_internal.hpp"
#include <aiforge/adapters/ops_observation_json.hpp>
#include <aiforge/runtime/local_source_worker.hpp>
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
auto valid_unit(std::string_view value) -> bool {
  return value.size() > 8 && value.size() <= 255 &&
         value.ends_with(".service") && value.front() != '-' &&
         std::ranges::all_of(value, [](char c) {
           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' ||
                  c == '@' || c == ':';
         });
}
auto operation(const Command::Request& request)
    -> Result<domain::OpsObservationOperation> {
  if (request.format != Command::OutputFormat::text &&
      request.format != Command::OutputFormat::json)
    return failure("Invalid Admin output format",
                   cli::CommandFailureKind::usage);
  if ((request.operation == Command::Operation::service) !=
          request.unit.has_value() ||
      (request.unit && !valid_unit(*request.unit)))
    return failure("Admin service requires a canonical .service unit",
                   cli::CommandFailureKind::usage);
  switch (request.operation) {
    case Command::Operation::targets:
    case Command::Operation::health:
      return domain::OpsObservationOperation::linux_health;
    case Command::Operation::services:
      return domain::OpsObservationOperation::linux_services;
    case Command::Operation::service:
      return domain::OpsObservationOperation::linux_service_health;
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
        local ? "health, services, service" : "unavailable";
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
  std::optional<runtime::OpsSourcePreparationToken> preparation;
  ~Execution() {
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
    return write(output,
                 R"({"schema":"aiforge.admin.observation.v1","display_name":)" +
                     label + ",\"observation\":" + *encoded + "}\n");
  }
  auto content = runtime::format_ops_observation_content(value.observation);
  if (!content || content->size() != 1)
    return failure("Admin observation formatting failed");
  const auto* text = std::get_if<domain::TextBlock>(&content->front());
  if (text == nullptr) return failure("Admin observation formatting failed");
  return write(output,
               "Target: " + target.display_name + " (" + target.id +
                   ")\nMode: Observe; bounded native reads; logs disabled\n" +
                   text->text);
}
auto observe(const Command::Request& request,
             const config::OpsTargetConfig& target,
             domain::OpsObservationOperation selected,
             Dependencies& dependencies, std::stop_token stop,
             std::ostream& output) -> Result<void> {
  auto identity = dependencies.instance_identity();
  if (!identity || identity->size() > 96)
    return failure("Admin instance identity is unavailable");
  auto session_id = domain::SessionId::from(*identity);
  auto owner_id = domain::OpsOwnerId::from(*identity);
  auto revision =
      domain::OpsConfigurationRevision::from(*identity + "-configuration");
  auto target_id = domain::OpsTargetId::from(target.id);
  if (!session_id || !owner_id || !revision || !target_id)
    return failure("Admin instance identity is invalid");
  Execution execution;
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
      {*session_id, timestamp(), domain::SurfaceId::from("admin-cli").value(),
       domain::WorkspaceId::from("ops").value()},
      *execution.store,
      {{domain::PermissionProfileId::from("observe").value(), *launch, {}},
       execution.broker,
       [suffix = std::uint64_t{}]() mutable { return ++suffix; }});
  if (!session) return failure("Admin durable session could not be created");
  execution.session = std::move(*session);
  auto endpoint = execution.broker->activate_session(*session_id);
  if (!endpoint) return failure("Admin session activation failed");
  const auto deadline = Clock::now() + std::chrono::seconds{5};
  auto request_id = execution.worker->allocate_request_id();
  if (!request_id) return failure("Admin preparation identity exhausted");
  runtime::OpsSourcePreparationIdentity selection{
      *target_id, *revision, domain::OpsTargetKind::linux_local};
  auto factory = dependencies.factory(selection);
  if (!factory) return std::unexpected(factory.error());
  auto prepared =
      prepare(execution, *factory, {*session_id, 1, *request_id, selection},
              deadline, stop);
  if (!prepared) return prepared;
  domain::OpsObservationAuthoritySpec spec{*owner_id,
                                           *session_id,
                                           execution.source->target_binding(),
                                           1,
                                           {selected}};
  auto authority = domain::OpsObservationAuthority::create(spec);
  if (!authority) return failure("Admin source authority is unavailable");
  auto bound = execution.session->bind_observation(*authority, execution.source,
                                                   *endpoint);
  if (!bound) return failure("Admin source binding failed");
  runtime::OpsObservationIntent intent{*target_id, 1, selected};
  if (request.unit)
    intent.resource = domain::LinuxServiceIdentity{*request.unit};
  auto result = collect(execution, std::move(intent), deadline, stop);
  if (!result) return std::unexpected(result.error());
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
    if (!std::holds_alternative<config::LinuxLocalTargetConfig>(found->source))
      return failure("This Admin operation requires a Linux-local target; "
                     "Kubernetes collection is unavailable");
    return observe(request, *found, *selected, dependencies, stop, output);
  } catch (...) {
    return std::unexpected(
        cli::CommandFailure{cli::CommandFailureKind::runtime, {}});
  }
}
} // namespace aiforge::adapters::admin_detail
