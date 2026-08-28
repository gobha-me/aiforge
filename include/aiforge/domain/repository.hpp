#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <aiforge/domain/digest.hpp>
#include <aiforge/domain/ids.hpp>

namespace aiforge::domain {

struct RepositoryRootIdentity {
  RepositoryId repository_id;
  std::string canonical_path;
  auto operator==(const RepositoryRootIdentity&) const -> bool = default;
};

enum class VcsHeadKind {
  branch,
  detached,
  unborn,
};

struct VcsState {
  std::string system;
  std::string object_format;
  VcsHeadKind head_kind{VcsHeadKind::detached};
  std::optional<std::string> branch;
  std::optional<std::string> revision;
  auto operator==(const VcsState&) const -> bool = default;
};

enum class RepositoryEntryKind {
  regular_file,
  symbolic_link,
  submodule,
};

enum class RepositoryChangeKind {
  added,
  modified,
  deleted,
  renamed,
  type_changed,
  conflicted,
  untracked,
};

enum class RepositoryChangeStage {
  index,
  worktree,
  index_and_worktree,
  untracked,
};

struct RepositoryChange {
  std::string relative_path;
  std::optional<std::string> previous_path;
  RepositoryEntryKind entry_kind{RepositoryEntryKind::regular_file};
  RepositoryChangeKind change_kind{RepositoryChangeKind::modified};
  RepositoryChangeStage stage{RepositoryChangeStage::worktree};
  std::optional<ContentDigest> index_digest;
  std::optional<ContentDigest> worktree_digest;
  auto operator==(const RepositoryChange&) const -> bool = default;
};

struct RepositorySnapshot {
  RepositoryRootIdentity root;
  std::optional<VcsState> vcs;
  std::vector<RepositoryChange> changes;
  ContentDigest fingerprint;
  std::chrono::sys_time<std::chrono::milliseconds> observed_at;
  auto operator==(const RepositorySnapshot&) const -> bool = default;
};

struct RepositorySnapshotIdentity {
  RepositoryId repository_id;
  ContentDigest fingerprint;
  auto operator==(const RepositorySnapshotIdentity&) const -> bool = default;
};

struct SourceByteRange {
  // Zero-based, half-open byte offsets into content_digest's exact bytes.
  std::uint64_t begin{};
  std::uint64_t end{};
  auto operator==(const SourceByteRange&) const -> bool = default;
};

struct RepositorySourceIdentity {
  RepositorySnapshotIdentity snapshot;
  std::string relative_path;
  ContentDigest content_digest;
  std::optional<SourceByteRange> range;
  auto operator==(const RepositorySourceIdentity&) const -> bool = default;
};

[[nodiscard]] auto snapshot_identity(const RepositorySnapshot& snapshot)
    -> RepositorySnapshotIdentity;

[[nodiscard]] auto same_source_state(const RepositorySnapshot& left,
                                     const RepositorySnapshot& right) noexcept
    -> bool;
[[nodiscard]] auto same_source_state(
    const RepositorySnapshotIdentity& left,
    const RepositorySnapshotIdentity& right) noexcept -> bool;

} // namespace aiforge::domain
