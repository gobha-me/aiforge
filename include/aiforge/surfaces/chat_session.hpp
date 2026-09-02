#pragma once

#include <aiforge/backend/backend.hpp>
#include <aiforge/persona/editor.hpp>
#include <aiforge/persona/source.hpp>
#include <aiforge/runtime/memory_controller.hpp>
#include <aiforge/runtime/plan_task_controller.hpp>
#include <aiforge/runtime/run_kernel.hpp>
#include <aiforge/storage/session_store.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
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
  persona::PersonaSource* persona_source{};
  persona::PersonaEditor* persona_editor{};
  persona::PersonaLimits persona_limits{};
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
  std::unique_ptr<Impl> m_impl;
};

} // namespace aiforge::surfaces
