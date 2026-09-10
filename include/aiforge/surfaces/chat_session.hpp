#pragma once

#include <aiforge/backend/backend.hpp>
#include <aiforge/instructions/editor.hpp>
#include <aiforge/instructions/source.hpp>
#include <aiforge/persona/editor.hpp>
#include <aiforge/persona/source.hpp>
#include <aiforge/runtime/conversation_policy.hpp>
#include <aiforge/runtime/memory_controller.hpp>
#include <aiforge/runtime/plan_task_controller.hpp>
#include <aiforge/runtime/run_kernel.hpp>
#include <aiforge/runtime/summary_controller.hpp>
#include <aiforge/runtime/tool_profiles.hpp>
#include <aiforge/storage/session_store.hpp>
#include <aiforge/surfaces/chat_evidence_work.hpp>
#include <aiforge/surfaces/chat_repository_context.hpp>
#include <aiforge/surfaces/manual_ops_session.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace aiforge::surfaces {

class LocalSourceBrowser;

enum class ChatSessionErrorCode {
  invalid_input,
  input_too_large,
  model_lookup_failed,
  context_failed,
  session_failed,
  spend_ceiling_reached,
  spend_accounting_unavailable,
  run_failed,
  cancelled,
  internal_failure,
};

struct ChatSessionError {
  ChatSessionErrorCode code{ChatSessionErrorCode::internal_failure};
  std::string message;
  bool retryable{};
  bool effect_may_have_applied{};
  auto operator==(const ChatSessionError&) const -> bool = default;
};

struct ChatSessionLimits {
  std::size_t maximum_input_bytes{1024U * 1024U};
  std::uint64_t preferred_output_tokens{4096};
  auto operator==(const ChatSessionLimits&) const -> bool = default;
};

struct ChatSessionOpen {
  enum class Mode {
    create,
    resume,
    continue_latest,
    ephemeral,
  };

  domain::ModelId model_id;
  Mode mode{Mode::create};
  std::optional<domain::SessionId> session_id;
  // Recorded on every run this session starts, so each turn is independently
  // answerable. Its tool section is filled by the run kernel.
  std::optional<domain::RunProvenance> provenance{};
  persona::PersonaDirective persona{};
  std::optional<domain::SessionSpendCeiling> session_spend_ceiling{};
  backend::GenerationOptions generation_options{};
};

struct ChatPersonaState {
  std::optional<domain::PersonaReference> selected;
  bool requires_attention{};
  std::string message;
  auto operator==(const ChatPersonaState&) const -> bool = default;
};

struct ChatRecoveryBlock {
  domain::SessionId session_id;
  domain::RunId run_id;
  ChatSessionError reason;
  auto operator==(const ChatRecoveryBlock&) const -> bool = default;
};

struct ChatSubmission {
  domain::RunId run_id;
  std::vector<domain::RunEvent> committed_events;
};

using ChatIdentitySuffixSource = std::function<std::uint64_t()>;

enum class ChatSurfaceKind { interactive, agent };

struct ChatObservationContext {
  domain::SurfaceId surface_id;
  domain::WorkspaceId workspace_id;
};

