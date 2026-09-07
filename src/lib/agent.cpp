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

struct RunBudget {
  std::chrono::steady_clock::time_point started{
      std::chrono::steady_clock::now()};
  std::chrono::milliseconds timeout;
  std::stop_token stop;

  [[nodiscard]] auto check() const -> Result {
    if (stop.stop_requested())
      return failure(AgentErrorCode::cancelled, "agent cancelled");
    if (std::chrono::steady_clock::now() - started >= timeout)
      return failure(AgentErrorCode::run_failed, "run deadline exceeded");
    return {};
  }
};

struct Delivery {
  AgentRecordSink& sink;
  const RunBudget& budget;
  bool usable{true};
  std::size_t emitted{};

  auto write(std::expected<std::string, AgentError> record,
             const bool event = false) -> Result {
    if (auto ready = budget.check(); !ready) return ready;
    if (!record) return std::unexpected(std::move(record.error()));
    auto written = sink.write_record(*record);
    if (!written) {
      usable = false;
      return written;
    }
    // A successful write remains delivered even if it consumed the budget.
    if (event) ++emitted;
    return budget.check();
  }

  auto events(const ChatSession& session) -> Result {
    const auto& events = session.event_log().events();
    while (emitted < events.size()) {
      auto written = write(
          agent_event_record(session.session_id(), events[emitted]), true);
      if (!written) return written;
    }
    return budget.check();
  }
};

auto has_terminal(const ChatSession& session, const domain::RunId& run)
    -> bool {
  return std::ranges::any_of(
      session.event_log().events(), [&](const auto& event) {
        return event.metadata.run_id == run &&
               (std::holds_alternative<domain::RunCompleted>(event.payload) ||
                std::holds_alternative<domain::RunFailed>(event.payload) ||
                std::holds_alternative<domain::RunCancelled>(event.payload));
      });
}

