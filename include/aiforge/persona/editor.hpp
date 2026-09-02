#pragma once

#include <expected>
#include <optional>
#include <stop_token>
#include <string>

#include <aiforge/persona/source.hpp>

namespace aiforge::persona {

enum class PersonaFileKind {
  markdown,
  text,
};

struct PersonaDraft {
  std::string name;
  PersonaFileKind file_kind{PersonaFileKind::markdown};
  std::string text;
  auto operator==(const PersonaDraft&) const -> bool = default;
};

struct PersonaCreate {
  PersonaDraft draft;
  PersonaLimits limits{};
  auto operator==(const PersonaCreate&) const -> bool = default;
};

struct PersonaReplace {
  domain::PersonaReference expected;
  std::string text;
  PersonaLimits limits{};
  auto operator==(const PersonaReplace&) const -> bool = default;
};

struct PersonaWriteReceipt {
  std::optional<domain::PersonaReference> previous;
  domain::PersonaReference resulting;
  auto operator==(const PersonaWriteReceipt&) const -> bool = default;
};

enum class PersonaEditorErrorCode {
  invalid_request,
  invalid_name,
  invalid_file_kind,
  malformed_text,
  already_exists,
  not_found,
  source_mismatch,
  concurrent_change,
  path_escape,
  unsupported_entry,
  resource_exhausted,
  permission_denied,
  durability_failure,
  cancelled,
  io_failure,
  internal_failure,
};

struct PersonaEditorError {
  PersonaEditorErrorCode code{PersonaEditorErrorCode::internal_failure};
  std::string message;
  std::optional<domain::PersonaReference> observed;
  bool retryable{};
  // True means publication may have completed and the caller must reload the
  // persona before retrying or updating live session state.
  bool may_have_applied{};
  auto operator==(const PersonaEditorError&) const -> bool = default;
};

class PersonaEditor {
 public:
  virtual ~PersonaEditor() = default;

  [[nodiscard]] virtual auto create(PersonaCreate request,
                                    std::stop_token stop_token = {})
      -> std::expected<PersonaWriteReceipt, PersonaEditorError> = 0;
  [[nodiscard]] virtual auto replace(PersonaReplace request,
                                     std::stop_token stop_token = {})
      -> std::expected<PersonaWriteReceipt, PersonaEditorError> = 0;
};

[[nodiscard]] auto prepare_persona_create(const PersonaCreate& request)
    -> std::expected<domain::PersonaDocument, PersonaEditorError>;
[[nodiscard]] auto prepare_persona_replace(const PersonaReplace& request)
    -> std::expected<domain::PersonaDocument, PersonaEditorError>;
[[nodiscard]] auto validate_persona_write_receipt(
    const PersonaCreate& request, const PersonaWriteReceipt& receipt)
    -> std::expected<void, PersonaEditorError>;
[[nodiscard]] auto validate_persona_write_receipt(
    const PersonaReplace& request, const PersonaWriteReceipt& receipt)
    -> std::expected<void, PersonaEditorError>;

} // namespace aiforge::persona
