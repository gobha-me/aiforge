#pragma once

#include <cstddef>
#include <variant>
#include <vector>

#include <aiforge/persona/editor.hpp>

namespace aiforge::testing {

using PersonaWriteOutcome =
    std::variant<persona::PersonaWriteReceipt, persona::PersonaEditorError>;

struct PersonaCreateExchange {
  persona::PersonaCreate expected_request;
  PersonaWriteOutcome outcome;
  auto operator==(const PersonaCreateExchange&) const -> bool = default;
};

struct PersonaReplaceExchange {
  persona::PersonaReplace expected_request;
  PersonaWriteOutcome outcome;
  auto operator==(const PersonaReplaceExchange&) const -> bool = default;
};

class ScriptedPersonaEditor final : public persona::PersonaEditor {
 public:
  explicit ScriptedPersonaEditor(
      std::vector<PersonaCreateExchange> create_exchanges = {},
      std::vector<PersonaReplaceExchange> replace_exchanges = {});

  [[nodiscard]] auto create(persona::PersonaCreate request,
                            std::stop_token stop_token = {})
      -> std::expected<persona::PersonaWriteReceipt,
                       persona::PersonaEditorError> override;
  [[nodiscard]] auto replace(persona::PersonaReplace request,
                             std::stop_token stop_token = {})
      -> std::expected<persona::PersonaWriteReceipt,
                       persona::PersonaEditorError> override;

  [[nodiscard]] auto recorded_creates() const noexcept
      -> const std::vector<persona::PersonaCreate>&;
  [[nodiscard]] auto recorded_replaces() const noexcept
      -> const std::vector<persona::PersonaReplace>&;
  [[nodiscard]] auto remaining_creates() const noexcept -> std::size_t;
  [[nodiscard]] auto remaining_replaces() const noexcept -> std::size_t;

 private:
  std::vector<PersonaCreateExchange> m_create_exchanges;
  std::vector<PersonaReplaceExchange> m_replace_exchanges;
  std::vector<persona::PersonaCreate> m_recorded_creates;
  std::vector<persona::PersonaReplace> m_recorded_replaces;
  std::size_t m_next_create{};
  std::size_t m_next_replace{};
};

} // namespace aiforge::testing