struct ChatSessionDependencies {
  ChatIdentitySuffixSource identity_suffix_source;
  runtime::TimestampSource timestamp_source;
  runtime::RunKernelLimits run_limits{};
  runtime::ToolRegistrySnapshot tools{};
  std::shared_ptr<runtime::ToolPolicy> tool_policy;
  std::map<domain::ModelId, domain::ToolProfileId> model_tool_profile_maximums;
  std::map<domain::PersonaId, domain::ToolProfileId>
      persona_tool_profile_maximums;
  // When present, every interactive run uses this stable launch-policy
  // identity. The default preserves the legacy per-run observe identity.
  std::optional<domain::PermissionProfileId> permission_profile_id;
  persona::PersonaSource* persona_source{};
  persona::PersonaEditor* persona_editor{};
  persona::PersonaLimits persona_limits{};
  instructions::UserGlobalInstructionSource* user_global_instruction_source{};
  instructions::UserGlobalInstructionEditor* user_global_instruction_editor{};
  instructions::UserGlobalInstructionLimits user_global_instruction_limits{};
  bool user_global_instructions_enabled{};
  runtime::MemoryController* memory_controller{};
  runtime::MemorySettings memory_settings{};
  std::optional<domain::RepositoryId> repository_id;
  std::string runtime_version{"unknown"};
  ChatSurfaceKind surface_kind{ChatSurfaceKind::interactive};
  runtime::RepositoryContextController* repository_context_controller{};
  std::optional<runtime::RepositoryContextRequest>
      repository_context_selection{};
  bool async_repository_preparation{};
  // Borrowed on the session owner thread only. The application keeps the
  // browser alive until this session is destroyed; workers never borrow it.
  LocalSourceBrowser* local_sources{};
  // Application ownership extends beyond this session. Opening a candidate
  // never activates or replaces the broker's selected session.
  std::shared_ptr<runtime::OpsObservationBroker> observation_broker{};
  std::optional<ChatObservationContext> observation_context{};
};

class PreparedChatGenerationOptions final {
 public:
  PreparedChatGenerationOptions(const PreparedChatGenerationOptions&) = delete;
  auto operator=(const PreparedChatGenerationOptions&)
      -> PreparedChatGenerationOptions& = delete;
  PreparedChatGenerationOptions(PreparedChatGenerationOptions&&) noexcept =
      default;
  auto operator=(PreparedChatGenerationOptions&&) noexcept
      -> PreparedChatGenerationOptions& = default;

 private:
  friend class ChatSession;
  PreparedChatGenerationOptions(domain::ModelId model_id,
                                backend::ModelContextInfo model,
                                std::uint64_t output_tokens,
                                backend::GenerationOptions options,
                                std::optional<domain::RunProvenance> provenance)
      : m_model_id(std::move(model_id)), m_model(std::move(model)),
        m_output_tokens(output_tokens), m_options(std::move(options)),
        m_provenance(std::move(provenance)) {}

  domain::ModelId m_model_id;
  backend::ModelContextInfo m_model;
  std::uint64_t m_output_tokens{};
  backend::GenerationOptions m_options;
  std::optional<domain::RunProvenance> m_provenance;
};

class PreparedToolProfileMaximum final {
 public:
  PreparedToolProfileMaximum(const PreparedToolProfileMaximum&) = delete;
  auto operator=(const PreparedToolProfileMaximum&)
      -> PreparedToolProfileMaximum& = delete;
  PreparedToolProfileMaximum(PreparedToolProfileMaximum&&) noexcept = default;
  auto operator=(PreparedToolProfileMaximum&&) noexcept
      -> PreparedToolProfileMaximum& = default;

 private:
  friend class ChatSession;
  using Subject = std::variant<domain::ModelId, domain::PersonaId>;

  PreparedToolProfileMaximum(Subject subject,
                             std::optional<domain::ToolProfileId> profile_id,
                             const std::uint64_t revision)
      : m_subject(std::move(subject)), m_profile_id(std::move(profile_id)),
        m_revision(revision) {}

  Subject m_subject;
  std::optional<domain::ToolProfileId> m_profile_id;
  std::uint64_t m_revision{};
};

struct ChatConversationGroupInspection {
  domain::RunId run_id;
  std::size_t entry_count{};
  std::uint64_t estimated_tokens{};
  bool pinned{};
  std::optional<runtime::ConversationSelectionDecision> decision{};
};
struct ChatConversationContextInspection {
  runtime::ConversationPolicySnapshot policy;
  domain::ModelId model_id;
  // Preserved even when the next context cannot fit. Includes external tool
  // reservation, required instructions and the unsubmitted composer input.
  domain::ContextBuildInput mandatory;
  std::vector<ChatConversationGroupInspection> groups;
  std::vector<domain::ConversationAdmittedSummary> summaries;
  std::optional<domain::ConstructedContext> next_context{};
  std::optional<domain::ConversationAdmission> next_admission{};
  std::optional<domain::ConversationAdmission> active_admission{};
  std::optional<ChatSessionError> preparation_error{};
  std::optional<domain::LocalContextAdmission> local_admission{};
};

