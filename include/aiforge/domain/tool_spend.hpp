#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include <aiforge/domain/digest.hpp>
#include <aiforge/domain/events_fwd.hpp>
#include <aiforge/domain/ids.hpp>
#include <aiforge/domain/money.hpp>
#include <aiforge/domain/usage_ledger.hpp>

namespace aiforge::domain {

enum class ToolSpendEstimateBasis {
  catalog_estimate,
  policy_upper_bound,
};

enum class ToolSpendFinalizationBasis {
  provider_reported,
  catalog_estimate,
  policy_upper_bound,
};

enum class ToolSpendReconciliationReason {
  transport_outcome_unknown,
  provider_cost_unavailable,
  provider_cost_mismatch,
  finalization_persistence_unknown,
};

struct ToolSpendQuote {
  MonetaryAmount maximum;
  ToolSpendEstimateBasis basis{ToolSpendEstimateBasis::policy_upper_bound};
  ContentDigest evidence_digest;
  EventTimestamp valid_until;
  auto operator==(const ToolSpendQuote&) const -> bool = default;
};

struct ToolSpendReservation {
  InvocationId invocation_id;
  MonetaryAmount maximum;
  ToolSpendEstimateBasis basis{ToolSpendEstimateBasis::policy_upper_bound};
  ContentDigest evidence_digest;
  EventTimestamp valid_until;
  auto operator==(const ToolSpendReservation&) const -> bool = default;
};

struct ToolSpendFinalization {
  InvocationId invocation_id;
  MonetaryAmount amount;
  ToolSpendFinalizationBasis basis{
      ToolSpendFinalizationBasis::policy_upper_bound};
  std::optional<ContentDigest> provider_evidence_digest;
  auto operator==(const ToolSpendFinalization&) const -> bool = default;
};

enum class ToolSpendStatus {
  reserved,
  released,
  finalized,
  reconciliation_required,
};

struct ToolSpendRecord {
  RunId run_id;
  ToolSpendReservation reservation;
  ToolSpendStatus status{ToolSpendStatus::reserved};
  std::optional<ToolSpendFinalization> finalization;
  std::optional<ToolSpendReconciliationReason> reconciliation_reason;
  bool tool_started{};
  auto operator==(const ToolSpendRecord&) const -> bool = default;
};

enum class ToolSpendLedgerErrorCode {
  invalid_envelope,
  duplicate_event_id,
  non_monotonic_sequence,
  invalid_reservation,
  duplicate_reservation,
  unknown_reservation,
  wrong_run,
  invalid_transition,
  invalid_finalization,
  amount_overflow,
};

struct ToolSpendLedgerError {
  ToolSpendLedgerErrorCode code{ToolSpendLedgerErrorCode::invalid_envelope};
  std::string message;
  auto operator==(const ToolSpendLedgerError&) const -> bool = default;
};

struct ToolSpendSummary {
  SessionSpendCeiling ceiling;
  MonetaryAmount accounted;
  MonetaryAmount remaining;
  MonetaryAmount reserved_maximum;
  MonetaryAmount reconciliation_maximum;
  MonetaryAmount provider_reported_amount;
  MonetaryAmount catalog_estimate_amount;
  MonetaryAmount policy_upper_bound_amount;
  std::size_t reservations{};
  std::size_t finalized{};
  std::size_t released{};
  std::size_t reconciliation_required{};
  std::size_t reserved{};
  std::size_t provider_reported{};
  std::size_t catalog_estimate{};
  std::size_t policy_upper_bound{};
  bool reached{};
  auto operator==(const ToolSpendSummary&) const -> bool = default;
};

class ToolSpendLedgerProjection final {
 public:
  [[nodiscard]] auto apply(const RunEvent& event)
      -> std::expected<void, ToolSpendLedgerError>;

  [[nodiscard]] auto records() const noexcept
      -> const std::vector<ToolSpendRecord>& {
    return m_records;
  }
  [[nodiscard]] auto last_sequence() const noexcept -> std::uint64_t {
    return m_last_sequence;
  }

 private:
  struct ProposalRecord {
    RunId run_id;
    std::optional<ToolSpendQuote> quote;
  };

  std::vector<ToolSpendRecord> m_records;
  std::map<InvocationId, ProposalRecord> m_proposals;
  std::set<EventId> m_event_ids;
  std::set<InvocationId> m_started_invocations;
  std::set<InvocationId> m_terminal_invocations;
  std::uint64_t m_last_sequence{};
};

[[nodiscard]] auto summarize_tool_spend(const ToolSpendLedgerProjection& ledger,
                                        const SessionSpendCeiling& ceiling)
    -> std::expected<ToolSpendSummary, ToolSpendLedgerError>;

[[nodiscard]] auto summarize_combined_session_spend(
    std::span<const InferenceUsageRecord> inference_records,
    const ToolSpendLedgerProjection& tool_ledger,
    const SessionSpendCeiling& ceiling)
    -> std::expected<SessionSpendSummary, ToolSpendLedgerError>;

[[nodiscard]] auto valid_tool_spend_reservation(
    const ToolSpendReservation& reservation) noexcept -> bool;
[[nodiscard]] auto valid_tool_spend_quote(const ToolSpendQuote& quote) noexcept
    -> bool;
[[nodiscard]] auto valid_tool_spend_finalization_shape(
    const ToolSpendFinalization& finalization) noexcept -> bool;
[[nodiscard]] auto valid_tool_spend_finalization(
    const ToolSpendFinalization& finalization,
    const ToolSpendReservation& reservation) noexcept -> bool;
[[nodiscard]] auto valid_tool_spend_reconciliation_reason(
    ToolSpendReconciliationReason reason) noexcept -> bool;

} // namespace aiforge::domain
