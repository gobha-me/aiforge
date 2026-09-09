#pragma once

#include <aiforge/domain/event_log.hpp>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace aiforge::runtime {

inline constexpr std::size_t maximum_ops_history_events = 1000000;
inline constexpr std::size_t maximum_ops_history_invocations = 4096;
inline constexpr std::size_t maximum_ops_history_request_bytes =
    std::size_t{16} * 1024U * 1024U;
inline constexpr std::size_t maximum_ops_observation_content_bytes =
    std::size_t{256} * 1024U;

enum class OpsHistoryErrorCode {
  invalid_history,
  resource_exhausted,
  internal_failure
};
struct OpsHistoryError {
  OpsHistoryErrorCode code;
  std::string message;
  auto operator==(const OpsHistoryError&) const -> bool = default;
};
enum class OpsInvocationPhase {
  proposed,
  awaiting_approval,
  allowed,
  running,
  succeeded,
  failed
};
struct RecordedOpsInvocation {
  domain::RunId run_id;
  domain::InvocationId invocation_id;
  domain::OpsObservationRequest request;
  bool human_origin{};
  OpsInvocationPhase phase{OpsInvocationPhase::proposed};
  std::optional<domain::EventId> observation_event_id{};
  std::optional<domain::EventId> result_event_id{};
};
struct OpsHistorySnapshot {
  std::uint64_t last_sequence{};
  std::vector<RecordedOpsInvocation> invocations;
  std::vector<domain::RunId> unfinished_manual_runs;
};

// Validate a complete historical snapshot or prospective atomic transaction.
// Never creates current authority, collections, retries, or external effects.
// Typed proof consistency does not establish the meaning of executor JSON;
// that remains the runtime argument adapter's separate admission obligation.
[[nodiscard]] auto recorded_ops_observations(
    const domain::SessionEventLog& log,
    std::span<const domain::RunEvent> prospective_suffix = {})
    -> std::expected<OpsHistorySnapshot, OpsHistoryError>;

// Version-1 deterministic, bounded, single-TextBlock projection of validated
// historical data. Exact typed fields and unknowns are retained in source
// order.
[[nodiscard]] auto format_ops_observation_content(
    const domain::OpsObservation& observation)
    -> std::expected<std::vector<domain::ContentBlock>, OpsHistoryError>;

} // namespace aiforge::runtime
