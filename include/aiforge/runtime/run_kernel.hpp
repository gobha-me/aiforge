#pragma once

#include <aiforge/backend/backend.hpp>
#include <aiforge/domain/event_log.hpp>
#include <aiforge/domain/local_context.hpp>
#include <aiforge/domain/plan_projection.hpp>
#include <aiforge/domain/project_backlog_projection.hpp>
#include <aiforge/domain/run_projection.hpp>
#include <aiforge/domain/task_scheduler.hpp>
#include <aiforge/runtime/child_runner.hpp>
#include <aiforge/runtime/ops_observation_tool.hpp>
#include <aiforge/runtime/tool_policy.hpp>
#include <aiforge/runtime/tool_registry.hpp>
#include <aiforge/storage/session_store.hpp>
#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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
  invalid_plan_state,
  wrong_plan,
  invalid_child_state,
  child_runner_unavailable,
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

// The one primary run whose tool/input authority RunKernel can safely restore.
// Ordinary running workflows (including video), terminal runs, and child runs
// are intentionally not recovery candidates for the interactive kernel.
struct RecoverableRun {
  domain::RunId run_id;
  domain::RunStarted attributes;
  std::optional<domain::RunProvenance> provenance;
  domain::RunStatus status{domain::RunStatus::not_started};
  bool recovering_unstarted_authority{};
  auto operator==(const RecoverableRun&) const -> bool = default;
};

[[nodiscard]] auto classify_recoverable_run(
    const domain::SessionEventLog& event_log)
    -> std::expected<std::optional<RecoverableRun>, RunKernelError>;

// Inspects only durable facts. A malformed/missing Dev admission is reported
// separately so a surface can preserve history inspection and cancellation.
[[nodiscard]] auto recorded_repository_context_admission(
    const domain::SessionEventLog& event_log, const domain::RunId& run_id)
    -> std::expected<std::optional<domain::RepositoryContextAdmission>,
                     RunKernelError>;

[[nodiscard]] auto recorded_local_context_admission(
    const domain::SessionEventLog& event_log, const domain::RunId& run_id)
    -> std::expected<std::optional<domain::LocalContextAdmission>,
                     RunKernelError>;

struct ToolApprovalPresentationLimits {
  std::size_t maximum_tool_name_bytes{256};
  std::size_t maximum_effects{16};
  std::size_t maximum_scopes{64};
  std::size_t maximum_scope_kind_bytes{256};
  std::size_t maximum_scope_value_bytes{4096};
  std::size_t maximum_canonical_argument_bytes{std::size_t{256} * 1024U};
  std::size_t maximum_total_text_bytes{std::size_t{512} * 1024U};
  auto operator==(const ToolApprovalPresentationLimits&) const
      -> bool = default;
};

struct RunKernelLimits {
  std::size_t pending_updates{256};
  std::size_t tool_argument_bytes{8U * 1024U * 1024U};
  domain::TaskSchedulingPolicy task_scheduling{};
  ToolApprovalPresentationLimits tool_approval_presentation{};
  auto operator==(const RunKernelLimits&) const -> bool = default;
};

enum class ToolApprovalSupplySource {
  per_invocation,
  implicit,
};

