#include <aiforge/domain/tool_spend.hpp>

#include <algorithm>
#include <compare>
#include <string_view>
#include <utility>

#include <aiforge/domain/events.hpp>

namespace aiforge::domain {
namespace {

[[nodiscard]] auto ledger_error(const ToolSpendLedgerErrorCode code,
                                std::string message) -> ToolSpendLedgerError {
  return {code, std::move(message)};
}

[[nodiscard]] auto valid_digest(const ContentDigest& digest) noexcept -> bool {
  return digest.algorithm == "sha256" && digest.value.size() == 64 &&
         digest.byte_size != 0 &&
         std::ranges::all_of(digest.value, [](const unsigned char byte) {
           return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
         });
}

[[nodiscard]] auto valid_usd(const MonetaryAmount& amount,
                             const bool positive) noexcept -> bool {
  return amount.unit() == "USD" && amount.amount().scale() <= 6 &&
         (!positive || amount.amount().coefficient() != 0);
}

[[nodiscard]] auto event_invocation_matches(const RunEvent& event,
                                            const InvocationId& invocation_id)
    -> bool {
  return event.metadata.invocation_id &&
         *event.metadata.invocation_id == invocation_id;
}

[[nodiscard]] auto valid_estimate_basis(
    const ToolSpendEstimateBasis basis) noexcept -> bool {
  switch (basis) {
    case ToolSpendEstimateBasis::catalog_estimate:
    case ToolSpendEstimateBasis::policy_upper_bound: return true;
  }
  return false;
}

[[nodiscard]] auto valid_finalization_basis(
    const ToolSpendFinalizationBasis basis) noexcept -> bool {
  switch (basis) {
    case ToolSpendFinalizationBasis::provider_reported:
    case ToolSpendFinalizationBasis::catalog_estimate:
    case ToolSpendFinalizationBasis::policy_upper_bound: return true;
  }
  return false;
}

} // namespace

auto valid_tool_spend_reservation(
    const ToolSpendReservation& reservation) noexcept -> bool {
  return valid_tool_spend_quote({reservation.maximum, reservation.basis,
                                 reservation.evidence_digest,
                                 reservation.valid_until});
}

auto valid_tool_spend_quote(const ToolSpendQuote& quote) noexcept -> bool {
  return valid_estimate_basis(quote.basis) && valid_usd(quote.maximum, true) &&
         valid_digest(quote.evidence_digest) &&
         quote.valid_until.time_since_epoch().count() > 0;
}

auto valid_tool_spend_finalization(
    const ToolSpendFinalization& finalization,
    const ToolSpendReservation& reservation) noexcept -> bool {
  try {
    if (finalization.invocation_id != reservation.invocation_id ||
        !valid_tool_spend_finalization_shape(finalization) ||
        compare(finalization.amount.amount(), reservation.maximum.amount()) ==
            std::strong_ordering::greater) {
      return false;
    }
    if (finalization.basis == ToolSpendFinalizationBasis::catalog_estimate &&
        reservation.basis != ToolSpendEstimateBasis::catalog_estimate) {
      return false;
    }
    if (finalization.basis == ToolSpendFinalizationBasis::policy_upper_bound &&
        reservation.basis != ToolSpendEstimateBasis::policy_upper_bound) {
      return false;
    }
    return true;
  } catch (...) {
    return false;
  }
}

auto valid_tool_spend_finalization_shape(
    const ToolSpendFinalization& finalization) noexcept -> bool {
  if (!valid_finalization_basis(finalization.basis) ||
      !valid_usd(finalization.amount, false)) {
    return false;
  }
  if (finalization.basis == ToolSpendFinalizationBasis::provider_reported)
    return finalization.provider_evidence_digest &&
           valid_digest(*finalization.provider_evidence_digest);
  return !finalization.provider_evidence_digest;
}

auto valid_tool_spend_reconciliation_reason(
    const ToolSpendReconciliationReason reason) noexcept -> bool {
  switch (reason) {
    case ToolSpendReconciliationReason::transport_outcome_unknown:
    case ToolSpendReconciliationReason::provider_cost_unavailable:
    case ToolSpendReconciliationReason::provider_cost_mismatch:
    case ToolSpendReconciliationReason::finalization_persistence_unknown:
      return true;
  }
  return false;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Spend reducer.
auto ToolSpendLedgerProjection::apply(const RunEvent& event)
    -> std::expected<void, ToolSpendLedgerError> {
  try {
    if (event.metadata.sequence == 0 || event.metadata.schema_version == 0) {
      return std::unexpected(
          ledger_error(ToolSpendLedgerErrorCode::invalid_envelope,
                       "tool spend event envelope is invalid"));
    }
    if (m_event_ids.contains(event.metadata.event_id)) {
      return std::unexpected(
          ledger_error(ToolSpendLedgerErrorCode::duplicate_event_id,
                       "tool spend event ID is duplicated"));
    }
    if (event.metadata.sequence <= m_last_sequence) {
      return std::unexpected(
          ledger_error(ToolSpendLedgerErrorCode::non_monotonic_sequence,
                       "tool spend event sequence did not advance"));
    }

    auto next = m_records;
    auto proposals = m_proposals;
    auto terminal_invocations = m_terminal_invocations;
    const auto find_next =
        [&](const InvocationId& invocation_id) -> ToolSpendRecord* {
      const auto found = std::ranges::find_if(next, [&](const auto& record) {
        return record.reservation.invocation_id == invocation_id;
      });
      return found == next.end() ? nullptr : &*found;
    };

    if (const auto* proposed = std::get_if<ToolProposed>(&event.payload)) {
      const bool paid =
          std::ranges::find(proposed->declared_effects, Effect::spend) !=
          proposed->declared_effects.end();
      if (!event_invocation_matches(event, proposed->invocation_id) ||
          event.metadata.schema_version != (paid ? 2U : 1U) ||
          paid != proposed->spend_quote.has_value() ||
          (proposed->spend_quote &&
           (!valid_tool_spend_quote(*proposed->spend_quote) ||
            event.metadata.timestamp >= proposed->spend_quote->valid_until)) ||
          !proposals
               .emplace(
                   proposed->invocation_id,
                   ProposalRecord{event.metadata.run_id, proposed->spend_quote})
               .second) {
        return std::unexpected(
            ledger_error(ToolSpendLedgerErrorCode::invalid_transition,
                         "tool spend proposal is invalid"));
      }
    } else if (const auto* reserved =
                   std::get_if<ToolSpendReserved>(&event.payload)) {
      const auto& reservation = reserved->reservation;
      const auto proposed = proposals.find(reservation.invocation_id);
      if (!event_invocation_matches(event, reservation.invocation_id) ||
          !valid_tool_spend_reservation(reservation) ||
          event.metadata.timestamp >= reservation.valid_until ||
          proposed == proposals.end() || !proposed->second.quote ||
          *proposed->second.quote != ToolSpendQuote{reservation.maximum,
                                                    reservation.basis,
                                                    reservation.evidence_digest,
                                                    reservation.valid_until}) {
        return std::unexpected(
            ledger_error(ToolSpendLedgerErrorCode::invalid_reservation,
                         "tool spend reservation is invalid"));
      }
      if (proposed->second.run_id != event.metadata.run_id) {
        return std::unexpected(
            ledger_error(ToolSpendLedgerErrorCode::wrong_run,
                         "tool spend reservation targets the wrong run"));
      }
      if (find_next(reservation.invocation_id) != nullptr) {
        return std::unexpected(
            ledger_error(ToolSpendLedgerErrorCode::duplicate_reservation,
                         "tool spend reservation is duplicated"));
      }
      if (m_started_invocations.contains(reservation.invocation_id)) {
        return std::unexpected(
            ledger_error(ToolSpendLedgerErrorCode::invalid_transition,
                         "tool spend reservation followed tool start"));
      }
      next.push_back({event.metadata.run_id, reservation,
                      ToolSpendStatus::reserved, std::nullopt, std::nullopt,
                      false});
    } else if (const auto* started = std::get_if<ToolStarted>(&event.payload)) {
      const auto proposed = proposals.find(started->invocation_id);
      if (!event_invocation_matches(event, started->invocation_id) ||
          proposed == proposals.end() ||
          m_started_invocations.contains(started->invocation_id)) {
        return std::unexpected(
            ledger_error(ToolSpendLedgerErrorCode::invalid_transition,
                         "tool start has no unique proposal"));
      }
      if (proposed->second.run_id != event.metadata.run_id) {
        return std::unexpected(ledger_error(
            ToolSpendLedgerErrorCode::wrong_run,
            "tool start targets a different run than its proposal"));
      }
      if (auto* record = find_next(started->invocation_id); record != nullptr) {
        if (record->run_id != event.metadata.run_id || record->tool_started ||
            record->status != ToolSpendStatus::reserved) {
          return std::unexpected(
              ledger_error(ToolSpendLedgerErrorCode::invalid_transition,
                           "paid tool start is invalid"));
        }
        record->tool_started = true;
      } else if (proposed->second.quote) {
        return std::unexpected(
            ledger_error(ToolSpendLedgerErrorCode::invalid_transition,
                         "paid tool started without a reservation"));
      }
    } else if (const auto* released =
                   std::get_if<ToolSpendReleased>(&event.payload)) {
      auto* record = find_next(released->invocation_id);
      if (record == nullptr) {
        return std::unexpected(
            ledger_error(ToolSpendLedgerErrorCode::unknown_reservation,
                         "tool spend release has no reservation"));
      }
      if (!event_invocation_matches(event, released->invocation_id) ||
          record->run_id != event.metadata.run_id) {
        return std::unexpected(
            ledger_error(ToolSpendLedgerErrorCode::wrong_run,
                         "tool spend release targets the wrong run"));
      }
      if (record->status != ToolSpendStatus::reserved) {
        return std::unexpected(
            ledger_error(ToolSpendLedgerErrorCode::invalid_transition,
                         "tool spend reservation is already terminal"));
      }
      if (terminal_invocations.contains(released->invocation_id)) {
        return std::unexpected(
            ledger_error(ToolSpendLedgerErrorCode::invalid_transition,
                         "tool spend release followed tool termination"));
      }
      record->status = ToolSpendStatus::released;
    } else if (const auto* finalized =
                   std::get_if<ToolSpendFinalized>(&event.payload)) {
      const auto& finalization = finalized->finalization;
      auto* record = find_next(finalization.invocation_id);
      if (record == nullptr) {
        return std::unexpected(
            ledger_error(ToolSpendLedgerErrorCode::unknown_reservation,
                         "tool spend finalization has no reservation"));
      }
      if (!event_invocation_matches(event, finalization.invocation_id) ||
          record->run_id != event.metadata.run_id) {
        return std::unexpected(
            ledger_error(ToolSpendLedgerErrorCode::wrong_run,
                         "tool spend finalization targets the wrong run"));
      }
      if (record->status != ToolSpendStatus::reserved ||
          !record->tool_started ||
          terminal_invocations.contains(finalization.invocation_id)) {
        return std::unexpected(
            ledger_error(ToolSpendLedgerErrorCode::invalid_transition,
                         "tool spend reservation is already terminal"));
      }
      if (!valid_tool_spend_finalization(finalization, record->reservation)) {
        return std::unexpected(
            ledger_error(ToolSpendLedgerErrorCode::invalid_finalization,
                         "tool spend finalization is invalid"));
      }
      record->status = ToolSpendStatus::finalized;
      record->finalization = finalization;
    } else if (const auto* reconciliation =
                   std::get_if<ToolSpendReconciliationRequired>(
                       &event.payload)) {
      auto* record = find_next(reconciliation->invocation_id);
      if (record == nullptr) {
        return std::unexpected(
            ledger_error(ToolSpendLedgerErrorCode::unknown_reservation,
                         "tool spend reconciliation has no reservation"));
      }
      if (!event_invocation_matches(event, reconciliation->invocation_id) ||
          record->run_id != event.metadata.run_id) {
        return std::unexpected(
            ledger_error(ToolSpendLedgerErrorCode::wrong_run,
                         "tool spend reconciliation targets the wrong run"));
      }
      if (record->status != ToolSpendStatus::reserved ||
          !record->tool_started ||
          terminal_invocations.contains(reconciliation->invocation_id) ||
          !valid_tool_spend_reconciliation_reason(reconciliation->reason)) {
        return std::unexpected(
            ledger_error(ToolSpendLedgerErrorCode::invalid_transition,
                         "tool spend reservation is already terminal"));
      }
      record->status = ToolSpendStatus::reconciliation_required;
      record->reconciliation_reason = reconciliation->reason;
    } else if (const auto* result =
                   std::get_if<ToolResultRecorded>(&event.payload)) {
      const auto proposed = proposals.find(result->invocation_id);
      const auto* record = find_next(result->invocation_id);
      if (!event_invocation_matches(event, result->invocation_id) ||
          proposed == proposals.end() ||
          terminal_invocations.contains(result->invocation_id) ||
          (proposed->second.quote &&
           (record == nullptr ||
            record->status == ToolSpendStatus::reserved))) {
        return std::unexpected(
            ledger_error(ToolSpendLedgerErrorCode::invalid_transition,
                         "tool result has invalid spend ordering"));
      }
      if (proposed->second.run_id != event.metadata.run_id) {
        return std::unexpected(ledger_error(
            ToolSpendLedgerErrorCode::wrong_run,
            "tool result targets a different run than its proposal"));
      }
      terminal_invocations.insert(result->invocation_id);
    } else if (const auto* error = std::get_if<ToolErrored>(&event.payload)) {
      const auto proposed = proposals.find(error->invocation_id);
      const auto* record = find_next(error->invocation_id);
      if (!event_invocation_matches(event, error->invocation_id) ||
          proposed == proposals.end() ||
          terminal_invocations.contains(error->invocation_id) ||
          (proposed->second.quote && record != nullptr &&
           record->status == ToolSpendStatus::reserved)) {
        return std::unexpected(
            ledger_error(ToolSpendLedgerErrorCode::invalid_transition,
                         "tool error has invalid spend ordering"));
      }
      if (proposed->second.run_id != event.metadata.run_id) {
        return std::unexpected(ledger_error(
            ToolSpendLedgerErrorCode::wrong_run,
            "tool error targets a different run than its proposal"));
      }
      terminal_invocations.insert(error->invocation_id);
    }

    m_records = std::move(next);
    m_proposals = std::move(proposals);
    m_terminal_invocations = std::move(terminal_invocations);
    if (const auto* started = std::get_if<ToolStarted>(&event.payload))
      m_started_invocations.insert(started->invocation_id);
    m_event_ids.insert(event.metadata.event_id);
    m_last_sequence = event.metadata.sequence;
    return {};
  } catch (...) {
    return std::unexpected(
        ledger_error(ToolSpendLedgerErrorCode::amount_overflow,
                     "tool spend projection failed internally"));
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Spend summary.
auto summarize_tool_spend(const ToolSpendLedgerProjection& ledger,
                          const SessionSpendCeiling& ceiling)
    -> std::expected<ToolSpendSummary, ToolSpendLedgerError> {
  try {
    auto total = *DecimalAmount::from("0");
    auto reserved_maximum = total;
    auto reconciliation_maximum = total;
    auto provider_reported_amount = total;
    auto catalog_estimate_amount = total;
    auto policy_upper_bound_amount = total;
    ToolSpendSummary summary{ceiling,
                             *MonetaryAmount::create("USD", total),
                             *MonetaryAmount::create("USD", total),
                             *MonetaryAmount::create("USD", total),
                             *MonetaryAmount::create("USD", total),
                             *MonetaryAmount::create("USD", total),
                             *MonetaryAmount::create("USD", total),
                             *MonetaryAmount::create("USD", total)};
    const auto accumulate = [&](DecimalAmount& subtotal,
                                const DecimalAmount& value) -> bool {
      auto combined = add(subtotal, value);
      if (!combined) return false;
      subtotal = *combined;
      return true;
    };
    for (const auto& record : ledger.records()) {
      ++summary.reservations;
      const DecimalAmount* charged = nullptr;
      switch (record.status) {
        case ToolSpendStatus::reserved:
          ++summary.reserved;
          charged = &record.reservation.maximum.amount();
          if (!accumulate(reserved_maximum, *charged)) {
            return std::unexpected(
                ledger_error(ToolSpendLedgerErrorCode::amount_overflow,
                             "reserved tool maximum overflowed"));
          }
          break;
        case ToolSpendStatus::released: ++summary.released; break;
        case ToolSpendStatus::finalized: {
          ++summary.finalized;
          if (!record.finalization) {
            return std::unexpected(ledger_error(
                ToolSpendLedgerErrorCode::invalid_transition,
                "finalized tool spend lacks finalization details"));
          }
          const auto& finalization = *record.finalization;
          switch (finalization.basis) {
            case ToolSpendFinalizationBasis::provider_reported:
              ++summary.provider_reported;
              if (!accumulate(provider_reported_amount,
                              finalization.amount.amount())) {
                return std::unexpected(
                    ledger_error(ToolSpendLedgerErrorCode::amount_overflow,
                                 "provider-reported tool amount overflowed"));
              }
              break;
            case ToolSpendFinalizationBasis::catalog_estimate:
              ++summary.catalog_estimate;
              if (!accumulate(catalog_estimate_amount,
                              finalization.amount.amount())) {
                return std::unexpected(
                    ledger_error(ToolSpendLedgerErrorCode::amount_overflow,
                                 "catalog-estimated tool amount overflowed"));
              }
              break;
            case ToolSpendFinalizationBasis::policy_upper_bound:
              ++summary.policy_upper_bound;
              if (!accumulate(policy_upper_bound_amount,
                              finalization.amount.amount())) {
                return std::unexpected(
                    ledger_error(ToolSpendLedgerErrorCode::amount_overflow,
                                 "policy-upper-bound tool amount overflowed"));
              }
              break;
          }
          charged = &finalization.amount.amount();
          break;
        }
        case ToolSpendStatus::reconciliation_required:
          ++summary.reconciliation_required;
          charged = &record.reservation.maximum.amount();
          if (!accumulate(reconciliation_maximum, *charged)) {
            return std::unexpected(
                ledger_error(ToolSpendLedgerErrorCode::amount_overflow,
                             "reconciliation tool maximum overflowed"));
          }
          break;
      }
      if (charged != nullptr) {
        auto combined = add(total, *charged);
        if (!combined) {
          return std::unexpected(
              ledger_error(ToolSpendLedgerErrorCode::amount_overflow,
                           "tool spend total overflowed"));
        }
        total = *combined;
      }
    }
    summary.accounted = *MonetaryAmount::create("USD", total);
    summary.reserved_maximum = *MonetaryAmount::create("USD", reserved_maximum);
    summary.reconciliation_maximum =
        *MonetaryAmount::create("USD", reconciliation_maximum);
    summary.provider_reported_amount =
        *MonetaryAmount::create("USD", provider_reported_amount);
    summary.catalog_estimate_amount =
        *MonetaryAmount::create("USD", catalog_estimate_amount);
    summary.policy_upper_bound_amount =
        *MonetaryAmount::create("USD", policy_upper_bound_amount);
    summary.reached =
        compare(total, ceiling.amount()) != std::strong_ordering::less;
    auto remaining = summary.reached ? DecimalAmount::from("0")
                                     : subtract(ceiling.amount(), total);
    if (!remaining) {
      return std::unexpected(
          ledger_error(ToolSpendLedgerErrorCode::amount_overflow,
                       "tool spend remaining amount is invalid"));
    }
    summary.remaining = *MonetaryAmount::create("USD", *remaining);
    return summary;
  } catch (...) {
    return std::unexpected(
        ledger_error(ToolSpendLedgerErrorCode::amount_overflow,
                     "tool spend summary failed internally"));
  }
}

auto summarize_combined_session_spend(
    const std::span<const InferenceUsageRecord> inference_records,
    const ToolSpendLedgerProjection& tool_ledger,
    const SessionSpendCeiling& ceiling)
    -> std::expected<SessionSpendSummary, ToolSpendLedgerError> {
  try {
    auto combined = summarize_session_spend(inference_records, ceiling);
    auto tools = summarize_tool_spend(tool_ledger, ceiling);
    if (!tools) return std::unexpected(std::move(tools.error()));

    combined.inference_accounted = combined.accounted;
    combined.tool_accounted = tools->accounted;
    combined.tool_reserved_maximum = tools->reserved_maximum;
    combined.tool_reconciliation_maximum = tools->reconciliation_maximum;
    combined.tool_provider_reported_amount = tools->provider_reported_amount;
    combined.tool_catalog_estimate_amount = tools->catalog_estimate_amount;
    combined.tool_policy_upper_bound_amount = tools->policy_upper_bound_amount;
    combined.tool_reservations = tools->reservations;
    combined.tool_reserved = tools->reserved;
    combined.tool_released = tools->released;
    combined.tool_reconciliation_required = tools->reconciliation_required;
    combined.tool_provider_reported = tools->provider_reported;
    combined.tool_catalog_estimate = tools->catalog_estimate;
    combined.tool_policy_upper_bound = tools->policy_upper_bound;
    if (!combined.accounted) return combined;

    auto total = add(combined.accounted->amount(), tools->accounted.amount());
    if (!total) {
      return std::unexpected(
          ledger_error(ToolSpendLedgerErrorCode::amount_overflow,
                       "combined session spend overflowed"));
    }
    auto accounted = MonetaryAmount::create("USD", *total);
    if (!accounted) {
      return std::unexpected(
          ledger_error(ToolSpendLedgerErrorCode::amount_overflow,
                       "combined session spend is invalid"));
    }
    combined.accounted = std::move(*accounted);
    combined.reached =
        compare(*total, ceiling.amount()) != std::strong_ordering::less;
    auto remaining = combined.reached ? DecimalAmount::from("0")
                                      : subtract(ceiling.amount(), *total);
    if (!remaining) {
      return std::unexpected(
          ledger_error(ToolSpendLedgerErrorCode::amount_overflow,
                       "combined session spend remaining amount is invalid"));
    }
    auto remaining_amount = MonetaryAmount::create("USD", *remaining);
    if (!remaining_amount) {
      return std::unexpected(
          ledger_error(ToolSpendLedgerErrorCode::amount_overflow,
                       "combined session spend remaining amount is invalid"));
    }
    combined.remaining = std::move(*remaining_amount);
    return combined;
  } catch (...) {
    return std::unexpected(
        ledger_error(ToolSpendLedgerErrorCode::amount_overflow,
                     "combined session spend failed internally"));
  }
}

} // namespace aiforge::domain
