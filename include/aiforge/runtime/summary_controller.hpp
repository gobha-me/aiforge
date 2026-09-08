#pragma once

#include <aiforge/runtime/conversation_summary_projection.hpp>
#include <aiforge/runtime/run_kernel.hpp>
#include <aiforge/runtime/session_context.hpp>
#include <memory>

namespace aiforge::runtime {

// The draft remains caller-owned and is never submitted. Optional memory is
// selected freshly through a forced read-only path; preview never creates a
// journal or captures new memory.
struct SummaryContextRequest {
  domain::ModelId model_id;
  const domain::ContextBuildInput& mandatory;
  MemoryController* memory_controller{};
  MemoryContextRequest memory{};
  ConversationHistoryLimits history_limits{};
  ConversationSelectionLimits selection_limits{};
};

enum class SummaryControllerErrorCode {
  invalid_request,
  stale_review,
  preparation_failed,
  kernel_failed,
  resource_exhausted,
  cancelled,
  internal_failure,
};
struct SummaryControllerError {
  SummaryControllerErrorCode code;
  std::string message;
  bool retryable{};
  auto operator==(const SummaryControllerError&) const -> bool = default;
};

struct SummaryReviewData;
class SummaryController;

// An immutable, owning review. Copies retain the same reviewed proof; no
// SessionContextRequest references or controller pointers escape the call.
class SummaryPreview final {
 public:
  [[nodiscard]] auto context() const noexcept -> const PreparedSessionContext&;
  [[nodiscard]] auto activation() const noexcept
      -> const domain::ConversationSummaryActivation&;
  [[nodiscard]] auto sequence() const noexcept -> std::uint64_t;

 private:
  explicit SummaryPreview(std::shared_ptr<const SummaryReviewData> data);
  std::shared_ptr<const SummaryReviewData> m_data;
  friend class SummaryController;
};

class SummaryController final {
 public:
  explicit SummaryController(RunKernel& kernel);
  [[nodiscard]] auto preview(ConversationSummaryActivationChange change,
                             const SummaryContextRequest& request,
                             std::stop_token stop = {})
      -> std::expected<SummaryPreview, SummaryControllerError>;
  // Rebuilds the reviewed next context against current inputs before the
  // kernel atomically appends activation. Stale reviews are never auto-applied.
  [[nodiscard]] auto apply(const SummaryPreview& review,
                           const SummaryContextRequest& request,
                           std::stop_token stop = {})
      -> std::expected<domain::ConversationSummaryActivation,
                       SummaryControllerError>;
  [[nodiscard]] auto inspect()
      -> std::expected<ConversationSummarySnapshot, SummaryControllerError>;
  [[nodiscard]] auto publish(ConversationSummaryPublication change)
      -> std::expected<domain::ConversationSummaryCandidate,
                       SummaryControllerError>;
  [[nodiscard]] auto edit(ConversationSummaryEdit change)
      -> std::expected<domain::ConversationSummaryCandidate,
                       SummaryControllerError>;
  [[nodiscard]] auto disable(ConversationSummaryDisableChange change)
      -> std::expected<void, SummaryControllerError>;

 private:
  RunKernel& m_kernel;
};
} // namespace aiforge::runtime
