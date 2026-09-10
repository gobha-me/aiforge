#include <aiforge/detail/sha256.hpp>
#include <aiforge/domain/usage_ledger.hpp>
#include <aiforge/runtime/context_builder.hpp>
#include <aiforge/runtime/conversation_summary_context.hpp>
#include <aiforge/runtime/conversation_summary_generation.hpp>
#include <aiforge/runtime/inference_spend.hpp>
#include <aiforge/runtime/memory_tool.hpp>
#include <aiforge/runtime/persona.hpp>
#include <aiforge/runtime/session_context.hpp>
#include <aiforge/runtime/session_evidence.hpp>
#include <aiforge/runtime/tool_profiles.hpp>
#include <aiforge/runtime/user_global_instructions.hpp>
#include <aiforge/surfaces/chat_session.hpp>
#include <aiforge/surfaces/local_source_browser.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <iterator>
#include <limits>
#include <set>
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

auto evidence_error(const runtime::SessionEvidenceError& failure)
    -> ChatSessionError {
  return {failure.code == runtime::SessionEvidenceErrorCode::cancelled
              ? ChatSessionErrorCode::cancelled
              : ChatSessionErrorCode::context_failed,
          failure.message, false};
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

auto observation_error(const runtime::RunKernelError& value)
    -> ManualOpsFailure {
  auto code = ManualOpsErrorCode::operation_failed;
  switch (value.code) {
    case runtime::RunKernelErrorCode::storage_failure:
      code = ManualOpsErrorCode::storage_failure;
      break;
    case runtime::RunKernelErrorCode::internal_failure:
      code = ManualOpsErrorCode::internal_failure;
      break;
    case runtime::RunKernelErrorCode::replay_rejected:
    case runtime::RunKernelErrorCode::event_log_rejected:
    case runtime::RunKernelErrorCode::projection_rejected:
      code = ManualOpsErrorCode::invalid_history;
      break;
    case runtime::RunKernelErrorCode::event_sequence_overflow:
      code = ManualOpsErrorCode::resource_exhausted;
      break;
    case runtime::RunKernelErrorCode::run_already_active:
      code = ManualOpsErrorCode::busy;
      break;
    default: break;
  }
  return {code, value.code, {}};
}

auto observation_chat_error(const ManualOpsFailure& value)
    -> std::unexpected<ChatSessionError> {
  return error(value.code == ManualOpsErrorCode::storage_failure
                   ? ChatSessionErrorCode::session_failed
                   : ChatSessionErrorCode::run_failed,
               "Manual observation is unavailable or could not complete");
}

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

[[nodiscard]] auto inference_spend_error(
    const runtime::InferenceSpendError& value) -> ChatSessionError {
  using Code = runtime::InferenceSpendErrorCode;
  switch (value.code) {
    case Code::invalid_history:
      return {ChatSessionErrorCode::session_failed, value.message, false};
    case Code::accounting_unavailable:
      return {ChatSessionErrorCode::spend_accounting_unavailable, value.message,
              false};
    case Code::ceiling_reached:
      if (value.summary && value.summary->accounted)
        return {ChatSessionErrorCode::spend_ceiling_reached,
                "session spend ceiling reached (USD " +
                    value.summary->accounted->amount().to_string() + " of " +
                    value.summary->ceiling.amount().to_string() + ")",
                false};
      return {ChatSessionErrorCode::spend_ceiling_reached, value.message,
              false};
    case Code::internal_failure: break;
  }
  return {ChatSessionErrorCode::internal_failure,
          "interactive submission failed internally", false};
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
    -> ChatSessionError;

[[nodiscard]] auto legacy_persona_id(const std::string_view name)
    -> std::optional<domain::PersonaId> {
  std::string canonical{name};
  std::ranges::transform(
      canonical, canonical.begin(), [](const unsigned char character) {
        return static_cast<char>(character >= 'A' && character <= 'Z'
                                     ? character + ('a' - 'A')
                                     : character);
      });
  auto identity = domain::PersonaId::from("persona:" + canonical);
  if (!identity) return std::nullopt;
  return std::move(*identity);
}

[[nodiscard]] auto persona_content_matches(
    const domain::PersonaDocument& document,
    const domain::ContentDigest& expected) -> bool {
  if (expected.algorithm != "sha256" ||
      document.text.size() != expected.byte_size) {
    return false;
  }
  aiforge::detail::Sha256 digest;
  digest.update(
      std::as_bytes(std::span{document.text.data(), document.text.size()}));
  return digest.finish() == expected.value;
}

[[nodiscard]] auto recovered_persona_selection(
    const domain::SessionEventLog& event_log, const domain::RunId& run_id)
    -> std::expected<std::optional<domain::PersonaSelection>,
                     ChatSessionError> {
  std::optional<domain::PersonaSelection> selection;
  for (const auto& event : event_log.events()) {
    if (event.metadata.run_id != run_id) continue;
    const auto* recorded =
        std::get_if<domain::PersonaSelectionRecorded>(&event.payload);
    if (recorded == nullptr) continue;
    if (selection) {
      return error(ChatSessionErrorCode::session_failed,
                   "recoverable run records multiple persona selections");
    }
    selection = recorded->selection;
  }
  return selection;
}

[[nodiscard]] auto persona_document_matches_reference(
    const domain::PersonaDocument& document,
    const domain::PersonaReference& expected) -> bool {
  const auto legacy_id = legacy_persona_id(expected.name);
  const bool legacy_identity =
      legacy_id && expected.persona_id == *legacy_id &&
      document.reference.name == expected.name &&
      document.reference.source_location == expected.source_location &&
      document.reference.content_digest == expected.content_digest;
  return (document.reference == expected || legacy_identity) &&
         persona_content_matches(document, expected.content_digest);
}

[[nodiscard]] auto recovered_persona_document(
    persona::PersonaSource* source, const persona::PersonaLimits limits,
    const domain::SessionEventLog& event_log,
    const runtime::RecoverableRun& recoverable,
    const std::stop_token stop_token)
    -> std::expected<std::optional<domain::PersonaDocument>, ChatSessionError> {
  auto selection = recovered_persona_selection(event_log, recoverable.run_id);
  if (!selection) return std::unexpected(std::move(selection.error()));
  if (!*selection) {
    if (recoverable.attributes.persona_id) {
      return error(ChatSessionErrorCode::context_failed,
                   "recoverable run persona metadata is incomplete");
    }
    return std::optional<domain::PersonaDocument>{};
  }
  const auto& selected = **selection;
  if (!domain::validate_persona_selection(selected)) {
    return error(ChatSessionErrorCode::context_failed,
                 "recoverable run persona selection is invalid");
  }
  if (selected.action == domain::PersonaSelectionAction::disabled) {
    if (recoverable.attributes.persona_id) {
      return error(ChatSessionErrorCode::context_failed,
                   "recoverable run disabled persona identity is invalid");
    }
    return std::optional<domain::PersonaDocument>{};
  }
  if (!selected.persona) {
    return error(ChatSessionErrorCode::context_failed,
                 "recoverable run selected persona is missing");
  }
  const auto& recorded = *selected.persona;
  if (recoverable.attributes.persona_id != recorded.persona_id) {
    return error(ChatSessionErrorCode::context_failed,
                 "recoverable run persona identity is inconsistent");
  }
  if (source == nullptr) {
    return error(ChatSessionErrorCode::context_failed,
                 "recorded persona source is unavailable");
  }
  auto loaded = source->load(recorded.name, limits, stop_token);
  if (!loaded) return std::unexpected(persona_error(loaded.error()));
  if (!domain::validate_persona_document(*loaded)) {
    return error(ChatSessionErrorCode::context_failed,
                 "recorded persona document is invalid");
  }
  if (!persona_document_matches_reference(*loaded, recorded)) {
    return error(ChatSessionErrorCode::context_failed,
                 "persona changed since this run started");
  }
  // Legacy pending runs used a name-derived identity. Preserve that recorded
  // reference in their continuation context even if loading the untagged file
  // assigned its new immutable identity.
  loaded->reference = recorded;
  return std::optional<domain::PersonaDocument>{std::move(*loaded)};
}

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
    const std::vector<backend::ToolDeclaration>& declarations,
    bool legacy = false) -> std::expected<std::uint64_t, ChatSessionError> {
  if (!legacy) {
    auto estimated = runtime::estimate_session_tool_declarations(declarations);
    if (!estimated)
      return error(ChatSessionErrorCode::context_failed,
                   estimated.error().message);
    return *estimated;
  }
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
[[nodiscard]] auto estimated_message_tokens(const domain::Message& message,
                                            bool legacy = false)
    -> std::expected<std::uint64_t, ChatSessionError> {
  // clang-format on
  if (!legacy) {
    auto estimated = runtime::estimate_conversation_message(message);
    if (!estimated)
      return error(ChatSessionErrorCode::context_failed,
                   estimated.error().message);
    return *estimated;
  }
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

auto context_run_attributes(const domain::SessionEventLog& log,
                            const std::optional<domain::RunId>& run_id)
    -> const domain::RunStarted* {
  if (!run_id) return nullptr;
  for (const auto& event : log.events())
    if (event.metadata.run_id == *run_id)
      if (const auto* started = std::get_if<domain::RunStarted>(&event.payload))
        return started;
  return nullptr;
}

auto legacy_context(const domain::SessionEventLog& log,
                    const std::optional<domain::RunId>& run_id) -> bool {
  const auto* attributes = context_run_attributes(log, run_id);
  return attributes == nullptr || !attributes->conversation_admission;
}

auto append_recovered_user_input(
    const domain::SessionEventLog& log, const domain::RunId& run_id,
    std::vector<domain::ContextContentInput> result, std::uint64_t order)
    -> std::expected<std::vector<domain::ContextContentInput>, std::string> {
  const domain::UserContentAdded* current{};
  const domain::RunEvent* source{};
  for (const auto& event : log.events()) {
    if (event.metadata.run_id != run_id) continue;
    if (const auto* user =
            std::get_if<domain::UserContentAdded>(&event.payload)) {
      if (current != nullptr)
        return std::unexpected(std::string{"original user input is ambiguous"});
      current = user;
      source = &event;
    }
  }
  if (current == nullptr || source == nullptr ||
      order == std::numeric_limits<std::uint64_t>::max())
    return std::unexpected(std::string{"original user input is unavailable"});
  if (!runtime::estimate_conversation_message(current->message))
    return std::unexpected(
        std::string{"original user input exceeds its resource bound"});
  const auto identity = std::to_string(source->metadata.sequence);
  auto entry_id =
      domain::ContextEntryId::from("recovered-user-entry-" + identity);
  auto source_id =
      domain::ContextSourceId::from("recovered-user-source-" + identity);
  auto estimate = estimated_message_tokens(current->message, true);
  if (!entry_id || !source_id || !estimate)
    return std::unexpected(
        std::string{"original user input cannot be bounded"});
  result.push_back(
      {*entry_id,
       domain::ContextContentKind::conversation,
       current->message,
       {*source_id,
        "session:" + std::string{log.session_id().value()} +
            "/event:" + std::string{source->metadata.event_id.value()},
        std::nullopt},
       order + 1,
       *estimate});
  return result;
}

auto frozen_summary_content(
    const std::optional<std::vector<domain::ContextContentInput>>& content)
    -> const std::vector<domain::ContextContentInput>* {
  return content ? &*content : nullptr;
}

auto recovered_conversation_input(
    const domain::SessionEventLog& log,
    const std::optional<domain::RunId>& run_id, std::uint64_t suffix,
    const std::vector<domain::ContextContentInput>* frozen_summaries = nullptr)
    -> std::expected<std::vector<domain::ContextContentInput>, std::string> {
  const auto* attributes = context_run_attributes(log, run_id);
  if (!run_id || attributes == nullptr || !attributes->conversation_admission)
    return detail::replayed_conversation(log, suffix);
  auto history = runtime::recover_conversation_context(
      log, *attributes->conversation_admission);
  if (!history) return std::unexpected(history.error().message);
  std::vector<domain::ContextContentInput> result;
  const auto& admission = *attributes->conversation_admission;
  if (frozen_summaries != nullptr) {
    result = *frozen_summaries;
  } else if (admission.version == 2 &&
             admission.mode == domain::ConversationMode::rolling) {
    auto restored = runtime::recover_conversation_summary_context(
        log, admission.summaries, admission.source_snapshot_sequence);
    if (!restored) return std::unexpected(restored.error().message);
    result = std::move(restored->content);
  }
  std::uint64_t order{};
  for (const auto& summary : result)
    order = std::max(order, summary.order);
  if (attributes->memory_selection)
    for (const auto& memory : attributes->memory_selection->entries)
      order = std::max(order, memory.order);
  for (auto& group : *history)
    for (auto& entry : group.entries) {
      order = std::max(order, entry.content.order);
      result.push_back(std::move(entry.content));
    }
  return append_recovered_user_input(log, *run_id, std::move(result), order);
}

[[nodiscard]] auto assistant_continuation_state(
    const std::span<const domain::RunEvent> events, const domain::RunId& run_id,
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
    if (event.metadata.run_id != run_id) continue;
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

[[nodiscard]] auto invalid_resumed_persona(
    const persona::PersonaDirective& directive, const bool allow_attention)
    -> std::expected<PersonaSetup, ChatSessionError> {
  if (allow_attention &&
      directive.kind == persona::PersonaDirectiveKind::inherit) {
    return PersonaSetup{std::nullopt, std::nullopt,
                        "Persona is invalid; select a persona or turn it off"};
  }
  return error(ChatSessionErrorCode::context_failed,
               "persona document is invalid");
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
    return invalid_resumed_persona(directive, allow_attention);
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
  ChatIdentitySuffixSource identity_suffix_source;
  std::optional<domain::RunProvenance> provenance;
  persona::PersonaSource* persona_source{};
  persona::PersonaEditor* persona_editor{};
  persona::PersonaLimits persona_limits{};
  instructions::UserGlobalInstructionSource* user_global_instruction_source{};
  instructions::UserGlobalInstructionEditor* user_global_instruction_editor{};
  instructions::UserGlobalInstructionLimits user_global_instruction_limits{};
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
  std::optional<domain::PersonaDocument> recovered_persona_document;
  std::optional<domain::UserGlobalInstructionDocument>
      recovered_user_global_instruction;
  std::vector<domain::RunEvent> pending_surface_events;
  std::unique_ptr<runtime::RunKernel> kernel;
  std::uint64_t tool_profile_revision{};
  std::optional<ChatRecoveryBlock> recovery_block;
  std::optional<runtime::RecoverableRun> recovered_run;
  std::vector<domain::ContextContentInput> recovered_memory_context{};
  runtime::RepositoryContextController* repository_controller{};
  std::optional<runtime::RepositoryContextRequest> repository_selection{};
  std::uint64_t repository_generation{};
  std::optional<ChatRepositoryWork> repository_work{};
  std::optional<std::string> repository_prompt{};
  std::optional<runtime::PreparedRepositoryContext> repository_prepared{};
  std::optional<domain::RepositoryContextAdmission> repository_admission{};
  std::optional<ChatRecoveryBlock> repository_block{};
  std::function<std::expected<void, ChatSessionError>()> repository_action{};
  std::string repository_message{};
  struct EvidenceWork {
    ChatEvidenceWorkToken token;
    std::uint64_t sequence{};
    std::uint64_t local_epoch{};
    std::uint64_t local_revision{};
    std::optional<domain::RunId> original_run{};
    std::optional<domain::LocalContextAdmission> original_local{};
    std::optional<runtime::LocalContextWorkToken> local_work{};
    std::optional<runtime::PreparedRepositoryContext> repository{};
    std::optional<runtime::PreparedLocalContext> local{};
    std::function<std::expected<ChatEvidenceResult, ChatSessionError>()> action;
    std::optional<std::string> required_draft{};
  };
  LocalSourceBrowser* local_sources{};
  std::uint64_t evidence_generation{};
  std::optional<EvidenceWork> evidence_work{};
  bool evidence_executing{};
  bool evidence_permit{};
  std::optional<runtime::PreparedRepositoryContext> evidence_repository{};
  std::optional<runtime::PreparedLocalContext> evidence_local{};
  std::optional<domain::LocalContextAdmission> local_admission{};
  std::optional<runtime::PreparedLocalContext> local_prepared{};

  ChatSurfaceKind surface_kind{ChatSurfaceKind::interactive};
  bool is_durable{};
  bool user_global_instructions_enabled{};
  bool recovered_pending_run_validation_required{};
  bool recovered_sources_pinned{};
  bool async_repository_preparation{};
  bool repository_proof_ready{};
  bool repository_recovery_pinned{};
  std::optional<std::vector<domain::ContextContentInput>>
      recovered_summary_context{};
  std::shared_ptr<runtime::OpsObservationBroker> observation_broker{};
  std::optional<ChatObservationContext> observation_context{};
  std::optional<ObservationSubmission> manual_observation{};
  ManualOpsInspection observation_inspection{};

  [[nodiscard]] auto manual_active() const -> bool {
    return manual_observation &&
           kernel->active_run_id() == manual_observation->run_id;
  }
  [[nodiscard]] auto observation_preparation_busy() const -> bool {
    return repository_work || evidence_work || repository_action;
  }
  auto remember_observation_events(std::size_t before) -> void {
    const auto tail = std::span{kernel->event_log().events()}.subspan(before);
    pending_surface_events.insert(pending_surface_events.end(), tail.begin(),
                                  tail.end());
  }
  auto close_observation_admission(ManualOpsFailure failure)
      -> std::unexpected<ManualOpsFailure> {
    if (observation_inspection.problem && observation_inspection.closed &&
        (observation_inspection.problem->code ==
             ManualOpsErrorCode::storage_failure ||
         failure.code != ManualOpsErrorCode::storage_failure))
      failure = *observation_inspection.problem;
    observation_inspection.problem = failure;
    observation_inspection.available = false;
    observation_inspection.closed = true;
    observation_inspection.approval.reset();
    return std::unexpected(failure);
  }
  auto report_observation_kernel_failure(const runtime::RunKernelError& value)
      -> std::unexpected<ManualOpsFailure> {
    const auto failure = observation_error(value);
    if (failure.code == ManualOpsErrorCode::storage_failure ||
        failure.code == ManualOpsErrorCode::internal_failure ||
        failure.code == ManualOpsErrorCode::invalid_history ||
        failure.code == ManualOpsErrorCode::resource_exhausted)
      return close_observation_admission(failure);
    return std::unexpected(failure);
  }
  auto synchronize_observations(bool force = false)
      -> std::expected<void, ManualOpsFailure> {
    if (force || observation_inspection.projection.last_sequence !=
                     kernel->event_log().last_sequence()) {
      auto projected =
          project_manual_observations(kernel->event_log(), manual_observation);
      if (!projected) return close_observation_admission(projected.error());
      observation_inspection.projection = std::move(*projected);
    }
    observation_inspection.busy = kernel->active_run_id().has_value() ||
                                  !kernel->active_session_tasks().empty() ||
                                  observation_preparation_busy();
    auto approval = kernel->pending_tool_approval();
    if (!manual_observation || !approval ||
        approval->run_id != manual_observation->run_id ||
        approval->invocation_id != manual_observation->invocation_id)
      approval.reset();
    if (observation_inspection.closed) approval.reset();
    observation_inspection.approval = std::move(approval);
    return {};
  }

  auto observe_pending_events()
      -> std::expected<std::vector<domain::RunEvent>, ChatSessionError> {
    // Failed cancellation retires the unusable kernel but leaves its recovery
    // block inspectable until reopening. No further observations can commit.
    if (!kernel->active_run_id() && (recovery_block || repository_block))
      return std::exchange(pending_surface_events, {});
    // Recording already-dispatched work never waits for a filesystem proof.
    auto drained = kernel->drain(runtime::RunDrainMode::observe_only);
    if (!drained) return std::unexpected(kernel_error(drained.error()));
    auto result = std::exchange(pending_surface_events, {});
    result.insert(result.end(), std::make_move_iterator(drained->begin()),
                  std::make_move_iterator(drained->end()));
    return result;
  }

  auto begin_repository_evidence(bool original_sources)
      -> std::expected<void, ChatSessionError> {
    if (!evidence_work)
      return error(ChatSessionErrorCode::internal_failure,
                   "Context work is unavailable");
    auto& work = *evidence_work;
    std::optional<ChatRepositoryWork> repository;
    if (original_sources) {
      work.original_run = kernel->active_run_id();
      if (!work.original_run)
        return error(ChatSessionErrorCode::run_failed,
                     "No active run requires source validation");
      auto local = runtime::recorded_local_context_admission(
          kernel->event_log(), *work.original_run);
      auto repo = runtime::recorded_repository_context_admission(
          kernel->event_log(), *work.original_run);
      if (!local) return std::unexpected(kernel_error(local.error()));
      if (!repo) return std::unexpected(kernel_error(repo.error()));
      work.original_local = std::move(*local);
      if (*repo)
        repository = ChatRepositoryWork{
            {kernel->event_log().session_id(), model_id,
             ++repository_generation,
             repository_selection ? repository_selection->selection_revision
                                  : 0,
             ChatRepositoryWorkPurpose::recovery},
            std::move(**repo)};
    } else if (repository_selection) {
      repository = ChatRepositoryWork{{kernel->event_log().session_id(),
                                       model_id, ++repository_generation,
                                       repository_selection->selection_revision,
                                       ChatRepositoryWorkPurpose::submit},
                                      *repository_selection};
    }
    if (repository && repository_controller == nullptr)
      return error(ChatSessionErrorCode::context_failed,
                   "Recorded repository context is unavailable");
    repository_work = std::move(repository);
    return {};
  }
  auto poll_local_evidence() -> std::expected<bool, ChatSessionError> {
    if (!evidence_work)
      return error(ChatSessionErrorCode::internal_failure,
                   "Context work is unavailable");
    auto& work = *evidence_work;
    if (!work.local_work) return true;
    if (local_sources == nullptr)
      return error(ChatSessionErrorCode::context_failed,
                   "Local file grants are unavailable");
    auto completed = local_sources->poll_context(*work.local_work);
    if (!completed)
      return error(ChatSessionErrorCode::context_failed,
                   completed.error().message);
    if (!*completed) return false;
    if (!(**completed).result)
      return error(ChatSessionErrorCode::context_failed,
                   (**completed).result.error().message);
    work.local = std::move(*(**completed).result);
    work.local_work.reset();
    return true;
  }
  [[nodiscard]] auto evidence_scope_matches() const -> bool {
    if (!evidence_work) return false;
    const auto& work = *evidence_work;
    if (work.token.session_id != kernel->event_log().session_id() ||
        work.token.model_id != model_id ||
        (work.original_run
             ? work.original_run != kernel->active_run_id()
             : work.sequence != kernel->event_log().last_sequence()))
      return false;
    if (local_sources == nullptr) return true;
    const auto& state = local_sources->state();
    return state.session_id == kernel->event_log().session_id() &&
           state.session_epoch == work.local_epoch &&
           (work.original_run ||
            state.selection_revision == work.local_revision);
  }

  [[nodiscard]] auto evidence_draft_matches(
      std::optional<std::string_view> draft) const noexcept -> bool {
    if (!evidence_work) return false;
    return evidence_work->token.purpose !=
               ChatEvidenceWorkPurpose::summary_apply ||
           (evidence_work->required_draft && draft &&
            *draft == *evidence_work->required_draft);
  }

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
ChatSession::~ChatSession() {
  cancel_evidence_work();
}

auto ChatSession::pending_evidence_work() const
    -> std::optional<ChatEvidenceWorkToken> {
  return m_impl->evidence_work ? std::optional{m_impl->evidence_work->token}
                               : std::nullopt;
}

auto ChatSession::cancel_evidence_work() -> void {
  if (m_impl->evidence_work && m_impl->evidence_work->local_work &&
      m_impl->local_sources != nullptr)
    m_impl->local_sources->cancel_context();
  m_impl->evidence_work.reset();
  m_impl->evidence_permit = false;
  m_impl->evidence_repository.reset();
  m_impl->evidence_local.reset();
  m_impl->repository_work.reset();
  m_impl->repository_prompt.reset();
  m_impl->repository_action = {};
}

auto ChatSession::begin_local_evidence_work()
    -> std::expected<void, ChatSessionError> {
  if (!m_impl->evidence_work)
    return error(ChatSessionErrorCode::internal_failure,
                 "Context work is unavailable");
  auto& work = *m_impl->evidence_work;
  if (m_impl->local_sources == nullptr) {
    if (work.original_local)
      return error(ChatSessionErrorCode::context_failed,
                   "Recorded local files require explicit folder grants");
    return {};
  }
  const auto& state = m_impl->local_sources->state();
  if (state.session_id != session_id() ||
      state.session_epoch != work.local_epoch ||
      (!work.original_run && state.selection_revision != work.local_revision))
    return error(ChatSessionErrorCode::context_failed,
                 "Local file selection changed during preparation");
  if (work.original_run && !work.original_local) return {};
  if (!work.original_run && state.selection.empty() &&
      !m_impl->local_sources->pending_evidence_selection())
    return {};
  auto started = work.original_local
                     ? m_impl->local_sources->revalidate(*work.original_local)
                     : m_impl->local_sources->prepare_selection();
  if (!started)
    return error(ChatSessionErrorCode::context_failed, started.error().message);
  work.local_work = *started;
  return {};
}

auto ChatSession::begin_evidence_work(
    ChatEvidenceWorkPurpose purpose,
    std::function<std::expected<ChatEvidenceResult, ChatSessionError>()> action,
    bool original_sources, std::optional<std::string> required_draft)
    -> std::expected<void, ChatSessionError> {
  try {
    if (m_impl->evidence_work || m_impl->repository_work ||
        m_impl->evidence_executing)
      return error(ChatSessionErrorCode::run_failed,
                   "Context preparation is already pending");
    if (m_impl->evidence_generation ==
            std::numeric_limits<std::uint64_t>::max() ||
        m_impl->repository_generation ==
            std::numeric_limits<std::uint64_t>::max())
      return error(ChatSessionErrorCode::context_failed,
                   "Context preparation identity exhausted");
    Impl::EvidenceWork work{
        {session_id(), model_id(), ++m_impl->evidence_generation, purpose},
        event_log().last_sequence(),
        0,
        0,
        {},
        {},
        {},
        {},
        {},
        std::move(action),
        std::move(required_draft)};
    if (m_impl->local_sources != nullptr) {
      const auto& state = m_impl->local_sources->state();
      if (state.session_id != session_id())
        return error(ChatSessionErrorCode::context_failed,
                     "Local file grants belong to another session");
      work.local_epoch = state.session_epoch;
      work.local_revision = state.selection_revision;
    }
    m_impl->evidence_work = std::move(work);
    auto repo = m_impl->begin_repository_evidence(original_sources);
    if (!repo) {
      cancel_evidence_work();
      return repo;
    }
    if (!m_impl->repository_work) {
      auto begun = begin_local_evidence_work();
      if (!begun) {
        cancel_evidence_work();
        return begun;
      }
    }
    return {};
  } catch (...) {
    cancel_evidence_work();
    return error(ChatSessionErrorCode::internal_failure,
                 "Context preparation failed internally");
  }
}

auto ChatSession::request_evidence_submit(std::string prompt)
    -> std::expected<void, ChatSessionError> {
  try {
    if (active())
      return error(ChatSessionErrorCode::run_failed,
                   "A run or context preparation is already active");
    if (prompt.empty() || !valid_text(prompt))
      return error(ChatSessionErrorCode::invalid_input,
                   "Prompt must be nonempty UTF-8 text without controls");
    if (prompt.size() > m_impl->limits.maximum_input_bytes)
      return error(ChatSessionErrorCode::input_too_large,
                   "Prompt exceeds the configured input limit");
    return begin_evidence_work(
        ChatEvidenceWorkPurpose::submit,
        [this, prompt = std::move(prompt)]() mutable
            -> std::expected<ChatEvidenceResult, ChatSessionError> {
          auto result = submit_prepared(std::move(prompt),
                                        std::move(m_impl->evidence_repository));
          if (!result) return std::unexpected(result.error());
          return ChatEvidenceResult{std::move(*result)};
        });

  } catch (...) {
    return error(ChatSessionErrorCode::internal_failure,
                 "Context operation failed internally");
  }
}

auto ChatSession::request_context_inspection(std::string draft)
    -> std::expected<void, ChatSessionError> {
  try {
    if (draft.size() > m_impl->limits.maximum_input_bytes)
      return error(ChatSessionErrorCode::input_too_large,
                   "Draft exceeds the configured input limit");
    return begin_evidence_work(
        ChatEvidenceWorkPurpose::inspection,
        [this, draft = std::move(draft)]() mutable
            -> std::expected<ChatEvidenceResult, ChatSessionError> {
          auto result = inspect_conversation_context(std::move(draft));
          if (!result) return std::unexpected(result.error());
          return ChatEvidenceResult{std::move(*result)};
        });

  } catch (...) {
    return error(ChatSessionErrorCode::internal_failure,
                 "Context operation failed internally");
  }
}

auto ChatSession::request_summary_preview(
    domain::ConversationSummaryVersion candidate,
    std::vector<domain::ConversationSummaryVersion> replacements,
    std::string draft) -> std::expected<void, ChatSessionError> {
  try {
    if (active())
      return error(ChatSessionErrorCode::run_failed,
                   "Summary preview requires an idle session");
    if (draft.size() > m_impl->limits.maximum_input_bytes ||
        replacements.size() > domain::summary_maximum_active)
      return error(ChatSessionErrorCode::input_too_large,
                   "Summary review input exceeds its limit");
    return begin_evidence_work(
        ChatEvidenceWorkPurpose::summary_preview,
        [this, candidate = std::move(candidate),
         replacements = std::move(replacements),
         draft = std::move(draft)]() mutable
            -> std::expected<ChatEvidenceResult, ChatSessionError> {
          auto result = preview_conversation_summary(
              std::move(candidate), std::move(replacements), std::move(draft));
          if (!result) return std::unexpected(result.error());
          return ChatEvidenceResult{std::move(*result)};
        });

  } catch (...) {
    return error(ChatSessionErrorCode::internal_failure,
                 "Context operation failed internally");
  }
}

auto ChatSession::request_summary_apply(const ChatSummaryPreview& preview,
                                        std::string current_draft)
    -> std::expected<void, ChatSessionError> {
  try {
    if (active())
      return error(ChatSessionErrorCode::run_failed,
                   "Summary application requires an idle session");
    if (current_draft.size() > m_impl->limits.maximum_input_bytes)
      return error(ChatSessionErrorCode::input_too_large,
                   "Draft exceeds the configured input limit");
    auto binding = current_draft;
    return begin_evidence_work(
        ChatEvidenceWorkPurpose::summary_apply,
        [this, preview, draft = std::move(current_draft)]() mutable
            -> std::expected<ChatEvidenceResult, ChatSessionError> {
          auto result = apply_conversation_summary(preview, std::move(draft));
          if (!result) return std::unexpected(result.error());
          return ChatEvidenceResult{std::move(*result)};
        },
        false, std::move(binding));

  } catch (...) {
    return error(ChatSessionErrorCode::internal_failure,
                 "Context operation failed internally");
  }
}

auto ChatSession::poll_evidence_work(
    std::optional<std::string_view> current_draft)
    -> std::expected<std::optional<ChatEvidenceOutcome>, ChatSessionError> {
  const auto before = event_log().last_sequence();
  try {
    if (!m_impl->evidence_work) return std::nullopt;
    auto& work = *m_impl->evidence_work;
    auto fail = [&](ChatSessionError failure)
        -> std::expected<std::optional<ChatEvidenceOutcome>, ChatSessionError> {
      const auto run = work.original_run;
      cancel_evidence_work();
      if (run)
        m_impl->repository_block =
            ChatRecoveryBlock{session_id(), *run, failure};
      return std::unexpected(std::move(failure));
    };
    if (m_impl->stop_token.stop_requested())
      return fail({ChatSessionErrorCode::cancelled,
                   "Context preparation was cancelled"});
    if (!m_impl->evidence_scope_matches())
      return fail({ChatSessionErrorCode::context_failed,
                   "Context preparation became stale"});
    if (!m_impl->evidence_draft_matches(current_draft))
      return fail({ChatSessionErrorCode::context_failed,
                   "Current draft no longer matches summary review"});
    if (m_impl->repository_work) return std::nullopt;
    auto local_ready = m_impl->poll_local_evidence();
    if (!local_ready) return fail(local_ready.error());
    if (!*local_ready) return std::nullopt;
    auto ready = std::move(work);
    m_impl->evidence_work.reset();
    m_impl->evidence_repository = std::move(ready.repository);
    m_impl->evidence_local = std::move(ready.local);
    if (ready.original_run) {
      m_impl->repository_prepared = m_impl->evidence_repository;
      m_impl->repository_admission =
          m_impl->repository_prepared
              ? std::optional{m_impl->repository_prepared->admission}
              : std::nullopt;
      m_impl->local_prepared = m_impl->evidence_local;
      m_impl->local_admission = ready.original_local;
      m_impl->repository_block.reset();
    }
    m_impl->evidence_executing = true;
    m_impl->evidence_permit = true;
    struct Clear {
      Impl& impl;
      ~Clear() {
        impl.evidence_executing = false;
        impl.evidence_permit = false;
        impl.evidence_repository.reset();
        impl.evidence_local.reset();
      }
    } clear{*m_impl};
    auto result = ready.action();
    if (!result) {
      auto failure = result.error();
      failure.effect_may_have_applied = failure.effect_may_have_applied ||
                                        event_log().last_sequence() != before;
      return std::unexpected(std::move(failure));
    }
    return std::optional{ChatEvidenceOutcome{ready.token, std::move(*result)}};
  } catch (...) {
    cancel_evidence_work();
    return std::unexpected(
        ChatSessionError{ChatSessionErrorCode::internal_failure,
                         "Context completion failed internally", false,
                         event_log().last_sequence() != before});
  }
}

auto ChatSession::pending_repository_work() const
    -> std::optional<ChatRepositoryWork> {
  return m_impl->repository_work;
}

auto ChatSession::cancel_repository_work() -> void {
  if (m_impl->evidence_work) cancel_evidence_work();
  m_impl->repository_work.reset();
  m_impl->repository_prompt.reset();
  m_impl->repository_action = {};
  m_impl->repository_proof_ready = false;
  if (m_impl->repository_generation !=
      std::numeric_limits<std::uint64_t>::max())
    ++m_impl->repository_generation;
}

auto ChatSession::repository_context_state() const
    -> ChatRepositoryContextState {
  ChatRepositoryContextState result;
  result.available = m_impl->repository_controller != nullptr;
  result.busy = m_impl->repository_work.has_value();
  result.enabled = m_impl->repository_selection.has_value();
  if (m_impl->repository_selection) {
    result.selection_revision =
        m_impl->repository_selection->selection_revision;
    result.target_subtree = m_impl->repository_selection->target_subtree;
    result.evidence_paths = m_impl->repository_selection->evidence_paths;
  }
  result.admission = m_impl->repository_admission;
  if (!result.admission && m_impl->repository_prepared)
    result.admission = m_impl->repository_prepared->admission;
  result.message = m_impl->repository_message;
  return result;
}

auto ChatSession::apply_repository_change(
    runtime::RepositoryContextRequest& request,
    const ChatRepositoryChange& change)
    -> std::expected<void, ChatSessionError> {
  switch (change.kind) {
    case ChatRepositoryChangeKind::select_target:
      request.target_subtree = change.subject;
      break;
    case ChatRepositoryChangeKind::add_evidence:
      if (!m_impl->repository_selection)
        return error(ChatSessionErrorCode::invalid_input,
                     "select a Dev target before adding evidence");
      if (std::ranges::find(request.evidence_paths, change.subject) !=
          request.evidence_paths.end())
        return error(ChatSessionErrorCode::invalid_input,
                     "repository evidence is already selected");
      request.evidence_paths.push_back(change.subject);
      break;
    case ChatRepositoryChangeKind::remove_evidence: {
      auto path = change.subject;
      if (m_impl->repository_prepared) {
        for (const auto& ref : m_impl->repository_prepared->admission.evidence)
          if (ref.evidence_id.value() == change.subject)
            path = ref.source.relative_path;
      }
      const auto found = std::ranges::find(request.evidence_paths, path);
      if (found == request.evidence_paths.end())
        return error(ChatSessionErrorCode::invalid_input,
                     "repository evidence selection was not found");
      request.evidence_paths.erase(found);
      break;
    }
    case ChatRepositoryChangeKind::clear_evidence:
      if (!m_impl->repository_selection)
        return error(ChatSessionErrorCode::invalid_input,
                     "Dev context is not enabled");
      request.evidence_paths.clear();
      break;
    case ChatRepositoryChangeKind::disable: break;
  }
  return {};
}

auto ChatSession::request_repository_change(ChatRepositoryChange change)
    -> std::expected<void, ChatSessionError> {
  try {
    if (active())
      return error(ChatSessionErrorCode::run_failed,
                   "repository selection is available only while idle");
    if (m_impl->repository_controller == nullptr)
      return error(ChatSessionErrorCode::context_failed,
                   "repository context is unavailable");
    if (change.kind == ChatRepositoryChangeKind::disable) {
      cancel_repository_work();
      m_impl->repository_selection.reset();
      m_impl->repository_prepared.reset();
      m_impl->repository_admission.reset();
      m_impl->repository_message = "Dev context is off";
      return {};
    }
    auto request = m_impl->repository_selection.value_or(
        runtime::RepositoryContextRequest{});
    if (request.selection_revision ==
            std::numeric_limits<std::uint64_t>::max() ||
        m_impl->repository_generation ==
            std::numeric_limits<std::uint64_t>::max())
      return error(ChatSessionErrorCode::context_failed,
                   "repository selection revision exhausted");
    auto changed = apply_repository_change(request, change);
    if (!changed) return changed;
    ++request.selection_revision;
    m_impl->repository_work = ChatRepositoryWork{
        {session_id(), m_impl->model_id, ++m_impl->repository_generation,
         m_impl->repository_selection
             ? m_impl->repository_selection->selection_revision
             : 0,
         ChatRepositoryWorkPurpose::selection},
        std::move(request)};
    m_impl->repository_message = "Preparing repository context";
    return {};
  } catch (...) {
    return error(ChatSessionErrorCode::internal_failure,
                 "repository selection failed internally");
  }
}

auto ChatSession::request_repository_submit(std::string prompt)
    -> std::expected<void, ChatSessionError> {
  if (m_impl->local_sources != nullptr) {
    if (!m_impl->repository_selection ||
        m_impl->repository_controller == nullptr)
      return error(ChatSessionErrorCode::context_failed,
                   "Dev context is not enabled");
    return request_evidence_submit(std::move(prompt));
  }
  try {
    if (active())
      return error(ChatSessionErrorCode::run_failed,
                   "a run or context preparation is already active");
    if (!m_impl->repository_selection ||
        m_impl->repository_controller == nullptr)
      return error(ChatSessionErrorCode::context_failed,
                   "Dev context is not enabled");
    if (prompt.empty() || !valid_text(prompt))
      return error(ChatSessionErrorCode::invalid_input,
                   "prompt must be nonempty UTF-8 text without controls");
    if (prompt.size() > m_impl->limits.maximum_input_bytes)
      return error(ChatSessionErrorCode::input_too_large,
                   "prompt exceeds the configured input limit");
    if (m_impl->repository_generation ==
        std::numeric_limits<std::uint64_t>::max())
      return error(ChatSessionErrorCode::context_failed,
                   "repository preparation identity exhausted");
    m_impl->repository_work = ChatRepositoryWork{
        {session_id(), m_impl->model_id, ++m_impl->repository_generation,
         m_impl->repository_selection->selection_revision,
         ChatRepositoryWorkPurpose::submit},
        *m_impl->repository_selection};
    m_impl->repository_prompt = std::move(prompt);
    m_impl->repository_message = "Validating repository inputs";
    return {};
  } catch (...) {
    return error(ChatSessionErrorCode::internal_failure,
                 "repository submission preparation failed internally");
  }
}

namespace {
auto evidence_purpose(ChatRepositoryWorkPurpose purpose)
    -> ChatEvidenceWorkPurpose {
  switch (purpose) {
    case ChatRepositoryWorkPurpose::answer:
      return ChatEvidenceWorkPurpose::answer;
    case ChatRepositoryWorkPurpose::approval:
      return ChatEvidenceWorkPurpose::approval;
    case ChatRepositoryWorkPurpose::recovery:
      return ChatEvidenceWorkPurpose::recovery;
    default: return ChatEvidenceWorkPurpose::continuation;
  }
}
} // namespace

auto ChatSession::validate_active_evidence(ChatRepositoryWorkPurpose purpose)
    -> std::expected<bool, ChatSessionError> {
  const auto run = m_impl->kernel->active_run_id();
  if (!run) return true;
  if (m_impl->evidence_permit) return true;
  if (m_impl->evidence_executing || m_impl->evidence_work) return false;
  auto local = runtime::recorded_local_context_admission(event_log(), *run);
  if (!local) return std::unexpected(kernel_error(local.error()));
  if (m_impl->local_sources == nullptr && !*local)
    return validate_active_repository(purpose);
  if (m_impl->repository_block &&
      purpose != ChatRepositoryWorkPurpose::answer &&
      purpose != ChatRepositoryWorkPurpose::approval)
    return false;
  auto repository =
      runtime::recorded_repository_context_admission(event_log(), *run);
  if (!repository) return std::unexpected(kernel_error(repository.error()));
  if (!*local && !*repository) return true;
  const auto kind = evidence_purpose(purpose);
  auto started = begin_evidence_work(
      kind,
      [this]() -> std::expected<ChatEvidenceResult, ChatSessionError> {
        auto action = std::move(m_impl->repository_action);
        m_impl->repository_action = {};
        if (action) {
          auto applied = action();
          if (!applied) return std::unexpected(applied.error());
        }
        auto observed = drain();
        if (!observed) return std::unexpected(observed.error());
        return ChatEvidenceResult{
            ChatEvidenceActionCompleted{std::move(*observed)}};
      },
      true);
  if (!started) {
    m_impl->repository_block =
        ChatRecoveryBlock{session_id(), *run, started.error()};
    return std::unexpected(started.error());
  }
  return false;
}

auto ChatSession::retry_context_sources()
    -> std::expected<void, ChatSessionError> {
  try {
    if (m_impl->evidence_work || m_impl->repository_work)
      return error(ChatSessionErrorCode::run_failed,
                   "Context source validation is already pending");
    if (!m_impl->kernel->active_run_id())
      return error(ChatSessionErrorCode::run_failed,
                   "No active run requires source validation");
    m_impl->repository_block.reset();
    auto ready = validate_active_evidence(ChatRepositoryWorkPurpose::recovery);
    if (!ready) return std::unexpected(ready.error());
    return {};

  } catch (...) {
    return error(ChatSessionErrorCode::internal_failure,
                 "Context operation failed internally");
  }
}

auto ChatSession::validate_active_repository(
    const ChatRepositoryWorkPurpose purpose)
    -> std::expected<bool, ChatSessionError> {
  try {
    const auto run = m_impl->kernel->active_run_id();
    if (!run) return true;
    if (m_impl->repository_proof_ready) {
      m_impl->repository_proof_ready = false;
      return true;
    }
    if (m_impl->repository_work) return false;
    auto recorded = runtime::recorded_repository_context_admission(
        m_impl->kernel->event_log(), *run);
    if (!recorded) {
      auto failure = kernel_error(recorded.error());
      m_impl->repository_block = ChatRecoveryBlock{session_id(), *run, failure};
      return std::unexpected(std::move(failure));
    }
    if (!*recorded) return true;
    if (m_impl->repository_controller == nullptr) {
      ChatSessionError failure{ChatSessionErrorCode::context_failed,
                               "recorded repository context is unavailable"};
      m_impl->repository_block = ChatRecoveryBlock{session_id(), *run, failure};
      return std::unexpected(std::move(failure));
    }
    if (m_impl->async_repository_preparation) {
      if (m_impl->repository_generation ==
          std::numeric_limits<std::uint64_t>::max())
        return error(ChatSessionErrorCode::context_failed,
                     "repository preparation identity exhausted");
      m_impl->repository_work = ChatRepositoryWork{
          {session_id(), m_impl->model_id, ++m_impl->repository_generation,
           m_impl->repository_selection
               ? m_impl->repository_selection->selection_revision
               : 0,
           purpose},
          **recorded};
      return false;
    }
    auto prepared = m_impl->repository_controller->revalidate(
        **recorded, m_impl->stop_token);
    if (!prepared) {
      ChatSessionError failure{ChatSessionErrorCode::context_failed,
                               prepared.error().message};
      m_impl->repository_block = ChatRecoveryBlock{session_id(), *run, failure};
      return std::unexpected(std::move(failure));
    }
    m_impl->repository_admission = prepared->admission;
    m_impl->repository_prepared = std::move(*prepared);
    m_impl->repository_block.reset();
    return true;
  } catch (...) {
    m_impl->repository_proof_ready = false;
    return error(ChatSessionErrorCode::internal_failure,
                 "repository source validation failed internally");
  }
}

auto ChatSession::retry_repository_context()
    -> std::expected<void, ChatSessionError> {
  try {
    if (!m_impl->kernel->active_run_id())
      return error(
          ChatSessionErrorCode::run_failed,
          "reopen the session before retrying blocked source validation");
    if (m_impl->repository_work)
      return error(ChatSessionErrorCode::run_failed,
                   "repository source validation is already pending");
    const auto purpose = m_impl->recovered_pending_run_validation_required ||
                                 m_impl->kernel->pending_question_input() ||
                                 m_impl->kernel->pending_tool_approval()
                             ? ChatRepositoryWorkPurpose::recovery
                             : ChatRepositoryWorkPurpose::continuation;
    auto ready = validate_active_evidence(purpose);
    if (!ready) return std::unexpected(std::move(ready.error()));
    return {};
  } catch (...) {
    return error(ChatSessionErrorCode::internal_failure,
                 "repository source retry failed internally");
  }
}

auto ChatSession::validate_repository_completion(
    const ChatRepositoryWork& work,
    const runtime::PreparedRepositoryContext& prepared) const
    -> std::expected<void, ChatSessionError> {
  if (const auto* request =
          std::get_if<runtime::RepositoryContextRequest>(&work.input)) {
    const auto target = request->target_subtree == "."
                            ? std::string{}
                            : request->target_subtree;
    if (prepared.admission.target_subtree != target ||
        prepared.admission.selection_revision != request->selection_revision ||
        prepared.admission.evidence.size() != request->evidence_paths.size())
      return error(ChatSessionErrorCode::context_failed,
                   "repository preparation does not match selection");
    for (std::size_t index{}; index < request->evidence_paths.size(); ++index)
      if (prepared.admission.evidence[index].source.relative_path !=
          request->evidence_paths[index])
        return error(
            ChatSessionErrorCode::context_failed,
            "repository evidence preparation does not match selection");
  } else if (!domain::repository_context_admission_successor(
                 std::get<domain::RepositoryContextAdmission>(work.input),
                 prepared.admission)) {
    return error(ChatSessionErrorCode::context_failed,
                 "repository continuation preparation changed recorded inputs");
  }
  if (m_impl->repository_id &&
      prepared.snapshot.root.repository_id != *m_impl->repository_id)
    return error(ChatSessionErrorCode::context_failed,
                 "repository preparation belongs to another repository");
  return {};
}

auto ChatSession::apply_repository_completion(
    ChatRepositoryWork work, runtime::PreparedRepositoryContext prepared,
    std::optional<std::string> prompt,
    std::function<std::expected<void, ChatSessionError>()> action)
    -> std::expected<ChatRepositoryWorkOutcome, ChatSessionError> {
  ChatRepositoryWorkOutcome outcome;
  m_impl->repository_message.clear();
  if (work.token.purpose == ChatRepositoryWorkPurpose::selection) {
    auto request = std::get<runtime::RepositoryContextRequest>(work.input);
    request.target_subtree = prepared.admission.target_subtree;
    m_impl->repository_selection = std::move(request);
    m_impl->repository_prepared = std::move(prepared);
    m_impl->repository_admission.reset();
    outcome.selection_changed = true;
    return outcome;
  }
  if (work.token.purpose == ChatRepositoryWorkPurpose::submit) {
    if (!prompt)
      return error(ChatSessionErrorCode::internal_failure,
                   "repository submission draft is unavailable");
    auto submitted = submit_prepared(std::move(*prompt), std::move(prepared));
    if (!submitted) return std::unexpected(std::move(submitted.error()));
    outcome.events = std::move(submitted->committed_events);
    outcome.submitted = true;
    return outcome;
  }
  m_impl->repository_admission = prepared.admission;
  m_impl->repository_prepared = std::move(prepared);
  m_impl->repository_block.reset();
  m_impl->repository_proof_ready = true;
  struct ClearProof {
    bool& ready;
    ~ClearProof() { ready = false; }
  } clear_proof{m_impl->repository_proof_ready};
  if (action) {
    auto applied = action();
    if (!applied) return std::unexpected(std::move(applied.error()));
  }
  auto events = drain();
  if (!events) return std::unexpected(std::move(events.error()));
  outcome.events = std::move(*events);
  m_impl->repository_proof_ready = false;
  return outcome;
}

auto ChatSession::complete_repository_work(
    ChatRepositoryWorkCompletion completion)
    -> std::expected<ChatRepositoryWorkOutcome, ChatSessionError> {
  try {
    if (m_impl->stop_token.stop_requested()) {
      cancel_repository_work();
      return error(ChatSessionErrorCode::cancelled,
                   "repository preparation was cancelled");
    }
    const auto revision = m_impl->repository_selection
                              ? m_impl->repository_selection->selection_revision
                              : 0;
    if (!m_impl->repository_work ||
        m_impl->repository_work->token != completion.token ||
        completion.token.session_id != session_id() ||
        completion.token.model_id != m_impl->model_id ||
        completion.token.selection_revision != revision)
      return error(ChatSessionErrorCode::context_failed,
                   "repository preparation became stale");
    auto work = std::move(*m_impl->repository_work);
    m_impl->repository_work.reset();
    auto prompt = std::move(m_impl->repository_prompt);
    m_impl->repository_prompt.reset();
    auto action = std::move(m_impl->repository_action);
    m_impl->repository_action = {};
    if (!completion.result) {
      m_impl->repository_message = completion.result.error().message;
      ChatSessionError failure{
          completion.result.error().code ==
                  domain::RepositoryContextErrorCode::cancelled
              ? ChatSessionErrorCode::cancelled
              : ChatSessionErrorCode::context_failed,
          m_impl->repository_message};
      if (const auto run = m_impl->kernel->active_run_id())
        m_impl->repository_block =
            ChatRecoveryBlock{session_id(), *run, failure};
      if (m_impl->evidence_work) cancel_evidence_work();
      return std::unexpected(std::move(failure));
    }
    auto prepared = std::move(*completion.result);
    auto matched = validate_repository_completion(work, prepared);
    if (!matched) {
      if (m_impl->evidence_work) cancel_evidence_work();
      return std::unexpected(std::move(matched.error()));
    }
    if (m_impl->evidence_work) {
      m_impl->repository_action = std::move(action);
      m_impl->evidence_work->repository = std::move(prepared);
      auto begun = begin_local_evidence_work();
      if (!begun) {
        cancel_evidence_work();
        return std::unexpected(begun.error());
      }
      return ChatRepositoryWorkOutcome{{}, false, false, true};
    }
    return apply_repository_completion(std::move(work), std::move(prepared),
                                       std::move(prompt), std::move(action));
  } catch (...) {
    return error(ChatSessionErrorCode::internal_failure,
                 "repository preparation completion failed internally");
  }
}

namespace {

[[nodiscard]] auto validate_recovered_persona_document(
    persona::PersonaSource* source, const persona::PersonaLimits limits,
    const domain::PersonaDocument& expected, const std::stop_token stop_token)
    -> std::expected<void, ChatSessionError> {
  if (source == nullptr) {
    return error(ChatSessionErrorCode::context_failed,
                 "recorded persona source is unavailable");
  }
  auto loaded = source->load(expected.reference.name, limits, stop_token);
  if (!loaded) return std::unexpected(persona_error(loaded.error()));
  if (!domain::validate_persona_document(*loaded)) {
    return error(ChatSessionErrorCode::context_failed,
                 "recorded persona document is invalid");
  }
  if (!persona_document_matches_reference(*loaded, expected.reference)) {
    return error(ChatSessionErrorCode::context_failed,
                 "persona changed since this run started");
  }
  return {};
}

} // namespace

// Insert within namespace aiforge::surfaces after ChatSession::Impl definition.
auto ChatSession::conversation_policy() const
    -> std::expected<runtime::ConversationPolicySnapshot, ChatSessionError> {
  try {
    auto policy =
        runtime::recorded_conversation_policy(m_impl->kernel->event_log());
    if (!policy)
      return error(ChatSessionErrorCode::session_failed,
                   policy.error().message);
    return std::move(*policy);
  } catch (...) {
    return error(ChatSessionErrorCode::internal_failure,
                 "conversation policy inspection failed");
  }
}

auto ChatSession::set_conversation_policy(
    std::uint64_t expected_revision, domain::ConversationMode mode,
    std::vector<domain::RunId> pinned_run_ids)
    -> std::expected<std::vector<domain::RunEvent>, ChatSessionError> {
  bool committed{};
  try {
    if (m_impl->stop_token.stop_requested())
      return error(ChatSessionErrorCode::cancelled,
                   "conversation policy change cancelled");
    const auto suffix = m_impl->identity_suffix_source();
    auto run = make_id<domain::RunId>("conversation-policy", suffix);
    auto surface = make_id<domain::SurfaceId>("session-policy", suffix);
    auto workspace = make_id<domain::WorkspaceId>("chat", suffix);
    auto permission = make_id<domain::PermissionProfileId>("observe", suffix);
    if (!run || !surface || !workspace || !permission)
      return error(ChatSessionErrorCode::internal_failure,
                   "conversation policy identity generation failed");
    const auto before = m_impl->kernel->event_log().events().size();
    auto changed = m_impl->kernel->record_conversation_policy(
        {*run,
         {*surface, *workspace,
          m_impl->permission_profile_id.value_or(*permission),
          m_impl->persona_document
              ? std::optional<domain::PersonaId>{m_impl->persona_document
                                                     ->reference.persona_id}
              : std::nullopt,
          std::nullopt, domain::RunPurpose::control},
         expected_revision,
         mode,
         std::move(pinned_run_ids)});
    if (!changed) return std::unexpected(kernel_error(changed.error()));
    committed = true;
    const auto& events = m_impl->kernel->event_log().events();
    return std::vector<domain::RunEvent>{
        events.begin() + static_cast<std::ptrdiff_t>(before), events.end()};
  } catch (...) {
    return std::unexpected(
        ChatSessionError{ChatSessionErrorCode::internal_failure,
                         committed ? "conversation policy committed but its "
                                     "result could not be returned"
                                   : "conversation policy change failed",
                         false, committed});
  }
}

struct ChatSummaryReviewData {
  runtime::SummaryPreview review;
  std::uint64_t identity;
  std::optional<domain::RepositoryContextAdmission> repository;
  std::optional<domain::LocalContextAdmission> local;
  domain::ConstructedContext context;
};
ChatSummaryPreview::ChatSummaryPreview(
    std::shared_ptr<const ChatSummaryReviewData> data)
    : m_data(std::move(data)) {
}
auto ChatSummaryPreview::context() const noexcept
    -> const domain::ConstructedContext& {
  return m_data->context;
}
auto ChatSummaryPreview::activation() const noexcept
    -> const domain::ConversationSummaryActivation& {
  return m_data->review.activation();
}

auto ChatSummaryPreview::local_admission() const noexcept
    -> const std::optional<domain::LocalContextAdmission>& {
  return m_data->local;
}

auto ChatSession::summary_control_attributes(std::uint64_t suffix) const
    -> std::expected<domain::RunStarted, ChatSessionError> {
  auto surface = make_id<domain::SurfaceId>("summary-control", suffix);
  auto workspace = make_id<domain::WorkspaceId>("chat", suffix);
  auto permission = make_id<domain::PermissionProfileId>("observe", suffix);
  if (!surface || !workspace || !permission)
    return error(ChatSessionErrorCode::internal_failure,
                 "summary control identity is invalid");
  return domain::RunStarted{
      *surface, *workspace, m_impl->permission_profile_id.value_or(*permission),
      {},       {},         domain::RunPurpose::control};
}

auto ChatSession::generate_conversation_summary(ChatSummaryGenerate request)
    -> std::expected<ChatSummaryGeneration, ChatSessionError> {
  bool committed{};
  try {
    if (m_impl->stop_token.stop_requested())
      return error(ChatSessionErrorCode::cancelled,
                   "summary generation cancelled");
    if (active())
      return error(ChatSessionErrorCode::run_failed,
                   "summary generation requires an idle session");
    const auto& log = m_impl->kernel->event_log();
    if (request.expected_sequence != log.last_sequence())
      return error(ChatSessionErrorCode::context_failed,
                   "summary source selection is stale");
    if (request.maximum_output_bytes == 0 ||
        request.maximum_output_bytes > domain::summary_maximum_text_bytes)
      return error(ChatSessionErrorCode::invalid_input,
                   "summary output bound is invalid");
    auto sources = runtime::prepare_conversation_summary_sources(
        {log, std::move(request.covered_run_ids)}, m_impl->stop_token);
    if (!sources)
      return error(ChatSessionErrorCode::context_failed,
                   sources.error().message);
    const auto suffix = m_impl->identity_suffix_source();
    auto attributes = summary_control_attributes(suffix);
    auto run = make_id<domain::RunId>("summary-run", suffix);
    auto summary = make_id<domain::ConversationSummaryId>("summary", suffix);
    auto inference = make_id<domain::InferenceId>("summary-inference", suffix);
    auto output = make_id<domain::MessageId>("summary-output", suffix);
    if (!attributes || !run || !summary || !inference || !output)
      return error(ChatSessionErrorCode::internal_failure,
                   "summary generation identities are invalid");
    auto prepared = runtime::prepare_conversation_summary_generation(
        log,
        {1,
         1,
         *summary,
         std::move(sources->sources),
         *run,
         *inference,
         m_impl->model_id,
         *output,
         m_impl->runtime_version,
         {m_impl->model.context_window_tokens, m_impl->output_tokens, 0},
         0,
         request.maximum_output_bytes,
         {}},
        {}, m_impl->stop_token);
    if (!prepared)
      return error(ChatSessionErrorCode::context_failed,
                   prepared.error().message);
    if (auto spend = runtime::preflight_inference_spend(log); !spend)
      return std::unexpected(inference_spend_error(spend.error()));
    attributes->purpose = domain::RunPurpose::summary;
    backend::GenerationOptions options;
    options.max_output_tokens = m_impl->output_tokens;
    const auto before = log.events().size();
    auto started = m_impl->kernel->start({*run,
                                          *attributes,
                                          prepared->user_message,
                                          {*inference,
                                           *output,
                                           m_impl->model_id,
                                           std::move(prepared->context),
                                           {},
                                           options,
                                           {}},
                                          {},
                                          {},
                                          m_impl->model.pricing_observation,
                                          {},
                                          {},
                                          std::move(prepared->intent)});
    committed = log.events().size() != before;
    if (!started) {
      auto failure = kernel_error(started.error());
      failure.effect_may_have_applied = committed;
      return std::unexpected(std::move(failure));
    }
    return ChatSummaryGeneration{
        *summary,
        *run,
        {log.events().begin() + static_cast<std::ptrdiff_t>(before),
         log.events().end()}};
  } catch (...) {
    return std::unexpected(
        ChatSessionError{ChatSessionErrorCode::internal_failure,
                         "summary generation failed", false, committed});
  }
}

auto ChatSession::summary_catalog() const
    -> std::expected<ChatSummaryCatalog, ChatSessionError> {
  try {
    auto snapshot = runtime::SummaryController{*m_impl->kernel}.inspect();
    if (!snapshot)
      return error(ChatSessionErrorCode::context_failed,
                   snapshot.error().message);
    ChatSummaryCatalog result{std::move(*snapshot), {}};
    std::set<domain::RunId> complete;
    for (const auto& event : m_impl->kernel->event_log().events())
      if (std::holds_alternative<domain::RunCompleted>(event.payload))
        complete.insert(event.metadata.run_id);
    for (const auto& intent : result.snapshot.intents) {
      if (!complete.contains(intent.producing_run_id) ||
          std::ranges::any_of(
              result.snapshot.candidates, [&](const auto& candidate) {
                return candidate.summary_id == intent.summary_id;
              }))
        continue;
      auto draft = runtime::recover_conversation_summary_draft(
          m_impl->kernel->event_log(), intent);
      if (!draft) {
        result.unpublishable.push_back(
            {intent.summary_id, draft.error().message});
        continue;
      }
      result.unpublished.push_back(std::move(*draft));
    }
    return result;
  } catch (...) {
    return error(ChatSessionErrorCode::internal_failure,
                 "summary catalog inspection failed");
  }
}

auto ChatSession::publish_conversation_summary(
    domain::ConversationSummaryId summary_id)
    -> std::expected<domain::ConversationSummaryCandidate, ChatSessionError> {
  const auto before = m_impl->kernel->event_log().last_sequence();
  try {
    const auto suffix = m_impl->identity_suffix_source();
    auto attributes = summary_control_attributes(suffix);
    auto run = make_id<domain::RunId>("summary-publication", suffix);
    if (!attributes || !run)
      return error(ChatSessionErrorCode::internal_failure,
                   "summary publication identity failed");
    auto published = runtime::SummaryController{*m_impl->kernel}.publish(
        {*run, *attributes, std::move(summary_id)});
    if (!published)
      return std::unexpected(ChatSessionError{
          ChatSessionErrorCode::context_failed, published.error().message,
          published.error().retryable,
          m_impl->kernel->event_log().last_sequence() != before});
    return std::move(*published);
  } catch (...) {
    return std::unexpected(ChatSessionError{
        ChatSessionErrorCode::internal_failure, "summary publication failed",
        false, m_impl->kernel->event_log().last_sequence() != before});
  }
}

auto ChatSession::edit_conversation_summary(
    std::uint64_t expected_sequence, domain::ConversationSummaryVersion parent,
    std::string text)
    -> std::expected<domain::ConversationSummaryCandidate, ChatSessionError> {
  const auto before = m_impl->kernel->event_log().last_sequence();
  try {
    const auto suffix = m_impl->identity_suffix_source();
    auto attributes = summary_control_attributes(suffix);
    auto run = make_id<domain::RunId>("summary-edit", suffix);
    if (!attributes || !run)
      return error(ChatSessionErrorCode::internal_failure,
                   "summary edit identity failed");
    auto edited = runtime::SummaryController{*m_impl->kernel}.edit(
        {*run, *attributes, expected_sequence, std::move(parent),
         std::move(text)});
    if (!edited)
      return std::unexpected(ChatSessionError{
          ChatSessionErrorCode::context_failed, edited.error().message,
          edited.error().retryable,
          m_impl->kernel->event_log().last_sequence() != before});
    return std::move(*edited);
  } catch (...) {
    return std::unexpected(ChatSessionError{
        ChatSessionErrorCode::internal_failure, "summary edit failed", false,
        m_impl->kernel->event_log().last_sequence() != before});
  }
}

auto ChatSession::summary_repository_context()
    -> std::expected<std::optional<runtime::PreparedRepositoryContext>,
                     ChatSessionError> {
  if (m_impl->evidence_executing) return m_impl->evidence_repository;
  if (m_impl->local_sources != nullptr)
    return error(ChatSessionErrorCode::context_failed,
                 "Use asynchronous context preparation for local files");
  if (m_impl->repository_work)
    return error(ChatSessionErrorCode::run_failed,
                 "repository preparation is still active");
  if (!m_impl->repository_selection)
    return std::optional<runtime::PreparedRepositoryContext>{};
  if (m_impl->repository_controller == nullptr)
    return error(ChatSessionErrorCode::context_failed,
                 "repository context is unavailable");
  auto prepared = m_impl->repository_controller->prepare(
      *m_impl->repository_selection, m_impl->stop_token);
  if (!prepared)
    return error(ChatSessionErrorCode::context_failed,
                 prepared.error().message);
  return std::optional{std::move(*prepared)};
}

namespace {
auto reviewed_persona_instruction(const domain::PersonaDocument& selected,
                                  persona::PersonaSource* source,
                                  const persona::PersonaLimits& limits,
                                  std::stop_token stop)
    -> std::expected<domain::InstructionInput, ChatSessionError> {
  if (source == nullptr)
    return error(ChatSessionErrorCode::context_failed,
                 "persona source is unavailable");
  auto loaded = source->load(selected.reference.name, limits, stop);
  if (!loaded || loaded->reference != selected.reference ||
      !domain::validate_persona_document(*loaded))
    return error(ChatSessionErrorCode::context_failed,
                 "persona changed since selection");
  auto instruction =
      runtime::persona_instruction_input(*loaded, loaded->text.size());
  if (!instruction)
    return error(ChatSessionErrorCode::context_failed,
                 instruction.error().message);
  return std::move(*instruction);
}
} // namespace

auto ChatSession::summary_mandatory_context(
    const std::string& draft, std::uint64_t identity,
    const std::optional<runtime::PreparedRepositoryContext>& repository)
    -> std::expected<domain::ContextBuildInput, ChatSessionError> {
  if (draft.size() > m_impl->limits.maximum_input_bytes || !valid_text(draft))
    return error(ChatSessionErrorCode::invalid_input,
                 "summary preview draft is invalid or too large");
  if (!m_impl->persona_attention.empty())
    return error(ChatSessionErrorCode::context_failed,
                 m_impl->persona_attention);
  auto profile = resolve_profile(
      m_impl->available_tools, m_impl->tool_selection(), *m_impl->tool_policy);
  if (!profile) return std::unexpected(profile.error());
  auto tools = tool_declaration_tokens(profile->effective_tools.declarations());
  if (!tools) return std::unexpected(tools.error());
  const auto suffix = std::to_string(identity);
  domain::ContextBuildInput input{
      {m_impl->model.context_window_tokens, m_impl->output_tokens, *tools},
      {{domain::ContextEntryId::from("summary-review-runtime-" + suffix)
            .value(),
        domain::InstructionLayer::application_runtime,
        domain::InstructionOperation::add,
        {},
        domain::Message{
            domain::MessageId::from("summary-review-runtime-message-" + suffix)
                .value(),
            domain::Role::system,
            {domain::TextBlock{std::string{detail::runtime_contract}}},
            {}},
        {domain::ContextSourceId::from("summary-review-runtime-source-" +
                                       suffix)
             .value(),
         "aiforge:runtime",
         {}},
        0,
        1,
        detail::runtime_contract.size()}},
      {{domain::ContextEntryId::from("summary-review-user-" + suffix).value(),
        domain::ContextContentKind::conversation,
        {domain::MessageId::from("summary-review-user-message-" + suffix)
             .value(),
         domain::Role::user,
         {domain::TextBlock{draft}},
         {}},
        {domain::ContextSourceId::from("summary-review-user-source-" + suffix)
             .value(),
         "interactive-composer-preview",
         {}},
        1,
        draft.size()}}};
  if (draft.empty()) {
    // This unsubmitted placeholder retains the empty composer exactly. Its
    // message envelope still consumes capacity; no user text is fabricated.
    auto estimated = estimated_message_tokens(input.content.front().message);
    if (!estimated) return std::unexpected(estimated.error());
    input.content.front().estimated_tokens = *estimated;
  }
  if (m_impl->persona_document) {
    auto instruction = reviewed_persona_instruction(
        *m_impl->persona_document, m_impl->persona_source,
        m_impl->persona_limits, m_impl->stop_token);
    if (!instruction) return std::unexpected(instruction.error());
    input.instructions.push_back(std::move(*instruction));
  }
  if (m_impl->user_global_instructions_enabled) {
    auto loaded = load_user_global_document(
        m_impl->user_global_instruction_source,
        m_impl->user_global_instruction_limits, m_impl->stop_token);
    if (!loaded) return std::unexpected(loaded.error());
    if (*loaded) {
      auto appended = append_user_global_instruction(input, **loaded);
      if (!appended) return std::unexpected(appended.error());
    }
  }
  if (repository)
    input.instructions.insert(input.instructions.end(),
                              repository->instructions.begin(),
                              repository->instructions.end());
  return input;
}

namespace {
struct SummaryFinalContext {
  domain::ConstructedContext context;
  std::optional<domain::RepositoryContextAdmission> repository;
  std::optional<domain::LocalContextAdmission> local;
};
auto final_summary_context(
    const runtime::SummaryPreview& preview,
    const std::optional<runtime::PreparedRepositoryContext>& repository,
    const std::optional<runtime::PreparedLocalContext>& local,
    std::stop_token stop)
    -> std::expected<SummaryFinalContext, ChatSessionError> {
  auto selected = runtime::select_session_evidence(preview.context(),
                                                   repository, local, stop);
  if (!selected) return std::unexpected(evidence_error(selected.error()));
  return SummaryFinalContext{std::move(selected->context),
                             std::move(selected->repository_admission),
                             std::move(selected->local_admission)};
}

} // namespace

namespace {
auto inspect_history_groups(const domain::SessionEventLog& log,
                            ChatConversationContextInspection& result,
                            std::stop_token stop)
    -> std::expected<void, ChatSessionError> {
  auto history = runtime::reconstruct_conversation_history({log}, stop);
  if (!history)
    return error(ChatSessionErrorCode::context_failed, history.error().message);
  for (const auto& group : *history) {
    std::uint64_t tokens{};
    for (const auto& entry : group.entries) {
      if (entry.content.estimated_tokens >
          std::numeric_limits<std::uint64_t>::max() - tokens)
        return error(ChatSessionErrorCode::context_failed,
                     "conversation inspection estimate overflowed");
      tokens += entry.content.estimated_tokens;
    }
    result.groups.push_back(
        {group.run_id,
         group.entries.size(),
         tokens,
         std::ranges::find(result.policy.policy.pinned_run_ids, group.run_id) !=
             result.policy.policy.pinned_run_ids.end(),
         {}});
  }
  auto summaries =
      runtime::prepare_conversation_summary_context(log, 1, {}, stop);
  if (!summaries)
    return error(ChatSessionErrorCode::context_failed,
                 summaries.error().message);
  result.summaries = std::move(summaries->summaries);
  return {};
}
auto inspect_selection_decisions(const domain::SessionEventLog& log,
                                 ChatConversationContextInspection& result,
                                 const domain::ConversationAdmission& admission,
                                 std::stop_token stop)
    -> std::expected<void, ChatSessionError> {
  auto mandatory = admission.mandatory_input_tokens;
  for (const auto& summary : admission.summaries) {
    if (summary.estimated_tokens > mandatory)
      return error(ChatSessionErrorCode::context_failed,
                   "conversation inspection summary accounting is invalid");
    mandatory -= summary.estimated_tokens;
  }
  auto selected = runtime::prepare_conversation_context(
      {log, result.model_id, admission.capacity, mandatory, 1, {}, {}}, stop);
  if (!selected)
    return error(ChatSessionErrorCode::context_failed,
                 selected.error().message);
  if (selected->selection.decisions.size() != result.groups.size())
    return error(ChatSessionErrorCode::context_failed,
                 "conversation inspection source count changed");
  for (std::size_t index{}; index < result.groups.size(); ++index)
    result.groups[index].decision =
        selected->selection.decisions[index].decision;
  return {};
}
} // namespace

auto ChatSession::inspect_conversation_context(std::string draft)
    -> std::expected<ChatConversationContextInspection, ChatSessionError> {
  try {
    if (m_impl->stop_token.stop_requested())
      return error(ChatSessionErrorCode::cancelled,
                   "context inspection cancelled");
    auto policy = conversation_policy();
    if (!policy) return std::unexpected(policy.error());
    auto repository = summary_repository_context();
    if (!repository) return std::unexpected(repository.error());
    auto input = summary_mandatory_context(
        draft, m_impl->identity_suffix_source(), *repository);
    if (!input) return std::unexpected(input.error());
    ChatConversationContextInspection result{
        *policy, m_impl->model_id, std::move(*input), {}, {}};
    const auto& log = m_impl->kernel->event_log();
    if (const auto* attributes =
            context_run_attributes(log, m_impl->kernel->active_run_id()))
      result.active_admission = attributes->conversation_admission;
    auto groups = inspect_history_groups(log, result, m_impl->stop_token);
    if (!groups) return std::unexpected(groups.error());
    auto prepared = runtime::prepare_session_context(
        {log,
         m_impl->model_id,
         result.mandatory,
         m_impl->memory_controller,
         {m_impl->repository_id,
          m_impl->persona_document
              ? std::optional{m_impl->persona_document->reference.persona_id}
              : std::nullopt,
          m_impl->memory_settings.context_tokens, 0, true},
         {},
         {}},
        m_impl->stop_token);
    if (!prepared) {
      result.preparation_error = ChatSessionError{
          ChatSessionErrorCode::context_failed, prepared.error().message,
          prepared.error().retryable};
      return result;
    }
    auto decisions = inspect_selection_decisions(
        log, result, prepared->conversation_admission, m_impl->stop_token);
    if (!decisions) return std::unexpected(decisions.error());
    auto selected = runtime::select_session_evidence(
        *prepared, *repository, m_impl->evidence_local, m_impl->stop_token);
    if (!selected) {
      result.preparation_error = evidence_error(selected.error());
      return result;
    }
    result.local_admission = std::move(selected->local_admission);
    result.next_context = std::move(selected->context);
    result.next_admission = std::move(selected->session.conversation_admission);
    return result;
  } catch (...) {
    return error(ChatSessionErrorCode::internal_failure,
                 "context inspection failed");
  }
}

auto ChatSession::preview_conversation_summary(
    domain::ConversationSummaryVersion candidate,
    std::vector<domain::ConversationSummaryVersion> replacements,
    std::string draft) -> std::expected<ChatSummaryPreview, ChatSessionError> {
  try {
    const auto identity = m_impl->identity_suffix_source();
    auto attributes = summary_control_attributes(identity);
    auto run = make_id<domain::RunId>("summary-activation", identity);
    if (!attributes || !run)
      return error(ChatSessionErrorCode::internal_failure,
                   "summary activation identity failed");
    auto policy = conversation_policy();
    if (!policy) return std::unexpected(policy.error());
    auto repository = summary_repository_context();
    if (!repository) return std::unexpected(repository.error());
    auto input = summary_mandatory_context(draft, identity, *repository);
    if (!input) return std::unexpected(input.error());
    runtime::SummaryContextRequest request{
        m_impl->model_id,
        *input,
        m_impl->memory_controller,
        {m_impl->repository_id,
         m_impl->persona_document
             ? std::optional{m_impl->persona_document->reference.persona_id}
             : std::nullopt,
         m_impl->memory_settings.context_tokens, 0, true}};
    auto review = runtime::SummaryController{*m_impl->kernel}.preview(
        {*run, *attributes, m_impl->kernel->event_log().last_sequence(),
         policy->policy.revision, std::move(candidate),
         std::move(replacements)},
        request, m_impl->stop_token);
    if (!review)
      return error(ChatSessionErrorCode::context_failed, review.error().message,
                   review.error().retryable);
    auto final = final_summary_context(
        *review, *repository, m_impl->evidence_local, m_impl->stop_token);
    if (!final) return std::unexpected(final.error());
    return ChatSummaryPreview{
        std::make_shared<ChatSummaryReviewData>(ChatSummaryReviewData{
            std::move(*review), identity, std::move(final->repository),
            std::move(final->local), std::move(final->context)})};
  } catch (...) {
    return error(ChatSessionErrorCode::internal_failure,
                 "summary preview failed");
  }
}

auto ChatSession::apply_conversation_summary(const ChatSummaryPreview& preview,
                                             std::string current_draft)
    -> std::expected<domain::ConversationSummaryActivation, ChatSessionError> {
  const auto before = m_impl->kernel->event_log().last_sequence();
  try {
    if (!preview.m_data)
      return error(ChatSessionErrorCode::invalid_input,
                   "summary review is unavailable");
    auto repository = summary_repository_context();
    if (!repository) return std::unexpected(repository.error());
    auto input = summary_mandatory_context(
        current_draft, preview.m_data->identity, *repository);
    if (!input) return std::unexpected(input.error());
    // Rebuild the optional Dev evidence with the reviewed base. The controller
    // separately rebuilds that base against current draft/model/memory below.
    auto final =
        final_summary_context(preview.m_data->review, *repository,
                              m_impl->evidence_local, m_impl->stop_token);
    if (!final) return std::unexpected(final.error());
    if (final->local != preview.m_data->local ||
        final->repository != preview.m_data->repository ||
        final->context != preview.m_data->context)
      return error(ChatSessionErrorCode::context_failed,
                   "repository context changed since summary review");
    auto applied = runtime::SummaryController{*m_impl->kernel}.apply(
        preview.m_data->review,
        {m_impl->model_id,
         *input,
         m_impl->memory_controller,
         {m_impl->repository_id,
          m_impl->persona_document
              ? std::optional{m_impl->persona_document->reference.persona_id}
              : std::nullopt,
          m_impl->memory_settings.context_tokens, 0, true}},
        m_impl->stop_token);
    if (!applied)
      return std::unexpected(ChatSessionError{
          ChatSessionErrorCode::context_failed, applied.error().message,
          applied.error().retryable,
          m_impl->kernel->event_log().last_sequence() != before});
    return std::move(*applied);
  } catch (...) {
    return std::unexpected(ChatSessionError{
        ChatSessionErrorCode::internal_failure, "summary activation failed",
        false, m_impl->kernel->event_log().last_sequence() != before});
  }
}

auto ChatSession::disable_conversation_summary(
    std::uint64_t expected_policy_revision,
    domain::ConversationSummaryVersion candidate,
    domain::EventId activation_event_id)
    -> std::expected<std::vector<domain::RunEvent>, ChatSessionError> {
  bool committed{};
  try {
    if (m_impl->stop_token.stop_requested())
      return error(ChatSessionErrorCode::cancelled,
                   "conversation summary disable cancelled");
    const auto suffix = m_impl->identity_suffix_source();
    auto run = make_id<domain::RunId>("summary-disable", suffix);
    auto surface = make_id<domain::SurfaceId>("session-policy", suffix);
    auto workspace = make_id<domain::WorkspaceId>("chat", suffix);
    auto permission = make_id<domain::PermissionProfileId>("observe", suffix);
    if (!run || !surface || !workspace || !permission)
      return error(ChatSessionErrorCode::internal_failure,
                   "conversation summary identity generation failed");
    const auto before = m_impl->kernel->event_log().events().size();
    auto changed = m_impl->kernel->disable_conversation_summary(
        {*run,
         {*surface, *workspace,
          m_impl->permission_profile_id.value_or(*permission),
          m_impl->persona_document
              ? std::optional<domain::PersonaId>{m_impl->persona_document
                                                     ->reference.persona_id}
              : std::nullopt,
          std::nullopt, domain::RunPurpose::control},
         m_impl->kernel->event_log().last_sequence(),
         expected_policy_revision,
         std::move(candidate),
         std::move(activation_event_id)});
    if (!changed) return std::unexpected(kernel_error(changed.error()));
    committed = true;
    const auto& events = m_impl->kernel->event_log().events();
    return std::vector<domain::RunEvent>{
        events.begin() + static_cast<std::ptrdiff_t>(before), events.end()};
  } catch (...) {
    return std::unexpected(
        ChatSessionError{ChatSessionErrorCode::internal_failure,
                         committed ? "conversation summary disabled but its "
                                     "result could not be returned"
                                   : "conversation summary disable failed",
                         false, committed});
  }
}

auto ChatSession::validate_recovered_pending_run(bool repository_validated)
    -> std::expected<void, ChatSessionError> {
  const auto run_id = m_impl->kernel->active_run_id();
  const auto* attributes =
      context_run_attributes(m_impl->kernel->event_log(), run_id);
  if (attributes != nullptr &&
      attributes->purpose == domain::RunPurpose::summary)
    return {};

  if (!run_id) {
    if (m_impl->repository_block)
      return std::unexpected(m_impl->repository_block->reason);
    if (m_impl->recovery_block)
      return std::unexpected(m_impl->recovery_block->reason);
  }
  if (m_impl->recovered_sources_pinned) return {};
  const bool previous_repository_pin = m_impl->repository_recovery_pinned;
  auto validated = load_recovered_pending_sources();
  if (validated) validated = validate_recovered_memory_capacity();
  // Decision callers have already completed the exact repository proof.
  // Freeze summaries only after the remaining source/capacity checks succeed.
  if (validated && repository_validated)
    m_impl->repository_recovery_pinned = true;
  if (validated) validated = pin_recovered_summary_sources();
  if (!validated) m_impl->repository_recovery_pinned = previous_repository_pin;
  if (!validated && run_id &&
      validated.error().code != ChatSessionErrorCode::cancelled) {
    m_impl->recovery_block =
        ChatRecoveryBlock{session_id(), *run_id, validated.error()};
  } else {
    m_impl->recovery_block.reset();
  }
  return validated;
}

namespace {
struct RecoveryCapacityBudget {
  std::uint64_t remaining{};
  bool fits{true};
  auto consume(const std::uint64_t tokens) -> void {
    if (tokens > remaining) {
      fits = false;
      return;
    }
    remaining -= tokens;
  }
};

[[nodiscard]] auto consume_recovered_tool_messages(
    const domain::SessionEventLog& log,
    const std::optional<domain::RunId>& run_id, RecoveryCapacityBudget& budget)
    -> std::expected<void, ChatSessionError> {
  try {
    if (!run_id)
      return error(ChatSessionErrorCode::context_failed,
                   "original tool continuation has no active run");
    auto messages = runtime::reconstruct_active_tool_continuation(log, *run_id);
    if (!messages)
      return error(ChatSessionErrorCode::context_failed,
                   "original tool continuation cannot be reconstructed");
    for (const auto& message : *messages) {
      auto tokens =
          estimated_message_tokens(message, legacy_context(log, run_id));
      if (!tokens) return std::unexpected(std::move(tokens.error()));
      budget.consume(*tokens);
    }
    return {};
  } catch (...) {
    return error(ChatSessionErrorCode::internal_failure,
                 "tool continuation capacity validation failed internally");
  }
}

[[nodiscard]] auto consume_recovered_repository(
    const std::optional<domain::RepositoryContextAdmission>& admission,
    const domain::ContextCapacity capacity, RecoveryCapacityBudget& budget)
    -> std::expected<void, ChatSessionError> {
  if (!admission) return {};
  if (admission->capacity != capacity)
    return error(ChatSessionErrorCode::context_failed,
                 "original repository context model capacity changed");
  for (const auto& instruction : admission->instructions)
    budget.consume(instruction.estimated_tokens);
  for (const auto& evidence : admission->evidence)
    if (evidence.decision == domain::RepositoryContextDecision::admitted)
      budget.consume(evidence.estimated_tokens);
  return {};
}
} // namespace

auto ChatSession::pin_recovered_summary_sources()
    -> std::expected<void, ChatSessionError> {
  if (!m_impl->recovered_pending_run_validation_required ||
      m_impl->recovered_summary_context ||
      (m_impl->repository_admission && !m_impl->repository_recovery_pinned))
    return {};
  const auto run = m_impl->kernel->active_run_id();
  const auto* attributes =
      context_run_attributes(m_impl->kernel->event_log(), run);
  if (!run || attributes == nullptr || !attributes->conversation_admission)
    return {};
  const auto& admission = *attributes->conversation_admission;
  if (admission.version != 2 ||
      admission.mode != domain::ConversationMode::rolling)
    return {};
  auto restored = runtime::recover_conversation_summary_context(
      m_impl->kernel->event_log(), admission.summaries,
      admission.source_snapshot_sequence, {}, m_impl->stop_token);
  if (!restored)
    return error(ChatSessionErrorCode::context_failed,
                 restored.error().message);
  auto pinned = m_impl->kernel->pin_conversation_summaries(*run);
  if (!pinned) return std::unexpected(kernel_error(pinned.error()));
  m_impl->recovered_summary_context = std::move(restored->content);
  return {};
}

namespace {
auto consume_recovered_local(
    const std::optional<domain::LocalContextAdmission>& admission,
    domain::ContextCapacity capacity, RecoveryCapacityBudget& budget)
    -> std::expected<void, ChatSessionError> {
  if (!admission) return {};
  if (admission->capacity != capacity)
    return error(ChatSessionErrorCode::context_failed,
                 "Original local evidence capacity changed");
  for (const auto& ref : admission->evidence)
    if (ref.decision == domain::LocalContextDecision::admitted)
      budget.consume(ref.estimated_tokens);
  return {};
}
auto consume_recovered_evidence(
    const std::optional<domain::RepositoryContextAdmission>& repository,
    const std::optional<domain::LocalContextAdmission>& local,
    domain::ContextCapacity capacity, RecoveryCapacityBudget& budget)
    -> std::expected<void, ChatSessionError> {
  auto result = consume_recovered_repository(repository, capacity, budget);
  if (!result) return result;
  return consume_recovered_local(local, capacity, budget);
}
} // namespace

auto ChatSession::validate_recovered_memory_capacity()
    -> std::expected<void, ChatSessionError> {
  if (!m_impl->recovered_pending_run_validation_required) return {};
  auto history = recovered_conversation_input(
      m_impl->kernel->event_log(), m_impl->kernel->active_run_id(), 0,
      frozen_summary_content(m_impl->recovered_summary_context));
  if (!history)
    return error(ChatSessionErrorCode::context_failed,
                 "original saved memory conversation cannot be reconstructed");
  const auto* tools = m_impl->kernel->active_tool_declarations();
  if (tools == nullptr)
    return error(ChatSessionErrorCode::context_failed,
                 "original saved memory tool context is unavailable");
  const bool legacy = legacy_context(m_impl->kernel->event_log(),
                                     m_impl->kernel->active_run_id());
  auto declarations = tool_declaration_tokens(*tools, legacy);
  if (!declarations) return std::unexpected(declarations.error());
  const auto* attributes = context_run_attributes(
      m_impl->kernel->event_log(), m_impl->kernel->active_run_id());
  if (attributes != nullptr && attributes->conversation_admission) {
    const auto& admission = *attributes->conversation_admission;
    if (admission.model_id != m_impl->model_id ||
        admission.capacity !=
            domain::ContextCapacity{m_impl->model.context_window_tokens,
                                    m_impl->output_tokens, *declarations})
      return error(ChatSessionErrorCode::context_failed,
                   "original conversation model capacity changed");
  }
  RecoveryCapacityBudget budget{m_impl->model.context_window_tokens};
  budget.consume(m_impl->output_tokens);
  budget.consume(*declarations);
  budget.consume(detail::runtime_contract.size());
  for (const auto& entry : *history)
    budget.consume(entry.estimated_tokens);
  if (m_impl->recovered_persona_document)
    budget.consume(m_impl->recovered_persona_document->text.size());
  if (m_impl->recovered_user_global_instruction)
    budget.consume(m_impl->recovered_user_global_instruction->text.size());
  auto evidence_fits = consume_recovered_evidence(
      m_impl->repository_admission, m_impl->local_admission,
      {m_impl->model.context_window_tokens, m_impl->output_tokens,
       *declarations},
      budget);
  if (!evidence_fits) return evidence_fits;
  auto count = history->size();
  for (const auto& memory : m_impl->recovered_memory_context) {
    if (memory.order == 0 || (legacy && memory.order > count + 1))
      return error(
          ChatSessionErrorCode::context_failed,
          "original saved memory admission order cannot be reconstructed");
    ++count;
    budget.consume(memory.estimated_tokens);
  }
  auto continuation_fits = consume_recovered_tool_messages(
      m_impl->kernel->event_log(), m_impl->kernel->active_run_id(), budget);
  if (!continuation_fits) return continuation_fits;
  if (!budget.fits)
    return error(ChatSessionErrorCode::context_failed,
                 "original saved memory no longer fits model context capacity");
  return {};
}

auto ChatSession::load_recovered_memory()
    -> std::expected<void, ChatSessionError> {
  const auto run_id = m_impl->kernel->active_run_id();
  if (!run_id || !m_impl->recovered_run ||
      m_impl->recovered_run->run_id != *run_id ||
      !m_impl->recovered_run->attributes.memory_selection) {
    return error(ChatSessionErrorCode::context_failed,
                 "original saved memory selection is unavailable for this "
                 "legacy run; cancel it to start a new run");
  }
  const auto& selection = *m_impl->recovered_run->attributes.memory_selection;
  if (selection.persona_id != m_impl->recovered_run->attributes.persona_id ||
      !domain::validate_memory_selection(selection)) {
    return error(ChatSessionErrorCode::context_failed,
                 "original saved memory selection is invalid");
  }
  if (!selection.entries.empty()) {
    if (m_impl->memory_controller == nullptr)
      return error(ChatSessionErrorCode::context_failed,
                   "original saved memory journal is unavailable");
    auto restored = m_impl->memory_controller->restore_context(selection);
    if (!restored)
      return error(restored.error().code ==
                           runtime::MemoryControllerErrorCode::cancelled
                       ? ChatSessionErrorCode::cancelled
                       : ChatSessionErrorCode::context_failed,
                   "original saved memory cannot be restored: " +
                       restored.error().message,
                   restored.error().retryable);
    m_impl->recovered_memory_context = std::move(*restored);
  } else {
    m_impl->recovered_memory_context.clear();
  }
  return {};
}

auto ChatSession::load_recovered_pending_sources()
    -> std::expected<void, ChatSessionError> {
  if (!m_impl->recovered_pending_run_validation_required) return {};
  const auto run_id = m_impl->kernel->active_run_id();
  if (!run_id) {
    m_impl->recovered_pending_run_validation_required = false;
    m_impl->recovered_persona_document.reset();
    m_impl->recovered_user_global_instruction.reset();
    return {};
  }
  if (auto memory = load_recovered_memory(); !memory) return memory;
  if (!m_impl->recovered_persona_document) {
    if (!m_impl->recovered_run || m_impl->recovered_run->run_id != *run_id) {
      return error(ChatSessionErrorCode::context_failed,
                   "recoverable run identity is unavailable");
    }
    auto loaded = recovered_persona_document(
        m_impl->persona_source, m_impl->persona_limits,
        m_impl->kernel->event_log(), *m_impl->recovered_run,
        m_impl->stop_token);
    if (!loaded) return std::unexpected(std::move(loaded.error()));
    m_impl->recovered_persona_document = std::move(*loaded);
  }
  if (m_impl->recovered_persona_document) {
    auto validated = validate_recovered_persona_document(
        m_impl->persona_source, m_impl->persona_limits,
        *m_impl->recovered_persona_document, m_impl->stop_token);
    if (!validated) return validated;
  }
  const auto expected =
      recorded_user_global_instruction(m_impl->kernel->event_log(), *run_id);
  if (!expected) {
    m_impl->recovered_user_global_instruction.reset();
    return {};
  }
  if (m_impl->recovered_user_global_instruction) {
    if (m_impl->recovered_user_global_instruction->reference != *expected) {
      return error(ChatSessionErrorCode::context_failed,
                   "user-global instruction changed since this run started");
    }
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
    domain::SessionEventLog recovery_history{selected};
    if (durable && request.mode != ChatSessionOpen::Mode::create) {
      auto replayed = session_store->replay_events(selected, stop_token);
      if (!replayed) {
        return error(ChatSessionErrorCode::session_failed,
                     "durable session could not be opened",
                     replayed.error().retryable);
      }
      for (auto& event : *replayed) {
        if (auto appended = recovery_history.append(std::move(event));
            !appended) {
          return error(ChatSessionErrorCode::session_failed,
                       "durable session could not be opened");
        }
      }
    }
    auto recoverable = runtime::classify_recoverable_run(recovery_history);
    if (!recoverable) {
      return error(ChatSessionErrorCode::session_failed,
                   "durable session could not be opened",
                   recoverable.error().retryable);
    }
    std::optional<domain::PersonaDocument> recovered_persona;
    if (*recoverable) {
      auto loaded = recovered_persona_document(
          dependencies.persona_source, dependencies.persona_limits,
          recovery_history, **recoverable, stop_token);
      if (loaded)
        recovered_persona = std::move(*loaded);
      else if (loaded.error().code == ChatSessionErrorCode::cancelled) {
        return std::unexpected(std::move(loaded.error()));
      }
    }
    const bool allow_persona_attention =
        request.mode == ChatSessionOpen::Mode::resume ||
        request.mode == ChatSessionOpen::Mode::continue_latest;
    auto persona_setup = resolve_persona(
        dependencies.persona_source, dependencies.persona_limits,
        request.persona, recovery_history, stop_token, allow_persona_attention);
    if (!persona_setup) {
      return std::unexpected(std::move(persona_setup.error()));
    }
    const auto initial_persona_id =
        persona_setup->document
            ? std::optional<domain::PersonaId>{persona_setup->document
                                                   ->reference.persona_id}
            : std::nullopt;
    const auto tool_persona_id = *recoverable
                                     ? (*recoverable)->attributes.persona_id
                                     : initial_persona_id;
    std::optional<std::string> recovered_memory_digest;
    if (*recoverable && (*recoverable)->provenance) {
      const auto found = std::ranges::find(
          (*recoverable)->provenance->tools, std::string_view{"propose_memory"},
          [](const auto& tool) { return std::string_view{tool.tool_name}; });
      if (found != (*recoverable)->provenance->tools.end()) {
        recovered_memory_digest = found->registration_digest;
      }
    }
    if (dependencies.memory_controller != nullptr &&
        available_tools.find("propose_memory") != nullptr) {
      auto bound = runtime::bind_memory_tool(
          available_tools,
          {dependencies.memory_settings.global_capture !=
               domain::MemoryCaptureMode::off,
           dependencies.repository_id &&
               dependencies.memory_settings.project_capture !=
                   domain::MemoryCaptureMode::off,
           dependencies.memory_settings.persona_capture !=
               domain::MemoryCaptureMode::off,
           tool_persona_id,
           {}},
          std::move(recovered_memory_digest));
      if (!bound) {
        return error(ChatSessionErrorCode::session_failed,
                     bound.error().message);
      }
      available_tools = std::move(*bound);
    }
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
          available_tools, tool_policy, {}, dependencies.observation_broker);
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
          available_tools, tool_policy, nullptr,
          dependencies.observation_broker);
    }
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
             std::move(dependencies.identity_suffix_source),
             std::move(request.provenance),
             dependencies.persona_source,
             dependencies.persona_editor,
             dependencies.persona_limits,
             dependencies.user_global_instruction_source,
             dependencies.user_global_instruction_editor,
             dependencies.user_global_instruction_limits,
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
             std::move(recovered_persona),
             std::nullopt,
             {},
             std::move(kernel),
             0,
             std::nullopt,
             std::move(*recoverable)});
    impl->observation_broker = std::move(dependencies.observation_broker);
    impl->observation_context = std::move(dependencies.observation_context);
    impl->is_durable = durable;
    impl->user_global_instructions_enabled =
        dependencies.user_global_instructions_enabled;
    impl->recovered_pending_run_validation_required = recovered_pending_run;
    impl->surface_kind = dependencies.surface_kind;
    impl->repository_controller = dependencies.repository_context_controller;
    impl->local_sources = dependencies.local_sources;
    impl->repository_selection =
        std::move(dependencies.repository_context_selection);
    impl->async_repository_preparation =
        dependencies.async_repository_preparation;
    if (impl->repository_selection && impl->repository_controller == nullptr)
      return error(ChatSessionErrorCode::context_failed,
                   "Dev repository context is unavailable");
    return std::unique_ptr<ChatSession>{new ChatSession{std::move(impl)}};
  } catch (...) {
    return error(ChatSessionErrorCode::internal_failure,
                 "interactive session setup failed internally");
  }
}