struct ChatSummaryGenerate {
  std::uint64_t expected_sequence{};
  std::vector<domain::RunId> covered_run_ids;
  std::size_t maximum_output_bytes{domain::summary_maximum_text_bytes};
};
struct ChatSummaryGeneration {
  domain::ConversationSummaryId summary_id;
  domain::RunId run_id;
  std::vector<domain::RunEvent> committed_events;
};
struct ChatSummaryOutputFailure {
  domain::ConversationSummaryId summary_id;
  std::string message;
};
struct ChatSummaryCatalog {
  runtime::ConversationSummarySnapshot snapshot;
  std::vector<runtime::ConversationSummaryDraft> unpublished;
  std::vector<ChatSummaryOutputFailure> unpublishable{};
};
struct ChatSummaryReviewData;
class ChatSummaryPreview final {
 public:
  [[nodiscard]] auto context() const noexcept
      -> const domain::ConstructedContext&;
  [[nodiscard]] auto activation() const noexcept
      -> const domain::ConversationSummaryActivation&;
  [[nodiscard]] auto local_admission() const noexcept
      -> const std::optional<domain::LocalContextAdmission>&;

 private:
  friend class ChatSession;
  explicit ChatSummaryPreview(
      std::shared_ptr<const ChatSummaryReviewData> data);
  std::shared_ptr<const ChatSummaryReviewData> m_data;
};

struct ChatEvidenceActionCompleted {
  std::vector<domain::RunEvent> events;
};
using ChatEvidenceResult =
    std::variant<ChatSubmission, ChatConversationContextInspection,
                 ChatSummaryPreview, domain::ConversationSummaryActivation,
                 ChatEvidenceActionCompleted>;
struct ChatEvidenceOutcome {
  ChatEvidenceWorkToken token;
  ChatEvidenceResult result;
};

class ChatSession final : public ManualOpsSession {
 public:
  [[nodiscard]] static auto open(ChatSessionOpen request,
                                 backend::Backend& backend,
                                 backend::ModelContextProvider& model_context,
                                 storage::SessionStore* session_store = nullptr,
                                 runtime::RunWakeSink* wake_sink = nullptr,
                                 std::stop_token stop_token = {},
                                 ChatSessionLimits limits = {},
                                 ChatSessionDependencies dependencies = {})
      -> std::expected<std::unique_ptr<ChatSession>, ChatSessionError>;

  ~ChatSession() override;

  ChatSession(const ChatSession&) = delete;
  auto operator=(const ChatSession&) -> ChatSession& = delete;
  ChatSession(ChatSession&&) = delete;
  auto operator=(ChatSession&&) -> ChatSession& = delete;

