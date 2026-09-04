#include <aiforge/domain/usage_ledger.hpp>

#include <aiforge/domain/events.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace aiforge::domain {
namespace {

template <typename... Visitors> struct Overloaded : Visitors... {
  using Visitors::operator()...;
};

template <typename... Visitors>
Overloaded(Visitors...) -> Overloaded<Visitors...>;

[[nodiscard]] auto error(const UsageLedgerErrorCode code, std::string message)
    -> std::expected<void, UsageLedgerError> {
  return std::unexpected(UsageLedgerError{code, std::move(message)});
}

[[nodiscard]] auto add_checked(std::uint64_t& total, const std::uint64_t amount)
    -> bool {
  if (amount > std::numeric_limits<std::uint64_t>::max() - total) return false;
  total += amount;
  return true;
}

[[nodiscard]] auto add_usage(Usage& total, const Usage& addition) -> bool {
  return add_checked(total.input_tokens, addition.input_tokens) &&
         add_checked(total.output_tokens, addition.output_tokens) &&
         add_checked(total.cached_input_tokens, addition.cached_input_tokens) &&
         add_checked(total.reasoning_tokens, addition.reasoning_tokens);
}

[[nodiscard]] auto multiplied_text(std::uint64_t left, std::uint64_t right)
    -> std::string {
  constexpr std::uint64_t base = 1'000'000'000;
  std::array<std::uint64_t, 3> left_parts{};
  std::array<std::uint64_t, 3> right_parts{};
  for (auto& part : left_parts) {
    part = left % base;
    left /= base;
  }
  for (auto& part : right_parts) {
    part = right % base;
    right /= base;
  }
  std::array<std::uint64_t, 7> result_parts{};
  for (std::size_t left_index = 0; left_index < left_parts.size();
       ++left_index) {
    for (std::size_t right_index = 0; right_index < right_parts.size();
         ++right_index) {
      result_parts[left_index + right_index] +=
          left_parts[left_index] * right_parts[right_index];
    }
  }
  for (std::size_t index = 0; index + 1 < result_parts.size(); ++index) {
    result_parts[index + 1] += result_parts[index] / base;
    result_parts[index] %= base;
  }
  auto highest = result_parts.size() - 1;
  while (highest > 0 && result_parts[highest] == 0)
    --highest;
  auto result = std::to_string(result_parts[highest]);
  while (highest > 0) {
    --highest;
    auto part = std::to_string(result_parts[highest]);
    result += std::string(9U - part.size(), '0') + part;
  }
  return result;
}

[[nodiscard]] auto scaled_amount(const DecimalAmount& rate,
                                 const std::uint64_t tokens)
    -> std::expected<DecimalAmount, CostEstimateUnavailable> {
  auto scale = static_cast<std::size_t>(rate.scale()) + 6U;
  auto text = multiplied_text(rate.coefficient(), tokens);
  while (scale > 0 && text.size() > 1 && text.back() == '0') {
    text.pop_back();
    --scale;
  }
  if (scale != 0) {
    if (text.size() <= scale) {
      text = "0." + std::string(scale - text.size(), '0') + text;
    } else {
      text.insert(text.size() - scale, 1, '.');
    }
  }
  auto amount = DecimalAmount::from(text);
  if (!amount) {
    return std::unexpected(CostEstimateUnavailable{
        CostEstimateUnavailableReason::arithmetic_overflow});
  }
  return *amount;
}

[[nodiscard]] auto price_for_unit(const PriceRate& price,
                                  const CostEstimateUnit unit)
    -> const std::optional<DecimalAmount>& {
  return unit == CostEstimateUnit::usd ? price.usd : price.diem;
}

[[nodiscard]] auto unit_present(const TextPriceTier& tier,
                                const CostEstimateUnit unit) -> bool {
  const auto present = [unit](const std::optional<PriceRate>& price) {
    return price && price_for_unit(*price, unit).has_value();
  };
  return present(tier.input) || present(tier.output) ||
         present(tier.cache_input) || present(tier.cache_write);
}

[[nodiscard]] auto select_tier(const Usage& usage, const TextPricing& pricing)
    -> std::expected<std::pair<const TextPriceTier*, PricingTierSelection>,
                     CostEstimateUnavailable> {
  if (!pricing.extended || !pricing.extended_threshold_tokens) {
    return std::pair{&pricing.base, PricingTierSelection::base};
  }
  if (usage.output_tokens >
      std::numeric_limits<std::uint64_t>::max() - usage.input_tokens) {
    return std::unexpected(CostEstimateUnavailable{
        CostEstimateUnavailableReason::arithmetic_overflow});
  }
  const auto total_tokens = usage.input_tokens + usage.output_tokens;
  if (total_tokens <= *pricing.extended_threshold_tokens) {
    return std::pair{&pricing.base, PricingTierSelection::base};
  }
  if (usage.input_tokens > *pricing.extended_threshold_tokens) {
    return std::pair{&*pricing.extended, PricingTierSelection::extended};
  }
  return std::unexpected(CostEstimateUnavailable{
      CostEstimateUnavailableReason::ambiguous_extended_tier});
}

[[nodiscard]] auto bucket_amount(const std::optional<PriceRate>& price,
                                 const CostEstimateUnit unit,
                                 const std::uint64_t tokens)
    -> std::expected<DecimalAmount, CostEstimateUnavailable> {
  if (tokens == 0) return DecimalAmount::from("0").value();
  if (!price || !price_for_unit(*price, unit)) {
    return std::unexpected(
        CostEstimateUnavailable{CostEstimateUnavailableReason::missing_rate});
  }
  return scaled_amount(*price_for_unit(*price, unit), tokens);
}

auto add_failure(SessionCostEstimate& summary,
                 const CostEstimateUnavailableReason reason) -> void {
  const auto found = std::ranges::find(summary.unavailable, reason,
                                       &CostEstimateFailureCount::reason);
  if (found == summary.unavailable.end()) {
    summary.unavailable.push_back({reason, 1});
  } else {
    ++found->count;
  }
}

auto add_failure(SessionSpendSummary& summary,
                 const CostEstimateUnavailableReason reason) -> void {
  const auto found = std::ranges::find(summary.unavailable, reason,
                                       &CostEstimateFailureCount::reason);
  if (found == summary.unavailable.end()) {
    summary.unavailable.push_back({reason, 1});
  } else {
    ++found->count;
  }
}

[[nodiscard]] auto reported_usd(const ReportedCost& cost)
    -> const MonetaryAmount* {
  const auto found = std::ranges::find(cost.amounts(), std::string_view{"USD"},
                                       &MonetaryAmount::unit);
  return found == cost.amounts().end() ? nullptr : &*found;
}

} // namespace

