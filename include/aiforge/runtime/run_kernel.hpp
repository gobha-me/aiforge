#pragma once

#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <aiforge/backend/backend.hpp>
#include <aiforge/domain/event_log.hpp>
#include <aiforge/domain/run_projection.hpp>
#include <aiforge/runtime/tool_policy.hpp>
#include <aiforge/runtime/tool_registry.hpp>
#include <aiforge/storage/session_store.hpp>

namespace aiforge::runtime {

enum class RunKernelErrorCode {
  invalid_limits,
  invalid_start,
  run_already_active,
  no_active_run,
  wrong_run,
  wrong_inference,
  already_terminal,
  protocol_failure,
  event_sequence_overflow,
  event_log_rejected,
  projection_rejected,
  storage_failure,
  replay_rejected,
  invalid_tool_state,
  wrong_invocation,
  policy_scope_widening,
  policy_failure,
  interactive_input_unavailable,
  continuation_not_ready,
  internal_failure,
};

struct RunKernelError {
  RunKernelErrorCode code;
  std::string message;
  bool retryable{};
  auto operator==(const RunKernelError&) const -> bool = default;
};

enum class DurableSessionMode {
  create,
  resume,
};

struct DurableSessionOpen {
  domain::SessionId session_id;
  DurableSessionMode mode{DurableSessionMode::create};
  domain::EventTimestamp created_at;
  auto operator==(const DurableSessionOpen&) const -> bool = default;
};

struct RunKernelLimits {
  std::size_t pending_updates{256};
  std::size_t tool_argument_bytes{8U * 1024U * 1024U};
  auto operator==(const RunKernelLimits&) const -> bool = default;
};

struct RunStart {
  domain::RunId run_id;
  domain::RunStarted attributes;
  domain::Message user_message;
  backend::BackendRequest request;
  // Recorded after `run.started` when present. Submit it with `tools` empty:
  // the kernel fills that section from its own registry snapshot so recorded
  // tool identity is the run's actual tool set.
  std::optional<domain::RunProvenance> provenance{};
  // Recorded atomically with run start when a persona is selected or explicitly
  // disabled. The kernel verifies it against attributes and constructed context.
  std::optional<domain::PersonaSelection> persona_selection{};
  auto operator==(const RunStart&) const -> bool = default;
};

struct ToolApprovalResolution {
  domain::ApprovalDecision decision{domain::ApprovalDecision::denied};
  std::vector<domain::CapabilityScope> granted_scopes;
  domain::ApprovalGrantLifetime lifetime{
      domain::ApprovalGrantLifetime::invocation};
  auto operator==(const ToolApprovalResolution&) const -> bool = default;
};

struct PendingQuestionInput {
  domain::RunId run_id;
  domain::InvocationId invocation_id;
  std::vector<domain::QuestionDefinition> questions;
  auto operator==(const PendingQuestionInput&) const -> bool = default;
};

// Implementations may be called from a backend worker. They must not mutate
// UI state directly; a surface should only wake or enqueue an event for its
// owner thread.
class RunWakeSink {
 public:
  virtual ~RunWakeSink() = default;
  virtual auto wake() noexcept -> void = 0;
};

using TimestampSource = std::function<domain::EventTimestamp()>;

class RunKernel final {
 public:
  RunKernel(domain::SessionId session_id, backend::Backend& backend,
            RunWakeSink* wake_sink = nullptr,
            TimestampSource timestamp_source = {}, RunKernelLimits limits = {},
            ToolRegistrySnapshot tools = {},
            std::shared_ptr<ToolPolicy> policy = {});

  [[nodiscard]] static auto open_durable(
      DurableSessionOpen session, storage::SessionStore& store,
      backend::Backend& backend, RunWakeSink* wake_sink = nullptr,
      TimestampSource timestamp_source = {}, RunKernelLimits limits = {},
      ToolRegistrySnapshot tools = {}, std::shared_ptr<ToolPolicy> policy = {})
      -> std::expected<std::unique_ptr<RunKernel>, RunKernelError>;
  ~RunKernel();

  RunKernel(const RunKernel&) = delete;
  auto operator=(const RunKernel&) -> RunKernel& = delete;
  RunKernel(RunKernel&&) = delete;
  auto operator=(RunKernel&&) -> RunKernel& = delete;

  [[nodiscard]] auto start(RunStart start)
      -> std::expected<void, RunKernelError>;
  [[nodiscard]] auto cancel(const domain::RunId& run_id,
                            const domain::InferenceId& inference_id,
                            std::optional<std::string> reason = std::nullopt)
      -> std::expected<void, RunKernelError>;
  [[nodiscard]] auto cancel_run(
      const domain::RunId& run_id,
      std::optional<std::string> reason = std::nullopt)
      -> std::expected<void, RunKernelError>;
  [[nodiscard]] auto decide_approval(
      const domain::RunId& run_id,
      const domain::InvocationId& invocation_id,
      ToolApprovalResolution resolution)
      -> std::expected<void, RunKernelError>;
  [[nodiscard]] auto answer_questions(
      const domain::RunId& run_id,
      const domain::InvocationId& invocation_id,
      std::vector<domain::QuestionAnswer> answers)
      -> std::expected<void, RunKernelError>;
  [[nodiscard]] auto cancel_questions(
      const domain::RunId& run_id,
      const domain::InvocationId& invocation_id,
      std::optional<std::string> reason = std::nullopt)
      -> std::expected<void, RunKernelError>;
  [[nodiscard]] auto continue_run(const domain::RunId& run_id,
                                  backend::BackendRequest request)
      -> std::expected<void, RunKernelError>;

  // Drain worker observations and apply their run events on the calling
  // thread. The returned events are exactly those committed by this call.
  [[nodiscard]] auto drain()
      -> std::expected<std::vector<domain::RunEvent>, RunKernelError>;

  [[nodiscard]] auto event_log() const noexcept
      -> const domain::SessionEventLog&;
  [[nodiscard]] auto projection(const domain::RunId& run_id) const noexcept
      -> const domain::RunProjection*;
  [[nodiscard]] auto active_run_id() const noexcept
      -> std::optional<domain::RunId>;
  [[nodiscard]] auto active_inference_id() const noexcept
      -> std::optional<domain::InferenceId>;
  [[nodiscard]] auto pending_question_input() const
      -> std::optional<PendingQuestionInput>;

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

}  // namespace aiforge::runtime