auto ChatSession::bind_observation(
    domain::OpsObservationAuthority authority,
    std::shared_ptr<runtime::OpsObservationSource> source,
    std::shared_ptr<runtime::OpsObservationEndpoint> endpoint)
    -> std::expected<void, ManualOpsFailure> {
  try {
    if (m_impl->observation_inspection.closed)
      return std::unexpected(ManualOpsFailure{ManualOpsErrorCode::closed});
    if (!m_impl->observation_broker || !m_impl->observation_context ||
        !m_impl->permission_profile_id)
      return std::unexpected(ManualOpsFailure{ManualOpsErrorCode::unavailable});
    if (m_impl->observation_preparation_busy())
      return std::unexpected(ManualOpsFailure{ManualOpsErrorCode::busy});
    const auto provenance = m_impl->tool_policy->provenance();
    if (provenance == nullptr ||
        provenance->permission_profile_id != *m_impl->permission_profile_id)
      return std::unexpected(ManualOpsFailure{ManualOpsErrorCode::unavailable});
    std::optional<domain::OpsTargetBinding> selected{
        authority.specification().target};
    auto bound = m_impl->kernel->bind_ops_observation(
        std::move(authority), std::move(source), std::move(endpoint));
    if (!bound) return m_impl->report_observation_kernel_failure(bound.error());
    static_assert(
        std::is_nothrow_move_assignable_v<runtime::ToolRegistrySnapshot>);
    static_assert(std::is_nothrow_move_assignable_v<
                  std::shared_ptr<runtime::ToolPolicy>>);
    static_assert(std::is_nothrow_move_assignable_v<decltype(selected)>);
    m_impl->available_tools = std::move(bound->available_tools);
    m_impl->tool_policy = std::move(bound->policy);
    m_impl->observation_inspection.selection = std::move(selected);
    m_impl->observation_inspection.problem.reset();
    m_impl->observation_inspection.available = true;
    m_impl->observation_inspection.busy = false;
    return {};
  } catch (...) {
    return m_impl->close_observation_admission(
        {ManualOpsErrorCode::internal_failure});
  }
}

