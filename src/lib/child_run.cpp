#include <aiforge/domain/child_run.hpp>

#include <algorithm>
#include <cctype>
#include <iterator>
#include <ranges>
#include <string_view>

#include <aiforge/runtime/tool_policy.hpp>

namespace aiforge::domain {
namespace {

[[nodiscard]] auto failure(const ChildRunContractErrorCode code,
                           std::string message)
    -> std::unexpected<ChildRunContractError> {
  return std::unexpected(ChildRunContractError{code, std::move(message)});
}

[[nodiscard]] auto valid_digest(const ContentDigest& digest) -> bool {
  return !digest.algorithm.empty() && digest.algorithm.size() <= 128 &&
         !digest.value.empty() && digest.value.size() <= 512 &&
         std::ranges::all_of(digest.algorithm,
                             [](const unsigned char value) {
                               return std::isalnum(value) != 0 ||
                                      value == '-' || value == '_' ||
                                      value == '.';
                             }) &&
         std::ranges::all_of(digest.value, [](const unsigned char value) {
           return std::isxdigit(value) != 0;
         });
}

template <typename Value>
[[nodiscard]] auto unique(const std::vector<Value>& values) -> bool {
  for (auto current = values.begin(); current != values.end(); ++current) {
    if (std::find(std::next(current), values.end(), *current) != values.end()) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] auto known_outcome(const SessionTaskOutcome outcome) -> bool {
  switch (outcome) {
    case SessionTaskOutcome::completed:
    case SessionTaskOutcome::failed:
    case SessionTaskOutcome::cancelled:
    case SessionTaskOutcome::timed_out:
    case SessionTaskOutcome::budget_exhausted:
    case SessionTaskOutcome::unavailable: return true;
  }
  return false;
}

[[nodiscard]] auto valid_error(const DomainError& error) -> bool {
  return !error.message.empty() && error.message.size() <= 16U * 1024U &&
         std::ranges::none_of(error.message, [](const unsigned char value) {
           return value == 0 || value == 0x7FU;
         });
}

} // namespace

auto validate_child_run_descriptor(const ChildRunDescriptor& descriptor)
    -> std::expected<void, ChildRunContractError> {
  try {
    constexpr std::uint32_t maximum_inferences{256};
    constexpr std::uint32_t maximum_tools{4096};
    constexpr std::uint64_t maximum_tokens{16U * 1024U * 1024U};
    constexpr auto maximum_timeout = std::chrono::hours{24};
    constexpr std::uint64_t maximum_context_bytes{256U * 1024U * 1024U};
    constexpr std::size_t maximum_context_evidence{1024};
    constexpr std::size_t maximum_effects{16};
    constexpr std::size_t maximum_scopes{256};
    constexpr std::uint32_t maximum_attempts{8};

    const auto& budget = descriptor.budget;
    if (descriptor.attempt == 0 || descriptor.attempt > maximum_attempts ||
        budget.maximum_inferences == 0 ||
        budget.maximum_inferences > maximum_inferences ||
        budget.maximum_tool_invocations > maximum_tools ||
        budget.maximum_input_tokens == 0 ||
        budget.maximum_input_tokens > maximum_tokens ||
        budget.maximum_output_tokens == 0 ||
        budget.maximum_output_tokens > maximum_tokens ||
        budget.timeout <= std::chrono::milliseconds::zero() ||
        budget.timeout > maximum_timeout) {
      return failure(ChildRunContractErrorCode::invalid_budget,
                     "child-run budget is outside its supported bounds");
    }
    const auto& context = descriptor.context;
    if (!valid_digest(context.target_snapshot.fingerprint) ||
        context.evidence_ids.empty() ||
        context.evidence_ids.size() > maximum_context_evidence ||
        !unique(context.evidence_ids) || context.represented_bytes == 0 ||
        context.represented_bytes > maximum_context_bytes ||
        context.estimated_tokens == 0 ||
        context.estimated_tokens > budget.maximum_input_tokens) {
      return failure(ChildRunContractErrorCode::invalid_context,
                     "child-run context binding is invalid or unbounded");
    }
    if (descriptor.effects.size() > maximum_effects ||
        descriptor.capability_scopes.size() > maximum_scopes ||
        !unique(descriptor.effects) || !unique(descriptor.capability_scopes)) {
      return failure(ChildRunContractErrorCode::invalid_capabilities,
                     "child-run capability subset is invalid");
    }
    for (const auto& scope : descriptor.capability_scopes) {
      if (std::ranges::find(descriptor.effects, scope.effect) ==
              descriptor.effects.end() ||
          !runtime::normalize_capability_scope(scope)) {
        return failure(ChildRunContractErrorCode::invalid_capabilities,
                       "child-run capability scope is invalid or undeclared");
      }
    }
    return {};
  } catch (...) {
    return failure(ChildRunContractErrorCode::invalid_descriptor,
                   "child-run descriptor validation failed internally");
  }
}

auto validate_session_task_result(const SessionTaskResult& result,
                                  const ChildRunBudget& budget)
    -> std::expected<void, ChildRunContractError> {
  try {
    constexpr std::size_t maximum_references{256};
    if (!known_outcome(result.outcome) ||
        result.evidence_ids.size() > maximum_references ||
        result.artifact_ids.size() > maximum_references ||
        !unique(result.evidence_ids) || !unique(result.artifact_ids) ||
        result.consumption.inference_count > budget.maximum_inferences ||
        result.consumption.tool_invocation_count >
            budget.maximum_tool_invocations ||
        result.consumption.usage.input_tokens > budget.maximum_input_tokens ||
        result.consumption.usage.output_tokens > budget.maximum_output_tokens) {
      return failure(ChildRunContractErrorCode::invalid_result,
                     "child-run result is invalid or exceeds its budget");
    }
    if ((result.outcome == SessionTaskOutcome::completed) !=
            !result.error.has_value() ||
        (result.error && !valid_error(*result.error))) {
      return failure(ChildRunContractErrorCode::invalid_result,
                     "child-run result outcome and error disagree");
    }
    return {};
  } catch (...) {
    return failure(ChildRunContractErrorCode::invalid_result,
                   "child-run result validation failed internally");
  }
}

} // namespace aiforge::domain
