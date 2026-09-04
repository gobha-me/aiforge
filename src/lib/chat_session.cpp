#include <aiforge/detail/sha256.hpp>
#include <aiforge/domain/usage_ledger.hpp>
#include <aiforge/runtime/context_builder.hpp>
#include <aiforge/runtime/persona.hpp>
#include <aiforge/runtime/tool_profiles.hpp>
#include <aiforge/runtime/user_global_instructions.hpp>
#include <aiforge/surfaces/chat_session.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <iterator>
#include <limits>
#include <span>
#include <string_view>
#include <type_traits>
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

[[nodiscard]] auto persona_editor_error(
    const persona::PersonaEditorError& value) -> ChatSessionError {
  return {value.code == persona::PersonaEditorErrorCode::cancelled
              ? ChatSessionErrorCode::cancelled
          : value.code == persona::PersonaEditorErrorCode::invalid_request ||
                  value.code == persona::PersonaEditorErrorCode::invalid_name ||
                  value.code ==
                      persona::PersonaEditorErrorCode::invalid_file_kind ||
                  value.code == persona::PersonaEditorErrorCode::malformed_text
              ? ChatSessionErrorCode::invalid_input
              : ChatSessionErrorCode::context_failed,
          value.message, value.retryable, value.may_have_applied};
}

[[nodiscard]] auto user_global_source_error(
    const instructions::UserGlobalInstructionError& value) -> ChatSessionError {
  return {value.code == instructions::UserGlobalInstructionErrorCode::cancelled
              ? ChatSessionErrorCode::cancelled
          : value.code ==
                  instructions::UserGlobalInstructionErrorCode::invalid_request
              ? ChatSessionErrorCode::invalid_input
              : ChatSessionErrorCode::context_failed,
          value.message, value.retryable};
}

[[nodiscard]] auto user_global_editor_error(
    const instructions::UserGlobalInstructionEditorError& value)
    -> ChatSessionError {
  return {value.code ==
                  instructions::UserGlobalInstructionEditorErrorCode::cancelled
              ? ChatSessionErrorCode::cancelled
          : value.code == instructions::UserGlobalInstructionEditorErrorCode::
                              invalid_request ||
                  value.code ==
                      instructions::UserGlobalInstructionEditorErrorCode::
                          malformed_text
              ? ChatSessionErrorCode::invalid_input
              : ChatSessionErrorCode::context_failed,
          value.message, value.retryable, value.may_have_applied};
}

[[nodiscard]] auto recorded_user_global_instruction(
    const domain::SessionEventLog& event_log, const domain::RunId& run_id)
    -> std::optional<domain::UserGlobalInstructionReference> {
  for (const auto& event : event_log.events()) {
    if (event.metadata.run_id != run_id) continue;
    if (const auto* recorded =
            std::get_if<domain::RunProvenanceRecorded>(&event.payload)) {
      return recorded->provenance.user_global_instruction;
    }
  }
  return std::nullopt;
}

[[nodiscard]] auto load_user_global_document(
    instructions::UserGlobalInstructionSource* source,
    const instructions::UserGlobalInstructionLimits limits,
    const std::stop_token stop_token)
    -> std::expected<std::optional<domain::UserGlobalInstructionDocument>,
                     ChatSessionError> {
  if (source == nullptr) return std::nullopt;
  auto loaded = source->load(limits, stop_token);
  if (!loaded) {
    return std::unexpected(user_global_source_error(loaded.error()));
  }
  if (*loaded) {
    if (!domain::validate_user_global_instruction_document(**loaded)) {
      return error(ChatSessionErrorCode::context_failed,
                   "user-global instruction document is invalid");
    }
    aiforge::detail::Sha256 digest;
    digest.update(std::as_bytes(
        std::span{(*loaded)->text.data(), (*loaded)->text.size()}));
    if (digest.finish() != (*loaded)->reference.content_digest.value) {
      return error(ChatSessionErrorCode::context_failed,
                   "user-global instruction content digest is invalid");
    }
  }
  return std::move(*loaded);
}

[[nodiscard]] auto append_user_global_instruction(
    domain::ContextBuildInput& input,
    const domain::UserGlobalInstructionDocument& document)
    -> std::expected<void, ChatSessionError> {
  auto instruction = runtime::user_global_instruction_input(
      document, static_cast<std::uint64_t>(document.text.size()));
  if (!instruction) {
    return error(ChatSessionErrorCode::context_failed,
                 instruction.error().message);
  }
  input.instructions.push_back(std::move(*instruction));
  return {};
}

[[nodiscard]] auto model_tool_calling_support(
    const backend::ModelContextInfo& model) -> std::optional<bool> {
  const auto found = model.capabilities.find("tools");
  return found == model.capabilities.end() ? std::nullopt : found->second;
}

[[nodiscard]] auto profile_error(const runtime::ToolProfileError& value)
    -> ChatSessionError {
  return {
      value.code == runtime::ToolProfileErrorCode::unknown_profile ||
              value.code == runtime::ToolProfileErrorCode::invalid_profile ||
              value.code == runtime::ToolProfileErrorCode::duplicate_profile ||
              value.code == runtime::ToolProfileErrorCode::duplicate_tool
          ? ChatSessionErrorCode::invalid_input
          : ChatSessionErrorCode::internal_failure,
      value.message};
}

[[nodiscard]] auto resolve_profile(const runtime::ToolRegistrySnapshot& tools,
                                   runtime::ToolProfileSelection selection,
                                   const runtime::ToolPolicy& tool_policy)
    -> std::expected<runtime::ToolProfileResolution, ChatSessionError> {
  auto resolved =
      runtime::resolve_tool_profile(tools, std::move(selection), tool_policy);
  if (!resolved) return std::unexpected(profile_error(resolved.error()));
  return std::move(*resolved);
}

template <typename Id>
[[nodiscard]] auto profile_maximum(
    const std::map<Id, domain::ToolProfileId>& maximums, const Id& id)
    -> std::optional<domain::ToolProfileId> {
  const auto found = maximums.find(id);
  return found == maximums.end()
             ? std::nullopt
             : std::optional<domain::ToolProfileId>{found->second};
}

[[nodiscard]] auto tool_declaration_tokens(
    const std::vector<backend::ToolDeclaration>& declarations)
    -> std::expected<std::uint64_t, ChatSessionError> {
  std::uint64_t bytes{};
  const auto add = [&bytes](const std::size_t amount) {
    if (amount > std::numeric_limits<std::uint64_t>::max() - bytes)
      return false;
    bytes += amount;
    return true;
  };
  for (const auto& declaration : declarations) {
    if (!add(declaration.name.size()) || !add(declaration.description.size()) ||
        !add(declaration.input_schema.data.size()) ||
        !add(declaration.effects.size())) {
      return error(ChatSessionErrorCode::context_failed,
                   "tool declarations exceed the context accounting limit");
    }
    for (const auto& scope : declaration.capability_scopes) {
      if (!add(scope.kind.size()) || !add(scope.value.size())) {
        return error(ChatSessionErrorCode::context_failed,
                     "tool declarations exceed the context accounting limit");
      }
    }
  }
  if (bytes == 0) return std::uint64_t{};
  if (bytes > std::numeric_limits<std::uint64_t>::max() - 3U) {
    return error(ChatSessionErrorCode::context_failed,
                 "tool declarations exceed the context accounting limit");
  }
  return (bytes + 3U) / 4U;
}

