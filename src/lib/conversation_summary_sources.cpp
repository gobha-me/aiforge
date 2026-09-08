#include <aiforge/runtime/conversation_summary_sources.hpp>

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <utility>

namespace aiforge::runtime {
namespace {
using namespace domain;
using Code = ConversationSummarySourceErrorCode;
using Result = std::expected<PreparedConversationSummarySources,
                             ConversationSummarySourceError>;

auto failure(Code code, std::string message)
    -> std::unexpected<ConversationSummarySourceError> {
  return std::unexpected(
      ConversationSummarySourceError{code, std::move(message)});
}

auto history_failure(const ConversationHistoryError& error)
    -> std::unexpected<ConversationSummarySourceError> {
  const auto code =
      error.code == ConversationHistoryErrorCode::cancelled ? Code::cancelled
      : error.code == ConversationHistoryErrorCode::resource_exhausted
          ? Code::resource_exhausted
          : Code::invalid_history;
  return failure(code, error.message);
}

auto summary_failure(const ConversationSummaryError& error)
    -> std::unexpected<ConversationSummarySourceError> {
  return failure(error.code == ConversationSummaryErrorCode::resource_exhausted
                     ? Code::resource_exhausted
                     : Code::invalid_sources,
                 error.message);
}

auto snapshot_events(const ConversationSummarySourceRequest& request)
    -> std::expected<std::span<const RunEvent>,
                     ConversationSummarySourceError> {
  const auto sequence =
      request.snapshot_sequence.value_or(request.log.last_sequence());
  const auto& events = request.log.events();
  const auto found =
      std::ranges::lower_bound(events, sequence, {}, [](const auto& event) {
        return event.metadata.sequence;
      });
  if (sequence == 0 || found == events.end() ||
      found->metadata.sequence != sequence)
    return failure(Code::invalid_request,
                   "summary source snapshot is unavailable");
  const auto count = static_cast<std::size_t>(found - events.begin()) + 1;
  if (count > request.limits.maximum_events)
    return failure(Code::resource_exhausted,
                   "summary source event limit exceeded");
  return std::span<const RunEvent>{events.data(), count};
}

struct SourceIndex {
  std::vector<RunId> excluded;
  std::map<RunId, const RunEvent*> terminals;
};

auto index_sources(const ConversationSummarySourceRequest& request,
                   std::span<const RunEvent> events, std::stop_token stop)
    -> std::expected<SourceIndex, ConversationSummarySourceError> {
  if (request.run_ids.empty())
    return failure(Code::invalid_request, "summary source coverage is empty");
  if (request.run_ids.size() > summary_maximum_groups)
    return failure(Code::resource_exhausted,
                   "too many requested summary sources");
  std::set<RunId> requested;
  for (const auto& run : request.run_ids)
    if (!requested.insert(run).second)
      return failure(Code::invalid_request,
                     "duplicate requested summary source");
  SourceIndex result;
  std::set<RunId> excluded;
  for (const auto& event : events) {
    if (stop.stop_requested())
      return failure(Code::cancelled, "summary source preparation cancelled");
    if (!requested.contains(event.metadata.run_id)) {
      excluded.insert(event.metadata.run_id);
      if (excluded.size() > request.limits.maximum_runs)
        return failure(Code::resource_exhausted,
                       "summary source run limit exceeded");
    } else if (std::holds_alternative<RunCompleted>(event.payload) ||
               std::holds_alternative<RunFailed>(event.payload) ||
               std::holds_alternative<RunCancelled>(event.payload)) {
      result.terminals.insert_or_assign(event.metadata.run_id, &event);
    }
  }
  result.excluded.assign(excluded.begin(), excluded.end());
  return result;
}

auto source_entry(const ConversationHistoryEntry& entry)
    -> std::expected<ConversationAdmittedEntry,
                     ConversationSummarySourceError> {
  auto digest = normalized_conversation_message_digest(entry.content.message);
  if (!digest)
    return failure(digest.error().code ==
                           ConversationAdmissionErrorCode::resource_exhausted
                       ? Code::resource_exhausted
                       : Code::invalid_history,
                   digest.error().message);
  return ConversationAdmittedEntry{entry.completed_event_id,
                                   entry.event_sequence,
                                   entry.content.entry_id,
                                   entry.content.message.message_id,
                                   entry.content.provenance,
                                   entry.content.order,
                                   entry.content.estimated_tokens,
                                   entry.content.kind,
                                   std::move(*digest)};
}

struct SourceBuilder {
  PreparedConversationSummarySources result;
  std::uint64_t source_bytes{};

