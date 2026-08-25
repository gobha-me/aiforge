#pragma once

#include <cstddef>
#include <expected>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include <aiforge/domain/persona.hpp>

namespace aiforge::persona {

struct PersonaLimits {
  std::size_t maximum_personas{256};
  std::size_t maximum_name_bytes{96};
  std::size_t maximum_file_bytes{1024U * 1024U};
  std::size_t maximum_description_bytes{160};
  auto operator==(const PersonaLimits&) const -> bool = default;
};

enum class PersonaErrorCode {
  invalid_request,
  invalid_name,
  missing_home,
  invalid_root,
  not_found,
  ambiguous_name,
  path_escape,
  unsupported_entry,
  malformed_text,
  unstable,
  resource_exhausted,
  permission_denied,
  cancelled,
  io_failure,
  internal_failure,
};

struct PersonaError {
  PersonaErrorCode code{PersonaErrorCode::internal_failure};
  std::string message;
  std::optional<std::string> name;
  bool retryable{};
  auto operator==(const PersonaError&) const -> bool = default;
};

enum class PersonaDirectiveKind {
  inherit,
  select,
  disable,
};

struct PersonaDirective {
  PersonaDirectiveKind kind{PersonaDirectiveKind::inherit};
  std::optional<std::string> name;
  domain::PersonaSelectionSource source{
      domain::PersonaSelectionSource::command_line};
  auto operator==(const PersonaDirective&) const -> bool = default;
};

class PersonaSource {
 public:
  virtual ~PersonaSource() = default;

  [[nodiscard]] virtual auto list(PersonaLimits limits = {},
                                  std::stop_token stop_token = {})
      -> std::expected<std::vector<domain::PersonaSummary>, PersonaError> = 0;
  [[nodiscard]] virtual auto load(std::string name, PersonaLimits limits = {},
                                  std::stop_token stop_token = {})
      -> std::expected<domain::PersonaDocument, PersonaError> = 0;
};

}  // namespace aiforge::persona
