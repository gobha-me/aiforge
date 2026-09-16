#pragma once

#include <aiforge/domain/context.hpp>
#include <aiforge/domain/event_log.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace aiforge::runtime {

inline constexpr std::uint32_t ops_explanation_projection_version{1};

struct OpsExplanationLimits {
  std::size_t maximum_events{1000000};
  std::size_t maximum_selections{4096};
  std::size_t maximum_evidence_bytes{std::size_t{64} * 1024U};
  std::uint64_t maximum_estimated_tokens{std::uint64_t{64} * 1024U};
  auto operator==(const OpsExplanationLimits&) const -> bool = default;
};

enum class OpsExplanationErrorCode {
  invalid_selection,
  stale_selection,
  not_observation,
  invalid_history,
  resource_exhausted,
  cancelled,
  internal_failure,
};

struct OpsExplanationError {
  OpsExplanationErrorCode code;
  std::string message;
  auto operator==(const OpsExplanationError&) const -> bool = default;
};

struct PreparedOpsExplanation {
  domain::OpsObservationExplanationSelected selection;
  domain::ContextContentInput evidence;
  std::uint64_t observation_sequence{};
  auto operator==(const PreparedOpsExplanation&) const -> bool = default;
};

struct RecordedOpsExplanation {
  domain::RunId run_id;
  domain::EventId selection_event_id;
  PreparedOpsExplanation prepared;
  auto operator==(const RecordedOpsExplanation&) const -> bool = default;
};

// Returns the one application-runtime instruction admitted by an Explain
// request. Its text, identity and digest are runtime-owned and deterministic.
[[nodiscard]] auto ops_explanation_runtime_instruction(std::uint64_t order)
    -> std::expected<domain::InstructionInput, OpsExplanationError>;

// Builds one bounded evidence entry from an exact event already committed to
// this session. This scans durable memory only: no source, broker, artifact,
// provider, or other I/O boundary is consulted.
[[nodiscard]] auto prepare_ops_explanation(
    const domain::SessionEventLog& log,
    const domain::EventId& observation_event_id, std::uint64_t order,
    const OpsExplanationLimits& limits = {}, std::stop_token stop = {})
    -> std::expected<PreparedOpsExplanation, OpsExplanationError>;

// Rebuilds every selection from durable history. Selection must occur in a
// conversation run before user/model content and may reference only an earlier
// successfully committed typed observation. Replay performs no external work.
[[nodiscard]] auto recorded_ops_explanations(
    const domain::SessionEventLog& log, const OpsExplanationLimits& limits = {},
    std::stop_token stop = {})
    -> std::expected<std::vector<RecordedOpsExplanation>, OpsExplanationError>;

// Requires the exact derived untrusted entry to be admitted once in a backend
// request. Its order may be assigned by the surrounding context builder.
[[nodiscard]] auto ops_explanation_matches_context(
    const PreparedOpsExplanation& prepared, const domain::Message& user_message,
    const domain::ConstructedContext& context) noexcept -> bool;

} // namespace aiforge::runtime
