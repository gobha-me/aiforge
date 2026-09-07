#include <aiforge/adapters/process_agent.hpp>

#include <aiforge/adapters/agent_transport.hpp>
#include <aiforge/adapters/process_chat_launch.hpp>
#include <aiforge/adapters/sqlite_session_store.hpp>
#include <aiforge/domain/run_projection.hpp>
#include <map>
#include <utility>

namespace aiforge::adapters {
namespace {
auto command_failure(const surfaces::AgentError& error) -> cli::CommandFailure {
  auto kind = cli::CommandFailureKind::runtime;
  if (error.code == surfaces::AgentErrorCode::invalid_request ||
      error.code == surfaces::AgentErrorCode::resource_exhausted)
    kind = cli::CommandFailureKind::usage;
  if (error.code == surfaces::AgentErrorCode::cancelled)
    kind = cli::CommandFailureKind::cancelled;
  return {kind, error.message};
}

auto report_failure(surfaces::AgentRecordSink& sink,
                    const surfaces::AgentError& error,
                    std::optional<domain::SessionId> session = {})
    -> std::unexpected<cli::CommandFailure> {
  const auto record = surfaces::agent_error_record(error);
  if (!record) return std::unexpected(command_failure(record.error()));
  auto written = sink.write_record(*record);
  if (!written) return std::unexpected(command_failure(error));
  const surfaces::AgentOutcome outcome{
      error.code == surfaces::AgentErrorCode::cancelled
          ? surfaces::AgentStatus::cancelled
          : surfaces::AgentStatus::failed,
      false,
      std::move(session),
      {},
      error.message};
  const auto terminal = surfaces::agent_terminal_record(outcome);
  if (!terminal) return std::unexpected(command_failure(terminal.error()));
  written = sink.write_record(*terminal);
  if (!written) return std::unexpected(command_failure(error));
  return std::unexpected(command_failure(error));
}

auto history(const domain::SessionId& session, const std::stop_token stop)
    -> std::expected<std::vector<domain::RunEvent>, surfaces::AgentError> {
  auto path = process_session_store_path();
  if (!path)
    return std::unexpected(
        surfaces::AgentError{surfaces::AgentErrorCode::unavailable,
                             "session storage path is unavailable"});
  auto store = SqliteSessionStore::open_existing_read_only(*path);
  if (!store)
    return std::unexpected(surfaces::AgentError{
        surfaces::AgentErrorCode::unavailable,
        "session storage could not be opened for reading"});
  auto events = (*store)->replay_events(session, stop);
  if (!events)
    return std::unexpected(surfaces::AgentError{
        stop.stop_requested() ? surfaces::AgentErrorCode::cancelled
                              : surfaces::AgentErrorCode::unavailable,
        events.error().message});
  return std::move(*events);
}

auto unresolved_run(const std::vector<domain::RunEvent>& events)
    -> std::expected<std::optional<domain::RunId>, surfaces::AgentError> {
  std::map<domain::RunId, domain::RunProjection> projections;
  for (const auto& event : events) {
    auto applied = projections[event.metadata.run_id].apply(event);
    if (!applied)
      return std::unexpected(
          surfaces::AgentError{surfaces::AgentErrorCode::run_failed,
                               "session history could not be projected"});
  }
  for (const auto& [run, projection] : projections) {
    const auto status = projection.status();
    if (status != domain::RunStatus::completed &&
        status != domain::RunStatus::failed &&
        status != domain::RunStatus::cancelled)
      return run;
  }
  return std::nullopt;
}

auto inspect_saved_session(const surfaces::AgentRequest& request,
                           const std::stop_token stop,
                           surfaces::AgentRecordSink& sink)
    -> std::expected<bool, cli::CommandFailure> {
  if (!request.session_id) return false;
  {
    auto events = history(*request.session_id, stop);
    if (!events)
      return report_failure(sink, events.error(), request.session_id);
    if (request.operation == surfaces::AgentOperation::replay) {
      auto replayed = surfaces::replay_agent_session(*request.session_id,
                                                     *events, sink, stop);
      if (!replayed)
        return report_failure(sink, replayed.error(), request.session_id);
      return true;
    }
    auto unresolved = unresolved_run(*events);
    if (!unresolved)
      return report_failure(sink, unresolved.error(), request.session_id);
    if (*unresolved) {
      auto terminal = surfaces::agent_terminal_record(
          {surfaces::AgentStatus::recovery_required, false, request.session_id,
           *unresolved,
           "unresolved recovered run requires interactive recovery"});
      if (!terminal)
        return report_failure(sink, terminal.error(), request.session_id);
      const auto written = sink.write_record(*terminal);
      if (!written) return std::unexpected(command_failure(written.error()));
      return std::unexpected(cli::CommandFailure{
          cli::CommandFailureKind::runtime,
          "unresolved recovered run requires interactive recovery"});
    }
  }
  return false;
}

auto execute_request(const surfaces::AgentRequest& request,
                     cli::AgentCommand::Request options,
                     cli::CommandEnvironment& environment,
                     surfaces::AgentRecordSink& sink, std::ostream& output,
                     std::ostream& diagnostics)
    -> std::expected<void, cli::CommandFailure> {
  auto inspected = inspect_saved_session(request, environment.stop_token, sink);
  if (!inspected) return std::unexpected(std::move(inspected.error()));
  if (*inspected) return {};
  cli::InteractiveCommand::Request launch;
  launch.session_mode = request.session_id
                            ? cli::InteractiveCommand::SessionMode::resume
                            : cli::InteractiveCommand::SessionMode::create;
  launch.session_id = request.session_id;
  if (request.model) launch.model = std::string{request.model->value()};
  launch.tool_restriction = std::move(options.tool_restriction);
  launch.tool_approval = std::move(options.tool_approval);
  ProcessAgentExecution agent{request, sink, std::move(options.repository)};
  auto result = execute_process_chat(std::move(launch), environment, output,
                                     diagnostics, &agent);
  if (!result && !agent.terminal_attempted) {
    auto code = surfaces::AgentErrorCode::run_failed;
    if (result.error().kind == cli::CommandFailureKind::usage)
      code = surfaces::AgentErrorCode::invalid_request;
    if (result.error().kind == cli::CommandFailureKind::cancelled)
      code = surfaces::AgentErrorCode::cancelled;
    return report_failure(sink, {code, result.error().message},
                          request.session_id);
  }
  return result;
}
} // namespace

auto ProcessAgentCommand::execute(Request options,
                                  cli::CommandEnvironment& environment,
                                  std::ostream& output,
                                  std::ostream& diagnostics)
    -> std::expected<void, cli::CommandFailure> {
  try {
    if (environment.input_is_terminal)
      return std::unexpected(cli::CommandFailure{
          cli::CommandFailureKind::usage,
          "agent --jsonl requires noninteractive standard input"});
    auto transport = AgentTransport::open(environment.input_descriptor,
                                          environment.output_descriptor,
                                          environment.stop_token);
    if (!transport) return std::unexpected(command_failure(transport.error()));
    auto input = (*transport)->read_request();
    if (!input) return report_failure(**transport, input.error());
    auto request = surfaces::parse_agent_request(*input);
    if (!request) return report_failure(**transport, request.error());
    return execute_request(*request, std::move(options), environment,
                           **transport, output, diagnostics);
  } catch (...) {
    return std::unexpected(cli::CommandFailure{
        cli::CommandFailureKind::runtime, "agent command failed internally"});
  }
}
} // namespace aiforge::adapters
