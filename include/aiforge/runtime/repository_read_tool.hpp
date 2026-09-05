#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>

#include <aiforge/repository/exact_source_edit.hpp>
#include <aiforge/repository/snapshot_source.hpp>
#include <aiforge/runtime/automatic_approval_matcher.hpp>
#include <aiforge/runtime/tool_registry.hpp>

namespace aiforge::runtime {

struct RepositoryReadToolConfiguration {
  std::string repository_root;
  repository::RepositorySnapshotLimits snapshot_limits{
      16384,
      4096,
      std::uint64_t{1024U} * 1024U,
      std::uint64_t{64U} * 1024U * 1024U,
      std::size_t{4U} * 1024U * 1024U,
      std::chrono::seconds{5},
      std::chrono::seconds{15}};
  repository::ExactSourceEditLimits read_limits{
      4096, std::uint64_t{1024U} * 1024U, 1, std::chrono::seconds{30}};
  std::size_t maximum_argument_bytes{std::size_t{16U} * 1024U};
  std::size_t maximum_result_bytes{std::size_t{2U} * 1024U * 1024U};
  auto operator==(const RepositoryReadToolConfiguration&) const
      -> bool = default;
};

[[nodiscard]] auto make_repository_read_approval_rule(
    std::shared_ptr<const DescriptorRelativePathAuthority> root,
    std::string allowed_relative_path,
    AutomaticApprovalRuleConstraints constraints)
    -> std::expected<AutomaticApprovalRule, AutomaticApprovalMatcherError>;

[[nodiscard]] auto repository_read_tool_declaration(
    const RepositoryReadToolConfiguration& configuration)
    -> std::expected<backend::ToolDeclaration, ToolRegistryError>;

[[nodiscard]] auto register_repository_read_tool(
    ToolRegistry& registry, repository::RepositorySnapshotSource& snapshots,
    repository::ExactSourceEditor& sources,
    RepositoryReadToolConfiguration configuration)
    -> std::expected<void, ToolRegistryError>;

} // namespace aiforge::runtime
