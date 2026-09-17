#include <aiforge/runtime/ops_observation_history.hpp>
#include <aiforge/surfaces/manual_ops_session.hpp>
#include <algorithm>
#include <array>
#include <limits>
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
  std::uint64_t sequence{};
};
using ResultEvents = std::map<domain::EventId, const domain::RunEvent*>;
auto terminal(const domain::RunEvent& event) -> std::optional<Terminal> {
  if (std::holds_alternative<domain::RunCompleted>(event.payload))
    return Terminal{domain::RunStatus::completed, {}, event.metadata.sequence};
  if (std::holds_alternative<domain::RunCancelled>(event.payload))
    return Terminal{domain::RunStatus::cancelled, {}, event.metadata.sequence};
  if (const auto* failed = std::get_if<domain::RunFailed>(&event.payload))
    return Terminal{domain::RunStatus::failed, failed->error.code,
                    event.metadata.sequence};
  return {};
}
auto committed(const runtime::RecordedOpsInvocation& record,
               const domain::RunEvent& event, const ResultEvents& results)
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
  const auto result = results.find(*record.result_event_id);
  if (result == results.end() || result->second == nullptr ||
      result->second->metadata.run_id != record.run_id)
    return invalid();
  const auto* payload =
      std::get_if<domain::ToolResultRecorded>(&result->second->payload);
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
                      const ManualRecords& manual, std::stop_token stop)
    -> std::expected<Terminals, Failure> {
  Terminals result;
  for (const auto& event : log.events()) {
    if (stop.stop_requested())
      return std::unexpected(Failure{ManualOpsErrorCode::cancelled});
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
struct CatalogProjection {
  std::optional<CommittedOpsObservation> latest;
  std::array<std::optional<CommittedOpsObservation>, manual_ops_catalog_slots>
      catalog{};
};
struct CatalogAccumulator {
  CatalogProjection projection;
  std::array<std::size_t, manual_ops_catalog_slots> slot_bytes{};
  std::array<std::uint64_t, manual_ops_catalog_slots> slot_completions{};
  std::uint64_t latest_completion{};
  std::size_t total_bytes{};
};
auto expected_result_events(const ManualRecords& manual,
                            const Terminals& terminals) -> ResultEvents {
  ResultEvents results;
  for (const auto& [run, record] : manual) {
    const auto ending = terminals.find(run);
    if (ending != terminals.end() &&
        ending->second.status == domain::RunStatus::completed &&
        record->phase == runtime::OpsInvocationPhase::succeeded &&
        record->result_event_id)
      results.emplace(*record->result_event_id, nullptr);
  }
  return results;
}
auto index_result_events(const domain::SessionEventLog& log,
                         ResultEvents& results, std::stop_token stop)
    -> std::expected<void, Failure> {
  for (const auto& event : log.events()) {
    if (stop.stop_requested())
      return std::unexpected(Failure{ManualOpsErrorCode::cancelled});
    const auto found = results.find(event.metadata.event_id);
    if (found == results.end()) continue;
    if (found->second != nullptr ||
        !std::holds_alternative<domain::ToolResultRecorded>(event.payload))
      return invalid();
    found->second = &event;
  }
  return {};
}
auto retain_catalog_event(const domain::RunEvent& event,
                          const ManualRecords& manual,
                          const Terminals& terminals,
                          const ResultEvents& results,
                          CatalogAccumulator& accumulator)
    -> std::expected<void, Failure> {
  if (!std::holds_alternative<domain::OpsObservationRecorded>(event.payload))
    return {};
  const auto found = manual.find(event.metadata.run_id);
  if (found == manual.end()) return {};
  const auto ending = terminals.find(event.metadata.run_id);
  if (ending == terminals.end() ||
      ending->second.status != domain::RunStatus::completed ||
      found->second->phase != runtime::OpsInvocationPhase::succeeded)
    return invalid();
  auto evidence = committed(*found->second, event, results);
  if (!evidence) return std::unexpected(evidence.error());
  const auto usage =
      domain::validate_recorded_ops_observation(evidence->observation);
  if (!usage)
    return std::unexpected(
        Failure{usage.error().code ==
                        domain::OpsObservationErrorCode::resource_exhausted
                    ? ManualOpsErrorCode::resource_exhausted
                    : ManualOpsErrorCode::invalid_history});
  const auto slot =
      manual_ops_catalog_slot(evidence->observation.request.operation);
  if (slot >= accumulator.projection.catalog.size()) return invalid();
  if (ending->second.sequence <= accumulator.slot_completions[slot]) return {};
  const auto retained_bytes =
      accumulator.total_bytes - accumulator.slot_bytes[slot];
  if (usage->evidence_bytes >
      std::numeric_limits<std::size_t>::max() - retained_bytes)
    return std::unexpected(Failure{ManualOpsErrorCode::resource_exhausted});
  accumulator.total_bytes = retained_bytes + usage->evidence_bytes;
  accumulator.slot_bytes[slot] = usage->evidence_bytes;
  accumulator.slot_completions[slot] = ending->second.sequence;
  if (ending->second.sequence > accumulator.latest_completion) {
    accumulator.latest_completion = ending->second.sequence;
    accumulator.projection.latest = *evidence;
  }
  accumulator.projection.catalog[slot] = std::move(*evidence);
  return {};
}
auto catalog_evidence(const domain::SessionEventLog& log,
                      const ManualRecords& manual, const Terminals& terminals,
                      std::stop_token stop)
    -> std::expected<CatalogProjection, Failure> {
  auto results = expected_result_events(manual, terminals);
  auto indexed = index_result_events(log, results, stop);
  if (!indexed) return std::unexpected(indexed.error());
  CatalogAccumulator accumulator;
  for (const auto& event : log.events()) {
    if (stop.stop_requested())
      return std::unexpected(Failure{ManualOpsErrorCode::cancelled});
    auto retained =
        retain_catalog_event(event, manual, terminals, results, accumulator);
    if (!retained) return std::unexpected(retained.error());
  }
  if (accumulator.total_bytes > maximum_manual_ops_catalog_bytes)
    return std::unexpected(Failure{ManualOpsErrorCode::resource_exhausted});
  return std::move(accumulator.projection);
}
} // namespace

auto project_manual_observations(
    const domain::SessionEventLog& log,
    const std::optional<ObservationSubmission>& current, std::stop_token stop)
    -> std::expected<ManualObservationProjection, ManualOpsFailure> {
  try {
    auto history = runtime::recorded_ops_observations(log, {}, stop);
    if (!history) {
      auto code = ManualOpsErrorCode::invalid_history;
      if (history.error().code ==
          runtime::OpsHistoryErrorCode::resource_exhausted)
        code = ManualOpsErrorCode::resource_exhausted;
      else if (history.error().code ==
               runtime::OpsHistoryErrorCode::internal_failure)
        code = ManualOpsErrorCode::internal_failure;
      else if (history.error().code == runtime::OpsHistoryErrorCode::cancelled)
        code = ManualOpsErrorCode::cancelled;
      return std::unexpected(Failure{code});
    }
    auto manual = index_manual(*history);
    if (!manual) return std::unexpected(manual.error());
    auto terminals = manual_terminals(log, *manual, stop);
    if (!terminals) return std::unexpected(terminals.error());
    auto progress = current_progress(*manual, *terminals, current);
    if (!progress) return std::unexpected(progress.error());
    auto evidence = catalog_evidence(log, *manual, *terminals, stop);
    if (!evidence) return std::unexpected(evidence.error());
    return ManualObservationProjection{log.last_sequence(),
                                       std::move(*progress),
                                       std::move(evidence->latest),
                                       std::move(evidence->catalog),
                                       std::move(history->latest_selection),
                                       history->maximum_selection_generation};
  } catch (...) {
    return std::unexpected(Failure{ManualOpsErrorCode::internal_failure});
  }
}
} // namespace aiforge::surfaces
