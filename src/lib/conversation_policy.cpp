#include <aiforge/runtime/conversation_policy.hpp>

#include <algorithm>
#include <limits>
#include <set>

namespace aiforge::runtime {
namespace {

auto invalid_policy(const char* message)
    -> std::unexpected<domain::ConversationAdmissionError> {
  return std::unexpected(domain::ConversationAdmissionError{
      domain::ConversationAdmissionErrorCode::invalid_policy, message});
}

auto complete_control_transaction(const domain::SessionEventLog& log,
                                  std::size_t index, std::size_t event_count)
    -> bool {
  const auto& events = log.events();
  if (index == 0 || index + 1 >= event_count) return false;
  const auto& before = events[index - 1];
  const auto& current = events[index];
  const auto& after = events[index + 1];
  const auto* started = std::get_if<domain::RunStarted>(&before.payload);
  return started != nullptr &&
         started->purpose == domain::RunPurpose::control &&
         !started->conversation_admission && !started->memory_selection &&
         before.metadata.schema_version == 3 &&
         current.metadata.schema_version == 1 &&
         after.metadata.schema_version == 1 &&
         std::holds_alternative<domain::RunCompleted>(after.payload) &&
         before.metadata.run_id == current.metadata.run_id &&
         after.metadata.run_id == current.metadata.run_id &&
         !before.metadata.parent_run_id && !current.metadata.parent_run_id &&
         !after.metadata.parent_run_id;
}

auto apply_policy_event(const domain::SessionEventLog& log, std::size_t index,
                        std::size_t count, ConversationPolicySnapshot& result)
    -> std::expected<void, domain::ConversationAdmissionError> {
  const auto& event = log.events()[index];
  const auto* unknown = std::get_if<domain::UnknownEvent>(&event.payload);
  if (unknown != nullptr &&
      (unknown->type_name == "session.conversation_policy_set" ||
       unknown->type_name == "session.conversation_summary_activated" ||
       unknown->type_name == "session.conversation_summary_disabled"))
    return std::unexpected(domain::ConversationAdmissionError{
        domain::ConversationAdmissionErrorCode::unsupported_version,
        "conversation policy event is unsupported"});
  const auto* set = std::get_if<domain::ConversationPolicySet>(&event.payload);
  const auto* activated =
      std::get_if<domain::ConversationSummaryActivated>(&event.payload);
  const auto* disabled =
      std::get_if<domain::ConversationSummaryDisabled>(&event.payload);
  if (set == nullptr && activated == nullptr && disabled == nullptr) return {};
  const auto previous = set != nullptr ? set->previous_revision
                        : activated != nullptr
                            ? activated->previous_policy_revision
                            : disabled->previous_policy_revision;
  if (!complete_control_transaction(log, index, count) ||
      result.policy.revision == std::numeric_limits<std::uint64_t>::max() ||
      previous != result.policy.revision)
    return invalid_policy("conversation policy transaction is invalid");
  auto policy = result.policy;
  ++policy.revision;
  if (set != nullptr) {
    if (!domain::validate_conversation_policy(set->policy) ||
        set->policy.revision != policy.revision)
      return invalid_policy("conversation policy transaction is invalid");
    policy = set->policy;
  }
  result = {std::move(policy), event.metadata.event_id,
            event.metadata.sequence};
  return {};
}

} // namespace

auto recorded_conversation_policy(
    const domain::SessionEventLog& log,
    std::optional<std::uint64_t> snapshot_sequence)
    -> std::expected<ConversationPolicySnapshot,
                     domain::ConversationAdmissionError> {
  try {
    const auto sequence = snapshot_sequence.value_or(log.last_sequence());
    const auto end = std::ranges::upper_bound(
        log.events(), sequence, {},
        [](const auto& event) { return event.metadata.sequence; });
    const auto count = static_cast<std::size_t>(end - log.events().begin());
    if (sequence > log.last_sequence() ||
        (sequence != 0 &&
         (count == 0 || log.events()[count - 1].metadata.sequence != sequence)))
      return invalid_policy("conversation policy snapshot is unavailable");
    if (count > 65536)
      return std::unexpected(domain::ConversationAdmissionError{
          domain::ConversationAdmissionErrorCode::resource_exhausted,
          "conversation policy event limit exceeded"});
    ConversationPolicySnapshot result;
    std::set<domain::RunId> started_runs;
    for (std::size_t index = 0; index < count; ++index) {
      const auto& event = log.events()[index];
      if (std::holds_alternative<domain::RunStarted>(event.payload) &&
          !started_runs.insert(event.metadata.run_id).second)
        return invalid_policy(
            "conversation policy history reuses a run identity");
      auto applied = apply_policy_event(log, index, count, result);
      if (!applied) return std::unexpected(applied.error());
    }
    return result;
  } catch (...) {
    return std::unexpected(domain::ConversationAdmissionError{
        domain::ConversationAdmissionErrorCode::internal_failure,
        "conversation policy reconstruction failed"});
  }
}

} // namespace aiforge::runtime
