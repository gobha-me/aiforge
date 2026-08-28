#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <aiforge/domain/repository_evidence.hpp>

namespace aiforge::domain {

struct ProjectInstructionDocument {
  ProjectInstructionId instruction_id;
  RepositorySourceIdentity source;
  // Empty denotes the repository root; otherwise this is a normalized,
  // repository-relative directory.
  std::string applicable_subtree;
  std::string text;
  std::uint32_t specificity{};
  std::uint64_t discovery_order{};
  auto operator==(const ProjectInstructionDocument&) const -> bool = default;
};

struct ProjectInstructionDiscovery {
  RepositorySnapshotIdentity source_snapshot;
  std::string target_subtree;
  std::vector<ProjectInstructionDocument> documents;
  auto operator==(const ProjectInstructionDiscovery&) const -> bool = default;
};

} // namespace aiforge::domain