auto ChatSession::submit_observation(runtime::OpsObservationIntent intent)
    -> std::expected<ObservationSubmission, ManualOpsFailure> {
  try {
    if (m_impl->stop_token.stop_requested())
      return std::unexpected(ManualOpsFailure{ManualOpsErrorCode::cancelled});
    if (m_impl->observation_inspection.closed)
      return std::unexpected(ManualOpsFailure{ManualOpsErrorCode::closed});
    const auto context = m_impl->observation_context;
    const auto permission = m_impl->permission_profile_id;
    if (!m_impl->observation_inspection.available || !context || !permission)
      return std::unexpected(ManualOpsFailure{ManualOpsErrorCode::unavailable});
    if (auto synced = m_impl->synchronize_observations(); !synced)
      return std::unexpected(synced.error());
    if (m_impl->observation_inspection.busy)
      return std::unexpected(ManualOpsFailure{ManualOpsErrorCode::busy});
    const auto suffix = m_impl->identity_suffix_source();
    auto run = make_id<domain::RunId>("chat-ops", suffix);
    auto invocation =
        make_id<domain::InvocationId>("chat-ops-invocation", suffix);
    if (!run || !invocation)
      return std::unexpected(
          ManualOpsFailure{ManualOpsErrorCode::invalid_input});
    ObservationSubmission submitted{*run, *invocation};
    std::optional<ObservationSubmission> retained{submitted};
    const auto before = m_impl->kernel->event_log().events().size();
    auto started = m_impl->kernel->start_observation_control(
        {*run,
         {context->surface_id,
          context->workspace_id,
          *permission,
          {},
          {},
          domain::RunPurpose::control},
         *invocation,
         std::move(intent)});
    const auto admitted =
        std::span{m_impl->kernel->event_log().events()}.subspan(before);
    const bool recorded = std::ranges::any_of(admitted, [&](const auto& event) {
      const auto* attributes = std::get_if<domain::RunStarted>(&event.payload);
      return event.metadata.run_id == *run && attributes != nullptr &&
             attributes->manual_observation_required;
    });
    if (started || recorded) m_impl->manual_observation = std::move(retained);
    m_impl->remember_observation_events(before);
    if (!started) {
      const auto failure = observation_error(started.error());
      if (recorded || failure.code == ManualOpsErrorCode::storage_failure)
        return m_impl->close_observation_admission(failure);
      return m_impl->report_observation_kernel_failure(started.error());
    }
    m_impl->observation_inspection.problem.reset();
    if (auto synced = m_impl->synchronize_observations(true); !synced)
      return std::unexpected(synced.error());
    return submitted;
  } catch (...) {
    return m_impl->close_observation_admission(
        {ManualOpsErrorCode::internal_failure});
  }
}

