#include "conversation_context_internal.hpp"
#include <aiforge/runtime/conversation_context.hpp>

#include <aiforge/detail/sha256.hpp>
#include <aiforge/runtime/conversation_policy.hpp>
#include <aiforge/runtime/conversation_summary_context.hpp>
#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <string_view>
#include <utility>

namespace aiforge::runtime {
namespace {
using Code = ConversationContextErrorCode;
using namespace domain;

auto failure(Code code, std::string message, std::optional<RunId> run = {})
    -> std::unexpected<ConversationContextError> {
  return std::unexpected(
      ConversationContextError{code, std::move(message), std::move(run)});
}

auto history_failure(const ConversationHistoryError& error)
    -> std::unexpected<ConversationContextError> {
  const auto code =
      error.code == ConversationHistoryErrorCode::cancelled ? Code::cancelled
      : error.code == ConversationHistoryErrorCode::resource_exhausted
          ? Code::resource_exhausted
          : Code::invalid_history;
  return failure(code, error.message, error.run_id);
}

auto domain_failure(const ConversationAdmissionError& error, Code fallback)
    -> std::unexpected<ConversationContextError> {
  return failure(error.code ==
                         ConversationAdmissionErrorCode::resource_exhausted
                     ? Code::resource_exhausted
                     : fallback,
                 error.message);
}

auto admitted_entry(const ConversationHistoryEntry& entry)
    -> std::expected<ConversationAdmittedEntry, ConversationContextError> {
  auto digest = normalized_conversation_message_digest(entry.content.message);
  if (!digest) return domain_failure(digest.error(), Code::invalid_history);
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

class OmissionDigest final {
 public:
  auto field(std::string_view value) -> void {
    if (!m_valid) return;
    const auto prefix = std::to_string(value.size()) + ':';
    if (prefix.size() > conversation_maximum_manifest_bytes - m_bytes ||
        value.size() >
            conversation_maximum_manifest_bytes - m_bytes - prefix.size()) {
      m_valid = false;
      return;
    }
    m_hash.update(std::as_bytes(std::span{prefix.data(), prefix.size()}));
    m_hash.update(std::as_bytes(std::span{value.data(), value.size()}));
    m_bytes += prefix.size() + value.size();
  }
  auto number(std::uint64_t value) -> void { field(std::to_string(value)); }
  auto group(const ConversationHistoryGroup& value) -> void {
    field(value.run_id.value());
    number(value.entries.size());
    for (const auto& entry : value.entries) {
      field(entry.completed_event_id.value());
      number(entry.event_sequence);
      field(entry.content.entry_id.value());
      field(entry.content.message.message_id.value());
      field(entry.content.provenance.source_id.value());
    }
  }
  auto finish() -> std::expected<ContentDigest, ConversationContextError> {
    if (!m_valid)
      return failure(Code::resource_exhausted,
                     "conversation omission manifest exceeds its bound");
    return ContentDigest{"sha256", m_hash.finish(), m_bytes};
  }

 private:
  detail::Sha256 m_hash;
  std::size_t m_bytes{};
  bool m_valid{true};
};

auto omitted_digest(const std::vector<ConversationHistoryGroup>& history,
                    const std::set<RunId>& selected, std::stop_token stop)
    -> std::expected<std::optional<ContentDigest>, ConversationContextError> {
  OmissionDigest digest;
  digest.field("aiforge.conversation-omissions.v1");
  digest.number(history.size() - selected.size());
  bool omitted{};
  for (const auto& group : history) {
    if (stop.stop_requested())
      return failure(Code::cancelled,
                     "conversation context operation cancelled");
    if (!selected.contains(group.run_id)) {
      digest.group(group);
      omitted = true;
    }
  }
  if (!omitted) return std::nullopt;
  auto result = digest.finish();
  if (!result) return std::unexpected(result.error());
  return std::optional<ContentDigest>{std::move(*result)};
}

auto renumber(std::vector<ConversationHistoryGroup>& groups,
              std::uint64_t first_order)
    -> std::expected<void, ConversationContextError> {
  if (first_order == 0)
    return failure(Code::invalid_order, "history order must be positive");
  auto order = first_order;
  bool exhausted{};
  for (auto& group : groups) {
    for (auto& entry : group.entries) {
      if (exhausted)
        return failure(Code::invalid_order, "history content order overflows");
      entry.content.order = order;
      exhausted = order == std::numeric_limits<std::uint64_t>::max();
      if (!exhausted) ++order;
    }
  }
  return {};
}

auto build_admission(const ConversationContextRequest& request,
                     const ConversationPolicySnapshot& policy,
                     const std::vector<ConversationHistoryGroup>& history,
                     const ConversationSelectionResult& selection,
                     std::uint64_t snapshot_sequence,
                     std::span<const ConversationAdmittedSummary> summaries,
                     std::stop_token stop)
    -> std::expected<ConversationAdmission, ConversationContextError> {
  ConversationAdmission result{2,
                               conversation_estimator_version,
                               request.log.session_id(),
                               request.model_id,
                               snapshot_sequence,
                               policy.event_id,
                               policy.policy.revision,
                               policy.policy.mode,
                               request.capacity,
                               request.mandatory_input_tokens,
                               {},
                               selection.omitted_group_count,
                               {},
                               {}};
  result.summaries.assign(summaries.begin(), summaries.end());
  std::set<RunId> selected;
  const std::set<RunId> pins{policy.policy.pinned_run_ids.begin(),
                             policy.policy.pinned_run_ids.end()};
  for (const auto& group : selection.selected_groups) {
    if (stop.stop_requested())
      return failure(Code::cancelled,
                     "conversation context operation cancelled");
    selected.insert(group.run_id);
    ConversationAdmittedGroup admitted{
        group.run_id, {}, pins.contains(group.run_id)};
    for (const auto& entry : group.entries) {
      if (stop.stop_requested())
        return failure(Code::cancelled,
                       "conversation context operation cancelled");
      auto metadata = admitted_entry(entry);
      if (!metadata) return std::unexpected(metadata.error());
      admitted.entries.push_back(std::move(*metadata));
    }
    result.groups.push_back(std::move(admitted));
  }
  auto omitted = omitted_digest(history, selected, stop);
  if (!omitted) return std::unexpected(omitted.error());
  result.omitted_groups_digest = std::move(*omitted);
  auto sealed = seal_conversation_admission(result);
  if (!sealed) return domain_failure(sealed.error(), Code::invalid_admission);
  return result;
}

auto summary_failure(const ConversationSummaryContextError& error)
    -> std::unexpected<ConversationContextError> {
  return failure(error.code == ConversationSummaryContextErrorCode::cancelled
                     ? Code::cancelled
                 : error.code ==
                         ConversationSummaryContextErrorCode::resource_exhausted
                     ? Code::resource_exhausted
                     : Code::invalid_admission,
                 error.message);
}

auto add_estimate(std::uint64_t& total, std::uint64_t amount)
    -> std::expected<void, ConversationContextError> {
  if (amount > std::numeric_limits<std::uint64_t>::max() - total)
    return failure(Code::invalid_history,
                   "conversation summary accounting overflows");
  total += amount;
  return {};
}

auto complete_decisions(ConversationSelectionResult& selection,
                        const std::vector<ConversationHistoryGroup>& history)
    -> std::expected<void, ConversationContextError> {
  std::map<RunId, ConversationSelectionDecisionRecord> selected_decisions;
  for (const auto& decision : selection.decisions)
    selected_decisions.emplace(decision.run_id, decision);
  selection.decisions.clear();
  for (const auto& group : history) {
    const auto found = selected_decisions.find(group.run_id);
    if (found != selected_decisions.end()) {
      selection.decisions.push_back(found->second);
      continue;
    }
    std::uint64_t tokens{};
    for (const auto& entry : group.entries) {
      auto added = add_estimate(tokens, entry.content.estimated_tokens);
      if (!added) return added;
    }
    auto added = add_estimate(selection.omitted_history_tokens, tokens);
    if (!added) return added;
    ++selection.omitted_group_count;
    selection.omitted_entry_count += group.entries.size();
    selection.decisions.push_back(
        {group.run_id, ConversationSelectionDecision::omitted_summary,
         group.entries.size(), tokens});
  }
  return {};
}

auto select_history(const ConversationContextRequest& request,
                    const ConversationPolicySnapshot& policy,
                    const std::vector<ConversationHistoryGroup>& history,
                    const PreparedConversationSummaryContext& summaries,
                    std::stop_token stop)
    -> std::expected<ConversationSelectionResult, ConversationContextError> {
  const std::set<RunId> covered{summaries.covered_run_ids.begin(),
                                summaries.covered_run_ids.end()};
  const std::set<RunId> pins{policy.policy.pinned_run_ids.begin(),
                             policy.policy.pinned_run_ids.end()};
  ConversationSelectionRequest selected{
      policy.policy.mode == ConversationMode::full
          ? ConversationSelectionMode::full
          : ConversationSelectionMode::rolling,
      request.capacity,
      request.mandatory_input_tokens,
      {},
      policy.policy.pinned_run_ids,
      request.selection_limits};
  for (const auto& group : history)
    if (!covered.contains(group.run_id) || pins.contains(group.run_id))
      selected.groups.push_back(group);
  auto selection = select_conversation(selected, stop);
  if (!selection) {
    const auto code =
        selection.error().code == ConversationSelectionErrorCode::cancelled
            ? Code::cancelled
        : selection.error().code ==
                ConversationSelectionErrorCode::resource_exhausted
            ? Code::resource_exhausted
            : Code::selection_failed;
    return failure(code, selection.error().message, selection.error().run_id);
  }
  auto decisions = complete_decisions(*selection, history);
  if (!decisions) return std::unexpected(decisions.error());
  return std::move(*selection);
}

auto recover_group(ConversationHistoryGroup& group,
                   const ConversationAdmittedGroup& recorded,
                   const std::set<RunId>& pins, std::stop_token stop)
    -> std::expected<void, ConversationContextError> {
  if (recorded.entries.size() != group.entries.size() ||
      recorded.pinned != pins.contains(group.run_id))
    return failure(Code::source_mismatch,
                   "admitted conversation group shape changed", group.run_id);
  for (std::size_t index = 0; index < group.entries.size(); ++index) {
    if (stop.stop_requested())
      return failure(Code::cancelled,
                     "conversation context operation cancelled");
    auto& entry = group.entries[index];
    entry.content.order = recorded.entries[index].order;
    auto metadata = admitted_entry(entry);
    if (!metadata) return std::unexpected(metadata.error());
    if (*metadata != recorded.entries[index])
      return failure(Code::source_mismatch,
                     "admitted conversation source changed", group.run_id);
  }
  return {};
}

auto recover_groups(std::vector<ConversationHistoryGroup>& history,
                    const ConversationAdmission& admission,
                    const std::set<RunId>& pins, std::stop_token stop)
    -> std::expected<std::vector<ConversationHistoryGroup>,
                     ConversationContextError> {
  std::set<RunId> selected;
  std::vector<ConversationHistoryGroup> result;
  std::size_t admitted_index{};
  for (auto& group : history) {
    if (stop.stop_requested())
      return failure(Code::cancelled,
                     "conversation context operation cancelled");
    if (admitted_index == admission.groups.size() ||
        group.run_id != admission.groups[admitted_index].run_id)
      continue;
    auto recovered =
        recover_group(group, admission.groups[admitted_index++], pins, stop);
    if (!recovered) return std::unexpected(recovered.error());
    selected.insert(group.run_id);
    // Copy only admitted groups; omission hashing still needs all identities.
    result.push_back(group);
  }
  if (admitted_index != admission.groups.size() ||
      history.size() - selected.size() != admission.omitted_group_count ||
      !std::ranges::all_of(
          pins, [&](const auto& pin) { return selected.contains(pin); }))
    return failure(Code::source_mismatch,
                   "conversation source selection is incomplete");
  auto omitted = omitted_digest(history, selected, stop);
  if (!omitted) return std::unexpected(omitted.error());
  if (*omitted != admission.omitted_groups_digest)
    return failure(Code::source_mismatch,
                   "conversation omitted source identities changed");
  return result;
}

auto validate_recovered_summary_coverage(
    const SessionEventLog& log, const ConversationAdmission& admission,
    const std::set<RunId>& pins, const ConversationHistoryLimits& limits,
    std::stop_token stop) -> std::expected<void, ConversationContextError> {
  if (admission.version != 2 || admission.mode != ConversationMode::rolling)
    return {};
  // Prove immutable source coverage only. The dispatch owner separately
  // checks current activation availability until the active base is pinned.
  auto summaries = recover_conversation_summary_context(
      log, admission.summaries, admission.source_snapshot_sequence, limits,
      stop, false);
  if (!summaries) return summary_failure(summaries.error());
  const std::set<RunId> covered{summaries->covered_run_ids.begin(),
                                summaries->covered_run_ids.end()};
  for (const auto& group : admission.groups)
    if (covered.contains(group.run_id) && !pins.contains(group.run_id))
      return failure(Code::source_mismatch,
                     "covered original conversation was admitted without a pin",
                     group.run_id);
  return {};
}

auto prepare_from_rendered(
    const ConversationContextRequest& request,
    const context_detail::ConversationContextSources& sources,
    PreparedConversationSummaryContext summaries, std::stop_token stop)
    -> std::expected<PreparedConversationContext, ConversationContextError> {
  auto adjusted = request;
  auto budget =
      add_estimate(adjusted.mandatory_input_tokens, summaries.estimated_tokens);
  if (!budget) return std::unexpected(budget.error());
  if (summaries.content.size() >
      std::numeric_limits<std::uint64_t>::max() - adjusted.first_history_order)
    return failure(Code::invalid_order,
                   "history order after summaries overflows");
  adjusted.first_history_order += summaries.content.size();
  auto selection = select_history(adjusted, sources.policy, sources.history,
                                  summaries, stop);
  if (!selection) return std::unexpected(selection.error());
  auto ordered =
      renumber(selection->selected_groups, adjusted.first_history_order);
  if (!ordered) return std::unexpected(ordered.error());
  auto admission =
      build_admission(adjusted, sources.policy, sources.history, *selection,
                      sources.snapshot_sequence, summaries.summaries, stop);
  if (!admission) return std::unexpected(admission.error());
  return PreparedConversationContext{std::move(*selection),
                                     std::move(*admission),
                                     std::move(summaries.content)};
}

} // namespace

auto prepare_conversation_context(const ConversationContextRequest& request,
                                  std::stop_token stop)
    -> std::expected<PreparedConversationContext, ConversationContextError> {
  try {
    if (stop.stop_requested())
      return failure(Code::cancelled,
                     "conversation context operation cancelled");
    if (request.first_history_order == 0)
      return failure(Code::invalid_order, "history order must be positive");
    auto policy = recorded_conversation_policy(request.log);
    if (!policy) return domain_failure(policy.error(), Code::invalid_policy);
    auto history =
        reconstruct_conversation_history({request.log,
                                          {},
                                          conversation_estimator_version,
                                          request.history_limits},
                                         stop);
    if (!history) return history_failure(history.error());
    PreparedConversationSummaryContext summaries;
    if (policy->policy.mode == ConversationMode::rolling) {
      auto prepared = prepare_conversation_summary_context(
          request.log, request.first_history_order, request.history_limits,
          stop);
      if (!prepared) return summary_failure(prepared.error());
      summaries = std::move(*prepared);
    }
    context_detail::ConversationContextSources sources{
        request.log.last_sequence(),
        std::move(*policy),
        std::move(*history),
        {}};
    // Normal preparation has already rendered its validated active snapshot.
    return prepare_from_rendered(request, sources, std::move(summaries), stop);
  } catch (...) {
    return failure(Code::internal_failure,
                   "conversation context preparation failed internally");
  }
}

auto context_detail::prepare_conversation_context_from_sources(
    const ConversationContextRequest& request,
    const ConversationContextSources& sources, std::stop_token stop)
    -> std::expected<PreparedConversationContext, ConversationContextError> {
  PreparedConversationSummaryContext summaries;
  if (sources.policy.policy.mode == ConversationMode::rolling) {
    auto prepared = prepare_summary_context_from_snapshot(
        sources.summaries, request.first_history_order, request.history_limits,
        stop);
    if (!prepared) return summary_failure(prepared.error());
    summaries = std::move(*prepared);
  }
  return prepare_from_rendered(request, sources, std::move(summaries), stop);
}

auto context_detail::resolve_summary_preview_sources(
    const ConversationContextRequest& request, std::span<const RunEvent> suffix,
    std::stop_token stop)
    -> std::expected<ConversationContextSources, ConversationContextError> {
  if (stop.stop_requested())
    return failure(Code::cancelled, "summary context preview cancelled");
  if (suffix.size() != 3 ||
      suffix.size() > request.history_limits.maximum_events ||
      request.log.events().size() >
          request.history_limits.maximum_events - suffix.size())
    return failure(Code::resource_exhausted,
                   "summary preview event bound exceeded");
  auto policy = recorded_conversation_policy(request.log);
  if (!policy) return domain_failure(policy.error(), Code::invalid_policy);
  auto summaries = preview_conversation_summary_transition(request.log, suffix);
  if (!summaries)
    return failure(Code::invalid_admission, summaries.error().message);
  auto history = reconstruct_conversation_history(
      {request.log, {}, conversation_estimator_version, request.history_limits},
      stop);
  if (!history) return history_failure(history.error());
  policy->policy.revision = summaries->policy_revision;
  policy->event_id = suffix[1].metadata.event_id;
  policy->event_sequence = suffix[1].metadata.sequence;
  return ConversationContextSources{suffix.back().metadata.sequence,
                                    std::move(*policy), std::move(*history),
                                    std::move(*summaries)};
}

auto recover_conversation_context(const SessionEventLog& log,
                                  const ConversationAdmission& admission,
                                  const ConversationHistoryLimits& limits,
                                  std::stop_token stop)
    -> std::expected<std::vector<ConversationHistoryGroup>,
                     ConversationContextError> {
  try {
    if (stop.stop_requested())
      return failure(Code::cancelled,
                     "conversation context operation cancelled");
    auto valid = validate_conversation_admission(admission, log.session_id());
    if (!valid) return domain_failure(valid.error(), Code::invalid_admission);
    auto policy =
        recorded_conversation_policy(log, admission.source_snapshot_sequence);
    if (!policy) return domain_failure(policy.error(), Code::invalid_policy);
    if (policy->event_id != admission.policy_event_id ||
        policy->policy.revision != admission.policy_revision ||
        policy->policy.mode != admission.mode)
      return failure(Code::source_mismatch,
                     "recorded conversation policy does not match admission");
    ConversationHistoryRequest request{
        log, {}, admission.estimator_version, limits};
    request.source_snapshot_sequence = admission.source_snapshot_sequence;
    auto history = reconstruct_conversation_history(request, stop);
    if (!history) return history_failure(history.error());
    const std::set<RunId> pins{policy->policy.pinned_run_ids.begin(),
                               policy->policy.pinned_run_ids.end()};
    auto coverage =
        validate_recovered_summary_coverage(log, admission, pins, limits, stop);
    if (!coverage) return std::unexpected(coverage.error());
    return recover_groups(*history, admission, pins, stop);
  } catch (...) {
    return failure(Code::internal_failure,
                   "conversation context recovery failed internally");
  }
}

} // namespace aiforge::runtime