struct RunStart {
  domain::RunId run_id;
  domain::RunStarted attributes;
  domain::Message user_message;
  // Tools must be an exact registry-ordered subset of the kernel snapshot.
  // The chosen subset remains fixed for every continuation of this run.
  backend::BackendRequest request;
  // Recorded after `run.started` when present. Submit it with `tools` empty:
  // the kernel fills that section from the validated effective registry subset
  // so recorded tool identity is the run's actual tool set.
  std::optional<domain::RunProvenance> provenance{};
  // Recorded atomically with run start when a persona is selected or explicitly
  // disabled. The kernel verifies it against attributes and constructed
  // context.
  std::optional<domain::PersonaSelection> persona_selection{};
  // Durable rate-card provenance for the inference, when the selected model
  // catalog supplied pricing. This is runtime metadata, not a backend option.
  std::optional<domain::PricingObservation> pricing_observation{};
  // Artifacts imported by an explicit surface before inference. Every entry
  // must be referenced by the user message and have no producing invocation
  // or inference identity. The kernel records the create/reference facts in
  // the same durable transaction as run start.
  std::vector<domain::ArtifactMetadata> imported_artifacts{};
  std::optional<domain::RepositoryContextAdmission> repository_admission{};
  // Required for a tool-free summary producer; committed before dispatch.
  std::optional<domain::ConversationSummaryIntent> summary_intent{};
  std::optional<domain::LocalContextAdmission> local_admission{};
  auto operator==(const RunStart&) const -> bool = default;
};

struct ToolApprovalResolution {
  domain::ApprovalDecision decision{domain::ApprovalDecision::denied};
  std::vector<domain::CapabilityScope> granted_scopes;
  domain::ApprovalGrantLifetime lifetime{
      domain::ApprovalGrantLifetime::invocation};
  auto operator==(const ToolApprovalResolution&) const -> bool = default;
};

struct PendingToolApproval {
  domain::RunId run_id;
  domain::InvocationId invocation_id;
  std::string tool_name;
  std::vector<domain::Effect> effects;
  std::vector<domain::CapabilityScope> scopes;
  CanonicalToolArguments canonical_arguments;
  std::optional<domain::ToolRestrictionLevel> selected_restriction;
  std::optional<domain::ToolRestrictionLevel> achieved_restriction;
  domain::ToolApprovalMode approval_mode{domain::ToolApprovalMode::prompt};
  ToolApprovalSupplySource supply_source{
      ToolApprovalSupplySource::per_invocation};
  ToolExecutionLimits executor_limits;
  auto operator==(const PendingToolApproval&) const -> bool = default;
};

struct SessionSpendCeilingChange {
  domain::RunId run_id;
  domain::RunStarted attributes;
  domain::SessionSpendCeiling ceiling;
  domain::SessionSpendCeilingSource source{
      domain::SessionSpendCeilingSource::command_line};
  auto operator==(const SessionSpendCeilingChange&) const -> bool = default;
};

struct ConversationPolicyChange {
  domain::RunId run_id;
  domain::RunStarted attributes;
  std::uint64_t expected_revision{};
  domain::ConversationMode mode{domain::ConversationMode::full};
  std::vector<domain::RunId> pinned_run_ids;
};

struct ConversationSummaryPublication {
  domain::RunId run_id;
  domain::RunStarted attributes;
  domain::ConversationSummaryId summary_id;
};

struct ConversationSummaryEdit {
  domain::RunId run_id;
  domain::RunStarted attributes;
  std::uint64_t expected_sequence{};
  domain::ConversationSummaryVersion previous;
  std::string text;
};

struct ConversationSummaryActivationChange {
  domain::RunId run_id;
  domain::RunStarted attributes;
  std::uint64_t expected_sequence{};
  std::uint64_t expected_policy_revision{};
  domain::ConversationSummaryVersion candidate;
  std::vector<domain::ConversationSummaryVersion> replaced_versions;
};

struct ConversationSummaryDisableChange {
  domain::RunId run_id;
  domain::RunStarted attributes;
  std::uint64_t expected_sequence{};
  std::uint64_t expected_policy_revision{};
  domain::ConversationSummaryVersion candidate;
  domain::EventId activation_event_id;
};

struct ConversationSummaryActivationPreview {
  domain::ConversationSummaryActivation activation;
  // Prospective control events, for bounded context preparation only. Preview
  // neither persists these events nor makes the activation available to runs.
  std::vector<domain::RunEvent> events;
};

