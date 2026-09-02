#include <aiforge/testing/scripted_persona_editor.hpp>

#include <utility>

namespace aiforge::testing {
namespace {

[[nodiscard]] auto failure(persona::PersonaEditorErrorCode code,
                           std::string message)
    -> std::unexpected<persona::PersonaEditorError> {
  return std::unexpected(persona::PersonaEditorError{
      code, std::move(message), std::nullopt, false, false});
}

[[nodiscard]] auto cancelled() -> std::unexpected<persona::PersonaEditorError> {
  return failure(persona::PersonaEditorErrorCode::cancelled,
                 "persona write cancelled");
}

} // namespace

ScriptedPersonaEditor::ScriptedPersonaEditor(
    std::vector<PersonaCreateExchange> create_exchanges,
    std::vector<PersonaReplaceExchange> replace_exchanges)
    : m_create_exchanges(std::move(create_exchanges)),
      m_replace_exchanges(std::move(replace_exchanges)) {
}

auto ScriptedPersonaEditor::create(persona::PersonaCreate request,
                                   const std::stop_token stop_token)
    -> std::expected<persona::PersonaWriteReceipt,
                     persona::PersonaEditorError> {
  try {
    if (stop_token.stop_requested()) return cancelled();
    if (auto prepared = persona::prepare_persona_create(request); !prepared) {
      return std::unexpected(std::move(prepared.error()));
    }
    m_recorded_creates.push_back(request);
    if (m_next_create >= m_create_exchanges.size()) {
      return failure(persona::PersonaEditorErrorCode::internal_failure,
                     "scripted persona creates are exhausted");
    }
    const auto& exchange = m_create_exchanges[m_next_create];
    if (exchange.expected_request != request) {
      return failure(persona::PersonaEditorErrorCode::invalid_request,
                     "persona create did not match the script");
    }
    ++m_next_create;
    if (const auto* error =
            std::get_if<persona::PersonaEditorError>(&exchange.outcome)) {
      return std::unexpected(*error);
    }
    const auto& receipt =
        std::get<persona::PersonaWriteReceipt>(exchange.outcome);
    if (auto valid = persona::validate_persona_write_receipt(request, receipt);
        !valid) {
      return std::unexpected(std::move(valid.error()));
    }
    return receipt;
  } catch (...) {
    return failure(persona::PersonaEditorErrorCode::internal_failure,
                   "scripted persona create failed internally");
  }
}

auto ScriptedPersonaEditor::replace(persona::PersonaReplace request,
                                    const std::stop_token stop_token)
    -> std::expected<persona::PersonaWriteReceipt,
                     persona::PersonaEditorError> {
  try {
    if (stop_token.stop_requested()) return cancelled();
    if (auto prepared = persona::prepare_persona_replace(request); !prepared) {
      return std::unexpected(std::move(prepared.error()));
    }
    m_recorded_replaces.push_back(request);
    if (m_next_replace >= m_replace_exchanges.size()) {
      return failure(persona::PersonaEditorErrorCode::internal_failure,
                     "scripted persona replacements are exhausted");
    }
    const auto& exchange = m_replace_exchanges[m_next_replace];
    if (exchange.expected_request != request) {
      return failure(persona::PersonaEditorErrorCode::invalid_request,
                     "persona replacement did not match the script");
    }
    ++m_next_replace;
    if (const auto* error =
            std::get_if<persona::PersonaEditorError>(&exchange.outcome)) {
      return std::unexpected(*error);
    }
    const auto& receipt =
        std::get<persona::PersonaWriteReceipt>(exchange.outcome);
    if (auto valid = persona::validate_persona_write_receipt(request, receipt);
        !valid) {
      return std::unexpected(std::move(valid.error()));
    }
    return receipt;
  } catch (...) {
    return failure(persona::PersonaEditorErrorCode::internal_failure,
                   "scripted persona replacement failed internally");
  }
}

auto ScriptedPersonaEditor::recorded_creates() const noexcept
    -> const std::vector<persona::PersonaCreate>& {
  return m_recorded_creates;
}

auto ScriptedPersonaEditor::recorded_replaces() const noexcept
    -> const std::vector<persona::PersonaReplace>& {
  return m_recorded_replaces;
}

auto ScriptedPersonaEditor::remaining_creates() const noexcept -> std::size_t {
  return m_create_exchanges.size() - m_next_create;
}

auto ScriptedPersonaEditor::remaining_replaces() const noexcept -> std::size_t {
  return m_replace_exchanges.size() - m_next_replace;
}

} // namespace aiforge::testing
