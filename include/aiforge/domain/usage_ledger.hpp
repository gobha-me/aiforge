#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <set>
#include <span>
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
  bool usage_observed{};
  std::optional<ReportedCost> reported_cost;
  std::optional<PricingObservation> pricing_observation;
  auto operator==(const InferenceUsageRecord &) const -> bool = default;
};

enum class CostEstimateUnit {
  usd,
  venice_diem,
};

enum class PricingTierSelection {
  base,
  extended,
};

enum class CostEstimateUnavailableReason {
  usage_unobserved,
  pricing_unobserved,
  invalid_pricing,
  inconsistent_usage,
  ambiguous_extended_tier,
  cache_write_usage_unavailable,
  missing_rate,
  arithmetic_overflow,
};

struct InferenceCostEstimate {
  MonetaryAmount amount;
  PricingTierSelection tier{PricingTierSelection::base};
  ContentDigest rate_card_digest;
  auto operator==(const InferenceCostEstimate &) const -> bool = default;
};

struct CostEstimateUnavailable {
  CostEstimateUnavailableReason reason{
      CostEstimateUnavailableReason::pricing_unobserved};
  auto operator==(const CostEstimateUnavailable &) const -> bool = default;
};

struct CostEstimateFailureCount {
  CostEstimateUnavailableReason reason{
      CostEstimateUnavailableReason::pricing_unobserved};
  std::size_t count{};
  auto operator==(const CostEstimateFailureCount &) const -> bool = default;
};

struct SessionCostEstimate {
  CostEstimateUnit unit{CostEstimateUnit::usd};
  std::optional<MonetaryAmount> subtotal;
  std::size_t estimated_inferences{};
  std::size_t total_inferences{};
  std::vector<CostEstimateFailureCount> unavailable;
  std::optional<CostEstimateUnavailableReason> aggregation_failure;
  auto operator==(const SessionCostEstimate &) const -> bool = default;
};

[[nodiscard]] auto estimate_inference_cost(const InferenceUsageRecord &record,
                                           CostEstimateUnit unit)
    -> std::expected<InferenceCostEstimate, CostEstimateUnavailable>;

[[nodiscard]] auto summarize_cost_estimates(
    std::span<const InferenceUsageRecord> records, CostEstimateUnit unit)
    -> SessionCostEstimate;

[[nodiscard]] auto cost_estimate_unit_name(CostEstimateUnit unit) noexcept
    -> std::string_view;

[[nodiscard]] auto cost_estimate_reason_name(
    CostEstimateUnavailableReason reason) noexcept -> std::string_view;

enum class UsageLedgerErrorCode {
  invalid_envelope,
  duplicate_event_id,
  non_monotonic_sequence,
  duplicate_inference,
  unknown_inference,
  wrong_run,
  invalid_transition,
  usage_overflow,
  cost_overflow,
  invalid_pricing,
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
  [[nodiscard]] auto total_reported_cost() const noexcept
      -> const std::optional<ReportedCost> & {
    return m_total_reported_cost;
  }
  [[nodiscard]] auto last_sequence() const noexcept -> std::uint64_t {
    return m_last_sequence;
  }

private:
  [[nodiscard]] auto find_record(const InferenceId &inference_id)
      -> InferenceUsageRecord *;

  std::vector<InferenceUsageRecord> m_records;
  Usage m_total_usage;
  std::optional<ReportedCost> m_total_reported_cost;
  std::set<EventId> m_event_ids;
  std::uint64_t m_last_sequence{};
};

} // namespace aiforge::domain
