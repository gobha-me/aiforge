#pragma once

#include <aiforge/backend/backend.hpp>
#include <aiforge/instructions/editor.hpp>
#include <aiforge/instructions/source.hpp>
#include <aiforge/persona/editor.hpp>
#include <aiforge/persona/source.hpp>
#include <aiforge/runtime/memory_controller.hpp>
#include <aiforge/runtime/plan_task_controller.hpp>
#include <aiforge/runtime/run_kernel.hpp>
#include <aiforge/runtime/tool_profiles.hpp>
#include <aiforge/storage/session_store.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <variant>
#include <vector>

namespace aiforge::surfaces {

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

struct ChatSubmission {
  domain::RunId run_id;
  std::vector<domain::RunEvent> committed_events;
};

using ChatIdentitySuffixSource = std::function<std::uint64_t()>;

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

class ChatSession final {
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

  ~ChatSession();

  ChatSession(const ChatSession&) = delete;
  auto operator=(const ChatSession&) -> ChatSession& = delete;
  ChatSession(ChatSession&&) = delete;
  auto operator=(ChatSession&&) -> ChatSession& = delete;

  [[nodiscard]] auto submit(std::string prompt)
      -> std::expected<ChatSubmission, ChatSessionError>;
  [[nodiscard]] auto drain()
      -> std::expected<std::vector<domain::RunEvent>, ChatSessionError>;
  [[nodiscard]] auto cancel_active(
      std::optional<std::string> reason = std::nullopt)
      -> std::expected<void, ChatSessionError>;
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
  [[nodiscard]] auto create_persona(persona::PersonaDraft draft)
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
  struct Impl;
  explicit ChatSession(std::unique_ptr<Impl> impl);
  [[nodiscard]] auto validate_recovered_pending_run()
      -> std::expected<void, ChatSessionError>;
  [[nodiscard]] auto continue_if_ready()
      -> std::expected<std::vector<domain::RunEvent>, ChatSessionError>;
  std::unique_ptr<Impl> m_impl;
};

} // namespace aiforge::surfaces
