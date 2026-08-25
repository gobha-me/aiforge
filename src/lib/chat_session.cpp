#include <aiforge/runtime/context_builder.hpp>
#include <aiforge/runtime/persona.hpp>
#include <aiforge/surfaces/chat_session.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string_view>
#include <utility>
#include <variant>

#include "conversation_context.hpp"

namespace aiforge::surfaces {
namespace {

[[nodiscard]] auto error(const ChatSessionErrorCode code, std::string message,
                         const bool retryable = false)
    -> std::unexpected<ChatSessionError> {
  return std::unexpected(ChatSessionError{code, std::move(message), retryable});
}

[[nodiscard]] auto valid_text(const std::string_view value) -> bool {
  std::size_t index{};
  while (index < value.size()) {
    const auto first = static_cast<unsigned char>(value[index]);
    if (first == 0 || (first < 0x20U && first != '\n' && first != '\t') ||
        first == 0x7FU) {
      return false;
    }
    std::size_t length{};
    std::uint32_t codepoint{};
    if (first <= 0x7FU) {
      length = 1;
      codepoint = first;
    } else if ((first & 0xE0U) == 0xC0U) {
      length = 2;
      codepoint = first & 0x1FU;
      if (codepoint < 2) return false;
    } else if ((first & 0xF0U) == 0xE0U) {
      length = 3;
      codepoint = first & 0x0FU;
    } else if ((first & 0xF8U) == 0xF0U) {
      length = 4;
      codepoint = first & 0x07U;
    } else {
      return false;
    }
    if (length > value.size() - index) return false;
    for (std::size_t offset = 1; offset < length; ++offset) {
      const auto next = static_cast<unsigned char>(value[index + offset]);
      if ((next & 0xC0U) != 0x80U) return false;
      codepoint = (codepoint << 6U) | (next & 0x3FU);
    }
    if ((length == 3 && codepoint < 0x800U) ||
        (length == 4 && codepoint < 0x10000U) ||
        (codepoint >= 0xD800U && codepoint <= 0xDFFFU) ||
        codepoint > 0x10FFFFU) {
      return false;
    }
    index += length;
  }
  return true;
}

template <typename IdType>
[[nodiscard]] auto make_id(const std::string_view prefix,
                           const std::uint64_t suffix)
    -> std::expected<IdType, ChatSessionError> {
  auto id = IdType::from(std::string{prefix} + '-' + std::to_string(suffix));
  if (!id) {
    return error(ChatSessionErrorCode::internal_failure,
                 "interactive identity generation failed");
  }
  return std::move(*id);
}

[[nodiscard]] auto next_suffix() -> std::uint64_t {
  static std::atomic<std::uint64_t> sequence{};
  const auto count = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
  const auto tick = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  return tick ^ count;
}

[[nodiscard]] auto kernel_error(const runtime::RunKernelError& value)
    -> ChatSessionError {
  return {ChatSessionErrorCode::run_failed, value.message, value.retryable};
}

struct PersonaSetup {
  std::optional<domain::PersonaDocument> document;
  std::optional<domain::PersonaSelection> next_selection;
  std::string attention;
};

[[nodiscard]] auto persona_error(const persona::PersonaError& value)
    -> ChatSessionError {
  return {value.code == persona::PersonaErrorCode::cancelled
              ? ChatSessionErrorCode::cancelled
              : value.code == persona::PersonaErrorCode::invalid_name
                    ? ChatSessionErrorCode::invalid_input
                    : ChatSessionErrorCode::context_failed,
          value.message, value.retryable};
}

[[nodiscard]] auto resolve_persona(
    persona::PersonaSource* source, const persona::PersonaLimits limits,
    const persona::PersonaDirective& directive,
    const domain::SessionEventLog& event_log,
    const std::stop_token stop_token, const bool allow_attention)
    -> std::expected<PersonaSetup, ChatSessionError> {
  if ((directive.kind == persona::PersonaDirectiveKind::select) !=
          directive.name.has_value() ||
      directive.source == domain::PersonaSelectionSource::unknown) {
    return error(ChatSessionErrorCode::invalid_input,
                 "persona selection is invalid");
  }
  auto latest = runtime::latest_persona_selection(event_log);
  if (!latest) {
    return error(ChatSessionErrorCode::session_failed,
                 "persona history is invalid");
  }
  std::optional<domain::PersonaReference> previous;
  if (*latest && (*latest)->action == domain::PersonaSelectionAction::selected) {
    previous = (*latest)->persona;
  }
  if (directive.kind == persona::PersonaDirectiveKind::disable) {
    return PersonaSetup{
        std::nullopt,
        domain::PersonaSelection{domain::PersonaSelectionAction::disabled,
                                 directive.source, std::nullopt, previous},
        {}};
  }
  if (directive.kind == persona::PersonaDirectiveKind::inherit) {
    if (!*latest) return PersonaSetup{};
    if ((*latest)->action == domain::PersonaSelectionAction::disabled) {
      return PersonaSetup{
          std::nullopt,
          domain::PersonaSelection{domain::PersonaSelectionAction::disabled,
                                   domain::PersonaSelectionSource::resumed,
                                   std::nullopt, std::nullopt},
          {}};
    }
  }
  if (source == nullptr) {
    if (allow_attention) {
      return PersonaSetup{
          std::nullopt, std::nullopt,
          "Persona source is unavailable; select a persona or turn it off"};
    }
    return error(ChatSessionErrorCode::context_failed,
                 "persona source is unavailable");
  }
  const auto name = directive.kind == persona::PersonaDirectiveKind::select
                        ? *directive.name
                        : previous->name;
  auto loaded = source->load(name, limits, stop_token);
  if (!loaded) {
    if (allow_attention &&
        directive.kind == persona::PersonaDirectiveKind::inherit) {
      return PersonaSetup{
          std::nullopt, std::nullopt,
          "Persona needs attention: " + loaded.error().message};
    }
    return std::unexpected(persona_error(loaded.error()));
  }
  if (!domain::validate_persona_document(*loaded)) {
    return error(ChatSessionErrorCode::context_failed,
                 "persona document is invalid");
  }
  if (directive.kind == persona::PersonaDirectiveKind::inherit &&
      loaded->reference != *previous) {
    if (allow_attention) {
      return PersonaSetup{
          std::nullopt, std::nullopt,
          "Persona changed since this session was recorded; select it again or turn it off"};
    }
    return error(ChatSessionErrorCode::context_failed,
                 "persona changed since this session was recorded");
  }
  const auto selection_source =
      directive.kind == persona::PersonaDirectiveKind::inherit
          ? domain::PersonaSelectionSource::resumed
          : directive.source;
  auto reference = loaded->reference;
  return PersonaSetup{
      std::move(*loaded),
      domain::PersonaSelection{domain::PersonaSelectionAction::selected,
                               selection_source, std::move(reference),
                               std::move(previous)},
      {}};
}

}  // namespace

struct ChatSession::Impl {
  domain::ModelId model_id;
  backend::ModelContextInfo model;
  std::uint64_t output_tokens{};
  ChatSessionLimits limits;
  bool is_durable{};
  ChatIdentitySuffixSource identity_suffix_source;
  std::optional<domain::RunProvenance> provenance;
  persona::PersonaSource* persona_source{};
  persona::PersonaLimits persona_limits{};
  std::stop_token stop_token;
  std::optional<domain::PersonaDocument> persona_document;
  std::optional<domain::PersonaSelection> next_persona_selection;
  std::string persona_attention;
  std::unique_ptr<runtime::RunKernel> kernel;
};

ChatSession::ChatSession(std::unique_ptr<Impl> impl)
    : m_impl(std::move(impl)) {}
ChatSession::~ChatSession() = default;

auto ChatSession::open(ChatSessionOpen request, backend::Backend& backend,
                       backend::ModelContextProvider& model_context,
                       storage::SessionStore* session_store,
                       runtime::RunWakeSink* wake_sink,
                       const std::stop_token stop_token,
                       const ChatSessionLimits limits,
                       ChatSessionDependencies dependencies)
    -> std::expected<std::unique_ptr<ChatSession>, ChatSessionError> {
  try {
    if (limits.maximum_input_bytes == 0 ||
        limits.preferred_output_tokens == 0) {
      return error(ChatSessionErrorCode::invalid_input,
                   "interactive limits are invalid");
    }
    const bool exact_resume = request.mode == ChatSessionOpen::Mode::resume;
    if (exact_resume != request.session_id.has_value()) {
      return error(ChatSessionErrorCode::invalid_input,
                   "interactive session selection is invalid");
    }
    if (request.mode != ChatSessionOpen::Mode::create &&
        request.mode != ChatSessionOpen::Mode::ephemeral &&
        session_store == nullptr) {
      return error(ChatSessionErrorCode::session_failed,
                   "durable session storage is unavailable");
    }
    if (stop_token.stop_requested()) {
      return error(ChatSessionErrorCode::cancelled, "request cancelled");
    }

    auto model = model_context.lookup(request.model_id, stop_token);
    if (!model) {
      if (model.error().kind == backend::BackendErrorKind::cancelled ||
          stop_token.stop_requested()) {
        return error(ChatSessionErrorCode::cancelled, "request cancelled");
      }
      return error(ChatSessionErrorCode::model_lookup_failed,
                   "model context lookup failed");
    }
    if (model->model_id != request.model_id ||
        model->context_window_tokens == 0) {
      return error(ChatSessionErrorCode::model_lookup_failed,
                   "model context metadata is invalid");
    }
    auto output_tokens = limits.preferred_output_tokens;
    if (model->maximum_output_tokens) {
      output_tokens = std::min(output_tokens, *model->maximum_output_tokens);
    }
    if (output_tokens == 0 || output_tokens >= model->context_window_tokens) {
      return error(ChatSessionErrorCode::context_failed,
                   "model context capacity is too small");
    }

    if (!dependencies.identity_suffix_source) {
      dependencies.identity_suffix_source = next_suffix;
    }
    const auto suffix = dependencies.identity_suffix_source();
    auto generated = make_id<domain::SessionId>("session", suffix);
    if (!generated) return std::unexpected(std::move(generated.error()));
    auto selected = request.session_id.value_or(*generated);
    if (request.mode == ChatSessionOpen::Mode::continue_latest) {
      auto sessions = session_store->list_sessions(1, stop_token);
      if (!sessions) {
        return error(
            stop_token.stop_requested() ? ChatSessionErrorCode::cancelled
                                        : ChatSessionErrorCode::session_failed,
            stop_token.stop_requested() ? "request cancelled"
                                        : "recent sessions could not be listed",
            sessions.error().retryable);
      }
      if (sessions->empty()) {
        return error(ChatSessionErrorCode::invalid_input,
                     "there is no durable session to continue");
      }
      selected = sessions->front().session_id;
    }

    const bool durable = request.mode != ChatSessionOpen::Mode::ephemeral &&
                         session_store != nullptr;
    std::unique_ptr<runtime::RunKernel> kernel;
    if (durable) {
      const auto mode = request.mode == ChatSessionOpen::Mode::create
                            ? runtime::DurableSessionMode::create
                            : runtime::DurableSessionMode::resume;
      auto opened = runtime::RunKernel::open_durable(
          {selected, mode,
           std::chrono::floor<std::chrono::milliseconds>(
               std::chrono::system_clock::now())},
          *session_store, backend, wake_sink,
          std::move(dependencies.timestamp_source), dependencies.run_limits,
          std::move(dependencies.tools), std::move(dependencies.tool_policy));
      if (!opened) {
        return error(ChatSessionErrorCode::session_failed,
                     "durable session could not be opened",
                     opened.error().retryable);
      }
      kernel = std::move(*opened);
    } else {
      kernel = std::make_unique<runtime::RunKernel>(
          selected, backend, wake_sink,
          std::move(dependencies.timestamp_source), dependencies.run_limits,
          std::move(dependencies.tools), std::move(dependencies.tool_policy));
    }

    const bool allow_persona_attention =
        request.mode == ChatSessionOpen::Mode::resume ||
        request.mode == ChatSessionOpen::Mode::continue_latest;
    auto persona_setup = resolve_persona(
        dependencies.persona_source, dependencies.persona_limits,
        request.persona, kernel->event_log(), stop_token,
        allow_persona_attention);
    if (!persona_setup) {
      return std::unexpected(std::move(persona_setup.error()));
    }
    auto impl = std::make_unique<Impl>(Impl{
        request.model_id,
        *model,
        output_tokens,
        limits,
        durable,
        std::move(dependencies.identity_suffix_source),
        std::move(request.provenance),
        dependencies.persona_source,
        dependencies.persona_limits,
        stop_token,
        std::move(persona_setup->document),
        std::move(persona_setup->next_selection),
        std::move(persona_setup->attention),
        std::move(kernel)});
    return std::unique_ptr<ChatSession>{new ChatSession{std::move(impl)}};
  } catch (...) {
    return error(ChatSessionErrorCode::internal_failure,
                 "interactive session setup failed internally");
  }
}

auto ChatSession::submit(std::string prompt)
    -> std::expected<ChatSubmission, ChatSessionError> {
  try {
    if (prompt.empty() || !valid_text(prompt)) {
      return error(ChatSessionErrorCode::invalid_input,
                   "prompt must be nonempty UTF-8 text without controls");
    }
    if (prompt.size() > m_impl->limits.maximum_input_bytes) {
      return error(ChatSessionErrorCode::input_too_large,
                   "prompt exceeds the configured input limit");
    }
    if (m_impl->kernel->active_run_id()) {
      return error(ChatSessionErrorCode::run_failed,
                   "another interactive run is active");
    }
    if (!m_impl->persona_attention.empty()) {
      return error(ChatSessionErrorCode::context_failed,
                   m_impl->persona_attention);
    }
    if (m_impl->persona_document) {
      if (m_impl->persona_source == nullptr) {
        m_impl->persona_attention = "Persona source is unavailable";
        return error(ChatSessionErrorCode::context_failed,
                     m_impl->persona_attention);
      }
      auto current = m_impl->persona_source->load(
          m_impl->persona_document->reference.name,
          m_impl->persona_limits, m_impl->stop_token);
      if (!current) {
        m_impl->persona_attention =
            "Persona needs attention: " + current.error().message;
        return std::unexpected(persona_error(current.error()));
      }
      if (!domain::validate_persona_document(*current) ||
          current->reference != m_impl->persona_document->reference) {
        m_impl->persona_attention =
            "Persona changed; select it again or turn it off";
        return error(ChatSessionErrorCode::context_failed,
                     m_impl->persona_attention);
      }
      m_impl->persona_document = std::move(*current);
    }

    const auto suffix = m_impl->identity_suffix_source();
    auto run_id = make_id<domain::RunId>("run", suffix);
    auto inference_id = make_id<domain::InferenceId>("inference", suffix);
    auto user_message_id = make_id<domain::MessageId>("user", suffix);
    auto assistant_message_id = make_id<domain::MessageId>("assistant", suffix);
    auto runtime_message_id = make_id<domain::MessageId>("runtime", suffix);
    auto runtime_entry_id =
        make_id<domain::ContextEntryId>("runtime-entry", suffix);
    auto user_entry_id = make_id<domain::ContextEntryId>("user-entry", suffix);
    auto runtime_source_id =
        make_id<domain::ContextSourceId>("runtime-source", suffix);
    auto user_source_id =
        make_id<domain::ContextSourceId>("user-source", suffix);
    auto surface_id = make_id<domain::SurfaceId>("interactive", suffix);
    auto workspace_id = make_id<domain::WorkspaceId>("chat", suffix);
    auto permission_id =
        make_id<domain::PermissionProfileId>("observe", suffix);
    if (!run_id || !inference_id || !user_message_id || !assistant_message_id ||
        !runtime_message_id || !runtime_entry_id || !user_entry_id ||
        !runtime_source_id || !user_source_id || !surface_id || !workspace_id ||
        !permission_id) {
      return error(ChatSessionErrorCode::internal_failure,
                   "interactive identity generation failed");
    }

    domain::Message user_message{*user_message_id,
                                 domain::Role::user,
                                 {domain::TextBlock{prompt}},
                                 std::nullopt};
    auto history =
        detail::replayed_conversation(m_impl->kernel->event_log(), suffix);
    if (!history) {
      return error(ChatSessionErrorCode::session_failed,
                   std::move(history.error()));
    }
    auto content = std::move(*history);
    content.push_back(
        {*user_entry_id,
         domain::ContextContentKind::conversation,
         user_message,
         {*user_source_id, std::string{"interactive-composer"}, std::nullopt},
         static_cast<std::uint64_t>(content.size()) + 1,
         prompt.size()});

    domain::ContextBuildInput input{
        {m_impl->model.context_window_tokens, m_impl->output_tokens, 0},
        {{*runtime_entry_id,
          domain::InstructionLayer::application_runtime,
          domain::InstructionOperation::add,
          std::nullopt,
          domain::Message{
              *runtime_message_id,
              domain::Role::system,
              {domain::TextBlock{std::string{detail::runtime_contract}}},
              std::nullopt},
          {*runtime_source_id, std::string{"aiforge:runtime"}, std::nullopt},
          0,
          1,
          detail::runtime_contract.size()}},
        std::move(content)};
    if (m_impl->persona_document) {
      auto persona_instruction = runtime::persona_instruction_input(
          *m_impl->persona_document, m_impl->persona_document->text.size());
      if (!persona_instruction) {
        return error(ChatSessionErrorCode::context_failed,
                     persona_instruction.error().message);
      }
      input.instructions.push_back(std::move(*persona_instruction));
    }
    auto context = runtime::ContextBuilder{}.build(std::move(input));
    if (!context) {
      return error(ChatSessionErrorCode::context_failed,
                   "prompt exceeds model context capacity");
    }

    const auto before = m_impl->kernel->event_log().events().size();
    backend::BackendRequest backend_request{
        *inference_id,
        *assistant_message_id,
        m_impl->model_id,
        std::move(*context),
        {},
        {std::nullopt, m_impl->output_tokens, std::nullopt, {}}};
    auto started = m_impl->kernel->start(
        {*run_id,
         {*surface_id, *workspace_id, *permission_id,
          m_impl->persona_document
              ? std::optional<domain::PersonaId>{
                    m_impl->persona_document->reference.persona_id}
              : std::nullopt},
         std::move(user_message),
         std::move(backend_request),
         m_impl->provenance,
         m_impl->next_persona_selection});
    if (!started) return std::unexpected(kernel_error(started.error()));

    if (m_impl->persona_document) {
      m_impl->next_persona_selection = domain::PersonaSelection{
          domain::PersonaSelectionAction::selected,
          domain::PersonaSelectionSource::retained,
          m_impl->persona_document->reference, std::nullopt};
    } else {
      m_impl->next_persona_selection.reset();
    }

    const auto events = m_impl->kernel->event_log().events();
    std::vector<domain::RunEvent> committed;
    committed.reserve(events.size() - before);
    for (std::size_t index = before; index < events.size(); ++index) {
      committed.push_back(events[index]);
    }
    return ChatSubmission{*run_id, std::move(committed)};
  } catch (...) {
    return error(ChatSessionErrorCode::internal_failure,
                 "interactive submission failed internally");
  }
}

auto ChatSession::drain()
    -> std::expected<std::vector<domain::RunEvent>, ChatSessionError> {
  auto drained = m_impl->kernel->drain();
  if (!drained) return std::unexpected(kernel_error(drained.error()));
  return std::move(*drained);
}

auto ChatSession::cancel_active(std::optional<std::string> reason)
    -> std::expected<void, ChatSessionError> {
  const auto run = m_impl->kernel->active_run_id();
  if (!run) return {};
  auto cancelled = m_impl->kernel->cancel_run(*run, std::move(reason));
  if (!cancelled) return std::unexpected(kernel_error(cancelled.error()));
  return {};
}

auto ChatSession::list_personas()
    -> std::expected<std::vector<domain::PersonaSummary>, ChatSessionError> {
  if (m_impl->persona_source == nullptr) {
    return error(ChatSessionErrorCode::context_failed,
                 "persona source is unavailable");
  }
  auto listed = m_impl->persona_source->list(m_impl->persona_limits,
                                             m_impl->stop_token);
  if (!listed) return std::unexpected(persona_error(listed.error()));
  return std::move(*listed);
}

auto ChatSession::select_persona(std::string name)
    -> std::expected<void, ChatSessionError> {
  if (active()) {
    return error(ChatSessionErrorCode::run_failed,
                 "finish or cancel the active run before selecting a persona");
  }
  if (m_impl->persona_source == nullptr) {
    return error(ChatSessionErrorCode::context_failed,
                 "persona source is unavailable");
  }
  auto loaded = m_impl->persona_source->load(
      std::move(name), m_impl->persona_limits, m_impl->stop_token);
  if (!loaded) return std::unexpected(persona_error(loaded.error()));
  if (!domain::validate_persona_document(*loaded)) {
    return error(ChatSessionErrorCode::context_failed,
                 "persona document is invalid");
  }
  std::optional<domain::PersonaReference> previous;
  if (m_impl->persona_document) previous = m_impl->persona_document->reference;
  auto reference = loaded->reference;
  m_impl->persona_document = std::move(*loaded);
  m_impl->next_persona_selection = domain::PersonaSelection{
      domain::PersonaSelectionAction::selected,
      domain::PersonaSelectionSource::interactive, std::move(reference),
      std::move(previous)};
  m_impl->persona_attention.clear();
  return {};
}

auto ChatSession::disable_persona() -> std::expected<void, ChatSessionError> {
  if (active()) {
    return error(ChatSessionErrorCode::run_failed,
                 "finish or cancel the active run before disabling a persona");
  }
  std::optional<domain::PersonaReference> previous;
  if (m_impl->persona_document) previous = m_impl->persona_document->reference;
  m_impl->persona_document.reset();
  m_impl->next_persona_selection = domain::PersonaSelection{
      domain::PersonaSelectionAction::disabled,
      domain::PersonaSelectionSource::interactive, std::nullopt,
      std::move(previous)};
  m_impl->persona_attention.clear();
  return {};
}

auto ChatSession::persona_state() const -> ChatPersonaState {
  return {m_impl->persona_document
              ? std::optional<domain::PersonaReference>{
                    m_impl->persona_document->reference}
              : std::nullopt,
          !m_impl->persona_attention.empty(), m_impl->persona_attention};
}

auto ChatSession::submitted_prompts() const -> std::vector<std::string> {
  std::vector<std::string> result;
  for (const auto& event : m_impl->kernel->event_log().events()) {
    const auto* added = std::get_if<domain::UserContentAdded>(&event.payload);
    if (added == nullptr) continue;
    std::string text;
    bool supported{true};
    for (const auto& block : added->message.content) {
      if (const auto* value = std::get_if<domain::TextBlock>(&block)) {
        text += value->text;
      } else {
        supported = false;
        break;
      }
    }
    if (supported && !text.empty()) result.push_back(std::move(text));
  }
  return result;
}

auto ChatSession::event_log() const noexcept -> const domain::SessionEventLog& {
  return m_impl->kernel->event_log();
}

auto ChatSession::session_id() const noexcept -> const domain::SessionId& {
  return m_impl->kernel->event_log().session_id();
}

auto ChatSession::model_id() const noexcept -> const domain::ModelId& {
  return m_impl->model_id;
}

auto ChatSession::durable() const noexcept -> bool {
  return m_impl->is_durable;
}

auto ChatSession::active() const noexcept -> bool {
  return m_impl->kernel->active_run_id().has_value();
}

}  // namespace aiforge::surfaces
