#pragma once

#include <optional>
#include <expected>
#include <string>

#include <aiforge/domain/digest.hpp>
#include <aiforge/domain/ids.hpp>

namespace aiforge::domain {

struct PersonaReference {
  PersonaId persona_id;
  std::string name;
  // Portable location below the AIForge configuration directory.
  std::string source_location;
  ContentDigest content_digest;
  auto operator==(const PersonaReference&) const -> bool = default;
};

struct PersonaDocument {
  PersonaReference reference;
  std::string text;
  auto operator==(const PersonaDocument&) const -> bool = default;
};

struct PersonaSummary {
  PersonaReference reference;
  std::string description;
  auto operator==(const PersonaSummary&) const -> bool = default;
};

enum class PersonaSelectionSource {
  command_line,
  interactive,
  resumed,
  retained,
  unknown,
};

enum class PersonaSelectionAction {
  selected,
  disabled,
  unknown,
};

struct PersonaSelection {
  PersonaSelectionAction action{PersonaSelectionAction::selected};
  PersonaSelectionSource source{PersonaSelectionSource::command_line};
  std::optional<PersonaReference> persona;
  std::optional<PersonaReference> previous_persona;
  auto operator==(const PersonaSelection&) const -> bool = default;
};

enum class PersonaValidationErrorCode {
  invalid_identity,
  invalid_name,
  invalid_location,
  invalid_digest,
  invalid_selection,
  invalid_document,
};

struct PersonaValidationError {
  PersonaValidationErrorCode code{PersonaValidationErrorCode::invalid_selection};
  std::string message;
  auto operator==(const PersonaValidationError&) const -> bool = default;
};

[[nodiscard]] auto validate_persona_reference(const PersonaReference& reference)
    -> std::expected<void, PersonaValidationError>;
[[nodiscard]] auto validate_persona_selection(const PersonaSelection& selection)
    -> std::expected<void, PersonaValidationError>;
[[nodiscard]] auto validate_persona_document(const PersonaDocument& document)
    -> std::expected<void, PersonaValidationError>;

}  // namespace aiforge::domain