struct PendingQuestionInput {
  domain::RunId run_id;
  domain::InvocationId invocation_id;
  std::vector<domain::QuestionDefinition> questions;
  auto operator==(const PendingQuestionInput&) const -> bool = default;
};

struct PlanStart {
  domain::RunId run_id;
  domain::RunStarted attributes;
  domain::PlanRevision revision;
  auto operator==(const PlanStart&) const -> bool = default;
};

struct PlanApprovalEnvironment {
  std::optional<domain::RepositorySnapshotIdentity> source_snapshot;
  std::vector<domain::PlanEvidenceBinding> evidence;
  auto operator==(const PlanApprovalEnvironment&) const -> bool = default;
};

enum class PlanDecisionOutcome {
  recorded,
  already_recorded,
  invalidated,
};

struct PlanApprovalRevalidation {
  domain::RunId run_id;
  domain::RunStarted attributes;
  domain::PlanId plan_id;
  domain::PlanRevisionId revision_id;
  PlanApprovalEnvironment environment;
  auto operator==(const PlanApprovalRevalidation&) const -> bool = default;
};

enum class PlanRevalidationOutcome {
  current,
  invalidated,
  already_invalidated,
};

struct PendingPlanDecision {
  domain::RunId run_id;
  domain::PlanId plan_id;
  domain::PlanRevisionId revision_id;
  auto operator==(const PendingPlanDecision&) const -> bool = default;
};

struct ChildRunStart {
  domain::RunId child_run_id;
  domain::RunId parent_run_id;
  domain::RunStarted attributes;
  domain::PlanId plan_id;
  domain::PlanRevisionId revision_id;
  domain::PlanTaskId task_id;
  domain::ContextParcel context;
  domain::ChildRunBudget budget;
  // Parent authority is supplied by trusted runtime policy, never by the plan
  // or model. The requested child set must be a subset of both it and
  // the accepted task's intended effects.
  std::vector<domain::Effect> parent_effects;
  std::vector<domain::CapabilityScope> parent_scopes;
  std::vector<domain::Effect> requested_effects;
  std::vector<domain::CapabilityScope> requested_scopes;
  std::uint32_t attempt{1};
  struct ReviewRequest {
    domain::ReviewReceiptDraft draft;
    domain::ReviewActor requested_by;
    auto operator==(const ReviewRequest&) const -> bool = default;
  };
  std::optional<ReviewRequest> review;
  auto operator==(const ChildRunStart&) const -> bool = default;
};

enum class SessionTaskState {
  pending,
  dispatched,
  completed,
  failed,
  cancelled,
  timed_out,
  budget_exhausted,
  unavailable,
};

struct ActiveSessionTask {
  domain::PlanId plan_id;
  domain::PlanRevisionId revision_id;
  domain::PlanTask task;
  SessionTaskState state{SessionTaskState::pending};
  std::optional<domain::RunId> child_run_id;
  std::optional<domain::SessionTaskResult> result;
  auto operator==(const ActiveSessionTask&) const -> bool = default;
};

struct ProjectTaskPromotion {
  domain::RunId run_id;
  domain::RunStarted attributes;
  domain::ProjectBacklogItem item;
  auto operator==(const ProjectTaskPromotion&) const -> bool = default;
};

struct ProjectTaskStatusUpdate {
  domain::RunId run_id;
  domain::RunStarted attributes;
  domain::ProjectBacklogStatusChange change;
  auto operator==(const ProjectTaskStatusUpdate&) const -> bool = default;
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

enum class RunDrainMode { dispatch_ready, observe_only };

struct ObservationControlStart {
  domain::RunId run_id;
  domain::RunStarted attributes;
  domain::InvocationId invocation_id;
  OpsObservationIntent intent;
};

class RunKernel final {
 public:
  RunKernel(domain::SessionId session_id, backend::Backend& backend,
            RunWakeSink* wake_sink = nullptr,
            TimestampSource timestamp_source = {}, RunKernelLimits limits = {},
            ToolRegistrySnapshot tools = {},
            std::shared_ptr<ToolPolicy> policy = {},
            std::shared_ptr<ChildRunner> child_runner = {},
            std::shared_ptr<OpsObservationBroker> observation_broker = {});

