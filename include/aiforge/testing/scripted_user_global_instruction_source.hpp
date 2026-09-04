#pragma once

#include <cstddef>
#include <optional>
#include <variant>
#include <vector>

#include <aiforge/instructions/source.hpp>

namespace aiforge::testing {

using UserGlobalInstructionLoadOutcome =
    std::variant<std::optional<domain::UserGlobalInstructionDocument>,
                 instructions::UserGlobalInstructionError>;

class ScriptedUserGlobalInstructionSource final
    : public instructions::UserGlobalInstructionSource {
 public:
  explicit ScriptedUserGlobalInstructionSource(
      std::vector<UserGlobalInstructionLoadOutcome> outcomes = {});

  [[nodiscard]] auto load(instructions::UserGlobalInstructionLimits limits = {},
                          std::stop_token stop_token = {})
      -> std::expected<std::optional<domain::UserGlobalInstructionDocument>,
                       instructions::UserGlobalInstructionError> override;

  [[nodiscard]] auto recorded_limits() const noexcept
      -> const std::vector<instructions::UserGlobalInstructionLimits>&;
  [[nodiscard]] auto remaining_loads() const noexcept -> std::size_t;

 private:
  std::vector<UserGlobalInstructionLoadOutcome> m_outcomes;
  std::vector<instructions::UserGlobalInstructionLimits> m_recorded_limits;
  std::size_t m_next{};
};

} // namespace aiforge::testing
