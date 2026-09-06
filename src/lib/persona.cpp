#include <aiforge/domain/persona.hpp>

#include <algorithm>
#include <ranges>
#include <string_view>

#include <aiforge/detail/utf8_text.hpp>

namespace aiforge::domain {
namespace {

[[nodiscard]] auto failure(PersonaValidationErrorCode code, std::string message)
    -> std::unexpected<PersonaValidationError> {
  return std::unexpected(PersonaValidationError{code, std::move(message)});
}

[[nodiscard]] auto valid_name(const std::string_view value) -> bool {
  if (value.empty() || value.size() > 96) return false;
  const auto is_ascii_alnum = [](const unsigned char character) {
    return (character >= '0' && character <= '9') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= 'a' && character <= 'z');
  };
  if (!is_ascii_alnum(static_cast<unsigned char>(value.front()))) return false;
  return std::ranges::all_of(value.substr(1),
                             [](const unsigned char character) {
                               return (character >= '0' && character <= '9') ||
                                      (character >= 'A' && character <= 'Z') ||
                                      (character >= 'a' && character <= 'z') ||
                                      character == '-' || character == '_';
                             });
}

[[nodiscard]] auto valid_identity(const std::string_view value) -> bool {
  constexpr std::string_view prefix{"persona:"};
  if (!value.starts_with(prefix) || value.size() <= prefix.size() ||
      value.size() > domain::PersonaId::max_size) {
    return false;
  }
  return std::ranges::all_of(value.substr(prefix.size()),
                             [](const unsigned char character) {
                               return (character >= '0' && character <= '9') ||
                                      (character >= 'A' && character <= 'Z') ||
                                      (character >= 'a' && character <= 'z') ||
                                      character == '-' || character == '_';
                             });
}

} // namespace

auto validate_persona_reference(const PersonaReference& reference)
    -> std::expected<void, PersonaValidationError> {
  if (!valid_name(reference.name)) {
    return failure(PersonaValidationErrorCode::invalid_name,
                   "persona name is invalid");
  }
  if (!valid_identity(reference.persona_id.value())) {
    return failure(PersonaValidationErrorCode::invalid_identity,
                   "persona identity is invalid");
  }
  const auto expected_md = "personas/" + reference.name + ".md";
  const auto expected_txt = "personas/" + reference.name + ".txt";
  if (reference.source_location != expected_md &&
      reference.source_location != expected_txt) {
    return failure(PersonaValidationErrorCode::invalid_location,
                   "persona source location is invalid");
  }
  if (reference.content_digest.algorithm != "sha256" ||
      reference.content_digest.value.size() != 64 ||
      reference.content_digest.byte_size == 0 ||
      reference.content_digest.byte_size > 1024U * 1024U ||
      !std::ranges::all_of(reference.content_digest.value,
                           [](const unsigned char character) {
                             return (character >= '0' && character <= '9') ||
                                    (character >= 'a' && character <= 'f');
                           })) {
    return failure(PersonaValidationErrorCode::invalid_digest,
                   "persona content digest is invalid");
  }
  return {};
}

auto validate_persona_selection(const PersonaSelection& selection)
    -> std::expected<void, PersonaValidationError> {
  if (selection.source == PersonaSelectionSource::unknown ||
      selection.action == PersonaSelectionAction::unknown ||
      (selection.action == PersonaSelectionAction::selected) !=
          selection.persona.has_value()) {
    return failure(PersonaValidationErrorCode::invalid_selection,
                   "persona selection is inconsistent");
  }
  if (selection.persona) {
    if (auto valid = validate_persona_reference(*selection.persona); !valid) {
      return valid;
    }
  }
  if (selection.previous_persona) {
    if (auto valid = validate_persona_reference(*selection.previous_persona);
        !valid) {
      return valid;
    }
  }
  return {};
}

auto validate_persona_document(const PersonaDocument& document)
    -> std::expected<void, PersonaValidationError> {
  if (auto valid = validate_persona_reference(document.reference); !valid) {
    return valid;
  }
  if (document.reference.content_digest.byte_size != document.text.size() ||
      document.text.size() > std::size_t{1024} * 1024U ||
      !detail::is_safe_utf8_text(document.text)) {
    return failure(PersonaValidationErrorCode::invalid_document,
                   "persona document text is invalid");
  }
  return {};
}

} // namespace aiforge::domain