  [[nodiscard]] static auto open_durable(
      DurableSessionOpen session, storage::SessionStore& store,
      backend::Backend& backend, RunWakeSink* wake_sink = nullptr,
      TimestampSource timestamp_source = {}, RunKernelLimits limits = {},
      ToolRegistrySnapshot tools = {}, std::shared_ptr<ToolPolicy> policy = {},
      std::shared_ptr<ChildRunner> child_runner = {},
      std::shared_ptr<OpsObservationBroker> observation_broker = {})
      -> std::expected<std::unique_ptr<RunKernel>, RunKernelError>;
  ~RunKernel();

  // All mutators/drain and broker owner methods share one caller-owned thread.
  // The application retains the broker past kernels and owns selection, pumping
  // and shutdown. Kernel destruction stops only its captured operation token;
  // it never closes or deactivates a potentially replaced broker session.
  [[nodiscard]] auto start_observation_control(ObservationControlStart start)
      -> std::expected<void, RunKernelError>;

  RunKernel(const RunKernel&) = delete;
  auto operator=(const RunKernel&) -> RunKernel& = delete;
  RunKernel(RunKernel&&) = delete;
  auto operator=(RunKernel&&) -> RunKernel& = delete;

  [[nodiscard]] auto start(RunStart start)
      -> std::expected<void, RunKernelError>;
  [[nodiscard]] auto replace_available_tools(ToolRegistrySnapshot tools)
      -> std::expected<void, RunKernelError>;
  [[nodiscard]] auto record_session_spend_ceiling(
      SessionSpendCeilingChange change) -> std::expected<void, RunKernelError>;
  [[nodiscard]] auto record_conversation_policy(ConversationPolicyChange change)
      -> std::expected<void, RunKernelError>;
  [[nodiscard]] auto publish_conversation_summary(
      ConversationSummaryPublication change)
      -> std::expected<domain::ConversationSummaryCandidate, RunKernelError>;
  [[nodiscard]] auto edit_conversation_summary(ConversationSummaryEdit change)
      -> std::expected<domain::ConversationSummaryCandidate, RunKernelError>;
  // Resolves the current availability of a recovered run's immutable summary
  // sources once. Subsequent policy changes cannot replace that pinned base.
  [[nodiscard]] auto pin_conversation_summaries(const domain::RunId& run_id)
      -> std::expected<void, RunKernelError>;
  [[nodiscard]] auto activate_conversation_summary(
      ConversationSummaryActivationChange change)
      -> std::expected<domain::ConversationSummaryActivation, RunKernelError>;
  [[nodiscard]] auto preview_conversation_summary(
      ConversationSummaryActivationChange change)
      -> std::expected<ConversationSummaryActivationPreview, RunKernelError>;
  [[nodiscard]] auto disable_conversation_summary(
      ConversationSummaryDisableChange change)
      -> std::expected<void, RunKernelError>;
  [[nodiscard]] auto start_plan(PlanStart start)
      -> std::expected<void, RunKernelError>;
  [[nodiscard]] auto revise_plan(const domain::RunId& run_id,
                                 domain::PlanRevision revision)
      -> std::expected<void, RunKernelError>;
  [[nodiscard]] auto decide_plan(const domain::RunId& run_id,
                                 domain::PlanRevisionDecision decision,
                                 PlanApprovalEnvironment environment = {})
      -> std::expected<PlanDecisionOutcome, RunKernelError>;
  [[nodiscard]] auto revalidate_plan_approval(
      PlanApprovalRevalidation revalidation)
      -> std::expected<PlanRevalidationOutcome, RunKernelError>;
  [[nodiscard]] auto dispatch_child(ChildRunStart start)
      -> std::expected<void, RunKernelError>;
  [[nodiscard]] auto promote_project_task(ProjectTaskPromotion promotion)
      -> std::expected<void, RunKernelError>;
  [[nodiscard]] auto update_project_task_status(ProjectTaskStatusUpdate update)
      -> std::expected<void, RunKernelError>;
  [[nodiscard]] auto cancel(const domain::RunId& run_id,
                            const domain::InferenceId& inference_id,
                            std::optional<std::string> reason = std::nullopt)
      -> std::expected<void, RunKernelError>;
  [[nodiscard]] auto cancel_run(
      const domain::RunId& run_id,
      std::optional<std::string> reason = std::nullopt)
      -> std::expected<void, RunKernelError>;
  [[nodiscard]] auto decide_approval(const domain::RunId& run_id,
                                     const domain::InvocationId& invocation_id,
                                     ToolApprovalResolution resolution)
      -> std::expected<void, RunKernelError>;
  [[nodiscard]] auto answer_questions(
      const domain::RunId& run_id, const domain::InvocationId& invocation_id,
      std::vector<domain::QuestionAnswer> answers)
      -> std::expected<void, RunKernelError>;
  [[nodiscard]] auto cancel_questions(
      const domain::RunId& run_id, const domain::InvocationId& invocation_id,
      std::optional<std::string> reason = std::nullopt)
      -> std::expected<void, RunKernelError>;
  [[nodiscard]] auto continue_run(
      const domain::RunId& run_id, backend::BackendRequest request,
      std::optional<domain::PricingObservation> pricing_observation =
          std::nullopt,
      std::optional<domain::RepositoryContextAdmission> repository_admission =
          std::nullopt,
      std::optional<domain::LocalContextAdmission> local_admission =
          std::nullopt) -> std::expected<void, RunKernelError>;