auto cost_estimate_unit_name(const CostEstimateUnit unit) noexcept
    -> std::string_view {
  switch (unit) {
    case CostEstimateUnit::usd: return "USD";
    case CostEstimateUnit::venice_diem: return "venice.diem";
  }
  return "unknown";
}

auto cost_estimate_reason_name(
    const CostEstimateUnavailableReason reason) noexcept -> std::string_view {
  switch (reason) {
    case CostEstimateUnavailableReason::usage_unobserved:
      return "usage unavailable";
    case CostEstimateUnavailableReason::pricing_unobserved:
      return "pricing unavailable";
    case CostEstimateUnavailableReason::invalid_pricing:
      return "pricing invalid";
    case CostEstimateUnavailableReason::inconsistent_usage:
      return "usage inconsistent";
    case CostEstimateUnavailableReason::ambiguous_extended_tier:
      return "pricing tier ambiguous";
    case CostEstimateUnavailableReason::cache_write_usage_unavailable:
      return "cache-write usage unavailable";
    case CostEstimateUnavailableReason::missing_rate: return "rate unavailable";
    case CostEstimateUnavailableReason::arithmetic_overflow:
      return "arithmetic unavailable";
  }
  return "unknown";
}

auto estimate_inference_cost(const InferenceUsageRecord& record,
                             const CostEstimateUnit unit)
    -> std::expected<InferenceCostEstimate, CostEstimateUnavailable> {
  if (!record.usage_observed) {
    return std::unexpected(CostEstimateUnavailable{
        CostEstimateUnavailableReason::usage_unobserved});
  }
  if (!record.pricing_observation) {
    return std::unexpected(CostEstimateUnavailable{
        CostEstimateUnavailableReason::pricing_unobserved});
  }
  if (!validate_pricing_observation(*record.pricing_observation)) {
    return std::unexpected(CostEstimateUnavailable{
        CostEstimateUnavailableReason::invalid_pricing});
  }
  if (record.usage.cached_input_tokens > record.usage.input_tokens ||
      record.usage.reasoning_tokens > record.usage.output_tokens) {
    return std::unexpected(CostEstimateUnavailable{
        CostEstimateUnavailableReason::inconsistent_usage});
  }
  auto selected =
      select_tier(record.usage, record.pricing_observation->pricing);
  if (!selected) return std::unexpected(selected.error());
  const auto& [tier, selection] = *selected;
  if (tier->cache_write) {
    return std::unexpected(CostEstimateUnavailable{
        CostEstimateUnavailableReason::cache_write_usage_unavailable});
  }
  if (!unit_present(*tier, unit)) {
    return std::unexpected(
        CostEstimateUnavailable{CostEstimateUnavailableReason::missing_rate});
  }

  const auto uncached =
      record.usage.input_tokens - record.usage.cached_input_tokens;
  auto input = bucket_amount(tier->input, unit, uncached);
  auto cached =
      bucket_amount(tier->cache_input, unit, record.usage.cached_input_tokens);
  auto output = bucket_amount(tier->output, unit, record.usage.output_tokens);
  if (!input) return std::unexpected(input.error());
  if (!cached) return std::unexpected(cached.error());
  if (!output) return std::unexpected(output.error());
  auto input_and_cache = add(*input, *cached);
  if (!input_and_cache) {
    return std::unexpected(CostEstimateUnavailable{
        CostEstimateUnavailableReason::arithmetic_overflow});
  }
  auto total = add(*input_and_cache, *output);
  if (!total) {
    return std::unexpected(CostEstimateUnavailable{
        CostEstimateUnavailableReason::arithmetic_overflow});
  }
  auto amount = MonetaryAmount::create(
      std::string{cost_estimate_unit_name(unit)}, *total);
  if (!amount) {
    return std::unexpected(CostEstimateUnavailable{
        CostEstimateUnavailableReason::arithmetic_overflow});
  }
  return InferenceCostEstimate{std::move(*amount), selection,
                               record.pricing_observation->rate_card_digest};
}

