#include <aiforge/domain/usage_ledger.hpp>
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

struct SpendState {
  domain::UsageLedgerProjection ledger;
  domain::SessionSpendCeilingProjection ceiling;
};

[[nodiscard]] auto rebuild_spend_ceiling(const domain::SessionEventLog& log)
    -> std::expected<domain::SessionSpendCeilingProjection, ChatSessionError> {
  domain::SessionSpendCeilingProjection ceiling;
  for (const auto& event : log.events()) {
    if (!ceiling.apply(event)) {
      return error(ChatSessionErrorCode::session_failed,
                   "session spend ceiling history is invalid");
    }
  }
  return ceiling;
}

[[nodiscard]] auto rebuild_spend_state(const domain::SessionEventLog& log)
    -> std::expected<SpendState, ChatSessionError> {
  SpendState state;
  for (const auto& event : log.events()) {
    if (!state.ledger.apply(event) || !state.ceiling.apply(event)) {
      return error(ChatSessionErrorCode::session_failed,
                   "session spend history is invalid");
    }
  }
  return state;
}

[[nodiscard]] auto apply_requested_spend_ceiling(
    runtime::RunKernel& kernel,
    const std::optional<domain::SessionSpendCeiling>& requested,
    const ChatIdentitySuffixSource& suffix_source,
    const std::optional<domain::PersonaId>& persona_id)
    -> std::expected<void, ChatSessionError> {
  auto ceiling = rebuild_spend_ceiling(kernel.event_log());
  if (!ceiling) return std::unexpected(std::move(ceiling.error()));
  if (!requested) return {};
  if (ceiling->ceiling()) {
    const auto ordering =
        domain::compare(requested->amount(), ceiling->ceiling()->amount());
    if (ordering == std::strong_ordering::greater) {
      return error(ChatSessionErrorCode::invalid_input,
                   "session spend ceiling cannot be widened");
    }
    if (ordering == std::strong_ordering::equal) return {};
  }

  const auto suffix = suffix_source();
  auto run_id = make_id<domain::RunId>("spend-policy", suffix);
  auto surface_id = make_id<domain::SurfaceId>("session-policy", suffix);
  auto workspace_id = make_id<domain::WorkspaceId>("chat", suffix);
  auto permission_id = make_id<domain::PermissionProfileId>("observe", suffix);
  if (!run_id || !surface_id || !workspace_id || !permission_id) {
    return error(ChatSessionErrorCode::internal_failure,
                 "session spend identity generation failed");
  }
  auto recorded = kernel.record_session_spend_ceiling(
      {*run_id,
       {*surface_id, *workspace_id, *permission_id, persona_id},
       *requested,
       domain::SessionSpendCeilingSource::command_line});
  if (!recorded) {
    return error(ChatSessionErrorCode::session_failed, recorded.error().message,
                 recorded.error().retryable);
  }
  return {};
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

[[nodiscard]] auto resolve_persona(persona::PersonaSource* source,
                                   const persona::PersonaLimits limits,
                                   const persona::PersonaDirective& directive,
                                   const domain::SessionEventLog& event_log,
                                   const std::stop_token stop_token,
                                   const bool allow_attention)
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
  if (*latest &&
      (*latest)->action == domain::PersonaSelectionAction::selected) {
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
      return PersonaSetup{std::nullopt, std::nullopt,
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
      return PersonaSetup{std::nullopt, std::nullopt,
                          "Persona changed since this session was recorded; "
                          "select it again or turn it off"};
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

} // namespace

struct ChatSession::Impl {
  domain::ModelId model_id;
  backend::ModelContextInfo model;
  backend::ModelContextProvider* model_context{};
  std::uint64_t output_tokens{};
  backend::GenerationOptions generation_options;
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
  storage::SessionStore* session_store{};
  runtime::MemoryController* memory_controller{};
  runtime::MemorySettings memory_settings{};
  std::optional<domain::RepositoryId> repository_id;
  std::string runtime_version;
  std::unique_ptr<runtime::RunKernel> kernel;
};

ChatSession::ChatSession(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {
}
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
    if (auto supported = backend::validate_generation_requirements(
            request.generation_options, *model);
        !supported) {
      return error(ChatSessionErrorCode::model_lookup_failed,
                   supported.error().redacted_message);
    }
    request.generation_options.max_output_tokens = output_tokens;

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
    auto persona_setup = resolve_persona(dependencies.persona_source,
                                         dependencies.persona_limits,
                                         request.persona, kernel->event_log(),
                                         stop_token, allow_persona_attention);
    if (!persona_setup) {
      return std::unexpected(std::move(persona_setup.error()));
    }
    const auto persona_id =
        persona_setup->document
            ? std::optional<domain::PersonaId>{persona_setup->document
                                                   ->reference.persona_id}
            : std::nullopt;
    if (auto applied = apply_requested_spend_ceiling(
            *kernel, request.session_spend_ceiling,
            dependencies.identity_suffix_source, persona_id);
        !applied) {
      return std::unexpected(std::move(applied.error()));
    }
    if (durable && dependencies.memory_controller != nullptr) {
      auto recovered = dependencies.memory_controller->capture_committed(
          kernel->event_log().session_id(), kernel->event_log().events(),
          dependencies.memory_settings, dependencies.repository_id,
          dependencies.runtime_version);
      if (!recovered) {
        return error(ChatSessionErrorCode::session_failed,
                     recovered.error().message, recovered.error().retryable);
      }
    }
    auto impl = std::make_unique<Impl>(
        Impl{request.model_id,
             *model,
             &model_context,
             output_tokens,
             std::move(request.generation_options),
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
             session_store,
             dependencies.memory_controller,
             dependencies.memory_settings,
             std::move(dependencies.repository_id),
             std::move(dependencies.runtime_version),
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
    auto ceiling = rebuild_spend_ceiling(m_impl->kernel->event_log());
    if (!ceiling) return std::unexpected(std::move(ceiling.error()));
    if (ceiling->ceiling()) {
      auto spend_state = rebuild_spend_state(m_impl->kernel->event_log());
      if (!spend_state) {
        return std::unexpected(std::move(spend_state.error()));
      }
      const auto spend = domain::summarize_session_spend(
          spend_state->ledger.records(), *spend_state->ceiling.ceiling());
      if (!spend.accounted) {
        return error(ChatSessionErrorCode::spend_accounting_unavailable,
                     "session spend accounting is unavailable; refusing "
                     "another inference");
      }
      if (spend.reached) {
        return error(ChatSessionErrorCode::spend_ceiling_reached,
                     "session spend ceiling reached (USD " +
                         spend.accounted->amount().to_string() + " of " +
                         spend.ceiling.amount().to_string() + ")");
      }
    }
    if (m_impl->persona_document) {
      if (m_impl->persona_source == nullptr) {
        m_impl->persona_attention = "Persona source is unavailable";
        return error(ChatSessionErrorCode::context_failed,
                     m_impl->persona_attention);
      }
      auto current = m_impl->persona_source->load(
          m_impl->persona_document->reference.name, m_impl->persona_limits,
          m_impl->stop_token);
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
    if (m_impl->memory_controller != nullptr) {
      std::uint64_t mandatory = detail::runtime_contract.size() + prompt.size();
      for (const auto& item : content)
        mandatory += item.estimated_tokens;
      if (m_impl->persona_document) {
        mandatory += m_impl->persona_document->text.size();
      }
      const auto maximum_input =
          m_impl->model.context_window_tokens - m_impl->output_tokens;
      const auto available = mandatory < maximum_input
                                 ? maximum_input - mandatory
                                 : std::uint64_t{};
      auto memory = runtime::select_memory_context(
          *m_impl->memory_controller,
          {m_impl->repository_id, m_impl->memory_settings.context_tokens,
           available});
      if (!memory) {
        return error(ChatSessionErrorCode::context_failed,
                     memory.error().message, memory.error().retryable);
      }
      for (auto& item : *memory) {
        item.order = static_cast<std::uint64_t>(content.size()) + 1;
        content.push_back(std::move(item));
      }
    }
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
    backend::BackendRequest backend_request{*inference_id,
                                            *assistant_message_id,
                                            m_impl->model_id,
                                            std::move(*context),
                                            {},
                                            m_impl->generation_options};
    auto started = m_impl->kernel->start(
        {*run_id,
         {*surface_id, *workspace_id, *permission_id,
          m_impl->persona_document
              ? std::optional<domain::PersonaId>{m_impl->persona_document
                                                     ->reference.persona_id}
              : std::nullopt},
         std::move(user_message),
         std::move(backend_request),
         m_impl->provenance,
         m_impl->next_persona_selection,
         m_impl->model.pricing_observation});
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
  if (m_impl->memory_controller != nullptr &&
      std::ranges::any_of(*drained, [](const auto& event) {
        return std::holds_alternative<domain::ToolResultRecorded>(
            event.payload);
      })) {
    auto captured = m_impl->memory_controller->capture_committed(
        m_impl->kernel->event_log().session_id(),
        m_impl->kernel->event_log().events(), m_impl->memory_settings,
        m_impl->repository_id, m_impl->runtime_version);
    if (!captured) {
      return error(ChatSessionErrorCode::session_failed,
                   captured.error().message, captured.error().retryable);
    }
  }
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
  auto listed =
      m_impl->persona_source->list(m_impl->persona_limits, m_impl->stop_token);
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
  m_impl->next_persona_selection =
      domain::PersonaSelection{domain::PersonaSelectionAction::selected,
                               domain::PersonaSelectionSource::interactive,
                               std::move(reference), std::move(previous)};
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
  m_impl->next_persona_selection =
      domain::PersonaSelection{domain::PersonaSelectionAction::disabled,
                               domain::PersonaSelectionSource::interactive,
                               std::nullopt, std::move(previous)};
  m_impl->persona_attention.clear();
  return {};
}

auto ChatSession::select_model(domain::ModelId model_id)
    -> std::expected<void, ChatSessionError> {
  if (active()) {
    return error(ChatSessionErrorCode::run_failed,
                 "finish or cancel the active run before selecting a model");
  }
  if (m_impl->model_context == nullptr) {
    return error(ChatSessionErrorCode::model_lookup_failed,
                 "model catalog is unavailable");
  }
  auto selected = m_impl->model_context->lookup(model_id, m_impl->stop_token);
  if (!selected) {
    return error(selected.error().kind == backend::BackendErrorKind::cancelled
                     ? ChatSessionErrorCode::cancelled
                     : ChatSessionErrorCode::model_lookup_failed,
                 selected.error().redacted_message, selected.error().retryable);
  }
  if (selected->model_id != model_id || selected->context_window_tokens == 0) {
    return error(ChatSessionErrorCode::model_lookup_failed,
                 "selected model context metadata is invalid");
  }
  auto output_tokens = m_impl->limits.preferred_output_tokens;
  if (selected->maximum_output_tokens)
    output_tokens = std::min(output_tokens, *selected->maximum_output_tokens);
  if (output_tokens == 0 || output_tokens >= selected->context_window_tokens) {
    return error(ChatSessionErrorCode::context_failed,
                 "selected model context capacity is too small");
  }
  if (auto supported = backend::validate_generation_requirements(
          m_impl->generation_options, *selected);
      !supported) {
    return error(ChatSessionErrorCode::model_lookup_failed,
                 supported.error().redacted_message);
  }
  m_impl->model_id = std::move(model_id);
  m_impl->model = std::move(*selected);
  m_impl->output_tokens = output_tokens;
  m_impl->generation_options.max_output_tokens = output_tokens;
  if (m_impl->provenance) m_impl->provenance->model_id = m_impl->model_id;
  return {};
}

auto ChatSession::persona_state() const -> ChatPersonaState {
  return {m_impl->persona_document
              ? std::optional<domain::PersonaReference>{m_impl->persona_document
                                                            ->reference}
              : std::nullopt,
          !m_impl->persona_attention.empty(), m_impl->persona_attention};
}

auto ChatSession::plan_task_state(
    std::optional<domain::RepositoryId> repository_id)
    -> std::expected<runtime::PlanTaskState, ChatSessionError> {
  runtime::PlanTaskController controller{*m_impl->kernel,
                                         m_impl->session_store};
  auto state = controller.inspect(std::move(repository_id));
  if (!state) {
    return error(ChatSessionErrorCode::session_failed, state.error().message,
                 state.error().retryable);
  }
  return std::move(*state);
}

auto ChatSession::decide_plan(const domain::RunId& run_id,
                              domain::PlanRevisionDecision decision,
                              runtime::PlanApprovalEnvironment environment)
    -> std::expected<runtime::PlanDecisionOutcome, ChatSessionError> {
  runtime::PlanTaskController controller{*m_impl->kernel,
                                         m_impl->session_store};
  auto result =
      controller.decide(run_id, std::move(decision), std::move(environment));
  if (!result) {
    return error(ChatSessionErrorCode::session_failed, result.error().message,
                 result.error().retryable);
  }
  return *result;
}

auto ChatSession::promote_project_task(runtime::ProjectTaskPromotion promotion)
    -> std::expected<void, ChatSessionError> {
  runtime::PlanTaskController controller{*m_impl->kernel,
                                         m_impl->session_store};
  auto result = controller.promote(std::move(promotion));
  if (!result) {
    return error(ChatSessionErrorCode::session_failed, result.error().message,
                 result.error().retryable);
  }
  return {};
}

auto ChatSession::update_project_task_status(
    runtime::ProjectTaskStatusUpdate update)
    -> std::expected<void, ChatSessionError> {
  runtime::PlanTaskController controller{*m_impl->kernel,
                                         m_impl->session_store};
  auto result = controller.set_backlog_status(std::move(update));
  if (!result) {
    return error(ChatSessionErrorCode::session_failed, result.error().message,
                 result.error().retryable);
  }
  return {};
}

auto ChatSession::memory_state(runtime::MemoryMutationTarget target)
    -> std::expected<runtime::MemoryState, ChatSessionError> {
  if (m_impl->memory_controller == nullptr) {
    return error(ChatSessionErrorCode::session_failed,
                 "durable memory is unavailable");
  }
  auto state = m_impl->memory_controller->inspect(std::move(target));
  if (!state) {
    return error(ChatSessionErrorCode::session_failed, state.error().message,
                 state.error().retryable);
  }
  return std::move(*state);
}

auto ChatSession::accept_memory(runtime::MemoryAcceptRequest request)
    -> std::expected<void, ChatSessionError> {
  if (m_impl->memory_controller == nullptr) {
    return error(ChatSessionErrorCode::session_failed,
                 "durable memory is unavailable");
  }
  auto accepted = m_impl->memory_controller->accept(std::move(request));
  if (!accepted) {
    return error(ChatSessionErrorCode::session_failed, accepted.error().message,
                 accepted.error().retryable);
  }
  return {};
}

auto ChatSession::reject_memory(runtime::MemoryRejectRequest request)
    -> std::expected<void, ChatSessionError> {
  if (m_impl->memory_controller == nullptr) {
    return error(ChatSessionErrorCode::session_failed,
                 "durable memory is unavailable");
  }
  auto rejected = m_impl->memory_controller->reject(std::move(request));
  if (!rejected) {
    return error(ChatSessionErrorCode::session_failed, rejected.error().message,
                 rejected.error().retryable);
  }
  return {};
}

auto ChatSession::expire_memory(runtime::MemoryExpireRequest request)
    -> std::expected<void, ChatSessionError> {
  if (m_impl->memory_controller == nullptr) {
    return error(ChatSessionErrorCode::session_failed,
                 "durable memory is unavailable");
  }
  auto expired = m_impl->memory_controller->expire(std::move(request));
  if (!expired) {
    return error(ChatSessionErrorCode::session_failed, expired.error().message,
                 expired.error().retryable);
  }
  return {};
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

} // namespace aiforge::surfaces
