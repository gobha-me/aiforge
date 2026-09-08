#pragma once

#include <expected>
#include <optional>

#include <aiforge/domain/event_log.hpp>

namespace aiforge::runtime {

struct ConversationPolicySnapshot {
  domain::ConversationPolicy policy;
  std::optional<domain::EventId> event_id;
  std::uint64_t event_sequence{};
  auto operator==(const ConversationPolicySnapshot&) const -> bool = default;
};

// A change is one atomic, top-level control run: start, policy, completed.
// A partial or unsupported policy transaction cannot silently restore full
// mode.
[[nodiscard]] auto recorded_conversation_policy(
    const domain::SessionEventLog& log,
    std::optional<std::uint64_t> snapshot_sequence = std::nullopt)
    -> std::expected<ConversationPolicySnapshot,
                     domain::ConversationAdmissionError>;

} // namespace aiforge::runtime
