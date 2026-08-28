#pragma once

#include <cstddef>
#include <variant>
#include <vector>

#include <aiforge/repository/project_instruction_source.hpp>

namespace aiforge::testing {

using ProjectInstructionOutcome =
    std::variant<domain::ProjectInstructionDiscovery,
                 repository::ProjectInstructionError>;

struct ProjectInstructionExchange {
  repository::ProjectInstructionRequest expected_request;
  ProjectInstructionOutcome outcome;
  auto operator==(const ProjectInstructionExchange&) const -> bool = default;
};

class ScriptedProjectInstructionSource final
    : public repository::ProjectInstructionSource {
 public:
  explicit ScriptedProjectInstructionSource(
      std::vector<ProjectInstructionExchange> exchanges = {});

  [[nodiscard]] auto discover(repository::ProjectInstructionRequest request,
                              std::stop_token stop_token = {})
      -> std::expected<domain::ProjectInstructionDiscovery,
                       repository::ProjectInstructionError> override;

  [[nodiscard]] auto recorded_requests() const noexcept
      -> const std::vector<repository::ProjectInstructionRequest>&;
  [[nodiscard]] auto remaining_exchanges() const noexcept -> std::size_t;

 private:
  std::vector<ProjectInstructionExchange> m_exchanges;
  std::vector<repository::ProjectInstructionRequest> m_recorded_requests;
  std::size_t m_next_exchange{};
};

} // namespace aiforge::testing