// clang-format off
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Exhaustively accounts for every message content variant.
[[nodiscard]] auto estimated_message_tokens(const domain::Message& message)
    -> std::expected<std::uint64_t, ChatSessionError> {
  // clang-format on
  std::uint64_t total{};
  const auto add = [&](const std::size_t size) -> bool {
    if (size > std::numeric_limits<std::uint64_t>::max() - total) return false;
    total += static_cast<std::uint64_t>(size);
    return true;
  };
  for (const auto& block : message.content) {
    const auto admitted = std::visit(
        [&](const auto& value) -> bool {
          using Value = std::remove_cvref_t<decltype(value)>;
          if constexpr (std::same_as<Value, domain::TextBlock>) {
            return add(value.text.size());
          } else if constexpr (std::same_as<Value,
                                            domain::StructuredDataBlock>) {
            return add(value.media_type.size()) && add(value.data.size());
          } else if constexpr (std::same_as<Value, domain::CitationBlock>) {
            return add(value.uri.size()) &&
                   (!value.title || add(value.title->size()));
          } else if constexpr (std::same_as<Value,
                                            domain::ArtifactReferenceBlock>) {
            return add(value.artifact_id.value().size()) &&
                   (!value.label || add(value.label->size()));
          } else {
            return false;
          }
        },
        block);
    if (!admitted) {
      return error(ChatSessionErrorCode::context_failed,
                   "tool result cannot enter interactive context");
    }
  }
  for (const auto& call : message.tool_calls) {
    if (!add(call.invocation_id.value().size()) ||
        !add(call.tool_name.size()) || !add(call.arguments.media_type.size()) ||
        !add(call.arguments.data.size())) {
      return error(ChatSessionErrorCode::context_failed,
                   "tool call cannot enter interactive context");
    }
  }
  return std::max<std::uint64_t>(total, 1);
}

[[nodiscard]] auto assistant_continuation_state(
    const std::span<const domain::RunEvent> events,
    const domain::ConstructedContext& context)
    -> std::expected<std::vector<backend::AssistantContinuationState>,
                     ChatSessionError> {
  struct AssistantInference {
    domain::InferenceId inference_id;
    domain::MessageId message_id;
  };
  std::vector<AssistantInference> assistants;
  std::vector<backend::AssistantContinuationState> result;
  for (const auto& event : events) {
    if (const auto* started =
            std::get_if<domain::AssistantContentStarted>(&event.payload)) {
      assistants.push_back({started->inference_id, started->message_id});
      continue;
    }
    const auto* reasoning =
        std::get_if<domain::ReasoningMetadataAdded>(&event.payload);
    if (reasoning == nullptr) continue;
    const auto assistant = std::ranges::find(
        assistants, reasoning->inference_id, &AssistantInference::inference_id);
    if (assistant == assistants.end()) {
      return error(ChatSessionErrorCode::run_failed,
                   "reasoning continuation has no assistant message");
    }
    const auto admitted = std::ranges::find_if(
        context.entries, [&](const domain::ContextEntry& entry) {
          return entry.message.message_id == assistant->message_id;
        });
    if (admitted == context.entries.end()) continue;
    auto state =
        std::ranges::find(result, assistant->message_id,
                          &backend::AssistantContinuationState::message_id);
    if (state == result.end()) {
      result.push_back(
          {assistant->message_id, std::nullopt, domain::Metadata{}});
      state = std::prev(result.end());
    }
    if (reasoning->text) {
      if (!state->reasoning_text) state->reasoning_text.emplace();
      state->reasoning_text->append(*reasoning->text);
    }
    state->metadata.insert(state->metadata.end(), reasoning->metadata.begin(),
                           reasoning->metadata.end());
  }
  return result;
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
  runtime::ToolRegistrySnapshot available_tools;
  domain::ToolProfileId tool_profile_id;
  std::optional<std::vector<std::string>> desired_tool_names;
  std::map<domain::ModelId, domain::ToolProfileId> model_tool_profile_maximums;
  std::map<domain::PersonaId, domain::ToolProfileId>
      persona_tool_profile_maximums;
  std::shared_ptr<runtime::ToolPolicy> tool_policy;
  std::optional<domain::PermissionProfileId> permission_profile_id;
  ChatSessionLimits limits;
  bool is_durable{};
  ChatIdentitySuffixSource identity_suffix_source;
  std::optional<domain::RunProvenance> provenance;
  persona::PersonaSource* persona_source{};
  persona::PersonaEditor* persona_editor{};
  persona::PersonaLimits persona_limits{};
  instructions::UserGlobalInstructionSource* user_global_instruction_source{};
  instructions::UserGlobalInstructionEditor* user_global_instruction_editor{};
  instructions::UserGlobalInstructionLimits user_global_instruction_limits{};
  bool user_global_instructions_enabled{};
  std::stop_token stop_token;
  std::optional<domain::PersonaDocument> persona_document;
  std::optional<domain::PersonaSelection> next_persona_selection;
  std::string persona_attention;
  storage::SessionStore* session_store{};
  runtime::MemoryController* memory_controller{};
  runtime::MemorySettings memory_settings{};
  std::optional<domain::RepositoryId> repository_id;
  std::string runtime_version;
  std::optional<domain::ContextBuildInput> active_context;
  bool recovered_pending_run_validation_required{};
  std::optional<domain::UserGlobalInstructionDocument>
      recovered_user_global_instruction;
  std::vector<domain::RunEvent> pending_surface_events;
  std::unique_ptr<runtime::RunKernel> kernel;
  std::uint64_t tool_profile_revision{};

  [[nodiscard]] auto tool_selection() const -> runtime::ToolProfileSelection {
    return {tool_profile_id, desired_tool_names,
            profile_maximum(model_tool_profile_maximums, model_id),
            persona_document
                ? profile_maximum(persona_tool_profile_maximums,
                                  persona_document->reference.persona_id)
                : std::nullopt,
            model_tool_calling_support(model)};
  }
};