auto cancel_and_drain(ChatSession& session, const domain::RunId& run,
                      const AgentRunLimits limits) -> Result {
  const auto started = std::chrono::steady_clock::now();
  // A durable terminal can precede worker EOF. It cannot be cancelled again,
  // but its worker and accounting still need to be drained.
  if (session.active() && !has_terminal(session, run)) {
    auto cancelled = session.cancel_active("noninteractive agent stopped");
    if (!cancelled)
      return failure(AgentErrorCode::cleanup_incomplete,
                     "cancellation failed; reopen the session: " +
                         cancelled.error().message);
  }
  while (true) {
    auto drained = session.drain();
    if (!drained)
      return failure(AgentErrorCode::cleanup_incomplete,
                     "accounting failed; reopen the session: " +
                         drained.error().message);
    // Check after drain as well: an EOF observed late is not bounded cleanup.
    if (std::chrono::steady_clock::now() - started >= limits.cleanup_timeout)
      return failure(AgentErrorCode::cleanup_incomplete,
                     "cancellation accounting incomplete; reopen the session");
    if (!session.active()) return {};
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
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

auto terminal_status(const domain::RunEvent& event)
    -> std::optional<AgentStatus> {
  if (std::holds_alternative<domain::RunCompleted>(event.payload))
    return AgentStatus::completed;
  if (std::holds_alternative<domain::RunFailed>(event.payload))
    return AgentStatus::failed;
  if (std::holds_alternative<domain::RunCancelled>(event.payload))
    return AgentStatus::cancelled;
  return {};
}

auto observed_outcome(const ChatSession& session, const domain::RunId& run,
                      const AgentStatus requested) -> AgentOutcome {
  AgentOutcome result{requested, false, session.session_id(), run, {}};
  for (const auto& event : session.event_log().events()) {
    if (event.metadata.run_id != run) continue;
    if (const auto terminal = terminal_status(event)) {
      result.durable_terminal = session.durable() && !session.active();
      if (requested == AgentStatus::completed) result.status = *terminal;
    }
  }
  if (!result.durable_terminal && result.status == AgentStatus::completed)
    result.status = AgentStatus::failed;
  return result;
}

auto drive(ChatSession& session, Delivery& delivery) -> Result {
  while (session.active()) {
    if (auto ready = delivery.budget.check(); !ready) return ready;
    auto drained = session.drain();
    if (!drained)
      return failure(AgentErrorCode::run_failed, drained.error().message);
    if (auto written = delivery.events(session); !written) return written;
    if (session.pending_question_input() || session.pending_tool_approval())
      return {};
    if (session.active())
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  return delivery.events(session);
}

auto finish(ChatSession& session, const domain::RunId& run, Delivery& delivery,
            Result progress, const AgentRunLimits limits)
    -> std::expected<AgentOutcome, AgentError> {
  const bool interaction = session.pending_question_input().has_value() ||
                           session.pending_tool_approval().has_value();
  auto requested =
      interaction ? AgentStatus::interaction_required : AgentStatus::completed;
  if (!progress)
    requested = progress.error().code == AgentErrorCode::cancelled
                    ? AgentStatus::cancelled
                    : AgentStatus::failed;
  const auto cleaned = cancel_and_drain(session, run, limits);
  if (!cleaned) {
    requested = AgentStatus::failed;
    progress = std::unexpected(cleaned.error());
  }
  if (!delivery.usable) return std::unexpected(progress.error());
  // Final event flushing shares the run budget, including the last write.
  // After expiry, durable history remains available through inert replay.
  if (progress) progress = delivery.events(session);
  if (!progress)
    requested = progress.error().code == AgentErrorCode::cancelled
                    ? AgentStatus::cancelled
                    : AgentStatus::failed;
  if (!delivery.usable) return std::unexpected(progress.error());
  auto outcome = observed_outcome(session, run, requested);
  outcome.durable_terminal = outcome.durable_terminal && cleaned.has_value();
  if (!progress) outcome.reason = progress.error().message;
  // Reporting a settled outcome is separate from running/flushing the run.
  // The sink owns its bounded per-record write, including this final record.
  auto written = emit(delivery.sink, agent_terminal_record(outcome));
  if (!written) return std::unexpected(std::move(written.error()));
  return outcome;
}
auto validate_request(const ChatSession& session, const AgentRequest& request,
                      const AgentRunLimits limits) -> Result {
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
      limits.cleanup_timeout <= std::chrono::milliseconds::zero() ||
      limits.run_timeout > AgentRunLimits{}.run_timeout ||
      limits.cleanup_timeout > AgentRunLimits{}.cleanup_timeout)
    return failure(
        AgentErrorCode::invalid_request,
        "agent deadlines must be positive and within the supported bounds");
  return {};
}
} // namespace

auto run_agent_session(ChatSession& session, const AgentRequest& request,
                       AgentRecordSink& sink, const std::stop_token stop_token,
                       const AgentRunLimits limits)
    -> std::expected<AgentOutcome, AgentError> {
  std::optional<domain::RunId> owned_run;
  try {
    if (auto valid = validate_request(session, request, limits); !valid)
      return std::unexpected(std::move(valid.error()));
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
    const RunBudget budget{std::chrono::steady_clock::now(), limits.run_timeout,
                           stop_token};
    auto tools = select_tools(session, request);
    if (!tools) return std::unexpected(std::move(tools.error()));
    if (auto ready = budget.check(); !ready)
      return std::unexpected(std::move(ready.error()));
    Delivery delivery{sink, budget, true, session.event_log().events().size()};
    auto submitted = session.submit(request.prompt);
    if (!submitted)
      return failure(AgentErrorCode::run_failed, submitted.error().message);
    owned_run = submitted->run_id;
    auto progress = delivery.write(
        agent_accepted_record(session.session_id(), submitted->run_id, *tools));
    if (progress) progress = drive(session, delivery);
    return finish(session, submitted->run_id, delivery, std::move(progress),
                  limits);
  } catch (...) {
    if (owned_run) {
      auto cleaned = cancel_and_drain(session, *owned_run, limits);
      if (!cleaned) return std::unexpected(std::move(cleaned.error()));
    }
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
    AgentOutcome result{
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