auto ChatSession::cancel_observation(const domain::RunId& run_id)
    -> std::expected<void, ManualOpsFailure> {
  try {
    const auto submission = m_impl->manual_observation;
    if (!submission || !m_impl->manual_active() || submission->run_id != run_id)
      return std::unexpected(
          ManualOpsFailure{ManualOpsErrorCode::wrong_operation});
    const auto before = m_impl->kernel->event_log().events().size();
    auto cancelled =
        m_impl->kernel->cancel_run(run_id, "manual observation cancelled");
    m_impl->remember_observation_events(before);
    if (!cancelled)
      return m_impl->close_observation_admission(
          observation_error(cancelled.error()));
    return m_impl->synchronize_observations();
  } catch (...) {
    return m_impl->close_observation_admission(
        {ManualOpsErrorCode::internal_failure});
  }
}

auto ChatSession::decide_observation_approval(
    const domain::RunId& run_id, const domain::InvocationId& invocation_id,
    runtime::ToolApprovalResolution decision)
    -> std::expected<void, ManualOpsFailure> {
  try {
    if (m_impl->observation_inspection.closed)
      return std::unexpected(ManualOpsFailure{ManualOpsErrorCode::closed});
    const auto submission = m_impl->manual_observation;
    if (!submission || !m_impl->manual_active() ||
        submission->run_id != run_id ||
        submission->invocation_id != invocation_id)
      return std::unexpected(
          ManualOpsFailure{ManualOpsErrorCode::wrong_operation});
    if (auto synced = m_impl->synchronize_observations(); !synced)
      return synced;
    const auto before = m_impl->kernel->event_log().events().size();
    auto decided = m_impl->kernel->decide_approval(run_id, invocation_id,
                                                   std::move(decision));
    m_impl->remember_observation_events(before);
    if (!decided) {
      auto failure = m_impl->report_observation_kernel_failure(decided.error());
      if (auto synced = m_impl->synchronize_observations(); !synced) {
        if (m_impl->observation_inspection.problem &&
            m_impl->observation_inspection.problem->code ==
                ManualOpsErrorCode::storage_failure)
          return std::unexpected(*m_impl->observation_inspection.problem);
        return synced;
      }
      return failure;
    }
    if (auto synced = m_impl->synchronize_observations(); !synced)
      return synced;
    return {};
  } catch (...) {
    return m_impl->close_observation_admission(
        {ManualOpsErrorCode::internal_failure});
  }
}