  [[nodiscard]] auto submit(std::string prompt)
      -> std::expected<ChatSubmission, ChatSessionError>;
  // Application owner operation; absent from the borrowed widget port.
  [[nodiscard]] auto bind_observation(
      domain::OpsObservationAuthority authority,
      std::shared_ptr<runtime::OpsObservationSource> source,
      std::shared_ptr<runtime::OpsObservationEndpoint> endpoint)
      -> std::expected<void, ManualOpsFailure>;
  [[nodiscard]] auto submit_observation(runtime::OpsObservationIntent intent)
      -> std::expected<ObservationSubmission, ManualOpsFailure> override;
  [[nodiscard]] auto cancel_observation(const domain::RunId& run_id)
      -> std::expected<void, ManualOpsFailure> override;
  [[nodiscard]] auto decide_observation_approval(
      const domain::RunId& run_id, const domain::InvocationId& invocation_id,
      runtime::ToolApprovalResolution decision)
      -> std::expected<void, ManualOpsFailure> override;
  [[nodiscard]] auto pump_observations()
      -> std::expected<void, ManualOpsFailure> override;
  [[nodiscard]] auto inspect_observations() const noexcept
      -> const ManualOpsInspection& override;
  [[nodiscard]] auto request_repository_change(ChatRepositoryChange change)
      -> std::expected<void, ChatSessionError>;
  [[nodiscard]] auto request_repository_submit(std::string prompt)
      -> std::expected<void, ChatSessionError>;
  [[nodiscard]] auto request_evidence_submit(std::string prompt)
      -> std::expected<void, ChatSessionError>;
  [[nodiscard]] auto request_context_inspection(std::string draft)
      -> std::expected<void, ChatSessionError>;
  [[nodiscard]] auto request_summary_preview(
      domain::ConversationSummaryVersion candidate,
      std::vector<domain::ConversationSummaryVersion> replacements,
      std::string draft) -> std::expected<void, ChatSessionError>;
  [[nodiscard]] auto request_summary_apply(const ChatSummaryPreview& preview,
                                           std::string current_draft)
      -> std::expected<void, ChatSessionError>;
  [[nodiscard]] auto pending_evidence_work() const
      -> std::optional<ChatEvidenceWorkToken>;
  // Summary application requires the current composer draft on every poll;
  // a missing or changed binding cancels the action before any mutation.
  // Other operations retain their captured request and do not need a draft.
  [[nodiscard]] auto poll_evidence_work(
      std::optional<std::string_view> current_draft = {})
      -> std::expected<std::optional<ChatEvidenceOutcome>, ChatSessionError>;
  auto cancel_evidence_work() -> void;
  [[nodiscard]] auto retry_context_sources()
      -> std::expected<void, ChatSessionError>;
  [[nodiscard]] auto pending_repository_work() const
      -> std::optional<ChatRepositoryWork>;
  [[nodiscard]] auto complete_repository_work(
      ChatRepositoryWorkCompletion completion)
      -> std::expected<ChatRepositoryWorkOutcome, ChatSessionError>;
  auto cancel_repository_work() -> void;
  [[nodiscard]] auto retry_repository_context()
      -> std::expected<void, ChatSessionError>;
  [[nodiscard]] auto repository_context_state() const
      -> ChatRepositoryContextState;
  [[nodiscard]] auto drain()
      -> std::expected<std::vector<domain::RunEvent>, ChatSessionError>;
  [[nodiscard]] auto cancel_active(
      std::optional<std::string> reason = std::nullopt)
      -> std::expected<void, ChatSessionError>;
  [[nodiscard]] auto blocked_recovery() const
      -> const std::optional<ChatRecoveryBlock>&;
  [[nodiscard]] auto pending_question_input() const
      -> std::optional<runtime::PendingQuestionInput>;
  [[nodiscard]] auto pending_tool_approval() const
      -> std::optional<runtime::PendingToolApproval>;
  [[nodiscard]] auto decide_tool_approval(
      const domain::RunId& run_id, const domain::InvocationId& invocation_id,
      runtime::ToolApprovalResolution resolution)
      -> std::expected<void, ChatSessionError>;
  [[nodiscard]] auto answer_questions(
      const domain::RunId& run_id, const domain::InvocationId& invocation_id,
      std::vector<domain::QuestionAnswer> answers)
      -> std::expected<void, ChatSessionError>;
  [[nodiscard]] auto cancel_questions(
      const domain::RunId& run_id, const domain::InvocationId& invocation_id,
      std::optional<std::string> reason = std::nullopt)
      -> std::expected<void, ChatSessionError>;
  [[nodiscard]] auto list_personas()
      -> std::expected<std::vector<domain::PersonaSummary>, ChatSessionError>;
  [[nodiscard]] auto load_persona(std::string name)
      -> std::expected<domain::PersonaDocument, ChatSessionError>;
  [[nodiscard]] auto select_persona(std::string name)
      -> std::expected<void, ChatSessionError>;
  [[nodiscard]] auto disable_persona() -> std::expected<void, ChatSessionError>;
  [[nodiscard]] auto create_persona(persona::PersonaCreate request)
      -> std::expected<persona::PersonaWriteReceipt, ChatSessionError>;
  [[nodiscard]] auto replace_persona(domain::PersonaReference expected,
                                     std::string text)
      -> std::expected<persona::PersonaWriteReceipt, ChatSessionError>;
  [[nodiscard]] auto select_model(domain::ModelId model_id)
      -> std::expected<void, ChatSessionError>;
  [[nodiscard]] auto select_tool_profile(domain::ToolProfileId profile_id)
      -> std::expected<void, ChatSessionError>;
  [[nodiscard]] auto reset_tool_narrowing()
      -> std::expected<void, ChatSessionError>;
  [[nodiscard]] auto set_tool_enabled(std::string tool_name, bool enabled)
      -> std::expected<void, ChatSessionError>;
  [[nodiscard]] auto set_tool_category_enabled(runtime::ToolCategory category,
                                               bool enabled)
      -> std::expected<void, ChatSessionError>;
  [[nodiscard]] auto set_model_tool_profile_maximum(
      std::optional<domain::ToolProfileId> profile_id)
      -> std::expected<void, ChatSessionError>;
  [[nodiscard]] auto prepare_model_tool_profile_maximum(
      std::optional<domain::ToolProfileId> profile_id) const
      -> std::expected<PreparedToolProfileMaximum, ChatSessionError>;
  [[nodiscard]] auto set_persona_tool_profile_maximum(
      std::optional<domain::ToolProfileId> profile_id)
      -> std::expected<void, ChatSessionError>;
  [[nodiscard]] auto prepare_persona_tool_profile_maximum(
      std::optional<domain::ToolProfileId> profile_id) const
      -> std::expected<PreparedToolProfileMaximum, ChatSessionError>;
  [[nodiscard]] auto commit_tool_profile_maximum(
      PreparedToolProfileMaximum prepared)
      -> std::expected<void, ChatSessionError>;
  [[nodiscard]] auto tool_profile_state() const
      -> std::expected<runtime::ToolProfileResolution, ChatSessionError>;
  [[nodiscard]] auto set_generation_options(
      backend::GenerationOptions options,
      std::vector<domain::EffectiveRequestOption> effective_request_options,
      std::optional<std::vector<domain::ConfigurationProvenanceEntry>>
          configuration = std::nullopt)
      -> std::expected<void, ChatSessionError>;
  [[nodiscard]] auto prepare_generation_options(
      backend::GenerationOptions options,
      std::vector<domain::EffectiveRequestOption> effective_request_options,
      std::optional<std::vector<domain::ConfigurationProvenanceEntry>>
          configuration = std::nullopt)
      -> std::expected<PreparedChatGenerationOptions, ChatSessionError>;
  [[nodiscard]] auto commit_generation_options(
      PreparedChatGenerationOptions prepared)
      -> std::expected<void, ChatSessionError>;
  [[nodiscard]] auto persona_state() const -> ChatPersonaState;
  [[nodiscard]] auto persona_limits() const noexcept -> persona::PersonaLimits;
  [[nodiscard]] auto load_user_global_instruction()
      -> std::expected<std::optional<domain::UserGlobalInstructionDocument>,
                       ChatSessionError>;
  [[nodiscard]] auto write_user_global_instruction(
      instructions::UserGlobalInstructionWrite request)
      -> std::expected<instructions::UserGlobalInstructionWriteReceipt,
                       ChatSessionError>;
  [[nodiscard]] auto set_user_global_instructions_enabled(
      bool enabled,
      std::optional<std::vector<domain::ConfigurationProvenanceEntry>>
          configuration = std::nullopt)
      -> std::expected<void, ChatSessionError>;
  [[nodiscard]] auto user_global_instructions_enabled() const noexcept -> bool;
  [[nodiscard]] auto user_global_instruction_limits() const noexcept
      -> instructions::UserGlobalInstructionLimits;