ChatSession::ChatSession(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {
}
ChatSession::~ChatSession() = default;

auto ChatSession::validate_recovered_pending_run()
    -> std::expected<void, ChatSessionError> {
  if (!m_impl->recovered_pending_run_validation_required) return {};
  const auto run_id = m_impl->kernel->active_run_id();
  if (!run_id) {
    m_impl->recovered_pending_run_validation_required = false;
    m_impl->recovered_user_global_instruction.reset();
    return {};
  }
  const auto expected =
      recorded_user_global_instruction(m_impl->kernel->event_log(), *run_id);
  if (!expected) {
    m_impl->recovered_pending_run_validation_required = false;
    m_impl->recovered_user_global_instruction.reset();
    return {};
  }
  if (m_impl->user_global_instruction_source == nullptr) {
    return error(ChatSessionErrorCode::context_failed,
                 "recorded user-global instruction source is unavailable");
  }
  auto loaded = load_user_global_document(
      m_impl->user_global_instruction_source,
      m_impl->user_global_instruction_limits, m_impl->stop_token);
  if (!loaded) return std::unexpected(std::move(loaded.error()));
  if (!*loaded || (*loaded)->reference != *expected) {
    return error(ChatSessionErrorCode::context_failed,
                 "user-global instruction changed since this run started");
  }
  m_impl->recovered_user_global_instruction = std::move(**loaded);
  m_impl->recovered_pending_run_validation_required = false;
  return {};
}

// clang-format off
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Explicit durable checks.
auto ChatSession::open(ChatSessionOpen request, backend::Backend& backend,
                       backend::ModelContextProvider& model_context,
                       storage::SessionStore* session_store,
                       runtime::RunWakeSink* wake_sink,
                       const std::stop_token stop_token,
                       const ChatSessionLimits limits,
                       ChatSessionDependencies dependencies)
    -> std::expected<std::unique_ptr<ChatSession>, ChatSessionError> {
  // clang-format on
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
    if (request.provenance) {
      if (auto exact = backend::validate_effective_request_options(
              request.generation_options,
              request.provenance->effective_request_options);
          !exact) {
        return error(ChatSessionErrorCode::invalid_input,
                     exact.error().redacted_message);
      }
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

    auto available_tools = dependencies.tools;
    auto initial_tool_profile =
        domain::ToolProfileId::from(std::string{"essentials"});
    if (!initial_tool_profile) {
      return error(ChatSessionErrorCode::internal_failure,
                   "default tool profile identity is invalid");
    }
    for (const auto& [model_id, maximum] :
         dependencies.model_tool_profile_maximums) {
      static_cast<void>(model_id);
      if (auto resolved =
              runtime::resolve_tool_profile(available_tools, maximum, true);
          !resolved) {
        return std::unexpected(profile_error(resolved.error()));
      }
    }
    for (const auto& [persona_id, maximum] :
         dependencies.persona_tool_profile_maximums) {
      static_cast<void>(persona_id);
      if (auto resolved =
              runtime::resolve_tool_profile(available_tools, maximum, true);
          !resolved) {
        return std::unexpected(profile_error(resolved.error()));
      }
    }
    auto tool_policy = dependencies.tool_policy
                           ? dependencies.tool_policy
                           : runtime::default_tool_policy();
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
          std::move(dependencies.tools), tool_policy);
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
          std::move(dependencies.tools), tool_policy);
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
    const auto initial_persona_id =
        persona_setup->document
            ? std::optional<domain::PersonaId>{persona_setup->document
                                                   ->reference.persona_id}
            : std::nullopt;
    auto initial_selection = runtime::ToolProfileSelection{
        *initial_tool_profile, std::nullopt,
        profile_maximum(dependencies.model_tool_profile_maximums,
                        request.model_id),
        initial_persona_id
            ? profile_maximum(dependencies.persona_tool_profile_maximums,
                              *initial_persona_id)
            : std::nullopt,
        model_tool_calling_support(*model)};
    if (auto resolved =
            resolve_profile(available_tools, initial_selection, *tool_policy);
        !resolved) {
      return std::unexpected(std::move(resolved.error()));
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
    const bool recovered_pending_run =
        durable && request.mode != ChatSessionOpen::Mode::create &&
        kernel->active_run_id().has_value();
    auto impl = std::make_unique<Impl>(
        Impl{request.model_id,
             *model,
             &model_context,
             output_tokens,
             std::move(request.generation_options),
             std::move(available_tools),
             std::move(*initial_tool_profile),
             std::nullopt,
             std::move(dependencies.model_tool_profile_maximums),
             std::move(dependencies.persona_tool_profile_maximums),
             std::move(tool_policy),
             std::move(dependencies.permission_profile_id),
             limits,
             durable,
             std::move(dependencies.identity_suffix_source),
             std::move(request.provenance),
             dependencies.persona_source,
             dependencies.persona_editor,
             dependencies.persona_limits,
             dependencies.user_global_instruction_source,
             dependencies.user_global_instruction_editor,
             dependencies.user_global_instruction_limits,
             dependencies.user_global_instructions_enabled,
             stop_token,
             std::move(persona_setup->document),
             std::move(persona_setup->next_selection),
             std::move(persona_setup->attention),
             session_store,
             dependencies.memory_controller,
             dependencies.memory_settings,
             std::move(dependencies.repository_id),
             std::move(dependencies.runtime_version),
             std::nullopt,
             recovered_pending_run,
             std::nullopt,
             {},
             std::move(kernel),
             0});
    return std::unique_ptr<ChatSession>{new ChatSession{std::move(impl)}};
  } catch (...) {
    return error(ChatSessionErrorCode::internal_failure,
                 "interactive session setup failed internally");
  }
}