auto ChatSession::pump_observations() -> std::expected<void, ManualOpsFailure> {
  try {
    if (!m_impl->observation_broker)
      return std::unexpected(ManualOpsFailure{ManualOpsErrorCode::unavailable});
    std::optional<ManualOpsFailure> failure;
    if (m_impl->observation_inspection.closed)
      failure = m_impl->observation_inspection.problem.value_or(
          ManualOpsFailure{ManualOpsErrorCode::closed});
    // Validate exact retained human proof before bypassing model preparation.
    if (auto synced = m_impl->synchronize_observations(); !synced)
      failure = synced.error();
    const auto service = [&] {
      auto serviced = m_impl->observation_broker->service();
      if (!serviced && !failure)
        failure = ManualOpsFailure{
            ManualOpsErrorCode::unavailable, {}, serviced.error().code};
    };
    const auto cancel_failed = [&] {
      if (!failure || !m_impl->manual_active()) return;
      const auto before = m_impl->kernel->event_log().events().size();
      auto cancelled = m_impl->kernel->cancel_run(
          m_impl->manual_observation->run_id, "observation source unavailable");
      m_impl->remember_observation_events(before);
      if (!cancelled) failure = observation_error(cancelled.error());
    };
    service();
    cancel_failed();
    if (m_impl->manual_active()) {
      auto events = m_impl->kernel->drain(
          failure ? runtime::RunDrainMode::observe_only
                  : runtime::RunDrainMode::dispatch_ready);
      if (!events) {
        failure = observation_error(events.error());
      } else {
        m_impl->pending_surface_events.insert(
            m_impl->pending_surface_events.end(),
            std::make_move_iterator(events->begin()),
            std::make_move_iterator(events->end()));
      }
    }
    service();
    cancel_failed();
    if (auto synced = m_impl->synchronize_observations(); !synced)
      failure = synced.error();
    if (failure) return m_impl->close_observation_admission(*failure);
    return {};
  } catch (...) {
    return m_impl->close_observation_admission(
        {ManualOpsErrorCode::internal_failure});
  }
}