  [[nodiscard]] auto plan_task_state(
      std::optional<domain::RepositoryId> repository_id = std::nullopt)
      -> std::expected<runtime::PlanTaskState, ChatSessionError>;
  [[nodiscard]] auto decide_plan(
      const domain::RunId& run_id, domain::PlanRevisionDecision decision,
      runtime::PlanApprovalEnvironment environment = {})
      -> std::expected<runtime::PlanDecisionOutcome, ChatSessionError>;
  [[nodiscard]] auto promote_project_task(
      runtime::ProjectTaskPromotion promotion)
      -> std::expected<void, ChatSessionError>;
  [[nodiscard]] auto update_project_task_status(
      runtime::ProjectTaskStatusUpdate update)
      -> std::expected<void, ChatSessionError>;

  [[nodiscard]] auto memory_state(runtime::MemoryMutationTarget target)
      -> std::expected<runtime::MemoryState, ChatSessionError>;
  [[nodiscard]] auto accept_memory(runtime::MemoryAcceptRequest request)
      -> std::expected<void, ChatSessionError>;
  [[nodiscard]] auto reject_memory(runtime::MemoryRejectRequest request)
      -> std::expected<void, ChatSessionError>;
  [[nodiscard]] auto expire_memory(runtime::MemoryExpireRequest request)
      -> std::expected<void, ChatSessionError>;

