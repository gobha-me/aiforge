#pragma once

#include <aiforge/backend/backend.hpp>
#include <aiforge/persona/source.hpp>
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
  run_failed,
  cancelled,
  internal_failure,
};

struct ChatSessionError {
  ChatSessionErrorCode code{ChatSessionErrorCode::internal_failure};
  std::string message;
  bool retryable{};
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
  persona::PersonaLimits persona_limits{};
};

class ChatSession final {
 public:
  [[nodiscard]] static auto open(
      ChatSessionOpen request, backend::Backend& backend,
      backend::ModelContextProvider& model_context,
      storage::SessionStore* session_store = nullptr,
      runtime::RunWakeSink* wake_sink = nullptr,
      std::stop_token stop_token = {}, ChatSessionLimits limits = {},
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
  [[nodiscard]] auto select_persona(std::string name)
      -> std::expected<void, ChatSessionError>;
  [[nodiscard]] auto disable_persona()
      -> std::expected<void, ChatSessionError>;
  [[nodiscard]] auto persona_state() const -> ChatPersonaState;

  [[nodiscard]] auto submitted_prompts() const -> std::vector<std::string>;
  [[nodiscard]] auto event_log() const noexcept
      -> const domain::SessionEventLog&;
  [[nodiscard]] auto session_id() const noexcept -> const domain::SessionId&;
  [[nodiscard]] auto model_id() const noexcept -> const domain::ModelId&;
  [[nodiscard]] auto durable() const noexcept -> bool;
  [[nodiscard]] auto active() const noexcept -> bool;

 private:
  struct Impl;
  explicit ChatSession(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> m_impl;
};

}  // namespace aiforge::surfaces
