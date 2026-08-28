#include <aiforge/domain/event_log.hpp>

#include <utility>

namespace aiforge::domain {

auto SessionEventLog::append(RunEvent event)
    -> std::expected<void, EventLogError> {
  if (event.metadata.sequence == 0) {
    return std::unexpected(EventLogError{EventLogErrorCode::zero_sequence,
                                         "event sequence must be positive"});
  }
  if (event.metadata.schema_version == 0) {
    return std::unexpected(
        EventLogError{EventLogErrorCode::zero_schema_version,
                      "event schema version must be positive"});
  }
  if (m_event_ids.contains(event.metadata.event_id)) {
    return std::unexpected(
        EventLogError{EventLogErrorCode::duplicate_event_id,
                      "event ID is already present in the session"});
  }
  if (event.metadata.sequence <= m_last_sequence) {
    return std::unexpected(
        EventLogError{EventLogErrorCode::non_monotonic_sequence,
                      "event sequence must increase"});
  }

  m_last_sequence = event.metadata.sequence;
  m_event_ids.insert(event.metadata.event_id);
  m_events.push_back(std::move(event));
  return {};
}

} // namespace aiforge::domain