  [[nodiscard]] auto conversation_policy() const
      -> std::expected<runtime::ConversationPolicySnapshot, ChatSessionError>;
  // Commits one explicit policy transaction. It affects subsequent top-level
  // turns; an active run retains its exact admitted context.
  [[nodiscard]] auto set_conversation_policy(
      std::uint64_t expected_revision, domain::ConversationMode mode,
      std::vector<domain::RunId> pinned_run_ids = {})
      -> std::expected<std::vector<domain::RunEvent>, ChatSessionError>;

  [[nodiscard]] auto inspect_conversation_context(std::string draft)
      -> std::expected<ChatConversationContextInspection, ChatSessionError>;
  [[nodiscard]] auto generate_conversation_summary(ChatSummaryGenerate request)
      -> std::expected<ChatSummaryGeneration, ChatSessionError>;
  [[nodiscard]] auto summary_catalog() const
      -> std::expected<ChatSummaryCatalog, ChatSessionError>;
  [[nodiscard]] auto publish_conversation_summary(
      domain::ConversationSummaryId summary_id)
      -> std::expected<domain::ConversationSummaryCandidate, ChatSessionError>;
  [[nodiscard]] auto edit_conversation_summary(
      std::uint64_t expected_sequence,
      domain::ConversationSummaryVersion parent, std::string text)
      -> std::expected<domain::ConversationSummaryCandidate, ChatSessionError>;
  [[nodiscard]] auto preview_conversation_summary(
      domain::ConversationSummaryVersion candidate,
      std::vector<domain::ConversationSummaryVersion> replacements,
      std::string draft) -> std::expected<ChatSummaryPreview, ChatSessionError>;
  [[nodiscard]] auto apply_conversation_summary(
      const ChatSummaryPreview& preview, std::string current_draft)
      -> std::expected<domain::ConversationSummaryActivation, ChatSessionError>;

  [[nodiscard]] auto disable_conversation_summary(
      std::uint64_t expected_policy_revision,
      domain::ConversationSummaryVersion candidate,
      domain::EventId activation_event_id)
      -> std::expected<std::vector<domain::RunEvent>, ChatSessionError>;

  [[nodiscard]] auto submitted_prompts() const -> std::vector<std::string>;
  [[nodiscard]] auto event_log() const noexcept
      -> const domain::SessionEventLog&;
  [[nodiscard]] auto session_id() const noexcept -> const domain::SessionId&;
  [[nodiscard]] auto model_id() const noexcept -> const domain::ModelId&;
  [[nodiscard]] auto model_info() const noexcept
      -> const backend::ModelContextInfo&;
  [[nodiscard]] auto durable() const noexcept -> bool;
  [[nodiscard]] auto active() const noexcept -> bool;

