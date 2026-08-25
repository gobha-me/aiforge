#include <aiforge/runtime/persona.hpp>

#include <utility>
#include <variant>

namespace aiforge::runtime {
namespace {

[[nodiscard]] auto failure(PersonaContextErrorCode code, std::string message)
    -> std::unexpected<PersonaContextError> {
  return std::unexpected(PersonaContextError{code, std::move(message)});
}

template <typename Id>
[[nodiscard]] auto prefixed_id(const std::string_view prefix,
                               const domain::PersonaId& persona_id)
    -> std::expected<Id, PersonaContextError> {
  auto value = Id::from(std::string{prefix} + std::string{persona_id.value()});
  if (!value) {
    return failure(PersonaContextErrorCode::invalid_identity,
                   "persona context identity is invalid");
  }
  return std::move(*value);
}

}  // namespace

auto persona_instruction_input(const domain::PersonaDocument& document,
                               const std::uint64_t estimated_tokens,
                               const std::uint64_t order)
    -> std::expected<domain::InstructionInput, PersonaContextError> {
  try {
    if (!domain::validate_persona_document(document)) {
      return failure(PersonaContextErrorCode::invalid_document,
                     "persona document is invalid");
    }
    if (estimated_tokens == 0 || order == 0) {
      return failure(PersonaContextErrorCode::invalid_estimate,
                     "persona context estimate and order must be positive");
    }
    auto entry_id = prefixed_id<domain::ContextEntryId>(
        "entry:", document.reference.persona_id);
    auto message_id = prefixed_id<domain::MessageId>(
        "message:", document.reference.persona_id);
    auto source_id = prefixed_id<domain::ContextSourceId>(
        "source:", document.reference.persona_id);
    if (!entry_id) return std::unexpected(entry_id.error());
    if (!message_id) return std::unexpected(message_id.error());
    if (!source_id) return std::unexpected(source_id.error());
    return domain::InstructionInput{
        std::move(*entry_id),
        domain::InstructionLayer::persona,
        domain::InstructionOperation::add,
        std::nullopt,
        domain::Message{std::move(*message_id), domain::Role::system,
                        {domain::TextBlock{document.text}}, std::nullopt},
        {std::move(*source_id), document.reference.source_location,
         document.reference.content_digest.algorithm + ":" +
             document.reference.content_digest.value},
        0,
        order,
        estimated_tokens};
  } catch (...) {
    return failure(PersonaContextErrorCode::internal_failure,
                   "persona context preparation failed internally");
  }
}

auto latest_persona_selection(const domain::SessionEventLog& event_log)
    -> std::expected<std::optional<domain::PersonaSelection>,
                     PersonaContextError> {
  try {
    std::optional<domain::PersonaSelection> latest;
    for (const auto& event : event_log.events()) {
      const auto* recorded =
          std::get_if<domain::PersonaSelectionRecorded>(&event.payload);
      if (recorded == nullptr) continue;
      if (!domain::validate_persona_selection(recorded->selection)) {
        return failure(PersonaContextErrorCode::invalid_history,
                       "session contains invalid persona provenance");
      }
      latest = recorded->selection;
    }
    return latest;
  } catch (...) {
    return failure(PersonaContextErrorCode::internal_failure,
                   "persona history inspection failed internally");
  }
}

}  // namespace aiforge::runtime