auto ChatSession::inspect_observations() const noexcept
    -> const ManualOpsInspection& {
  return m_impl->observation_inspection;
}

auto ChatSession::submit(std::string prompt)
    -> std::expected<ChatSubmission, ChatSessionError> {
  if (m_impl->local_sources != nullptr)
    return error(ChatSessionErrorCode::context_failed,
                 "Use asynchronous evidence submission for local files");
  try {
    if (prompt.empty() || !valid_text(prompt))
      return error(ChatSessionErrorCode::invalid_input,
                   "prompt must be nonempty UTF-8 text without controls");
    if (prompt.size() > m_impl->limits.maximum_input_bytes)
      return error(ChatSessionErrorCode::input_too_large,
                   "prompt exceeds the configured input limit");
    if (active())
      return error(ChatSessionErrorCode::run_failed,
                   "a run or context preparation is already active");
    std::optional<runtime::PreparedRepositoryContext> prepared;
    if (m_impl->repository_selection) {
      auto result = m_impl->repository_controller->prepare(
          *m_impl->repository_selection, m_impl->stop_token);
      if (!result)
        return error(result.error().code ==
                             domain::RepositoryContextErrorCode::cancelled
                         ? ChatSessionErrorCode::cancelled
                         : ChatSessionErrorCode::context_failed,
                     result.error().message);
      prepared = std::move(*result);
    }
    return submit_prepared(std::move(prompt), std::move(prepared));
  } catch (...) {
    return error(ChatSessionErrorCode::internal_failure,
                 "repository submission failed internally");
  }
}