auto summarize_cost_estimates(
    const std::span<const InferenceUsageRecord> records,
    const CostEstimateUnit unit) -> SessionCostEstimate {
  SessionCostEstimate result;
  result.unit = unit;
  result.total_inferences = records.size();
  std::optional<DecimalAmount> total;
  for (const auto& record : records) {
    auto estimate = estimate_inference_cost(record, unit);
    if (!estimate) {
      add_failure(result, estimate.error().reason);
      continue;
    }
    ++result.estimated_inferences;
    if (!total) {
      total = estimate->amount.amount();
      continue;
    }
    auto combined = add(*total, estimate->amount.amount());
    if (!combined) {
      result.aggregation_failure =
          CostEstimateUnavailableReason::arithmetic_overflow;
      total.reset();
      continue;
    }
    if (!result.aggregation_failure) total = *combined;
  }
  if (total && !result.aggregation_failure) {
    auto subtotal = MonetaryAmount::create(
        std::string{cost_estimate_unit_name(unit)}, *total);
    if (subtotal) {
      result.subtotal = std::move(*subtotal);
    } else {
      result.aggregation_failure =
          CostEstimateUnavailableReason::arithmetic_overflow;
    }
  }
  return result;
}

auto summarize_session_spend(
    const std::span<const InferenceUsageRecord> records,
    const SessionSpendCeiling& ceiling) -> SessionSpendSummary {
  SessionSpendSummary result{ceiling, std::nullopt, std::nullopt, 0,    0,
                             0,       {},           std::nullopt, false};
  result.total_inferences = records.size();
  std::optional<DecimalAmount> total{DecimalAmount::from("0").value()};
  for (const auto& record : records) {
    std::optional<DecimalAmount> amount;
    if (record.reported_cost) {
      if (const auto* reported = reported_usd(*record.reported_cost)) {
        amount = reported->amount();
        ++result.reported_inferences;
      }
    }
    if (!amount) {
      auto estimate = estimate_inference_cost(record, CostEstimateUnit::usd);
      if (!estimate) {
        add_failure(result, estimate.error().reason);
        continue;
      }
      amount = estimate->amount.amount();
      ++result.estimated_inferences;
    }
    auto combined = add(*total, *amount);
    if (!combined) {
      result.aggregation_failure =
          CostEstimateUnavailableReason::arithmetic_overflow;
      total.reset();
      break;
    }
    total = *combined;
  }

  const auto accounted_count =
      result.reported_inferences + result.estimated_inferences;
  if (!total || result.aggregation_failure ||
      accounted_count != result.total_inferences) {
    return result;
  }
  auto accounted = MonetaryAmount::create("USD", *total);
  if (!accounted) {
    result.aggregation_failure =
        CostEstimateUnavailableReason::arithmetic_overflow;
    return result;
  }
  result.accounted = std::move(*accounted);
  result.reached =
      compare(*total, ceiling.amount()) != std::strong_ordering::less;
  const auto remaining_amount = result.reached
                                    ? DecimalAmount::from("0")
                                    : subtract(ceiling.amount(), *total);
  if (!remaining_amount) {
    result.accounted.reset();
    result.aggregation_failure =
        CostEstimateUnavailableReason::arithmetic_overflow;
    return result;
  }
  auto remaining = MonetaryAmount::create("USD", *remaining_amount);
  if (!remaining) {
    result.accounted.reset();
    result.aggregation_failure =
        CostEstimateUnavailableReason::arithmetic_overflow;
    return result;
  }
  result.remaining = std::move(*remaining);
  return result;
}

