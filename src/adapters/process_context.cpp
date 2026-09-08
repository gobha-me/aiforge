#include <aiforge/adapters/process_context.hpp>

#include <aiforge/adapters/context_jsonl.hpp>
#include <aiforge/adapters/process_chat_launch.hpp>
#include <aiforge/adapters/sqlite_session_store.hpp>
#include <aiforge/domain/run_projection.hpp>
#include <aiforge/surfaces/context_control.hpp>
#include <chrono>
#include <map>
#include <random>
#include <thread>

namespace aiforge::adapters {
namespace {
using Result = std::expected<void, cli::CommandFailure>;
auto failure(std::string message) -> std::unexpected<cli::CommandFailure> {
  return std::unexpected(cli::CommandFailure{cli::CommandFailureKind::runtime,
                                             std::move(message)});
}
auto write(AgentTransport& transport, const surfaces::ContextReply& reply)
    -> Result {
  auto encoded = encode_context_reply(reply);
  if (!encoded) return failure(encoded.error().message);
  auto written = transport.write_record(*encoded);
  if (!written) return failure(written.error().message);
  return {};
}
auto drain(surfaces::ContextController& controller, AgentTransport& transport)
    -> Result {
  auto result = controller.poll();
  if (!result) return failure(result.error().message);
  if (*result) return write(transport, **result);
  return {};
}
auto cleanup(surfaces::ContextController& controller, AgentTransport& transport)
    -> Result {
  auto cancelled = controller.cancel_producer();
  if (!cancelled) return failure(cancelled.error().message);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{2};
  while (controller.producer_active()) {
    auto result = drain(controller, transport);
    if (!result) return result;
    if (std::chrono::steady_clock::now() >= deadline)
      return failure(
          "summary cancellation cleanup did not finish before the deadline");
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  return drain(controller, transport);
}
auto loop(surfaces::ChatSession& session,
          surfaces::ContextController& controller, AgentTransport& transport,
          std::stop_token stop) -> Result {
  while (!controller.closed() && !stop.stop_requested()) {
    auto result = drain(controller, transport);
    if (!result) return result;
    auto line = transport.poll_line();
    if (!line) return failure(line.error().message);
    if (line->state == TransportLineState::end) return {};
    if (line->state == TransportLineState::idle) continue;
    auto request = parse_context_request(line->text);
    const auto reply =
        request ? controller.execute(*request)
                : surfaces::ContextReply{0, session.event_log().last_sequence(),
                                         request.error()};
    result = write(transport, reply);
    if (!result) return result;
  }
  if (stop.stop_requested())
    return std::unexpected(cli::CommandFailure{
        cli::CommandFailureKind::cancelled, "context control cancelled"});
  return {};
}
auto inspect_session(const domain::SessionId& session, std::stop_token stop)
    -> Result {
  auto path = process_session_store_path();
  if (!path) return failure("session storage path is unavailable");
  auto store = SqliteSessionStore::open_existing_read_only(*path);
  if (!store) return failure("session storage could not be opened for reading");
  auto events = (*store)->replay_events(session, stop);
  if (!events) return failure(events.error().message);
  std::map<domain::RunId, domain::RunProjection> runs;
  for (const auto& event : *events) {
    auto applied = runs[event.metadata.run_id].apply(event);
    if (!applied) return failure("session history could not be projected");
  }
  for (const auto& [id, run] : runs) {
    static_cast<void>(id);
    if (run.status() != domain::RunStatus::completed &&
        run.status() != domain::RunStatus::failed &&
        run.status() != domain::RunStatus::cancelled)
      return failure(
          "unfinished session run requires recovery before context control");
  }
  return {};
}
auto instance_identity() -> std::string {
  std::random_device random;
  std::string result;
  for (int index = 0; index < 4; ++index) {
    if (index != 0) result += '-';
    result += std::to_string(random());
  }
  return result;
}
} // namespace

auto run_context_jsonl(surfaces::ChatSession& session,
                       AgentTransport& transport, std::stop_token stop,
                       std::string identity) -> Result {
  if (session.active())
    return failure(
        "unfinished session run requires recovery before context control");
  surfaces::ContextController controller{session, std::move(identity)};
  try {
    auto result = loop(session, controller, transport, stop);
    auto cleaned = cleanup(controller, transport);
    if (!cleaned) return cleaned;
    return result;
  } catch (...) {
    const auto cancelled = controller.cancel_producer();
    if (!cancelled) return failure(cancelled.error().message);
    return failure("context control failed internally");
  }
}
auto ProcessContextCommand::execute(Request request,
                                    cli::CommandEnvironment& environment,
                                    std::ostream& output,
                                    std::ostream& diagnostics) -> Result {
  try {
    auto inspected =
        inspect_session(request.session_id, environment.stop_token);
    if (!inspected) return inspected;
    auto transport = AgentTransport::open(environment.input_descriptor,
                                          environment.output_descriptor,
                                          environment.stop_token);
    if (!transport) return failure(transport.error().message);
    cli::InteractiveCommand::Request launch;
    launch.session_mode = cli::InteractiveCommand::SessionMode::resume;
    launch.session_id = request.session_id;
    launch.model = std::string{request.model_id.value()};
    ProcessHeadlessExecution execution{[&](surfaces::ChatSession& session) {
      return run_context_jsonl(session, **transport, environment.stop_token,
                               instance_identity());
    }};
    return execute_process_chat(std::move(launch), environment, output,
                                diagnostics, nullptr, &execution);
  } catch (...) {
    return failure("context control could not be opened");
  }
}
} // namespace aiforge::adapters