// clang-format off
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Explicitly stages interactive run admission and durable startup.
auto ChatSession::submit_prepared(std::string prompt,
    std::optional<runtime::PreparedRepositoryContext> prepared)
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
    if (m_impl->memory_controller != nullptr &&
        m_impl->available_tools.find("propose_memory") != nullptr) {
      auto bound = runtime::bind_memory_tool(
          m_impl->available_tools,
          {m_impl->memory_settings.global_capture !=
               domain::MemoryCaptureMode::off,
           m_impl->repository_id && m_impl->memory_settings.project_capture !=
                                        domain::MemoryCaptureMode::off,
           m_impl->memory_settings.persona_capture !=
               domain::MemoryCaptureMode::off,
           m_impl->persona_document
               ? std::optional<domain::PersonaId>{m_impl->persona_document
                                                      ->reference.persona_id}
               : std::nullopt,
           {}});
      if (!bound) {
        return error(ChatSessionErrorCode::run_failed, bound.error().message);
      }
      if (auto replaced = m_impl->kernel->replace_available_tools(*bound);
          !replaced) {
        return std::unexpected(kernel_error(replaced.error()));
      }
      m_impl->available_tools = std::move(*bound);
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
    if (auto spend =
            runtime::preflight_inference_spend(m_impl->kernel->event_log());
        !spend)
      return std::unexpected(inference_spend_error(spend.error()));
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
    auto surface_id = make_id<domain::SurfaceId>(
        m_impl->surface_kind == ChatSurfaceKind::agent ? "agent"
                                                       : "interactive",
        suffix);
    auto workspace_id = make_id<domain::WorkspaceId>("chat", suffix);
    if (prepared) {
      auto dev_workspace = domain::WorkspaceId::from("code");
      if (!dev_workspace)
        return error(ChatSessionErrorCode::internal_failure,
                     "Dev workspace identity is invalid");
      workspace_id = std::move(*dev_workspace);
    }
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
    std::vector<domain::ContextContentInput> content{
        {*user_entry_id,
         domain::ContextContentKind::conversation,
         user_message,
         {*user_source_id,
          std::string{m_impl->surface_kind == ChatSurfaceKind::agent
                          ? "agent-protocol"
                          : "interactive-composer"},
          std::nullopt},
         1,
         prompt.size()}};

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
    const auto& continuation_persona =
        m_impl->recovered_pending_run_validation_required
            ? m_impl->recovered_persona_document
            : m_impl->persona_document;
    if (continuation_persona) {
      auto persona_instruction = runtime::persona_instruction_input(
          *continuation_persona, continuation_persona->text.size());
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
    if (prepared)
      input.instructions.insert(input.instructions.end(),
                                prepared->instructions.begin(),
                                prepared->instructions.end());
    auto session_context = runtime::prepare_session_context(
        {m_impl->kernel->event_log(),
         m_impl->model_id,
         input,
         m_impl->memory_controller,
         {m_impl->repository_id,
          m_impl->persona_document
              ? std::optional{m_impl->persona_document->reference.persona_id}
              : std::nullopt,
          m_impl->memory_settings.context_tokens, 0},
         {},
         {}},
        m_impl->stop_token);
    if (!session_context)
      return error(ChatSessionErrorCode::context_failed,
                   session_context.error().message,
                   session_context.error().retryable);
    auto selected = runtime::select_session_evidence(
        *session_context, prepared, m_impl->evidence_local, m_impl->stop_token);
    if (!selected) return std::unexpected(evidence_error(selected.error()));
    auto continuation_context = std::move(selected->session.input);
    auto memory_selection = std::move(selected->session.memory_selection);
    auto conversation_admission =
        std::move(selected->session.conversation_admission);
    auto repository_admission = std::move(selected->repository_admission);
    auto local_admission = std::move(selected->local_admission);
    auto context = std::move(selected->context);
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
        std::move(context),
        tool_profile->effective_tools.declarations(),
        m_impl->generation_options};
    if (auto sealed = domain::seal_memory_selection(memory_selection);
        !sealed) {
      return error(ChatSessionErrorCode::context_failed,
                   sealed.error().message);
    }
    m_impl->evidence_permit = false;
    auto started = m_impl->kernel->start(
        {*run_id,
         {*surface_id, *workspace_id, *permission_id,
          m_impl->persona_document
              ? std::optional<domain::PersonaId>{m_impl->persona_document
                                                     ->reference.persona_id}
              : std::nullopt,
          std::move(memory_selection), domain::RunPurpose::conversation,
          std::move(conversation_admission)},
         std::move(user_message),
         std::move(backend_request),
         std::move(provenance),
         m_impl->next_persona_selection,
         m_impl->model.pricing_observation,
         {},
         repository_admission,
         {},
         local_admission});
    if (!started) return std::unexpected(kernel_error(started.error()));
    m_impl->active_context = std::move(continuation_context);
    m_impl->repository_admission = std::move(repository_admission);
    m_impl->local_admission = std::move(local_admission);
    m_impl->local_prepared = m_impl->evidence_local;
    m_impl->repository_prepared = std::move(prepared);
    m_impl->repository_block.reset();
    m_impl->repository_recovery_pinned = false;
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
  const auto* attributes =
      context_run_attributes(m_impl->kernel->event_log(), run_id);
  if (attributes != nullptr &&
      attributes->purpose == domain::RunPurpose::summary)
    return std::vector<domain::RunEvent>{};

  if (!run_id || m_impl->kernel->active_inference_id() ||
      m_impl->kernel->pending_tool_approval() ||
      m_impl->kernel->pending_question_input()) {
    return std::vector<domain::RunEvent>{};
  }
  const auto* projection = m_impl->kernel->projection(*run_id);
  if (projection != nullptr &&
      (projection->status() == domain::RunStatus::completed ||
       projection->status() == domain::RunStatus::failed ||
       projection->status() == domain::RunStatus::cancelled))
    return std::vector<domain::RunEvent>{};
  if (auto validated = validate_recovered_pending_run(); !validated) {
    return std::unexpected(std::move(validated.error()));
  }

  const auto* active_tools = m_impl->kernel->active_tool_declarations();
  if (active_tools == nullptr) {
    return error(ChatSessionErrorCode::run_failed,
                 "active run tool declarations are unavailable");
  }
  const bool legacy = legacy_context(m_impl->kernel->event_log(), run_id);
  auto declaration_tokens = tool_declaration_tokens(*active_tools, legacy);
  if (!declaration_tokens) {
    return std::unexpected(std::move(declaration_tokens.error()));
  }

  const auto suffix = m_impl->identity_suffix_source();
  domain::ContextBuildInput base;
  if (m_impl->active_context) {
    base = *m_impl->active_context;
  } else {
    auto history = recovered_conversation_input(
        m_impl->kernel->event_log(), m_impl->kernel->active_run_id(), suffix,
        frozen_summary_content(m_impl->recovered_summary_context));
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
    const auto& continuation_persona =
        m_impl->recovered_pending_run_validation_required
            ? m_impl->recovered_persona_document
            : m_impl->persona_document;
    if (continuation_persona) {
      auto persona_instruction = runtime::persona_instruction_input(
          *continuation_persona, continuation_persona->text.size());
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
    for (const auto& memory : m_impl->recovered_memory_context) {
      if (memory.order == 0 ||
          (legacy && memory.order > base.content.size() + 1))
        return error(
            ChatSessionErrorCode::context_failed,
            "original saved memory admission order cannot be reconstructed");
      if (legacy)
        base.content.insert(base.content.begin() +
                                static_cast<std::ptrdiff_t>(memory.order - 1),
                            memory);
      else
        base.content.push_back(memory);
    }
    if (legacy) {
      for (std::size_t index{}; index < base.content.size(); ++index)
        base.content[index].order = index + 1;
    } else {
      std::ranges::sort(base.content, {}, &domain::ContextContentInput::order);
    }
    m_impl->active_context = base;
  }

  auto tool_messages = runtime::reconstruct_active_tool_continuation(
      m_impl->kernel->event_log(), *run_id, {}, m_impl->stop_token);
  if (!tool_messages) {
    return error(ChatSessionErrorCode::run_failed,
                 tool_messages.error().message);
  }
  if (tool_messages->empty()) return std::vector<domain::RunEvent>{};
  if (m_impl->repository_block) return std::vector<domain::RunEvent>{};
  auto repository_ready =
      validate_active_evidence(ChatRepositoryWorkPurpose::continuation);
  if (!repository_ready) {
    if (m_impl->repository_block) return std::vector<domain::RunEvent>{};
    return std::unexpected(std::move(repository_ready.error()));
  }
  if (!*repository_ready) return std::vector<domain::RunEvent>{};
  if (m_impl->repository_admission && m_impl->repository_prepared) {
    const auto& prepared = *m_impl->repository_prepared;
    for (const auto& instruction : prepared.instructions)
      if (std::ranges::none_of(base.instructions, [&](const auto& entry) {
            return entry.entry_id == instruction.entry_id;
          }))
        base.instructions.push_back(instruction);
    for (const auto& ref : m_impl->repository_admission->evidence) {
      if (ref.decision != domain::RepositoryContextDecision::admitted ||
          std::ranges::any_of(base.content, [&](const auto& entry) {
            return entry.entry_id == ref.entry_id;
          }))
        continue;
      const auto item =
          std::ranges::find(prepared.evidence.parcel.items, ref.evidence_id,
                            &domain::ContextParcelItem::evidence_id);
      if (item == prepared.evidence.parcel.items.end())
        return error(ChatSessionErrorCode::context_failed,
                     "recorded repository evidence is unavailable");
      base.content.push_back({ref.entry_id,
                              domain::ContextContentKind::evidence,
                              {ref.message_id, domain::Role::evidence,
                               item->content, std::nullopt},
                              {ref.source_id, ref.source.relative_path,
                               ref.source.content_digest.algorithm + ":" +
                                   ref.source.content_digest.value},
                              ref.order,
                              ref.estimated_tokens});
    }
    m_impl->active_context = base;
  }

  if (m_impl->local_admission) {
    if (!m_impl->local_prepared)
      return error(ChatSessionErrorCode::context_failed,
                   "Recorded local evidence is unavailable");
    for (const auto& ref : m_impl->local_admission->evidence) {
      if (ref.decision != domain::LocalContextDecision::admitted ||
          std::ranges::any_of(base.content, [&](const auto& entry) {
            return entry.entry_id == ref.entry_id;
          }))
        continue;
      const auto found = std::ranges::find_if(
          m_impl->local_prepared->candidates, [&](const auto& candidate) {
            return candidate.content.entry_id == ref.entry_id;
          });
      if (found == m_impl->local_prepared->candidates.end())
        return error(ChatSessionErrorCode::context_failed,
                     "Recorded local evidence is unavailable");
      base.content.push_back(found->content);
    }
    std::ranges::sort(base.content, {}, &domain::ContextContentInput::order);
    m_impl->active_context = base;
  }

  std::uint64_t continuation_order{};
  for (const auto& entry : base.content)
    continuation_order = std::max(continuation_order, entry.order);
  for (auto& message : *tool_messages) {
    if (continuation_order == std::numeric_limits<std::uint64_t>::max())
      return error(ChatSessionErrorCode::context_failed,
                   "tool continuation order overflowed");
    const auto message_suffix = m_impl->identity_suffix_source();
    auto entry_id =
        make_id<domain::ContextEntryId>("tool-result-entry", message_suffix);
    auto source_id =
        make_id<domain::ContextSourceId>("tool-result-source", message_suffix);
    auto estimated = estimated_message_tokens(message, legacy);
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
         ++continuation_order,
         *estimated});
  }
  auto context = runtime::ContextBuilder{}.build(std::move(base));
  if (!context) {
    return error(ChatSessionErrorCode::context_failed,
                 "tool results exceed model context capacity");
  }
  auto continuation_state = assistant_continuation_state(
      m_impl->kernel->event_log().events(), *run_id, *context);
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
  m_impl->evidence_permit = false;
  auto continued = m_impl->kernel->continue_run(
      *run_id,
      {*inference_id, *assistant_message_id, m_impl->model_id,
       std::move(*context), *active_tools, m_impl->generation_options,
       std::move(*continuation_state)},
      m_impl->model.pricing_observation, m_impl->repository_admission,
      m_impl->local_admission);
  if (!continued) {
    if (continued.error().code ==
        runtime::RunKernelErrorCode::continuation_not_ready) {
      return std::vector<domain::RunEvent>{};
    }
    return std::unexpected(kernel_error(continued.error()));
  }
  m_impl->recovered_pending_run_validation_required = false;
  const auto events = m_impl->kernel->event_log().events();
  std::vector<domain::RunEvent> committed;
  committed.reserve(events.size() - before);
  for (std::size_t index = before; index < events.size(); ++index) {
    committed.push_back(events[index]);
  }
  return committed;
}