auto UsageLedgerProjection::find_record(const InferenceId& inference_id)
    -> InferenceUsageRecord* {
  const auto found = std::ranges::find(m_records, inference_id,
                                       &InferenceUsageRecord::inference_id);
  return found == m_records.end() ? nullptr : &*found;
}

auto UsageLedgerProjection::apply(const RunEvent& event)
    -> std::expected<void, UsageLedgerError> {
  if (event.metadata.sequence == 0 || event.metadata.schema_version == 0) {
    return error(UsageLedgerErrorCode::invalid_envelope,
                 "event sequence and schema version must be positive");
  }
  if (m_event_ids.contains(event.metadata.event_id)) {
    return error(UsageLedgerErrorCode::duplicate_event_id,
                 "event ID is already present in the usage ledger");
  }
  if (event.metadata.sequence <= m_last_sequence) {
    return error(UsageLedgerErrorCode::non_monotonic_sequence,
                 "event sequence must increase");
  }

  const auto finish = [&](const InferenceId& inference_id,
                          const InferenceUsageStatus status)
      -> std::expected<void, UsageLedgerError> {
    auto* record = find_record(inference_id);
    if (record == nullptr) {
      return error(UsageLedgerErrorCode::unknown_inference,
                   "inference terminal event has no matching start");
    }
    if (record->run_id != event.metadata.run_id) {
      return error(UsageLedgerErrorCode::wrong_run,
                   "inference terminal event belongs to another run");
    }
    if (record->status != InferenceUsageStatus::active) {
      return error(UsageLedgerErrorCode::invalid_transition,
                   "inference usage record is already terminal");
    }
    record->status = status;
    record->ended_at = event.metadata.timestamp;
    return {};
  };

  const auto result = std::visit(
      Overloaded{
          [&](const InferenceStarted& started)
              -> std::expected<void, UsageLedgerError> {
            if (find_record(started.inference_id) != nullptr) {
              return error(
                  UsageLedgerErrorCode::duplicate_inference,
                  "inference ID is already present in the usage ledger");
            }
            m_records.push_back({event.metadata.run_id,
                                 started.inference_id,
                                 started.model_id,
                                 event.metadata.timestamp,
                                 std::nullopt,
                                 InferenceUsageStatus::active,
                                 {},
                                 {},
                                 {},
                                 {}});
            return {};
          },
          [&](const InferencePricingObserved& observed)
              -> std::expected<void, UsageLedgerError> {
            auto* record = find_record(observed.inference_id);
            if (record == nullptr) {
              return error(UsageLedgerErrorCode::unknown_inference,
                           "pricing has no matching inference start");
            }
            if (record->run_id != event.metadata.run_id) {
              return error(UsageLedgerErrorCode::wrong_run,
                           "pricing belongs to another run");
            }
            if (record->status != InferenceUsageStatus::active ||
                record->pricing_observation ||
                record->model_id != observed.observation.model_id) {
              return error(
                  UsageLedgerErrorCode::invalid_transition,
                  "pricing cannot follow a terminal or prior observation");
            }
            if (!validate_pricing_observation(observed.observation)) {
              return error(UsageLedgerErrorCode::invalid_pricing,
                           "pricing observation is malformed");
            }
            record->pricing_observation = observed.observation;
            return {};
          },
          [&](const UsageRecorded& recorded)
              -> std::expected<void, UsageLedgerError> {
            auto* record = find_record(recorded.inference_id);
            if (record == nullptr) {
              return error(UsageLedgerErrorCode::unknown_inference,
                           "usage event has no matching inference start");
            }
            if (record->run_id != event.metadata.run_id) {
              return error(UsageLedgerErrorCode::wrong_run,
                           "usage event belongs to another run");
            }
            if (record->status != InferenceUsageStatus::active) {
              return error(UsageLedgerErrorCode::invalid_transition,
                           "usage cannot follow an inference terminal event");
            }

            auto next_record_usage = record->usage;
            auto next_total_usage = m_total_usage;
            if (!add_usage(next_record_usage, recorded.usage) ||
                !add_usage(next_total_usage, recorded.usage)) {
              return error(UsageLedgerErrorCode::usage_overflow,
                           "usage ledger total overflow");
            }
            record->usage = next_record_usage;
            record->usage_observed = true;
            m_total_usage = next_total_usage;
            return {};
          },
          [&](const InferenceCostRecorded& recorded)
              -> std::expected<void, UsageLedgerError> {
            auto* record = find_record(recorded.inference_id);
            if (record == nullptr) {
              return error(UsageLedgerErrorCode::unknown_inference,
                           "cost event has no matching inference start");
            }
            if (record->run_id != event.metadata.run_id) {
              return error(UsageLedgerErrorCode::wrong_run,
                           "cost event belongs to another run");
            }
            if (record->status != InferenceUsageStatus::active ||
                record->reported_cost) {
              return error(UsageLedgerErrorCode::invalid_transition,
                           "cost cannot follow a terminal or prior cost event");
            }

            auto next_total = m_total_reported_cost;
            if (next_total) {
              auto combined = add(*next_total, recorded.cost);
              if (!combined) {
                return error(UsageLedgerErrorCode::cost_overflow,
                             "reported cost ledger total overflow");
              }
              next_total = std::move(*combined);
            } else {
              next_total = recorded.cost;
            }
            record->reported_cost = recorded.cost;
            m_total_reported_cost = std::move(next_total);
            return {};
          },
          [&](const InferenceFinished& finished) {
            return finish(finished.inference_id,
                          InferenceUsageStatus::completed);
          },
          [&](const InferenceFailed& failed) {
            return finish(failed.inference_id, InferenceUsageStatus::failed);
          },
          [&](const InferenceCancelled& cancelled) {
            return finish(cancelled.inference_id,
                          InferenceUsageStatus::cancelled);
          },
          [&](const auto&) -> std::expected<void, UsageLedgerError> {
            return {};
          },
      },
      event.payload);

  if (result) {
    m_event_ids.insert(event.metadata.event_id);
    m_last_sequence = event.metadata.sequence;
  }
  return result;
}