 private:
  [[nodiscard]] auto drain_model_events()
      -> std::expected<std::vector<domain::RunEvent>, ChatSessionError>;
  [[nodiscard]] auto cancel_model_run(std::optional<std::string> reason)
      -> std::expected<void, ChatSessionError>;
  [[nodiscard]] auto summary_mandatory_context(
      const std::string& draft, std::uint64_t identity,
      const std::optional<runtime::PreparedRepositoryContext>& repository)
      -> std::expected<domain::ContextBuildInput, ChatSessionError>;
  [[nodiscard]] auto summary_repository_context()
      -> std::expected<std::optional<runtime::PreparedRepositoryContext>,
                       ChatSessionError>;
  [[nodiscard]] auto summary_control_attributes(std::uint64_t suffix) const
      -> std::expected<domain::RunStarted, ChatSessionError>;
  struct Impl;
  explicit ChatSession(std::unique_ptr<Impl> impl);
  [[nodiscard]] auto begin_evidence_work(
      ChatEvidenceWorkPurpose purpose,
      std::function<std::expected<ChatEvidenceResult, ChatSessionError>()>
          action,
      bool original_sources = false,
      std::optional<std::string> required_draft = {})
      -> std::expected<void, ChatSessionError>;
  [[nodiscard]] auto begin_local_evidence_work()
      -> std::expected<void, ChatSessionError>;
  [[nodiscard]] auto validate_active_evidence(ChatRepositoryWorkPurpose purpose)
      -> std::expected<bool, ChatSessionError>;
  [[nodiscard]] auto validate_recovered_pending_run(
      bool repository_validated = false)
      -> std::expected<void, ChatSessionError>;
  [[nodiscard]] auto load_recovered_memory()
      -> std::expected<void, ChatSessionError>;
  [[nodiscard]] auto pin_recovered_summary_sources()
      -> std::expected<void, ChatSessionError>;
  [[nodiscard]] auto validate_recovered_memory_capacity()
      -> std::expected<void, ChatSessionError>;
  [[nodiscard]] auto load_recovered_pending_sources()
      -> std::expected<void, ChatSessionError>;
  [[nodiscard]] auto continue_if_ready()
      -> std::expected<std::vector<domain::RunEvent>, ChatSessionError>;
  [[nodiscard]] auto dispatch_ready_tools()
      -> std::expected<std::optional<std::vector<domain::RunEvent>>,
                       ChatSessionError>;
  [[nodiscard]] auto apply_repository_change(
      runtime::RepositoryContextRequest& request,
      const ChatRepositoryChange& change)
      -> std::expected<void, ChatSessionError>;
  [[nodiscard]] auto validate_repository_completion(
      const ChatRepositoryWork& work,
      const runtime::PreparedRepositoryContext& prepared) const
      -> std::expected<void, ChatSessionError>;
  [[nodiscard]] auto apply_repository_completion(
      ChatRepositoryWork work, runtime::PreparedRepositoryContext prepared,
      std::optional<std::string> prompt,
      std::function<std::expected<void, ChatSessionError>()> action)
      -> std::expected<ChatRepositoryWorkOutcome, ChatSessionError>;
  [[nodiscard]] auto prepare_recovered_repository()
      -> std::expected<bool, ChatSessionError>;
  [[nodiscard]] auto validate_active_repository(
      ChatRepositoryWorkPurpose purpose)
      -> std::expected<bool, ChatSessionError>;
  [[nodiscard]] auto submit_prepared(
      std::string prompt,
      std::optional<runtime::PreparedRepositoryContext> prepared)
      -> std::expected<ChatSubmission, ChatSessionError>;
  std::unique_ptr<Impl> m_impl;
};

} // namespace aiforge::surfaces