auto ChatSession::prepare_recovered_repository()
    -> std::expected<bool, ChatSessionError> {
  const auto* attributes = context_run_attributes(
      m_impl->kernel->event_log(), m_impl->kernel->active_run_id());
  if (attributes != nullptr &&
      attributes->purpose == domain::RunPurpose::summary)
    return true;
  if (!m_impl->recovered_pending_run_validation_required ||
      m_impl->repository_recovery_pinned)
    return true;
  if (m_impl->repository_block) return false;
  auto ready = validate_active_evidence(ChatRepositoryWorkPurpose::recovery);
  if (!ready) {
    if (m_impl->repository_block) return false;
    return std::unexpected(std::move(ready.error()));
  }
  if (!*ready) return false;
  auto fits = validate_recovered_memory_capacity();
  if (!fits) {
    if (const auto run = m_impl->kernel->active_run_id())
      m_impl->repository_block =
          ChatRecoveryBlock{session_id(), *run, fits.error()};
    return false;
  }
  m_impl->repository_recovery_pinned = true;
  auto pinned = pin_recovered_summary_sources();
  if (!pinned) return std::unexpected(pinned.error());
  return true;
}

auto ChatSession::dispatch_ready_tools()
    -> std::expected<std::optional<std::vector<domain::RunEvent>>,
                     ChatSessionError> {
  if (!m_impl->kernel->pending_tool_dispatch())
    return std::optional{std::vector<domain::RunEvent>{}};
  auto ready =
      validate_active_evidence(ChatRepositoryWorkPurpose::continuation);
  if (!ready) return std::unexpected(ready.error());
  if (!*ready) return std::nullopt;
  m_impl->evidence_permit = false;
  auto dispatched =
      m_impl->kernel->drain(runtime::RunDrainMode::dispatch_ready);
  if (!dispatched) return std::unexpected(kernel_error(dispatched.error()));
  return std::optional{std::move(*dispatched)};
}

auto ChatSession::drain()
    -> std::expected<std::vector<domain::RunEvent>, ChatSessionError> {
  if (m_impl->manual_active()) {
    auto pumped = pump_observations();
    if (!pumped) return observation_chat_error(pumped.error());
    return std::exchange(m_impl->pending_surface_events, {});
  }
  if (m_impl->observation_broker) {
    auto pumped = pump_observations();
    if (!pumped && pumped.error().code == ManualOpsErrorCode::storage_failure)
      return observation_chat_error(pumped.error());
  }
  return drain_model_events();
}

auto ChatSession::drain_model_events()
    -> std::expected<std::vector<domain::RunEvent>, ChatSessionError> {
  auto observed = m_impl->observe_pending_events();
  if (!observed) return std::unexpected(observed.error());
  auto result = std::move(*observed);
  if (auto validated = validate_recovered_pending_run(); !validated) {
    m_impl->repository_proof_ready = false;
    if (m_impl->recovery_block || m_impl->repository_block) return result;
    m_impl->pending_surface_events = std::move(result);
    return std::unexpected(std::move(validated.error()));
  }
  auto repository_ready = prepare_recovered_repository();
  if (!repository_ready) {
    m_impl->pending_surface_events = std::move(result);
    return std::unexpected(std::move(repository_ready.error()));
  }
  if (!*repository_ready) return result;
  if (m_impl->recovered_pending_run_validation_required &&
      !m_impl->kernel->pending_question_input() &&
      !m_impl->kernel->pending_tool_approval())
    m_impl->recovered_sources_pinned = true;
  auto dispatched = dispatch_ready_tools();
  if (!dispatched) {
    if (m_impl->repository_block) return result;
    m_impl->pending_surface_events = std::move(result);
    return std::unexpected(dispatched.error());
  }
  if (!*dispatched) return result;
  result.insert(result.end(), std::make_move_iterator((**dispatched).begin()),
                std::make_move_iterator((**dispatched).end()));
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
    m_impl->recovered_memory_context.clear();
    m_impl->recovered_summary_context.reset();
    m_impl->recovered_sources_pinned = false;
    m_impl->recovered_pending_run_validation_required = false;
    m_impl->recovered_persona_document.reset();
    m_impl->recovered_user_global_instruction.reset();
  }
  return result;
}

auto ChatSession::cancel_active(std::optional<std::string> reason)
    -> std::expected<void, ChatSessionError> {
  const auto& submission = m_impl->manual_observation;
  if (submission && m_impl->manual_active()) {
    auto cancelled = cancel_observation(submission->run_id);
    if (!cancelled) return observation_chat_error(cancelled.error());
    return {};
  }
  return cancel_model_run(std::move(reason));
}

auto ChatSession::cancel_model_run(std::optional<std::string> reason)
    -> std::expected<void, ChatSessionError> {
  cancel_repository_work();
  const auto run = m_impl->kernel->active_run_id();
  if (!run) {
    if (m_impl->recovery_block || m_impl->repository_block) {
      return error(ChatSessionErrorCode::session_failed,
                   "reopen the session before retrying blocked cancellation");
    }
    return {};
  }
  const auto before = m_impl->kernel->event_log().events().size();
  auto cancelled = m_impl->kernel->cancel_run(*run, std::move(reason));
  if (!cancelled) {
    auto failure = kernel_error(cancelled.error());
    if (!m_impl->kernel->active_run_id()) {
      auto* block = m_impl->repository_block ? &*m_impl->repository_block
                    : m_impl->recovery_block ? &*m_impl->recovery_block
                                             : nullptr;
      if (block != nullptr) {
        block->reason = failure;
        block->reason.message = "cancellation persistence failed; reopen the "
                                "session before retrying";
      }
    }
    return std::unexpected(std::move(failure));
  }
  m_impl->active_context.reset();
  m_impl->recovered_memory_context.clear();
  m_impl->recovered_summary_context.reset();
  m_impl->recovered_sources_pinned = false;
  m_impl->recovered_pending_run_validation_required = false;
  m_impl->recovered_persona_document.reset();
  m_impl->recovered_user_global_instruction.reset();
  m_impl->recovery_block.reset();
  m_impl->repository_block.reset();
  m_impl->repository_admission.reset();
  m_impl->local_admission.reset();
  m_impl->local_prepared.reset();
  m_impl->repository_recovery_pinned = false;
  const auto events = m_impl->kernel->event_log().events();
  for (std::size_t index = before; index < events.size(); ++index) {
    m_impl->pending_surface_events.push_back(events[index]);
  }
  return {};
}

auto ChatSession::blocked_recovery() const
    -> const std::optional<ChatRecoveryBlock>& {
  return m_impl->repository_block ? m_impl->repository_block
                                  : m_impl->recovery_block;
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
  if (m_impl->manual_active()) {
    auto decided = decide_observation_approval(run_id, invocation_id,
                                               std::move(resolution));
    if (!decided) return observation_chat_error(decided.error());
    return {};
  }
  if (m_impl->repository_work || m_impl->evidence_work)
    return error(ChatSessionErrorCode::run_failed,
                 "repository source validation is already pending");
  {
    auto ready = validate_active_evidence(ChatRepositoryWorkPurpose::approval);
    if (!ready) return std::unexpected(std::move(ready.error()));
    if (!*ready) {
      m_impl->repository_action = [this, run_id, invocation_id,
                                   resolution =
                                       std::move(resolution)]() mutable {
        return decide_tool_approval(run_id, invocation_id,
                                    std::move(resolution));
      };
      return {};
    }
  }
  if (auto validated = validate_recovered_pending_run(true); !validated) {
    return std::unexpected(std::move(validated.error()));
  }
  const auto before = m_impl->kernel->event_log().events().size();
  m_impl->evidence_permit = false;
  auto decided = m_impl->kernel->decide_approval(run_id, invocation_id,
                                                 std::move(resolution));
  if (!decided) return std::unexpected(kernel_error(decided.error()));
  if (m_impl->recovered_pending_run_validation_required) {
    m_impl->recovered_sources_pinned = true;
    m_impl->repository_recovery_pinned = true;
  }
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
  if (m_impl->repository_work || m_impl->evidence_work)
    return error(ChatSessionErrorCode::run_failed,
                 "repository source validation is already pending");
  auto ready = validate_active_evidence(ChatRepositoryWorkPurpose::answer);
  if (!ready) return std::unexpected(std::move(ready.error()));
  if (!*ready) {
    m_impl->repository_action = [this, run_id, invocation_id,
                                 answers = std::move(answers)]() mutable {
      return answer_questions(run_id, invocation_id, std::move(answers));
    };
    return {};
  }
  if (auto validated = validate_recovered_pending_run(true); !validated) {
    return std::unexpected(std::move(validated.error()));
  }
  const auto before = m_impl->kernel->event_log().events().size();
  m_impl->evidence_permit = false;
  auto answered = m_impl->kernel->answer_questions(run_id, invocation_id,
                                                   std::move(answers));
  if (!answered) return std::unexpected(kernel_error(answered.error()));
  if (m_impl->recovered_pending_run_validation_required) {
    m_impl->recovered_sources_pinned = true;
    m_impl->repository_recovery_pinned = true;
  }
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
  if (m_impl->repository_work || m_impl->evidence_work)
    return error(ChatSessionErrorCode::run_failed,
                 "repository source validation is already pending");
  auto ready = validate_active_evidence(ChatRepositoryWorkPurpose::answer);
  if (!ready) return std::unexpected(std::move(ready.error()));
  if (!*ready) {
    m_impl->repository_action = [this, run_id, invocation_id,
                                 reason = std::move(reason)]() mutable {
      return cancel_questions(run_id, invocation_id, std::move(reason));
    };
    return {};
  }
  if (auto validated = validate_recovered_pending_run(true); !validated) {
    return std::unexpected(std::move(validated.error()));
  }
  const auto before = m_impl->kernel->event_log().events().size();
  m_impl->evidence_permit = false;
  auto cancelled = m_impl->kernel->cancel_questions(run_id, invocation_id,
                                                    std::move(reason));
  if (!cancelled) return std::unexpected(kernel_error(cancelled.error()));
  if (m_impl->recovered_pending_run_validation_required) {
    m_impl->recovered_sources_pinned = true;
    m_impl->repository_recovery_pinned = true;
  }
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

auto ChatSession::create_persona(persona::PersonaCreate request)
    -> std::expected<persona::PersonaWriteReceipt, ChatSessionError> {
  if (active()) {
    return error(ChatSessionErrorCode::run_failed,
                 "finish or cancel the active run before creating a persona");
  }
  if (m_impl->persona_editor == nullptr) {
    return error(ChatSessionErrorCode::context_failed,
                 "persona editor is unavailable");
  }
  request.limits = m_impl->persona_limits;
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
  std::set<domain::RunId> excluded;
  for (const auto& event : m_impl->kernel->event_log().events())
    if (const auto* started = std::get_if<domain::RunStarted>(&event.payload);
        started != nullptr &&
        started->purpose != domain::RunPurpose::conversation)
      excluded.insert(event.metadata.run_id);
  for (const auto& event : m_impl->kernel->event_log().events()) {
    if (excluded.contains(event.metadata.run_id)) continue;
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
  return m_impl->kernel->active_run_id().has_value() ||
         m_impl->repository_work.has_value() ||
         m_impl->evidence_work.has_value();
}

} // namespace aiforge::surfaces
