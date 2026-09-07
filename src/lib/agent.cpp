#include <aiforge/surfaces/agent.hpp>

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>

namespace aiforge::surfaces {
namespace {
using Result = std::expected<void, AgentError>;

auto failure(const AgentErrorCode code, std::string message)
    -> std::unexpected<AgentError> {
  return std::unexpected(AgentError{code, std::move(message)});
}

auto emit(AgentRecordSink& sink, std::expected<std::string, AgentError> record)
    -> Result {
  if (!record) return std::unexpected(std::move(record.error()));
  return sink.write_record(*record);
}

auto emit_events(ChatSession& session, AgentRecordSink& sink,
                 std::size_t& emitted) -> Result {
  const auto& events = session.event_log().events();
  while (emitted < events.size()) {
    auto result =
        emit(sink, agent_event_record(session.session_id(), events[emitted]));
    if (!result) return result;
    ++emitted;
  }
  return {};
}

auto cancel_and_drain(ChatSession& session, const AgentRunLimits limits)
    -> Result {
  auto cancelled = session.cancel_active("noninteractive agent stopped");
  if (!cancelled)
    return failure(AgentErrorCode::run_failed, cancelled.error().message);
  const auto deadline =
      std::chrono::steady_clock::now() + limits.cleanup_timeout;
  do {
    auto drained = session.drain();
    if (!drained)
      return failure(AgentErrorCode::run_failed, drained.error().message);
    if (!session.active()) return {};
    if (std::chrono::steady_clock::now() >= deadline)
      return failure(AgentErrorCode::cleanup_incomplete,
                     "cancellation accounting incomplete; reopen the session");
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  } while (true);
}

auto select_tools(ChatSession& session, const AgentRequest& request)
    -> std::expected<std::vector<backend::ToolDeclaration>, AgentError> {
  if (!request.profile || request.tools.empty() || request.tools.size() > 2)
    return failure(AgentErrorCode::invalid_request,
                   "agent requires an explicit profile and tools");
  for (const auto& name : request.tools)
    if (name != "read_repository_file" && name != "run_process")
      return failure(AgentErrorCode::invalid_request,
                     "agent tool is unsupported");
  auto selected = session.select_tool_profile(*request.profile);
  if (!selected)
    return failure(AgentErrorCode::unavailable, selected.error().message);
  auto state = session.tool_profile_state();
  if (!state)
    return failure(AgentErrorCode::unavailable, state.error().message);
  for (const auto& name : state->selected_profile.tool_names) {
    const bool enabled =
        std::ranges::find(request.tools, name) != request.tools.end();
    auto changed = session.set_tool_enabled(name, enabled);
    if (!changed)
      return failure(AgentErrorCode::unavailable, changed.error().message);
  }
  state = session.tool_profile_state();
  if (!state)
    return failure(AgentErrorCode::unavailable, state.error().message);
  for (const auto& name : request.tools) {
    if (state->effective_tools.find(name) == nullptr)
      return failure(AgentErrorCode::unavailable,
                     "requested agent tool is unavailable under current "
                     "profile or policy: " +
                         name);
  }
  if (state->effective_tools.size() != request.tools.size())
    return failure(AgentErrorCode::unavailable,
                   "agent tool selection is not exact");
  return state->effective_tools.declarations();
}

auto observed_outcome(const ChatSession& session, const domain::RunId& run,
                      const AgentStatus requested) -> AgentOutcome {
  AgentOutcome result{requested, false, session.session_id(), run, {}};
  for (const auto& event : session.event_log().events()) {
    if (event.metadata.run_id != run) continue;
    if (std::holds_alternative<domain::RunCompleted>(event.payload)) {
      result.durable_terminal = session.durable() && !session.active();
      if (requested == AgentStatus::completed)
        result.status = AgentStatus::completed;
    } else if (std::holds_alternative<domain::RunFailed>(event.payload)) {
      result.durable_terminal = session.durable() && !session.active();
      if (requested == AgentStatus::completed)
        result.status = AgentStatus::failed;
    } else if (std::holds_alternative<domain::RunCancelled>(event.payload)) {
      result.durable_terminal = session.durable() && !session.active();
      if (requested == AgentStatus::completed)
        result.status = AgentStatus::cancelled;
    }
  }
  if (!result.durable_terminal && result.status == AgentStatus::completed)
    result.status = AgentStatus::failed;
  return result;
}

auto stopped_output(ChatSession& session, AgentError error,
                    const AgentRunLimits limits)
    -> std::unexpected<AgentError> {
  const auto cancelled = cancel_and_drain(session, limits);
  if (!cancelled) return std::unexpected(std::move(cancelled.error()));
  return std::unexpected(std::move(error));
}
} // namespace

auto run_agent_session(ChatSession& session, const AgentRequest& request,
                       AgentRecordSink& sink, const std::stop_token stop_token,
                       const AgentRunLimits limits)
    -> std::expected<AgentOutcome, AgentError> {
  try {
    if (request.operation != AgentOperation::submit)
      return failure(AgentErrorCode::invalid_request,
                     "agent driver requires a submit request");
    if (!session.durable())
      return failure(AgentErrorCode::unavailable,
                     "agent requires durable storage");
    if ((request.session_id && *request.session_id != session.session_id()) ||
        (request.model && *request.model != session.model_id()))
      return failure(AgentErrorCode::invalid_request,
                     "agent request does not match the opened session");
    if (limits.run_timeout <= std::chrono::milliseconds::zero() ||
        limits.cleanup_timeout <= std::chrono::milliseconds::zero())
      return failure(AgentErrorCode::invalid_request,
                     "agent deadlines must be positive");
    // Opening history is inert. Never drain a recovered pending invocation.
    if (session.active() || session.blocked_recovery()) {
      AgentOutcome result{AgentStatus::recovery_required,
                          false,
                          session.session_id(),
                          {},
                          "unresolved recovered run"};
      if (session.blocked_recovery()) {
        result.run_id = session.blocked_recovery()->run_id;
        result.reason = session.blocked_recovery()->reason.message;
      } else if (!session.event_log().events().empty()) {
        result.run_id = session.event_log().events().back().metadata.run_id;
      }
      auto written = emit(sink, agent_terminal_record(result));
      if (!written) return std::unexpected(std::move(written.error()));
      return result;
    }
    if (stop_token.stop_requested())
      return failure(AgentErrorCode::cancelled,
                     "agent cancelled before submission");
    auto tools = select_tools(session, request);
    if (!tools) return std::unexpected(std::move(tools.error()));
    auto emitted = session.event_log().events().size();
    auto submitted = session.submit(request.prompt);
    if (!submitted)
      return failure(AgentErrorCode::run_failed, submitted.error().message);
    auto accepted =
        emit(sink, agent_accepted_record(session.session_id(),
                                         submitted->run_id, *tools));
    if (!accepted)
      return stopped_output(session, std::move(accepted.error()), limits);
    auto requested = AgentStatus::completed;
    const auto deadline = std::chrono::steady_clock::now() + limits.run_timeout;
    while (session.active()) {
      if (stop_token.stop_requested() ||
          std::chrono::steady_clock::now() >= deadline) {
        requested = stop_token.stop_requested() ? AgentStatus::cancelled
                                                : AgentStatus::failed;
        auto cancelled = cancel_and_drain(session, limits);
        if (!cancelled) {
          auto written = emit(sink, agent_error_record(cancelled.error()));
          if (!written) return std::unexpected(std::move(written.error()));
          break;
        }
      } else {
        auto drained = session.drain();
        if (!drained) {
          requested = AgentStatus::failed;
          auto error =
              AgentError{AgentErrorCode::run_failed, drained.error().message};
          auto written = emit(sink, agent_error_record(error));
          if (!written)
            return stopped_output(session, std::move(written.error()), limits);
          auto cancelled = cancel_and_drain(session, limits);
          if (!cancelled) break;
        }
      }
      auto written = emit_events(session, sink, emitted);
      if (!written)
        return stopped_output(session, std::move(written.error()), limits);
      if (session.pending_question_input() || session.pending_tool_approval()) {
        requested = AgentStatus::interaction_required;
        auto cancelled = cancel_and_drain(session, limits);
        if (!cancelled) break;
      }
      if (session.active())
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    auto written = emit_events(session, sink, emitted);
    if (!written)
      return stopped_output(session, std::move(written.error()), limits);
    auto result = observed_outcome(session, submitted->run_id, requested);
    if (requested == AgentStatus::failed &&
        std::chrono::steady_clock::now() >= deadline)
      result.reason = "run deadline exceeded";
    written = emit(sink, agent_terminal_record(result));
    if (!written) return std::unexpected(std::move(written.error()));
    return result;
  } catch (...) {
    static_cast<void>(cancel_and_drain(session, limits));
    return failure(AgentErrorCode::internal_failure,
                   "agent run failed internally");
  }
}

auto replay_agent_session(const domain::SessionId& session,
                          const std::vector<domain::RunEvent>& events,
                          AgentRecordSink& sink,
                          const std::stop_token stop_token)
    -> std::expected<AgentOutcome, AgentError> {
  try {
    for (const auto& event : events) {
      if (stop_token.stop_requested())
        return failure(AgentErrorCode::cancelled, "agent replay cancelled");
      auto written = emit(sink, agent_event_record(session, event));
      if (!written) return std::unexpected(std::move(written.error()));
    }
    // This describes completed replay, never a terminal claim about its runs.
    const AgentOutcome result{
        AgentStatus::completed, false, session, {}, "replay completed"};
    auto written = emit(sink, agent_terminal_record(result));
    if (!written) return std::unexpected(std::move(written.error()));
    return result;
  } catch (...) {
    return failure(AgentErrorCode::internal_failure,
                   "agent replay failed internally");
  }
}
} // namespace aiforge::surfaces
