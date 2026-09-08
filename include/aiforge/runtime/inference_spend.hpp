#pragma once

#include <aiforge/domain/event_log.hpp>
#include <aiforge/domain/usage_ledger.hpp>

#include <expected>
#include <optional>
#include <string>

namespace aiforge::runtime {

enum class InferenceSpendErrorCode {
  invalid_history,
  accounting_unavailable,
  ceiling_reached,
  internal_failure,
};

struct InferenceSpendError {
  InferenceSpendErrorCode code;
  std::string message;
  // Present when combined accounting produced a summary. Reached errors carry
  // its exact accounted and ceiling amounts for presentation by the caller.
  std::optional<domain::SessionSpendSummary> summary{};
  auto operator==(const InferenceSpendError&) const -> bool = default;
};

// Refuses another inference when a recorded session ceiling has been reached
// or combined inference/tool accounting is unavailable. Outstanding tool
// reservations retain their existing conservative accounting semantics.
// Without a ceiling only ceiling history is checked and success is nullopt.
// This preflight does not reserve or predict the next inference's cost.
[[nodiscard]] auto preflight_inference_spend(const domain::SessionEventLog& log)
    -> std::expected<std::optional<domain::SessionSpendSummary>,
                     InferenceSpendError>;

} // namespace aiforge::runtime
