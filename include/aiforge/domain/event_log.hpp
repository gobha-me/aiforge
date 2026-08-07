#pragma once

#include <cstdint>
#include <expected>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <aiforge/domain/events.hpp>

namespace aiforge::domain {

enum class EventLogErrorCode {
  zero_sequence,
  zero_schema_version,
  duplicate_event_id,
  non_monotonic_sequence,
};

struct EventLogError {
  EventLogErrorCode code;
  std::string message;
  auto operator==(const EventLogError&) const -> bool = default;
};

class SessionEventLog final {
 public:
  explicit SessionEventLog(SessionId session_id) : m_session_id(std::move(session_id)) {}

  [[nodiscard]] auto append(RunEvent event) -> std::expected<void, EventLogError>;

  [[nodiscard]] auto session_id() const noexcept -> const SessionId& { return m_session_id; }
  [[nodiscard]] auto events() const noexcept -> const std::vector<RunEvent>& { return m_events; }
  [[nodiscard]] auto last_sequence() const noexcept -> std::uint64_t { return m_last_sequence; }

 private:
  SessionId m_session_id;
  std::vector<RunEvent> m_events;
  std::set<EventId> m_event_ids;
  std::uint64_t m_last_sequence{};
};

}  // namespace aiforge::domain
