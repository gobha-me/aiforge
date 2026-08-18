#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <aiforge/domain/content.hpp>

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

struct ChildRunCreated {
  RunId child_run_id;
  auto operator==(const ChildRunCreated&) const -> bool = default;
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
    RunStarted, RunAwaitingInput, RunResumed, RunCompletionRequested, RunCompleted,
    RunFailed, RunCancelRequested, RunCancelled, UserContentAdded,
    AssistantContentStarted, AssistantContentDeltaAdded, AssistantContentFinished,
    InferenceStarted, ReasoningMetadataAdded, UsageRecorded, InferenceFinished,
    InferenceFailed, InferenceCancelled, ToolProposed, ToolPolicyDecided,
    ToolApprovalRequested, ToolApprovalDecided, ToolPolicyFailed, ToolStarted,
    ToolProgressed, ToolResultRecorded, ToolErrored, QuestionRequested,
    QuestionAnswered, QuestionCancelled, ArtifactCreated, ArtifactReferenced,
    ArtifactDisplayed, ArtifactRemovedFromView, ChildRunCreated,
    InterRunMessageSent, UnknownEvent>;

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
