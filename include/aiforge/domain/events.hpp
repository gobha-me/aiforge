#pragma once

#include <aiforge/domain/child_run.hpp>
#include <aiforge/domain/content.hpp>
#include <aiforge/domain/money.hpp>
#include <aiforge/domain/persona.hpp>
#include <aiforge/domain/plan.hpp>
#include <aiforge/domain/pricing.hpp>
#include <aiforge/domain/project_backlog.hpp>
#include <aiforge/domain/provenance.hpp>
#include <aiforge/domain/review_receipt.hpp>
#include <aiforge/domain/verification_evidence.hpp>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace aiforge::domain {

using Metadata = std::vector<std::pair<std::string, std::string>>;
using EventTimestamp = std::chrono::sys_time<std::chrono::milliseconds>;

struct RunStarted {
  SurfaceId surface_id;
  WorkspaceId workspace_id;
  PermissionProfileId permission_profile_id;
  std::optional<PersonaId> persona_id;
  auto operator==(const RunStarted&) const -> bool = default;
};

// Recorded once per run, immediately after `RunStarted`, so a replayed run can
// explain the configuration, backend, credential source, and tool and runtime
// versions that produced it.
struct RunProvenanceRecorded {
  RunProvenance provenance;
  auto operator==(const RunProvenanceRecorded&) const -> bool = default;
};

struct PersonaSelectionRecorded {
  PersonaSelection selection;
  auto operator==(const PersonaSelectionRecorded&) const -> bool = default;
};

enum class SessionSpendCeilingSource {
  command_line,
};

struct SessionSpendCeilingSet {
  SessionSpendCeiling ceiling;
  SessionSpendCeilingSource source{SessionSpendCeilingSource::command_line};
  auto operator==(const SessionSpendCeilingSet&) const -> bool = default;
};

struct RunAwaitingInput {
  QuestionId question_id;
  auto operator==(const RunAwaitingInput&) const -> bool = default;
};

struct RunResumed {
  std::optional<QuestionId> question_id;
  auto operator==(const RunResumed&) const -> bool = default;
};

struct RunCompletionRequested {
  auto operator==(const RunCompletionRequested&) const -> bool = default;
};

struct RunCompleted {
  auto operator==(const RunCompleted&) const -> bool = default;
};

struct RunFailed {
  DomainError error;
  auto operator==(const RunFailed&) const -> bool = default;
};

struct RunCancelRequested {
  std::optional<std::string> reason;
  auto operator==(const RunCancelRequested&) const -> bool = default;
};

struct RunCancelled {
  std::optional<std::string> reason;
  auto operator==(const RunCancelled&) const -> bool = default;
};

struct UserContentAdded {
  Message message;
  auto operator==(const UserContentAdded&) const -> bool = default;
};

struct AssistantContentStarted {
  MessageId message_id;
  InferenceId inference_id;
  auto operator==(const AssistantContentStarted&) const -> bool = default;
};

struct AssistantContentDeltaAdded {
  MessageId message_id;
  InferenceId inference_id;
  ContentBlock delta;
  auto operator==(const AssistantContentDeltaAdded&) const -> bool = default;
};

struct AssistantContentFinished {
  MessageId message_id;
  InferenceId inference_id;
  auto operator==(const AssistantContentFinished&) const -> bool = default;
};

struct InferenceStarted {
  InferenceId inference_id;
  ModelId model_id;
  auto operator==(const InferenceStarted&) const -> bool = default;
};

struct InferencePricingObserved {
  InferenceId inference_id;
  PricingObservation observation;
  auto operator==(const InferencePricingObserved &) const -> bool = default;
};

struct ReasoningMetadataAdded {
  InferenceId inference_id;
  std::optional<std::string> text;
  Metadata metadata;
  auto operator==(const ReasoningMetadataAdded&) const -> bool = default;
};

struct UsageRecorded {
  InferenceId inference_id;
  Usage usage;
  auto operator==(const UsageRecorded&) const -> bool = default;
};

struct InferenceCostRecorded {
  InferenceId inference_id;
  // Actual backend-reported amounts. Quotes and estimates are separate facts.
  ReportedCost cost;
  auto operator==(const InferenceCostRecorded&) const -> bool = default;
};

struct InferenceFinished {
  InferenceId inference_id;
  FinishReason reason;
  auto operator==(const InferenceFinished&) const -> bool = default;
};

struct InferenceFailed {
  InferenceId inference_id;
  DomainError error;
  auto operator==(const InferenceFailed&) const -> bool = default;
};

struct InferenceCancelled {
  InferenceId inference_id;
  std::optional<std::string> reason;
  auto operator==(const InferenceCancelled&) const -> bool = default;
};

struct ToolProposed {
  InvocationId invocation_id;
  std::string tool_name;
  StructuredDataBlock arguments;
  std::vector<Effect> declared_effects;
  std::optional<InvocationId> parent_invocation_id{};
  // Replay may relaunch a queued invocation without validation only when its
  // validated value was byte-for-byte identical to the recorded arguments.
  bool arguments_replayable{};
  std::vector<CapabilityScope> validated_required_scopes{};
  std::vector<CapabilityScope> requested_scopes{};
  std::optional<MessageId> result_message_id{};
  auto operator==(const ToolProposed&) const -> bool = default;
};

