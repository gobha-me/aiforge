#include <aiforge/runtime/ops_observation_history.hpp>
#include <aiforge/surfaces/manual_ops_session.hpp>
#include <algorithm>
#include <map>

namespace aiforge::surfaces {
namespace {
using Failure = ManualOpsFailure;
auto invalid() -> std::unexpected<Failure> {
  return std::unexpected(Failure{ManualOpsErrorCode::invalid_history});
}
struct Terminal {
  domain::RunStatus status{domain::RunStatus::running};
  std::optional<domain::ErrorCode> failure{};
};
auto terminal(const domain::RunEvent& event) -> std::optional<Terminal> {
  if (std::holds_alternative<domain::RunCompleted>(event.payload))
    return Terminal{domain::RunStatus::completed, {}};
  if (std::holds_alternative<domain::RunCancelled>(event.payload))
    return Terminal{domain::RunStatus::cancelled, {}};
  if (const auto* failed = std::get_if<domain::RunFailed>(&event.payload))
    return Terminal{domain::RunStatus::failed, failed->error.code};
  return {};
}
auto committed(const domain::SessionEventLog& log,
               const runtime::RecordedOpsInvocation& record,
               const domain::RunEvent& event)
    -> std::expected<CommittedOpsObservation, Failure> {
  const auto* observed =
      std::get_if<domain::OpsObservationRecorded>(&event.payload);
  if (observed == nullptr || !record.observation_event_id ||
      !record.result_event_id ||
      event.metadata.event_id != *record.observation_event_id ||
      event.metadata.run_id != record.run_id ||
      observed->invocation_id != record.invocation_id ||
      observed->observation.request != record.request)
    return invalid();
  const auto result = std::ranges::find(
      log.events(), *record.result_event_id,
      [](const auto& value) { return value.metadata.event_id; });
  if (result == log.events().end() || result->metadata.run_id != record.run_id)
    return invalid();
  const auto* payload =
      std::get_if<domain::ToolResultRecorded>(&result->payload);
  if (payload == nullptr || payload->invocation_id != record.invocation_id)
    return invalid();
  return CommittedOpsObservation{{record.run_id, record.invocation_id},
                                 *record.observation_event_id,
                                 *record.result_event_id,
                                 observed->observation};
}
using ManualRecords =
    std::map<domain::RunId, const runtime::RecordedOpsInvocation*>;
using Terminals = std::map<domain::RunId, Terminal>;
auto index_manual(const runtime::OpsHistorySnapshot& history)
    -> std::expected<ManualRecords, Failure> {
  ManualRecords result;
  for (const auto& record : history.invocations) {
    if (record.human_origin && !result.emplace(record.run_id, &record).second)
      return invalid();
  }
  return result;
}
auto manual_terminals(const domain::SessionEventLog& log,
                      const ManualRecords& manual) -> Terminals {
  Terminals result;
  for (const auto& event : log.events()) {
    if (!manual.contains(event.metadata.run_id)) continue;
    if (auto ending = terminal(event))
      result.insert_or_assign(event.metadata.run_id, *ending);
  }
  return result;
}
auto current_progress(const ManualRecords& manual, const Terminals& terminals,
                      const std::optional<ObservationSubmission>& current)
    -> std::expected<std::optional<ManualObservationProgress>, Failure> {
  if (!current) return std::nullopt;
  const auto found = manual.find(current->run_id);
  if (found == manual.end() ||
      found->second->invocation_id != current->invocation_id)
    return std::unexpected(Failure{ManualOpsErrorCode::wrong_operation});
  const auto& selected = *found->second;
  ManualObservationProgress progress{
      *current, domain::RunStatus::running, {}, {}};
  if (selected.phase == runtime::OpsInvocationPhase::awaiting_approval)
    progress.status = domain::RunStatus::awaiting_approval;
  if (const auto ending = terminals.find(selected.run_id);
      ending != terminals.end()) {
    progress.status = ending->second.status;
    progress.failure = ending->second.failure;
    if (progress.status == domain::RunStatus::completed) {
      if (selected.phase != runtime::OpsInvocationPhase::succeeded ||
          !selected.observation_event_id || !selected.result_event_id)
        return invalid();
      progress.observation_event_id = selected.observation_event_id;
    }
  }
  return progress;
}
auto latest_evidence(const domain::SessionEventLog& log,
                     const ManualRecords& manual, const Terminals& terminals)
    -> std::expected<std::optional<CommittedOpsObservation>, Failure> {
  const domain::RunEvent* latest{};
  const runtime::RecordedOpsInvocation* latest_record{};
  for (const auto& event : log.events()) {
    if (!std::holds_alternative<domain::OpsObservationRecorded>(event.payload))
      continue;
    const auto found = manual.find(event.metadata.run_id);
    if (found == manual.end()) continue;
    const auto ending = terminals.find(event.metadata.run_id);
    if (ending == terminals.end() ||
        ending->second.status != domain::RunStatus::completed ||
        found->second->phase != runtime::OpsInvocationPhase::succeeded)
      return invalid();
    latest = &event;
    latest_record = found->second;
  }
  if (latest == nullptr || latest_record == nullptr) return std::nullopt;
  auto evidence = committed(log, *latest_record, *latest);
  if (!evidence) return std::unexpected(evidence.error());
  return std::move(*evidence);
}
} // namespace

auto project_manual_observations(
    const domain::SessionEventLog& log,
    const std::optional<ObservationSubmission>& current)
    -> std::expected<ManualObservationProjection, ManualOpsFailure> {
  try {
    auto history = runtime::recorded_ops_observations(log);
    if (!history) {
      auto code = ManualOpsErrorCode::invalid_history;
      if (history.error().code ==
          runtime::OpsHistoryErrorCode::resource_exhausted)
        code = ManualOpsErrorCode::resource_exhausted;
      else if (history.error().code ==
               runtime::OpsHistoryErrorCode::internal_failure)
        code = ManualOpsErrorCode::internal_failure;
      return std::unexpected(Failure{code});
    }
    auto manual = index_manual(*history);
    if (!manual) return std::unexpected(manual.error());
    const auto terminals = manual_terminals(log, *manual);
    auto progress = current_progress(*manual, terminals, current);
    if (!progress) return std::unexpected(progress.error());
    auto evidence = latest_evidence(log, *manual, terminals);
    if (!evidence) return std::unexpected(evidence.error());
    return ManualObservationProjection{
        log.last_sequence(), std::move(*progress), std::move(*evidence)};
  } catch (...) {
    return std::unexpected(Failure{ManualOpsErrorCode::internal_failure});
  }
}
} // namespace aiforge::surfaces