  // Drain worker observations and apply their run events on the calling
  // thread. The returned events are exactly those committed by this call.
  // observe_only never launches queued tools; a later dispatch_ready call
  // may launch them after the caller has revalidated its source proofs.
  [[nodiscard]] auto drain(RunDrainMode mode = RunDrainMode::dispatch_ready)
      -> std::expected<std::vector<domain::RunEvent>, RunKernelError>;

  [[nodiscard]] auto pending_tool_dispatch() const noexcept -> bool;

  [[nodiscard]] auto event_log() const noexcept
      -> const domain::SessionEventLog&;
  [[nodiscard]] auto projection(const domain::RunId& run_id) const noexcept
      -> const domain::RunProjection*;
  [[nodiscard]] auto active_run_id() const noexcept
      -> std::optional<domain::RunId>;
  [[nodiscard]] auto active_child_run_ids() const -> std::vector<domain::RunId>;
  [[nodiscard]] auto active_inference_id() const noexcept
      -> std::optional<domain::InferenceId>;
  // The exact run-owned declaration snapshot. Continuation callers must use
  // this instead of re-resolving mutable profile or model metadata.
  [[nodiscard]] auto active_tool_declarations() const noexcept
      -> const std::vector<backend::ToolDeclaration>*;
  [[nodiscard]] auto pending_tool_approval() const
      -> std::optional<PendingToolApproval>;
  [[nodiscard]] auto pending_question_input() const
      -> std::optional<PendingQuestionInput>;
  [[nodiscard]] auto pending_plan_decision() const
      -> std::optional<PendingPlanDecision>;
  [[nodiscard]] auto plan_projection(const domain::PlanId& plan_id)
      const noexcept -> const domain::PlanGraphProjection*;
  [[nodiscard]] auto active_session_tasks() const
      -> std::vector<ActiveSessionTask>;
  [[nodiscard]] auto project_backlog(const domain::RepositoryId& repository_id)
      const -> std::expected<domain::ProjectBacklogProjection, RunKernelError>;

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace aiforge::runtime