  auto append(ConversationHistoryGroup& group, const RunEvent& terminal,
              std::stop_token stop)
      -> std::expected<void, ConversationSummarySourceError> {
    if (group.entries.size() > summary_maximum_entries - result.content.size())
      return failure(Code::resource_exhausted,
                     "too many summary source entries");
    ConversationSummarySourceGroup saved{group.run_id,
                                         terminal.metadata.event_id,
                                         terminal.metadata.sequence,
                                         {}};
    for (auto& entry : group.entries) {
      if (stop.stop_requested())
        return failure(Code::cancelled, "summary source preparation cancelled");
      entry.content.order = result.content.size() + 1;
      auto recorded = source_entry(entry);
      if (!recorded) return std::unexpected(recorded.error());
      if (recorded->message_digest.byte_size >
          summary_maximum_source_bytes - source_bytes)
        return failure(Code::resource_exhausted,
                       "summary source bytes exceed their bound");
      if (entry.content.estimated_tokens >
          std::numeric_limits<std::uint64_t>::max() - result.estimated_tokens)
        return failure(Code::invalid_history,
                       "summary source token estimates overflow");
      source_bytes += recorded->message_digest.byte_size;
      result.estimated_tokens += entry.content.estimated_tokens;
      saved.entries.push_back(std::move(*recorded));
      result.content.push_back(std::move(entry.content));
    }
    result.sources.groups.push_back(std::move(saved));
    return {};
  }
};

auto prepare(const ConversationSummarySourceRequest& request,
             std::stop_token stop) -> Result {
  if (stop.stop_requested())
    return failure(Code::cancelled, "summary source preparation cancelled");
  auto events = snapshot_events(request);
  if (!events) return std::unexpected(events.error());
  auto index = index_sources(request, *events, stop);
  if (!index) return std::unexpected(index.error());
  const auto snapshot = events->back().metadata.sequence;
  auto groups = reconstruct_conversation_history(
      {request.log, std::move(index->excluded), conversation_estimator_version,
       request.limits, snapshot},
      stop);
  if (!groups) return history_failure(groups.error());
  if (groups->size() != request.run_ids.size())
    return failure(
        Code::invalid_sources,
        "requested summary source is missing or not an original terminal run");
  SourceBuilder builder{{{1,
                          conversation_estimator_version,
                          request.log.session_id(),
                          snapshot,
                          {},
                          {}},
                         {},
                         0},
                        0};
  for (auto& group : *groups) {
    const auto terminal = index->terminals.find(group.run_id);
    if (terminal == index->terminals.end())
      return failure(Code::invalid_sources,
                     "summary source terminal is missing");
    auto added = builder.append(group, *terminal->second, stop);
    if (!added) return std::unexpected(added.error());
  }
  auto sealed = seal_conversation_summary_sources(builder.result.sources);
  if (!sealed) return summary_failure(sealed.error());
  return std::move(builder.result);
}
} // namespace

auto prepare_conversation_summary_sources(
    const ConversationSummarySourceRequest& request, std::stop_token stop)
    -> Result {
  try {
    return prepare(request, stop);
  } catch (...) {
    return failure(Code::internal_failure,
                   "summary source preparation failed internally");
  }
}

auto resolve_conversation_summary_sources(
    const SessionEventLog& log, const ConversationSummarySources& sources,
    const ConversationHistoryLimits& limits, std::stop_token stop) -> Result {
  try {
    if (stop.stop_requested())
      return failure(Code::cancelled, "summary source recovery cancelled");
    if (sources.session_id != log.session_id())
      return failure(Code::foreign_scope,
                     "summary sources belong to another session");
    auto valid = validate_conversation_summary_sources(sources);
    if (!valid) return summary_failure(valid.error());
    ConversationSummarySourceRequest request{
        log, {}, sources.snapshot_sequence, limits};
    for (const auto& group : sources.groups)
      request.run_ids.push_back(group.run_id);
    auto prepared = prepare(request, stop);
    if (!prepared) return prepared;
    if (prepared->sources != sources)
      return failure(Code::invalid_sources,
                     "summary source reconstruction differs from its seal");
    return prepared;
  } catch (...) {
    return failure(Code::internal_failure,
                   "summary source recovery failed internally");
  }
}
} // namespace aiforge::runtime
