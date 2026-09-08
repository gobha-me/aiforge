#include <aiforge/runtime/conversation_context.hpp>

#include <aiforge/detail/sha256.hpp>
#include <aiforge/runtime/conversation_policy.hpp>
#include <algorithm>
#include <limits>
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
                     std::stop_token stop)
    -> std::expected<ConversationAdmission, ConversationContextError> {
  ConversationAdmission result{1,
                               conversation_estimator_version,
                               request.log.session_id(),
                               request.model_id,
                               request.log.last_sequence(),
                               policy.event_id,
                               policy.policy.revision,
                               policy.policy.mode,
                               request.capacity,
                               request.mandatory_input_tokens,
                               {},
                               selection.omitted_group_count,
                               {},
                               {}};
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
    ConversationSelectionRequest selection_request{
        policy->policy.mode == ConversationMode::full
            ? ConversationSelectionMode::full
            : ConversationSelectionMode::rolling,
        request.capacity,
        request.mandatory_input_tokens,
        std::move(*history),
        policy->policy.pinned_run_ids,
        request.selection_limits};
    auto selection = select_conversation(selection_request, stop);
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
    auto ordered =
        renumber(selection->selected_groups, request.first_history_order);
    if (!ordered) return std::unexpected(ordered.error());
    auto admission = build_admission(request, *policy, selection_request.groups,
                                     *selection, stop);
    if (!admission) return std::unexpected(admission.error());
    return PreparedConversationContext{std::move(*selection),
                                       std::move(*admission)};
  } catch (...) {
    return failure(Code::internal_failure,
                   "conversation context preparation failed internally");
  }
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
    return recover_groups(*history, admission, pins, stop);
  } catch (...) {
    return failure(Code::internal_failure,
                   "conversation context recovery failed internally");
  }
}

} // namespace aiforge::runtime