auto SessionSpendCeilingProjection::apply(const RunEvent& event)
    -> std::expected<void, UsageLedgerError> {
  if (event.metadata.sequence == 0 || event.metadata.schema_version == 0) {
    return error(UsageLedgerErrorCode::invalid_envelope,
                 "event sequence and schema version must be positive");
  }
  if (m_event_ids.contains(event.metadata.event_id)) {
    return error(UsageLedgerErrorCode::duplicate_event_id,
                 "event ID is already present in the spend ceiling projection");
  }
  if (event.metadata.sequence <= m_last_sequence) {
    return error(UsageLedgerErrorCode::non_monotonic_sequence,
                 "event sequence must increase");
  }

  if (const auto* set = std::get_if<SessionSpendCeilingSet>(&event.payload)) {
    if (set->source != SessionSpendCeilingSource::command_line ||
        !SessionSpendCeiling::create(set->ceiling.amount())) {
      return error(UsageLedgerErrorCode::invalid_ceiling,
                   "session spend ceiling event is invalid");
    }
    if (m_ceiling && compare(set->ceiling.amount(), m_ceiling->amount()) ==
                         std::strong_ordering::greater) {
      return error(UsageLedgerErrorCode::ceiling_widening,
                   "session spend ceiling cannot be widened");
    }
    m_ceiling = set->ceiling;
  }

  m_event_ids.insert(event.metadata.event_id);
  m_last_sequence = event.metadata.sequence;
  return {};
}

} // namespace aiforge::domain
