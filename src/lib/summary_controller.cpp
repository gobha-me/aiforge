#include <aiforge/runtime/summary_controller.hpp>
#include <utility>

namespace aiforge::runtime {
struct SummaryReviewData {
  domain::SessionId session_id;
  std::uint64_t sequence;
  ConversationSummaryActivationChange change;
  domain::ModelId model_id;
  domain::ContextBuildInput mandatory;
  bool memory_enabled;
  MemoryContextRequest memory;
  ConversationHistoryLimits history_limits;
  ConversationSelectionLimits selection_limits;
  domain::ConversationSummaryActivation activation;
  PreparedSessionContext context;
};
namespace {
using Code = SummaryControllerErrorCode;
auto failure(Code code, std::string message, bool retryable = false)
    -> std::unexpected<SummaryControllerError> {
  return std::unexpected(
      SummaryControllerError{code, std::move(message), retryable});
}
auto kernel_failure(const RunKernelError& error)
    -> std::unexpected<SummaryControllerError> {
  return failure(Code::kernel_failed, error.message, error.retryable);
}
struct PreparedReview {
  domain::ConversationSummaryActivation activation;
  PreparedSessionContext context;
};
auto prepare_review(RunKernel& kernel,
                    const ConversationSummaryActivationChange& change,
                    const SummaryContextRequest& request, std::stop_token stop)
    -> std::expected<PreparedReview, SummaryControllerError> {
  auto prospective = kernel.preview_conversation_summary(change);
  if (!prospective) return kernel_failure(prospective.error());
  auto context = preview_session_context_after_summary_activation(
      {kernel.event_log(), request.model_id, request.mandatory,
       request.memory_controller, request.memory, request.history_limits,
       request.selection_limits},
      prospective->events, stop);
  if (!context)
    return failure(context.error().code == SessionContextErrorCode::cancelled
                       ? Code::cancelled
                       : Code::preparation_failed,
                   context.error().message, context.error().retryable);
  return PreparedReview{std::move(prospective->activation),
                        std::move(*context)};
}

auto same_inputs(const SummaryReviewData& review,
                 const SummaryContextRequest& request) -> bool {
  return review.model_id == request.model_id &&
         review.mandatory == request.mandatory &&
         review.memory_enabled == (request.memory_controller != nullptr) &&
         review.memory.repository_id == request.memory.repository_id &&
         review.memory.persona_id == request.memory.persona_id &&
         review.memory.maximum_tokens == request.memory.maximum_tokens &&
         review.history_limits == request.history_limits &&
         review.selection_limits == request.selection_limits;
}
} // namespace
SummaryPreview::SummaryPreview(std::shared_ptr<const SummaryReviewData> data)
    : m_data(std::move(data)) {
}
auto SummaryPreview::context() const noexcept -> const PreparedSessionContext& {
  return m_data->context;
}
auto SummaryPreview::activation() const noexcept
    -> const domain::ConversationSummaryActivation& {
  return m_data->activation;
}
auto SummaryPreview::sequence() const noexcept -> std::uint64_t {
  return m_data->sequence;
}
SummaryController::SummaryController(RunKernel& kernel) : m_kernel(kernel) {
}
auto SummaryController::preview(ConversationSummaryActivationChange change,
                                const SummaryContextRequest& request,
                                std::stop_token stop)
    -> std::expected<SummaryPreview, SummaryControllerError> {
  try {
    if (stop.stop_requested())
      return failure(Code::cancelled, "summary review cancelled");
    auto prepared = prepare_review(m_kernel, change, request, stop);
    if (!prepared) return std::unexpected(prepared.error());
    const auto& log = m_kernel.event_log();
    return SummaryPreview{
        std::make_shared<const SummaryReviewData>(SummaryReviewData{
            log.session_id(), log.last_sequence(), std::move(change),
            request.model_id, request.mandatory,
            request.memory_controller != nullptr, request.memory,
            request.history_limits, request.selection_limits,
            std::move(prepared->activation), std::move(prepared->context)})};
  } catch (...) {
    return failure(Code::internal_failure, "summary review failed internally");
  }
}
auto SummaryController::apply(const SummaryPreview& review,
                              const SummaryContextRequest& request,
                              std::stop_token stop)
    -> std::expected<domain::ConversationSummaryActivation,
                     SummaryControllerError> {
  try {
    if (stop.stop_requested())
      return failure(Code::cancelled, "summary apply cancelled");
    const auto& log = m_kernel.event_log();
    if (!review.m_data || review.m_data->session_id != log.session_id() ||
        review.m_data->sequence != log.last_sequence() ||
        !same_inputs(*review.m_data, request))
      return failure(Code::stale_review,
                     "summary review inputs changed; preview again");
    auto prepared =
        prepare_review(m_kernel, review.m_data->change, request, stop);
    if (!prepared) return std::unexpected(prepared.error());
    if (prepared->activation != review.m_data->activation ||
        prepared->context != review.m_data->context)
      return failure(Code::stale_review,
                     "summary review sources changed; preview again");
    if (stop.stop_requested())
      return failure(Code::cancelled, "summary apply cancelled");
    auto activated =
        m_kernel.activate_conversation_summary(review.m_data->change);
    if (!activated) return kernel_failure(activated.error());
    return std::move(*activated);
  } catch (...) {
    return failure(Code::internal_failure, "summary apply failed internally");
  }
}
auto SummaryController::inspect()
    -> std::expected<ConversationSummarySnapshot, SummaryControllerError> {
  try {
    auto result = recorded_conversation_summaries(m_kernel.event_log());
    if (!result)
      return failure(Code::preparation_failed, result.error().message);
    return std::move(*result);
  } catch (...) {
    return failure(Code::internal_failure,
                   "summary inspection failed internally");
  }
}
auto SummaryController::publish(ConversationSummaryPublication change)
    -> std::expected<domain::ConversationSummaryCandidate,
                     SummaryControllerError> {
  try {
    auto result = m_kernel.publish_conversation_summary(std::move(change));
    if (!result) return kernel_failure(result.error());
    return std::move(*result);
  } catch (...) {
    return failure(Code::internal_failure, "summary publish failed internally");
  }
}
auto SummaryController::edit(ConversationSummaryEdit change)
    -> std::expected<domain::ConversationSummaryCandidate,
                     SummaryControllerError> {
  try {
    auto result = m_kernel.edit_conversation_summary(std::move(change));
    if (!result) return kernel_failure(result.error());
    return std::move(*result);
  } catch (...) {
    return failure(Code::internal_failure, "summary edit failed internally");
  }
}
auto SummaryController::disable(ConversationSummaryDisableChange change)
    -> std::expected<void, SummaryControllerError> {
  try {
    auto result = m_kernel.disable_conversation_summary(std::move(change));
    if (!result) return kernel_failure(result.error());
    return {};
  } catch (...) {
    return failure(Code::internal_failure, "summary disable failed internally");
  }
}
} // namespace aiforge::runtime