enum class PolicyDecision {
  allow,
  deny,
  require_approval,
};

enum class PolicyDecisionSource {
  fallback,
  permission_profile,
  session_grant,
  saved_grant,
  user_approval,
};

struct ToolPolicyDecided {
  InvocationId invocation_id;
  PolicyDecision decision;
  std::vector<CapabilityScope> scopes;
  std::optional<std::string> reason;
  PolicyDecisionSource source{PolicyDecisionSource::fallback};
  auto operator==(const ToolPolicyDecided&) const -> bool = default;
};

struct ToolApprovalRequested {
  InvocationId invocation_id;
  std::vector<CapabilityScope> requested_scopes;
  std::optional<std::string> reason{};
  auto operator==(const ToolApprovalRequested&) const -> bool = default;
};

enum class ApprovalDecision {
  approved,
  denied,
  cancelled,
};

enum class ApprovalGrantLifetime {
  invocation,
  session,
  saved,
};

struct ToolApprovalDecided {
  InvocationId invocation_id;
  ApprovalDecision decision;
  std::vector<CapabilityScope> granted_scopes;
  ApprovalGrantLifetime lifetime{ApprovalGrantLifetime::invocation};
  auto operator==(const ToolApprovalDecided&) const -> bool = default;
};

struct ToolPolicyFailed {
  InvocationId invocation_id;
  DomainError error;
  auto operator==(const ToolPolicyFailed&) const -> bool = default;
};

struct ToolStarted {
  InvocationId invocation_id;
  auto operator==(const ToolStarted&) const -> bool = default;
};

struct ToolProgressed {
  InvocationId invocation_id;
  std::vector<ContentBlock> content;
  auto operator==(const ToolProgressed&) const -> bool = default;
};

struct ToolResultRecorded {
  InvocationId invocation_id;
  std::vector<ContentBlock> content;
  std::optional<MessageId> result_message_id{};
  auto operator==(const ToolResultRecorded&) const -> bool = default;
};

struct ToolErrored {
  InvocationId invocation_id;
  DomainError error;
  std::optional<MessageId> result_message_id{};
  auto operator==(const ToolErrored&) const -> bool = default;
};

struct QuestionRequested {
  QuestionDefinition question;
  auto operator==(const QuestionRequested&) const -> bool = default;
};

struct QuestionAnswered {
  QuestionAnswer answer;
  auto operator==(const QuestionAnswered&) const -> bool = default;
};

struct QuestionCancelled {
  QuestionId question_id;
  std::optional<std::string> reason;
  auto operator==(const QuestionCancelled&) const -> bool = default;
};

struct ArtifactMetadata {
  ArtifactId artifact_id;
  std::string media_type;
  std::uint64_t byte_size{};
  std::string digest;
  std::optional<InvocationId> producing_invocation_id;
  std::optional<std::uint32_t> width;
  std::optional<std::uint32_t> height;
  auto operator==(const ArtifactMetadata&) const -> bool = default;
};

struct ArtifactCreated {
  ArtifactMetadata artifact;
  auto operator==(const ArtifactCreated&) const -> bool = default;
};

struct ArtifactReferenced {
  ArtifactId artifact_id;
  std::optional<MessageId> message_id;
  auto operator==(const ArtifactReferenced&) const -> bool = default;
};

struct ArtifactDisplayed {
  ArtifactId artifact_id;
  ViewId view_id;
  std::string semantic_slot;
  auto operator==(const ArtifactDisplayed&) const -> bool = default;
};

struct ArtifactRemovedFromView {
  ArtifactId artifact_id;
  ViewId view_id;
  auto operator==(const ArtifactRemovedFromView&) const -> bool = default;
};

struct VerificationEvidenceRecorded {
  VerificationEvidence evidence;
  auto operator==(const VerificationEvidenceRecorded&) const -> bool = default;
};

struct ReviewReceiptDrafted {
  ReviewReceiptDraft draft;
  auto operator==(const ReviewReceiptDrafted&) const -> bool = default;
};

struct ReviewRequested {
  ReviewReceiptId receipt_id;
  ReviewActor requested_by;
  auto operator==(const ReviewRequested&) const -> bool = default;
};

struct ReviewFindingOpened {
  ReviewReceiptId receipt_id;
  ReviewFinding finding;
  auto operator==(const ReviewFindingOpened&) const -> bool = default;
};

struct ReviewFindingResolved {
  ReviewReceiptId receipt_id;
  ReviewFindingId finding_id;
  ReviewActor resolved_by;
  std::optional<std::string> reason;
  auto operator==(const ReviewFindingResolved&) const -> bool = default;
};

struct ReviewVerdictRecorded {
  ReviewReceiptId receipt_id;
  ReviewVerdict verdict{ReviewVerdict::rejected};
  ReviewActor reviewer;
  std::optional<ReviewParticipantProvenance> reviewer_provenance;
  auto operator==(const ReviewVerdictRecorded&) const -> bool = default;
};

