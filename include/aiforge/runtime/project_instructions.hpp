#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include <aiforge/domain/context.hpp>
#include <aiforge/domain/project_instructions.hpp>

namespace aiforge::runtime {

struct ProjectInstructionTokenEstimate {
  domain::ProjectInstructionId instruction_id;
  std::uint64_t estimated_tokens{};
  auto operator==(const ProjectInstructionTokenEstimate&) const
      -> bool = default;
};

enum class ProjectInstructionContextErrorCode {
  invalid_discovery,
  stale_snapshot,
  duplicate_estimate,
  missing_estimate,
  invalid_estimate,
  invalid_identity,
  internal_failure,
};

struct ProjectInstructionContextError {
  ProjectInstructionContextErrorCode code{
      ProjectInstructionContextErrorCode::internal_failure};
  std::string message;
  auto operator==(const ProjectInstructionContextError&) const
      -> bool = default;
};

[[nodiscard]] auto project_instruction_inputs(
    const domain::ProjectInstructionDiscovery& discovery,
    const domain::RepositorySnapshotIdentity& current_snapshot,
    std::span<const ProjectInstructionTokenEstimate> estimates)
    -> std::expected<std::vector<domain::InstructionInput>,
                     ProjectInstructionContextError>;

} // namespace aiforge::runtime
