#include <aiforge/domain/usage_ledger.hpp>

#include <algorithm>
#include <limits>
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

[[nodiscard]] auto add_checked(std::uint64_t &total, const std::uint64_t amount)
    -> bool {
  if (amount > std::numeric_limits<std::uint64_t>::max() - total)
    return false;
  total += amount;
  return true;
}

[[nodiscard]] auto add_usage(Usage &total, const Usage &addition) -> bool {
  return add_checked(total.input_tokens, addition.input_tokens) &&
         add_checked(total.output_tokens, addition.output_tokens) &&
         add_checked(total.cached_input_tokens, addition.cached_input_tokens) &&
         add_checked(total.reasoning_tokens, addition.reasoning_tokens);
}

} // namespace

auto UsageLedgerProjection::find_record(const InferenceId &inference_id)
    -> InferenceUsageRecord * {
  const auto found = std::ranges::find(m_records, inference_id,
                                       &InferenceUsageRecord::inference_id);
  return found == m_records.end() ? nullptr : &*found;
}

auto UsageLedgerProjection::apply(const RunEvent &event)
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

  const auto finish = [&](const InferenceId &inference_id,
                          const InferenceUsageStatus status)
      -> std::expected<void, UsageLedgerError> {
    auto *record = find_record(inference_id);
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
          [&](const InferenceStarted &started)
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
                                 {}});
            return {};
          },
          [&](const UsageRecorded &recorded)
              -> std::expected<void, UsageLedgerError> {
            auto *record = find_record(recorded.inference_id);
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
            m_total_usage = next_total_usage;
            return {};
          },
          [&](const InferenceFinished &finished) {
            return finish(finished.inference_id,
                          InferenceUsageStatus::completed);
          },
          [&](const InferenceFailed &failed) {
            return finish(failed.inference_id, InferenceUsageStatus::failed);
          },
          [&](const InferenceCancelled &cancelled) {
            return finish(cancelled.inference_id,
                          InferenceUsageStatus::cancelled);
          },
          [&](const auto &) -> std::expected<void, UsageLedgerError> {
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

} // namespace aiforge::domain