struct ReviewVerdictRevoked {
  ReviewReceiptId receipt_id;
  EventId verdict_event_id;
  ReviewActor revoked_by;
  std::string reason;
  auto operator==(const ReviewVerdictRevoked&) const -> bool = default;
};

struct ReviewOverrideRecorded {
  ReviewOverride override;
  auto operator==(const ReviewOverrideRecorded&) const -> bool = default;
};

struct ReviewOverrideRevoked {
  ReviewReceiptId receipt_id;
  ReviewOverrideId override_id;
  ReviewActor revoked_by;
  std::string reason;
  auto operator==(const ReviewOverrideRevoked&) const -> bool = default;
};

struct PlanRevisionProposed {
  PlanRevision revision;
  auto operator==(const PlanRevisionProposed&) const -> bool = default;
};

struct PlanRevisionDecisionRecorded {
  PlanRevisionDecision decision;
  auto operator==(const PlanRevisionDecisionRecorded&) const -> bool = default;
};

struct PlanRevisionInvalidated {
  PlanRevisionInvalidation invalidation;
  auto operator==(const PlanRevisionInvalidated&) const -> bool = default;
};

struct SessionTasksMaterialized {
  PlanId plan_id;
  PlanRevisionId revision_id;
  auto operator==(const SessionTasksMaterialized&) const -> bool = default;
};

struct ChildRunCreated {
  RunId child_run_id;
  // Schema version 1 carried only child_run_id. Schema version 2 carries a
  // complete bounded dispatch descriptor with an implicit first attempt.
  // Schema version 3 records the explicit attempt number.
  std::optional<ChildRunDescriptor> descriptor;
  auto operator==(const ChildRunCreated&) const -> bool = default;
};

struct SessionTaskResultRecorded {
  SessionTaskResult result;
  auto operator==(const SessionTaskResultRecorded&) const -> bool = default;
};

struct ProjectBacklogItemPromoted {
  ProjectBacklogItem item;
  auto operator==(const ProjectBacklogItemPromoted &) const -> bool = default;
};

struct ProjectBacklogItemStatusChanged {
  ProjectBacklogStatusChange change;
  auto operator==(const ProjectBacklogItemStatusChanged &) const
      -> bool = default;
};

struct InterRunMessageSent {
  RunId target_run_id;
  std::vector<ContentBlock> content;
  auto operator==(const InterRunMessageSent&) const -> bool = default;
};

struct UnknownEvent {
  std::string type_name;
  // Persisted readers retain the canonical opaque payload for event types or
  // schema versions they do not understand. JSON is the storage adapter's
  // current media type, not a public JSON-library object.
  StructuredDataBlock payload{"application/json", "null"};
  auto operator==(const UnknownEvent&) const -> bool = default;
};

using RunEventPayload = std::variant<
    RunStarted, RunProvenanceRecorded, PersonaSelectionRecorded,
    SessionSpendCeilingSet,
    RunAwaitingInput, RunResumed,
    RunCompletionRequested, RunCompleted,
    RunFailed, RunCancelRequested, RunCancelled, UserContentAdded,
    AssistantContentStarted, AssistantContentDeltaAdded,
    AssistantContentFinished, InferenceStarted, InferencePricingObserved,
    ReasoningMetadataAdded, UsageRecorded, InferenceCostRecorded,
    InferenceFinished, InferenceFailed, InferenceCancelled, ToolProposed,
    ToolPolicyDecided, ToolApprovalRequested, ToolApprovalDecided,
    ToolPolicyFailed, ToolStarted, ToolProgressed, ToolResultRecorded,
    ToolErrored, QuestionRequested, QuestionAnswered, QuestionCancelled,
    ArtifactCreated, ArtifactReferenced, ArtifactDisplayed,
    ArtifactRemovedFromView, VerificationEvidenceRecorded, ReviewReceiptDrafted,
    ReviewRequested, ReviewFindingOpened, ReviewFindingResolved,
    ReviewVerdictRecorded, ReviewVerdictRevoked, ReviewOverrideRecorded,
    ReviewOverrideRevoked, PlanRevisionProposed,
    PlanRevisionDecisionRecorded, PlanRevisionInvalidated,
    SessionTasksMaterialized, ChildRunCreated,
    SessionTaskResultRecorded, ProjectBacklogItemPromoted,
    ProjectBacklogItemStatusChanged, InterRunMessageSent,
    UnknownEvent>;

struct EventMetadata {
  EventId event_id;
  RunId run_id;
  std::uint64_t sequence{};
  std::uint32_t schema_version{1};
  EventTimestamp timestamp;
  std::optional<EventId> caused_by_event_id;
  std::optional<RunId> parent_run_id;
  std::optional<InvocationId> invocation_id;
  auto operator==(const EventMetadata&) const -> bool = default;
};

struct RunEvent {
  EventMetadata metadata;
  RunEventPayload payload;
  auto operator==(const RunEvent&) const -> bool = default;
};

}  // namespace aiforge::domain
