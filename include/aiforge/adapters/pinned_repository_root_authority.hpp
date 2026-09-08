#pragma once

#include <expected>
#include <filesystem>
#include <memory>

#include <aiforge/runtime/automatic_approval_matcher.hpp>
#include <aiforge/runtime/repository_context_controller.hpp>

namespace aiforge::adapters {
class GitExactSourceEditor;
class GitProjectInstructionSource;
class GitRepositorySnapshotSource;
} // namespace aiforge::adapters

namespace aiforge::adapters {

// Every exported handle aliases the same stable owning envelope. The snapshot,
// exact reader and instruction adapter outlive both pinned authority and
// source.
struct OwnedPinnedRepositorySources {
  std::shared_ptr<GitRepositorySnapshotSource> snapshots;
  std::shared_ptr<GitExactSourceEditor> exact;
  std::shared_ptr<const runtime::PinnedRepositoryReadAuthority> authority;
  std::shared_ptr<runtime::RepositoryContextSource> context;
};
[[nodiscard]] auto open_owned_pinned_repository_sources(
    std::filesystem::path repository_root, GitRepositorySnapshotSource source,
    repository::RepositorySnapshotLimits snapshot_limits = {})
    -> std::expected<OwnedPinnedRepositorySources,
                     runtime::AutomaticApprovalMatcherError>;

// Pins one absolute repository root for the application lifetime. Every match
// reopens the recorded root chain without following symlinks and traverses the
// candidate relative to that verified descriptor.
[[nodiscard]] auto open_pinned_repository_root_authority(
    std::filesystem::path repository_root,
    GitRepositorySnapshotSource& snapshot_source,
    GitExactSourceEditor& exact_source,
    repository::RepositorySnapshotLimits snapshot_limits = {})
    -> std::expected<
        std::shared_ptr<const runtime::PinnedRepositoryReadAuthority>,
        runtime::AutomaticApprovalMatcherError>;

[[nodiscard]] auto make_pinned_repository_context_source(
    std::shared_ptr<const runtime::PinnedRepositoryReadAuthority> authority,
    GitProjectInstructionSource& instructions)
    -> std::expected<std::shared_ptr<runtime::RepositoryContextSource>,
                     runtime::AutomaticApprovalMatcherError>;

} // namespace aiforge::adapters