// clang-format off
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Explicitly stages interactive run admission and durable startup.
auto ChatSession::submit(std::string prompt)
    -> std::expected<ChatSubmission, ChatSessionError> {
  // clang-format on
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
    auto tool_profile =
        resolve_profile(m_impl->available_tools, m_impl->tool_selection(),
                        *m_impl->tool_policy);
    if (!tool_profile) {
      return std::unexpected(std::move(tool_profile.error()));
    }
    auto declaration_tokens =
        tool_declaration_tokens(tool_profile->effective_tools.declarations());
    if (!declaration_tokens) {
      return std::unexpected(std::move(declaration_tokens.error()));
    }
    if (!m_impl->persona_attention.empty()) {
      return error(ChatSessionErrorCode::context_failed,
                   m_impl->persona_attention);
    }
    std::optional<domain::UserGlobalInstructionDocument>
        user_global_instruction;
    if (m_impl->user_global_instructions_enabled) {
      auto loaded = load_user_global_document(
          m_impl->user_global_instruction_source,
          m_impl->user_global_instruction_limits, m_impl->stop_token);
      if (!loaded) return std::unexpected(std::move(loaded.error()));
      user_global_instruction = std::move(*loaded);
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
    auto permission_id = m_impl->permission_profile_id;
    if (!permission_id) {
      auto generated_permission_id =
          make_id<domain::PermissionProfileId>("observe", suffix);
      if (!generated_permission_id) {
        return error(ChatSessionErrorCode::internal_failure,
                     "interactive permission identity generation failed");
      }
      permission_id = std::move(*generated_permission_id);
    }
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
      std::uint64_t mandatory{};
      const auto add_mandatory = [&](const std::uint64_t amount) {
        if (amount > std::numeric_limits<std::uint64_t>::max() - mandatory) {
          return false;
        }
        mandatory += amount;
        return true;
      };
      bool bounded = add_mandatory(detail::runtime_contract.size()) &&
                     add_mandatory(prompt.size()) &&
                     add_mandatory(*declaration_tokens);
      for (const auto& item : content) {
        bounded = bounded && add_mandatory(item.estimated_tokens);
      }
      if (m_impl->persona_document) {
        bounded =
            bounded && add_mandatory(m_impl->persona_document->text.size());
      }
      if (user_global_instruction) {
        bounded =
            bounded && add_mandatory(user_global_instruction->text.size());
      }
      if (!bounded) {
        return error(ChatSessionErrorCode::context_failed,
                     "required context accounting overflowed");
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
        {m_impl->model.context_window_tokens, m_impl->output_tokens,
         *declaration_tokens},
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
    if (user_global_instruction) {
      if (auto appended =
              append_user_global_instruction(input, *user_global_instruction);
          !appended) {
        return std::unexpected(std::move(appended.error()));
      }
    }
    auto continuation_context = input;
    auto context = runtime::ContextBuilder{}.build(std::move(input));
    if (!context) {
      return error(ChatSessionErrorCode::context_failed,
                   "prompt exceeds model context capacity");
    }

    const auto before = m_impl->kernel->event_log().events().size();
    auto provenance = m_impl->provenance;
    if (provenance) {
      provenance->user_global_instruction =
          user_global_instruction
              ? std::optional<
                    domain::
                        UserGlobalInstructionReference>{user_global_instruction
                                                            ->reference}
              : std::nullopt;
      provenance->tool_profile = domain::ToolProfileProvenance{
          tool_profile->selection.selected_profile_id,
          tool_profile->selection.model_maximum_profile_id,
          tool_profile->selection.persona_maximum_profile_id,
          tool_profile->selection.desired_tool_names.value_or(
              tool_profile->selected_profile.tool_names)};
    } else if (user_global_instruction) {
      return error(ChatSessionErrorCode::context_failed,
                   "user-global instruction provenance is unavailable");
    }
    backend::BackendRequest backend_request{
        *inference_id,
        *assistant_message_id,
        m_impl->model_id,
        std::move(*context),
        tool_profile->effective_tools.declarations(),
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
         std::move(provenance),
         m_impl->next_persona_selection,
         m_impl->model.pricing_observation});
    if (!started) return std::unexpected(kernel_error(started.error()));
    m_impl->active_context = std::move(continuation_context);
    ++m_impl->tool_profile_revision;

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

// clang-format off
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Explicitly rebuilds the exact same-run continuation contract.
auto ChatSession::continue_if_ready()
    -> std::expected<std::vector<domain::RunEvent>, ChatSessionError> {
  // clang-format on
  const auto run_id = m_impl->kernel->active_run_id();
  if (!run_id || m_impl->kernel->active_inference_id() ||
      m_impl->kernel->pending_tool_approval() ||
      m_impl->kernel->pending_question_input()) {
    return std::vector<domain::RunEvent>{};
  }
  if (auto validated = validate_recovered_pending_run(); !validated) {
    return std::unexpected(std::move(validated.error()));
  }

  const auto* active_tools = m_impl->kernel->active_tool_declarations();
  if (active_tools == nullptr) {
    return error(ChatSessionErrorCode::run_failed,
                 "active run tool declarations are unavailable");
  }
  auto declaration_tokens = tool_declaration_tokens(*active_tools);
  if (!declaration_tokens) {
    return std::unexpected(std::move(declaration_tokens.error()));
  }

  const auto suffix = m_impl->identity_suffix_source();
  domain::ContextBuildInput base;
  if (m_impl->active_context) {
    base = *m_impl->active_context;
  } else {
    auto history =
        detail::replayed_conversation(m_impl->kernel->event_log(), suffix);
    if (!history) {
      return error(ChatSessionErrorCode::session_failed,
                   std::move(history.error()));
    }
    auto runtime_message_id = make_id<domain::MessageId>("runtime", suffix);
    auto runtime_entry_id =
        make_id<domain::ContextEntryId>("runtime-entry", suffix);
    auto runtime_source_id =
        make_id<domain::ContextSourceId>("runtime-source", suffix);
    if (!runtime_message_id || !runtime_entry_id || !runtime_source_id) {
      return error(ChatSessionErrorCode::internal_failure,
                   "interactive continuation identity generation failed");
    }
    base = domain::ContextBuildInput{
        {m_impl->model.context_window_tokens, m_impl->output_tokens,
         *declaration_tokens},
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
        std::move(*history)};
    if (m_impl->persona_document) {
      auto persona_instruction = runtime::persona_instruction_input(
          *m_impl->persona_document, m_impl->persona_document->text.size());
      if (!persona_instruction) {
        return error(ChatSessionErrorCode::context_failed,
                     persona_instruction.error().message);
      }
      base.instructions.push_back(std::move(*persona_instruction));
    }
    const auto recorded_instruction =
        recorded_user_global_instruction(m_impl->kernel->event_log(), *run_id);
    if (recorded_instruction) {
      std::optional<domain::UserGlobalInstructionDocument> loaded;
      if (m_impl->recovered_user_global_instruction) {
        loaded = m_impl->recovered_user_global_instruction;
      } else {
        if (m_impl->user_global_instruction_source == nullptr) {
          return error(
              ChatSessionErrorCode::context_failed,
              "recorded user-global instruction source is unavailable");
        }
        auto current = load_user_global_document(
            m_impl->user_global_instruction_source,
            m_impl->user_global_instruction_limits, m_impl->stop_token);
        if (!current) return std::unexpected(std::move(current.error()));
        loaded = std::move(*current);
      }
      if (!loaded || loaded->reference != *recorded_instruction) {
        return error(ChatSessionErrorCode::context_failed,
                     "user-global instruction changed since this run started");
      }
      if (auto appended = append_user_global_instruction(base, *loaded);
          !appended) {
        return std::unexpected(std::move(appended.error()));
      }
    }
    m_impl->active_context = base;
  }

  std::vector<domain::RunEvent> run_events;
  for (const auto& event : m_impl->kernel->event_log().events()) {
    if (event.metadata.run_id == *run_id) run_events.push_back(event);
  }
  auto tool_messages = runtime::tool_continuation_messages(run_events);
  if (!tool_messages) {
    return error(ChatSessionErrorCode::run_failed,
                 tool_messages.error().message,
                 tool_messages.error().retryable);
  }
  if (tool_messages->empty()) return std::vector<domain::RunEvent>{};

  for (auto& message : *tool_messages) {
    const auto message_suffix = m_impl->identity_suffix_source();
    auto entry_id =
        make_id<domain::ContextEntryId>("tool-result-entry", message_suffix);
    auto source_id =
        make_id<domain::ContextSourceId>("tool-result-source", message_suffix);
    auto estimated = estimated_message_tokens(message);
    if (!entry_id || !source_id || !estimated) {
      return error(ChatSessionErrorCode::context_failed,
                   "tool result context could not be built");
    }
    base.content.push_back(
        {*entry_id,
         message.role == domain::Role::assistant
             ? domain::ContextContentKind::conversation
             : domain::ContextContentKind::tool_result,
         std::move(message),
         {*source_id, std::string{"interactive:tool-continuation"},
          std::nullopt},
         static_cast<std::uint64_t>(base.content.size()) + 1,
         *estimated});
  }
  auto context = runtime::ContextBuilder{}.build(std::move(base));
  if (!context) {
    return error(ChatSessionErrorCode::context_failed,
                 "tool results exceed model context capacity");
  }
  auto continuation_state = assistant_continuation_state(run_events, *context);
  if (!continuation_state) {
    return std::unexpected(std::move(continuation_state.error()));
  }
  const auto identity_suffix = m_impl->identity_suffix_source();
  auto inference_id =
      make_id<domain::InferenceId>("inference", identity_suffix);
  auto assistant_message_id =
      make_id<domain::MessageId>("assistant", identity_suffix);
  if (!inference_id || !assistant_message_id) {
    return error(ChatSessionErrorCode::internal_failure,
                 "interactive continuation identity generation failed");
  }

  const auto before = m_impl->kernel->event_log().events().size();
  auto continued = m_impl->kernel->continue_run(
      *run_id,
      {*inference_id, *assistant_message_id, m_impl->model_id,
       std::move(*context), *active_tools, m_impl->generation_options,
       std::move(*continuation_state)},
      m_impl->model.pricing_observation);
  if (!continued) {
    if (continued.error().code ==
        runtime::RunKernelErrorCode::continuation_not_ready) {
      return std::vector<domain::RunEvent>{};
    }
    return std::unexpected(kernel_error(continued.error()));
  }
  const auto events = m_impl->kernel->event_log().events();
  std::vector<domain::RunEvent> committed;
  committed.reserve(events.size() - before);
  for (std::size_t index = before; index < events.size(); ++index) {
    committed.push_back(events[index]);
  }
  return committed;
}

auto ChatSession::drain()
    -> std::expected<std::vector<domain::RunEvent>, ChatSessionError> {
  auto drained = m_impl->kernel->drain();
  if (!drained) return std::unexpected(kernel_error(drained.error()));
  auto result = std::move(m_impl->pending_surface_events);
  m_impl->pending_surface_events.clear();
  result.insert(result.end(), std::make_move_iterator(drained->begin()),
                std::make_move_iterator(drained->end()));
  if (m_impl->memory_controller != nullptr &&
      std::ranges::any_of(result, [](const auto& event) {
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
  auto continued = continue_if_ready();
  if (!continued) {
    m_impl->pending_surface_events = std::move(result);
    return std::unexpected(std::move(continued.error()));
  }
  result.insert(result.end(), std::make_move_iterator(continued->begin()),
                std::make_move_iterator(continued->end()));
  if (!m_impl->kernel->active_run_id()) {
    m_impl->active_context.reset();
    m_impl->recovered_pending_run_validation_required = false;
    m_impl->recovered_user_global_instruction.reset();
  }
  return result;
}

auto ChatSession::cancel_active(std::optional<std::string> reason)
    -> std::expected<void, ChatSessionError> {
  const auto run = m_impl->kernel->active_run_id();
  if (!run) return {};
  const auto before = m_impl->kernel->event_log().events().size();
  auto cancelled = m_impl->kernel->cancel_run(*run, std::move(reason));
  if (!cancelled) return std::unexpected(kernel_error(cancelled.error()));
  m_impl->active_context.reset();
  m_impl->recovered_pending_run_validation_required = false;
  m_impl->recovered_user_global_instruction.reset();
  const auto events = m_impl->kernel->event_log().events();
  for (std::size_t index = before; index < events.size(); ++index) {
    m_impl->pending_surface_events.push_back(events[index]);
  }
  return {};
}

auto ChatSession::pending_question_input() const
    -> std::optional<runtime::PendingQuestionInput> {
  return m_impl->kernel->pending_question_input();
}

auto ChatSession::pending_tool_approval() const
    -> std::optional<runtime::PendingToolApproval> {
  return m_impl->kernel->pending_tool_approval();
}

auto ChatSession::decide_tool_approval(
    const domain::RunId& run_id, const domain::InvocationId& invocation_id,
    runtime::ToolApprovalResolution resolution)
    -> std::expected<void, ChatSessionError> {
  if (auto validated = validate_recovered_pending_run(); !validated) {
    return std::unexpected(std::move(validated.error()));
  }
  const auto before = m_impl->kernel->event_log().events().size();
  auto decided = m_impl->kernel->decide_approval(run_id, invocation_id,
                                                 std::move(resolution));
  if (!decided) return std::unexpected(kernel_error(decided.error()));
  const auto events = m_impl->kernel->event_log().events();
  for (std::size_t index = before; index < events.size(); ++index) {
    m_impl->pending_surface_events.push_back(events[index]);
  }
  return {};
}

auto ChatSession::answer_questions(const domain::RunId& run_id,
                                   const domain::InvocationId& invocation_id,
                                   std::vector<domain::QuestionAnswer> answers)
    -> std::expected<void, ChatSessionError> {
  if (auto validated = validate_recovered_pending_run(); !validated) {
    return std::unexpected(std::move(validated.error()));
  }
  const auto before = m_impl->kernel->event_log().events().size();
  auto answered = m_impl->kernel->answer_questions(run_id, invocation_id,
                                                   std::move(answers));
  if (!answered) return std::unexpected(kernel_error(answered.error()));
  const auto events = m_impl->kernel->event_log().events();
  for (std::size_t index = before; index < events.size(); ++index) {
    m_impl->pending_surface_events.push_back(events[index]);
  }
  return {};
}

auto ChatSession::cancel_questions(const domain::RunId& run_id,
                                   const domain::InvocationId& invocation_id,
                                   std::optional<std::string> reason)
    -> std::expected<void, ChatSessionError> {
  if (auto validated = validate_recovered_pending_run(); !validated) {
    return std::unexpected(std::move(validated.error()));
  }
  const auto before = m_impl->kernel->event_log().events().size();
  auto cancelled = m_impl->kernel->cancel_questions(run_id, invocation_id,
                                                    std::move(reason));
  if (!cancelled) return std::unexpected(kernel_error(cancelled.error()));
  const auto events = m_impl->kernel->event_log().events();
  for (std::size_t index = before; index < events.size(); ++index) {
    m_impl->pending_surface_events.push_back(events[index]);
  }
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

auto ChatSession::load_persona(std::string name)
    -> std::expected<domain::PersonaDocument, ChatSessionError> {
  if (active()) {
    return error(ChatSessionErrorCode::run_failed,
                 "finish or cancel the active run before editing a persona");
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
  return std::move(*loaded);
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
  auto candidate_selection = m_impl->tool_selection();
  candidate_selection.persona_maximum_profile_id = profile_maximum(
      m_impl->persona_tool_profile_maximums, loaded->reference.persona_id);
  if (auto profile =
          resolve_profile(m_impl->available_tools,
                          std::move(candidate_selection), *m_impl->tool_policy);
      !profile) {
    return std::unexpected(std::move(profile.error()));
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
  ++m_impl->tool_profile_revision;
  return {};
}

auto ChatSession::disable_persona() -> std::expected<void, ChatSessionError> {
  if (active()) {
    return error(ChatSessionErrorCode::run_failed,
                 "finish or cancel the active run before disabling a persona");
  }
  auto candidate_selection = m_impl->tool_selection();
  candidate_selection.persona_maximum_profile_id.reset();
  if (auto profile =
          resolve_profile(m_impl->available_tools,
                          std::move(candidate_selection), *m_impl->tool_policy);
      !profile) {
    return std::unexpected(std::move(profile.error()));
  }
  std::optional<domain::PersonaReference> previous;
  if (m_impl->persona_document) previous = m_impl->persona_document->reference;
  m_impl->persona_document.reset();
  m_impl->next_persona_selection =
      domain::PersonaSelection{domain::PersonaSelectionAction::disabled,
                               domain::PersonaSelectionSource::interactive,
                               std::nullopt, std::move(previous)};
  m_impl->persona_attention.clear();
  ++m_impl->tool_profile_revision;
  return {};
}

auto ChatSession::create_persona(persona::PersonaDraft draft)
    -> std::expected<persona::PersonaWriteReceipt, ChatSessionError> {
  if (active()) {
    return error(ChatSessionErrorCode::run_failed,
                 "finish or cancel the active run before creating a persona");
  }
  if (m_impl->persona_editor == nullptr) {
    return error(ChatSessionErrorCode::context_failed,
                 "persona editor is unavailable");
  }
  persona::PersonaCreate request{std::move(draft), m_impl->persona_limits};
  auto written = m_impl->persona_editor->create(request, m_impl->stop_token);
  if (!written) return std::unexpected(persona_editor_error(written.error()));
  if (auto valid = persona::validate_persona_write_receipt(request, *written);
      !valid) {
    return std::unexpected(persona_editor_error(valid.error()));
  }
  return std::move(*written);
}

auto ChatSession::replace_persona(domain::PersonaReference expected,
                                  std::string text)
    -> std::expected<persona::PersonaWriteReceipt, ChatSessionError> {
  if (active()) {
    return error(ChatSessionErrorCode::run_failed,
                 "finish or cancel the active run before editing a persona");
  }
  if (m_impl->persona_editor == nullptr) {
    return error(ChatSessionErrorCode::context_failed,
                 "persona editor is unavailable");
  }
  const bool selected =
      m_impl->persona_document &&
      m_impl->persona_document->reference.persona_id == expected.persona_id;
  persona::PersonaReplace request{std::move(expected), std::move(text),
                                  m_impl->persona_limits};
  auto written = m_impl->persona_editor->replace(request, m_impl->stop_token);
  if (!written) {
    const bool observed_changed =
        written.error()
            .observed
            .transform([&request](const auto& observed) {
              return observed != request.expected;
            })
            .value_or(false);
    if (selected && (written.error().may_have_applied || observed_changed)) {
      m_impl->persona_attention =
          "Persona content may have changed during editing; select it again "
          "or turn it off";
    }
    return std::unexpected(persona_editor_error(written.error()));
  }
  if (auto valid = persona::validate_persona_write_receipt(request, *written);
      !valid) {
    if (selected) {
      m_impl->persona_attention = "Persona edit returned an invalid result; "
                                  "select it again or turn it off";
    }
    return std::unexpected(persona_editor_error(valid.error()));
  }
  if (selected) {
    m_impl->persona_attention =
        "Persona changed in the manager; select it again or turn it off";
  }
  return std::move(*written);
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
  auto candidate_selection = m_impl->tool_selection();
  candidate_selection.model_maximum_profile_id =
      profile_maximum(m_impl->model_tool_profile_maximums, model_id);
  candidate_selection.model_tool_calling_support =
      model_tool_calling_support(*selected);
  if (auto profile =
          resolve_profile(m_impl->available_tools,
                          std::move(candidate_selection), *m_impl->tool_policy);
      !profile) {
    return std::unexpected(std::move(profile.error()));
  }
  m_impl->model_id = std::move(model_id);
  m_impl->model = std::move(*selected);
  m_impl->output_tokens = output_tokens;
  m_impl->generation_options.max_output_tokens = output_tokens;
  if (m_impl->provenance) m_impl->provenance->model_id = m_impl->model_id;
  ++m_impl->tool_profile_revision;
  return {};
}

auto ChatSession::select_tool_profile(domain::ToolProfileId profile_id)
    -> std::expected<void, ChatSessionError> {
  if (active()) {
    return error(ChatSessionErrorCode::run_failed,
                 "finish or cancel the active run before selecting tools");
  }
  auto resolved = resolve_profile(
      m_impl->available_tools,
      runtime::ToolProfileSelection{
          profile_id, std::nullopt,
          profile_maximum(m_impl->model_tool_profile_maximums,
                          m_impl->model_id),
          m_impl->persona_document
              ? profile_maximum(m_impl->persona_tool_profile_maximums,
                                m_impl->persona_document->reference.persona_id)
              : std::nullopt,
          model_tool_calling_support(m_impl->model)},
      *m_impl->tool_policy);
  if (!resolved) return std::unexpected(std::move(resolved.error()));
  m_impl->tool_profile_id = std::move(profile_id);
  m_impl->desired_tool_names.reset();
  ++m_impl->tool_profile_revision;
  return {};
}

auto ChatSession::reset_tool_narrowing()
    -> std::expected<void, ChatSessionError> {
  if (active()) {
    return error(ChatSessionErrorCode::run_failed,
                 "finish or cancel the active run before selecting tools");
  }
  auto selection = m_impl->tool_selection();
  selection.desired_tool_names.reset();
  auto resolved =
      resolve_profile(m_impl->available_tools, selection, *m_impl->tool_policy);
  if (!resolved) return std::unexpected(std::move(resolved.error()));
  m_impl->desired_tool_names.reset();
  ++m_impl->tool_profile_revision;
  return {};
}

auto ChatSession::set_tool_enabled(std::string tool_name, const bool enabled)
    -> std::expected<void, ChatSessionError> {
  if (active()) {
    return error(ChatSessionErrorCode::run_failed,
                 "finish or cancel the active run before selecting tools");
  }
  auto current = tool_profile_state();
  if (!current) return std::unexpected(std::move(current.error()));
  if (std::ranges::find(current->selected_profile.tool_names, tool_name) ==
      current->selected_profile.tool_names.end()) {
    return error(ChatSessionErrorCode::invalid_input,
                 "tool is not a member of the selected profile");
  }
  const auto desired = current->selection.desired_tool_names.value_or(
      current->selected_profile.tool_names);
  std::vector<std::string> candidate;
  candidate.reserve(current->selected_profile.tool_names.size());
  for (const auto& name : current->selected_profile.tool_names) {
    const bool currently_enabled =
        std::ranges::find(desired, name) != desired.end();
    if ((name == tool_name && enabled) ||
        (name != tool_name && currently_enabled)) {
      candidate.push_back(name);
    }
  }
  auto selection = current->selection;
  selection.desired_tool_names = candidate;
  auto resolved =
      resolve_profile(m_impl->available_tools, selection, *m_impl->tool_policy);
  if (!resolved) return std::unexpected(std::move(resolved.error()));
  m_impl->desired_tool_names = std::move(candidate);
  ++m_impl->tool_profile_revision;
  return {};
}

auto ChatSession::set_tool_category_enabled(
    const runtime::ToolCategory category, const bool enabled)
    -> std::expected<void, ChatSessionError> {
  if (active()) {
    return error(ChatSessionErrorCode::run_failed,
                 "finish or cancel the active run before selecting tools");
  }
  auto current = tool_profile_state();
  if (!current) return std::unexpected(std::move(current.error()));
  auto members = runtime::tool_profile_category_members(
      m_impl->available_tools, m_impl->tool_profile_id, category);
  if (!members) return std::unexpected(profile_error(members.error()));
  auto desired = current->selection.desired_tool_names.value_or(
      current->selected_profile.tool_names);
  std::vector<std::string> candidate;
  candidate.reserve(current->selected_profile.tool_names.size());
  for (const auto& name : current->selected_profile.tool_names) {
    const bool category_member =
        std::ranges::find(*members, name) != members->end();
    const bool currently_enabled =
        std::ranges::find(desired, name) != desired.end();
    if ((category_member && enabled) ||
        (!category_member && currently_enabled)) {
      candidate.push_back(name);
    }
  }
  auto selection = current->selection;
  selection.desired_tool_names = candidate;
  auto resolved =
      resolve_profile(m_impl->available_tools, selection, *m_impl->tool_policy);
  if (!resolved) return std::unexpected(std::move(resolved.error()));
  m_impl->desired_tool_names = std::move(candidate);
  ++m_impl->tool_profile_revision;
  return {};
}

auto ChatSession::set_model_tool_profile_maximum(
    std::optional<domain::ToolProfileId> profile_id)
    -> std::expected<void, ChatSessionError> {
  auto prepared = prepare_model_tool_profile_maximum(std::move(profile_id));
  if (!prepared) return std::unexpected(std::move(prepared.error()));
  return commit_tool_profile_maximum(std::move(*prepared));
}

auto ChatSession::prepare_model_tool_profile_maximum(
    std::optional<domain::ToolProfileId> profile_id) const
    -> std::expected<PreparedToolProfileMaximum, ChatSessionError> {
  if (active()) {
    return error(ChatSessionErrorCode::run_failed,
                 "finish or cancel the active run before selecting tools");
  }
  auto selection = m_impl->tool_selection();
  selection.model_maximum_profile_id = profile_id;
  auto resolved =
      resolve_profile(m_impl->available_tools, selection, *m_impl->tool_policy);
  if (!resolved) return std::unexpected(std::move(resolved.error()));
  return PreparedToolProfileMaximum{
      PreparedToolProfileMaximum::Subject{m_impl->model_id},
      std::move(profile_id), m_impl->tool_profile_revision};
}

auto ChatSession::set_persona_tool_profile_maximum(
    std::optional<domain::ToolProfileId> profile_id)
    -> std::expected<void, ChatSessionError> {
  auto prepared = prepare_persona_tool_profile_maximum(std::move(profile_id));
  if (!prepared) return std::unexpected(std::move(prepared.error()));
  return commit_tool_profile_maximum(std::move(*prepared));
}

auto ChatSession::prepare_persona_tool_profile_maximum(
    std::optional<domain::ToolProfileId> profile_id) const
    -> std::expected<PreparedToolProfileMaximum, ChatSessionError> {
  if (active()) {
    return error(ChatSessionErrorCode::run_failed,
                 "finish or cancel the active run before selecting tools");
  }
  if (!m_impl->persona_document) {
    return error(ChatSessionErrorCode::invalid_input,
                 "select a persona before configuring its tool maximum");
  }
  auto selection = m_impl->tool_selection();
  selection.persona_maximum_profile_id = profile_id;
  auto resolved =
      resolve_profile(m_impl->available_tools, selection, *m_impl->tool_policy);
  if (!resolved) return std::unexpected(std::move(resolved.error()));
  return PreparedToolProfileMaximum{
      PreparedToolProfileMaximum::Subject{
          m_impl->persona_document->reference.persona_id},
      std::move(profile_id), m_impl->tool_profile_revision};
}

auto ChatSession::commit_tool_profile_maximum(
    PreparedToolProfileMaximum prepared)
    -> std::expected<void, ChatSessionError> {
  if (active()) {
    return error(ChatSessionErrorCode::run_failed,
                 "finish or cancel the active run before selecting tools");
  }
  if (prepared.m_revision != m_impl->tool_profile_revision) {
    return error(ChatSessionErrorCode::run_failed,
                 "tool selection changed after the maximum was prepared");
  }
  auto selection = m_impl->tool_selection();
  if (const auto* model = std::get_if<domain::ModelId>(&prepared.m_subject)) {
    if (*model != m_impl->model_id) {
      return error(ChatSessionErrorCode::run_failed,
                   "model changed after the maximum was prepared");
    }
    selection.model_maximum_profile_id = prepared.m_profile_id;
  } else {
    const auto& persona = std::get<domain::PersonaId>(prepared.m_subject);
    if (!m_impl->persona_document ||
        persona != m_impl->persona_document->reference.persona_id) {
      return error(ChatSessionErrorCode::run_failed,
                   "persona changed after the maximum was prepared");
    }
    selection.persona_maximum_profile_id = prepared.m_profile_id;
  }
  auto resolved =
      resolve_profile(m_impl->available_tools, selection, *m_impl->tool_policy);
  if (!resolved) return std::unexpected(std::move(resolved.error()));

  if (const auto* model = std::get_if<domain::ModelId>(&prepared.m_subject)) {
    if (prepared.m_profile_id) {
      m_impl->model_tool_profile_maximums.insert_or_assign(
          *model, *prepared.m_profile_id);
    } else {
      m_impl->model_tool_profile_maximums.erase(*model);
    }
  } else {
    const auto& persona = std::get<domain::PersonaId>(prepared.m_subject);
    if (prepared.m_profile_id) {
      m_impl->persona_tool_profile_maximums.insert_or_assign(
          persona, *prepared.m_profile_id);
    } else {
      m_impl->persona_tool_profile_maximums.erase(persona);
    }
  }
  ++m_impl->tool_profile_revision;
  return {};
}

auto ChatSession::tool_profile_state() const
    -> std::expected<runtime::ToolProfileResolution, ChatSessionError> {
  return resolve_profile(m_impl->available_tools, m_impl->tool_selection(),
                         *m_impl->tool_policy);
}

auto ChatSession::set_generation_options(
    backend::GenerationOptions options,
    std::vector<domain::EffectiveRequestOption> effective_request_options,
    std::optional<std::vector<domain::ConfigurationProvenanceEntry>>
        configuration) -> std::expected<void, ChatSessionError> {
  auto prepared = prepare_generation_options(
      std::move(options), std::move(effective_request_options),
      std::move(configuration));
  if (!prepared) return std::unexpected(std::move(prepared.error()));
  return commit_generation_options(std::move(*prepared));
}

auto ChatSession::prepare_generation_options(
    backend::GenerationOptions options,
    std::vector<domain::EffectiveRequestOption> effective_request_options,
    std::optional<std::vector<domain::ConfigurationProvenanceEntry>>
        configuration)
    -> std::expected<PreparedChatGenerationOptions, ChatSessionError> {
  if (active()) {
    return error(
        ChatSessionErrorCode::run_failed,
        "finish or cancel the active run before changing request settings");
  }
  if (m_impl->model_context == nullptr) {
    return error(ChatSessionErrorCode::model_lookup_failed,
                 "model catalog is unavailable");
  }
  auto model =
      m_impl->model_context->lookup(m_impl->model_id, m_impl->stop_token);
  if (!model) {
    return error(model.error().kind == backend::BackendErrorKind::cancelled
                     ? ChatSessionErrorCode::cancelled
                     : ChatSessionErrorCode::model_lookup_failed,
                 model.error().redacted_message, model.error().retryable);
  }
  if (model->model_id != m_impl->model_id ||
      model->context_window_tokens == 0) {
    return error(ChatSessionErrorCode::model_lookup_failed,
                 "selected model context metadata is invalid");
  }
  auto output_tokens = m_impl->limits.preferred_output_tokens;
  if (model->maximum_output_tokens) {
    output_tokens = std::min(output_tokens, *model->maximum_output_tokens);
  }
  if (output_tokens == 0 || output_tokens >= model->context_window_tokens) {
    return error(ChatSessionErrorCode::context_failed,
                 "selected model context capacity is too small");
  }
  if (auto supported =
          backend::validate_generation_requirements(options, *model);
      !supported) {
    return error(ChatSessionErrorCode::model_lookup_failed,
                 supported.error().redacted_message);
  }
  if (auto exact = backend::validate_effective_request_options(
          options, effective_request_options);
      !exact) {
    return error(ChatSessionErrorCode::invalid_input,
                 exact.error().redacted_message);
  }
  options.max_output_tokens = output_tokens;
  auto provenance = m_impl->provenance;
  if (provenance) {
    provenance->effective_request_options =
        std::move(effective_request_options);
    if (configuration) {
      provenance->configuration = std::move(*configuration);
    }
    if (auto valid = domain::validate_run_provenance(*provenance); !valid) {
      return error(ChatSessionErrorCode::invalid_input,
                   "effective request option provenance is invalid");
    }
  }
  return PreparedChatGenerationOptions{m_impl->model_id, std::move(*model),
                                       output_tokens, std::move(options),
                                       std::move(provenance)};
}

auto ChatSession::commit_generation_options(
    PreparedChatGenerationOptions prepared)
    -> std::expected<void, ChatSessionError> {
  if (active()) {
    return error(
        ChatSessionErrorCode::run_failed,
        "finish or cancel the active run before changing request settings");
  }
  if (prepared.m_model_id != m_impl->model_id) {
    return error(ChatSessionErrorCode::model_lookup_failed,
                 "selected model changed while request settings were saved");
  }
  m_impl->model = std::move(prepared.m_model);
  m_impl->output_tokens = prepared.m_output_tokens;
  m_impl->generation_options = std::move(prepared.m_options);
  m_impl->provenance = std::move(prepared.m_provenance);
  return {};
}

auto ChatSession::persona_state() const -> ChatPersonaState {
  return {m_impl->persona_document
              ? std::optional<domain::PersonaReference>{m_impl->persona_document
                                                            ->reference}
              : std::nullopt,
          !m_impl->persona_attention.empty(), m_impl->persona_attention};
}

auto ChatSession::persona_limits() const noexcept -> persona::PersonaLimits {
  return m_impl->persona_limits;
}

auto ChatSession::load_user_global_instruction()
    -> std::expected<std::optional<domain::UserGlobalInstructionDocument>,
                     ChatSessionError> {
  if (active()) {
    return error(ChatSessionErrorCode::run_failed,
                 "finish or cancel the active run before viewing global "
                 "instructions");
  }
  if (m_impl->user_global_instruction_source == nullptr) {
    return error(ChatSessionErrorCode::context_failed,
                 "user-global instruction source is unavailable");
  }
  return load_user_global_document(m_impl->user_global_instruction_source,
                                   m_impl->user_global_instruction_limits,
                                   m_impl->stop_token);
}

auto ChatSession::write_user_global_instruction(
    instructions::UserGlobalInstructionWrite request)
    -> std::expected<instructions::UserGlobalInstructionWriteReceipt,
                     ChatSessionError> {
  if (active()) {
    return error(ChatSessionErrorCode::run_failed,
                 "finish or cancel the active run before editing global "
                 "instructions");
  }
  if (m_impl->user_global_instruction_editor == nullptr) {
    return error(ChatSessionErrorCode::context_failed,
                 "user-global instruction editor is unavailable");
  }
  request.limits = m_impl->user_global_instruction_limits;
  auto written = m_impl->user_global_instruction_editor->write(
      std::move(request), m_impl->stop_token);
  if (!written) {
    return std::unexpected(user_global_editor_error(written.error()));
  }
  return std::move(*written);
}

auto ChatSession::set_user_global_instructions_enabled(
    const bool enabled,
    std::optional<std::vector<domain::ConfigurationProvenanceEntry>>
        configuration) -> std::expected<void, ChatSessionError> {
  if (active()) {
    return error(ChatSessionErrorCode::run_failed,
                 "finish or cancel the active run before changing global "
                 "instructions");
  }
  auto provenance = m_impl->provenance;
  if (provenance && configuration) {
    provenance->configuration = std::move(*configuration);
    if (auto valid = domain::validate_run_provenance(*provenance); !valid) {
      return error(ChatSessionErrorCode::invalid_input,
                   "user-global instruction configuration provenance is "
                   "invalid");
    }
  }
  m_impl->user_global_instructions_enabled = enabled;
  m_impl->provenance = std::move(provenance);
  return {};
}

auto ChatSession::user_global_instructions_enabled() const noexcept -> bool {
  return m_impl->user_global_instructions_enabled;
}

auto ChatSession::user_global_instruction_limits() const noexcept
    -> instructions::UserGlobalInstructionLimits {
  return m_impl->user_global_instruction_limits;
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
  if (auto validated = validate_recovered_pending_run(); !validated) {
    return std::unexpected(std::move(validated.error()));
  }
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

auto ChatSession::model_info() const noexcept
    -> const backend::ModelContextInfo& {
  return m_impl->model;
}

auto ChatSession::durable() const noexcept -> bool {
  return m_impl->is_durable;
}

auto ChatSession::active() const noexcept -> bool {
  return m_impl->kernel->active_run_id().has_value();
}

} // namespace aiforge::surfaces
