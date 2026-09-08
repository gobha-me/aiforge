#include <aiforge/runtime/inference_spend.hpp>

#include <aiforge/domain/tool_spend.hpp>

#include <utility>

namespace aiforge::runtime {
namespace {
using Code = InferenceSpendErrorCode;
using Result = std::expected<std::optional<domain::SessionSpendSummary>,
                             InferenceSpendError>;

auto preflight(const domain::SessionEventLog& log) -> Result {
  domain::SessionSpendCeilingProjection ceiling;
  for (const auto& event : log.events()) {
    if (!ceiling.apply(event))
      return std::unexpected(InferenceSpendError{
          Code::invalid_history, "session spend ceiling history is invalid"});
  }
  if (!ceiling.ceiling()) return std::nullopt;

  domain::UsageLedgerProjection ledger;
  domain::ToolSpendLedgerProjection tools;
  for (const auto& event : log.events()) {
    if (!ledger.apply(event) || !tools.apply(event))
      return std::unexpected(InferenceSpendError{
          Code::invalid_history, "session spend history is invalid"});
  }
  auto spend = domain::summarize_combined_session_spend(ledger.records(), tools,
                                                        *ceiling.ceiling());
  if (!spend || !spend->accounted) {
    std::optional<domain::SessionSpendSummary> summary;
    if (spend) summary = std::move(*spend);
    return std::unexpected(InferenceSpendError{
        Code::accounting_unavailable,
        "session spend accounting is unavailable; refusing another inference",
        std::move(summary)});
  }
  if (spend->reached)
    return std::unexpected(InferenceSpendError{Code::ceiling_reached,
                                               "session spend ceiling reached",
                                               std::move(*spend)});
  return std::optional{std::move(*spend)};
}
} // namespace

auto preflight_inference_spend(const domain::SessionEventLog& log) -> Result {
  try {
    return preflight(log);
  } catch (...) {
    return std::unexpected(InferenceSpendError{
        Code::internal_failure, "inference spend preflight failed internally"});
  }
}

} // namespace aiforge::runtime
