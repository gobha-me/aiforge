#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <aiforge/domain/events.hpp>

namespace aiforge::domain {

enum class InferenceUsageStatus {
  active,
  completed,
  failed,
  cancelled,
};

struct InferenceUsageRecord {
  RunId run_id;
  InferenceId inference_id;
  ModelId model_id;
  EventTimestamp started_at;
  std::optional<EventTimestamp> ended_at;
  InferenceUsageStatus status{InferenceUsageStatus::active};
  Usage usage;
  auto operator==(const InferenceUsageRecord &) const -> bool = default;
};

enum class UsageLedgerErrorCode {
  invalid_envelope,
  duplicate_event_id,
  non_monotonic_sequence,
  duplicate_inference,
  unknown_inference,
  wrong_run,
  invalid_transition,
  usage_overflow,
};

struct UsageLedgerError {
  UsageLedgerErrorCode code;
  std::string message;
  auto operator==(const UsageLedgerError &) const -> bool = default;
};

class UsageLedgerProjection final {
public:
  // Callers apply exactly one session's stream in session-sequence order.
  [[nodiscard]] auto apply(const RunEvent &event)
      -> std::expected<void, UsageLedgerError>;

  [[nodiscard]] auto records() const noexcept
      -> const std::vector<InferenceUsageRecord> & {
    return m_records;
  }
  [[nodiscard]] auto total_usage() const noexcept -> const Usage & {
    return m_total_usage;
  }
  [[nodiscard]] auto last_sequence() const noexcept -> std::uint64_t {
    return m_last_sequence;
  }

private:
  [[nodiscard]] auto find_record(const InferenceId &inference_id)
      -> InferenceUsageRecord *;

  std::vector<InferenceUsageRecord> m_records;
  Usage m_total_usage;
  std::set<EventId> m_event_ids;
  std::uint64_t m_last_sequence{};
};

} // namespace aiforge::domain
