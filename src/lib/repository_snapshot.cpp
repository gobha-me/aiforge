#include <aiforge/repository/snapshot_source.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <limits>
#include <string_view>

namespace aiforge::domain {

auto snapshot_identity(const RepositorySnapshot& snapshot)
    -> RepositorySnapshotIdentity {
  return {snapshot.root.repository_id, snapshot.fingerprint};
}

auto same_source_state(const RepositorySnapshotIdentity& left,
                       const RepositorySnapshotIdentity& right) noexcept
    -> bool {
  return left.repository_id == right.repository_id &&
         left.fingerprint == right.fingerprint;
}

auto same_source_state(const RepositorySnapshot& left,
                       const RepositorySnapshot& right) noexcept -> bool {
  return same_source_state(snapshot_identity(left), snapshot_identity(right));
}

} // namespace aiforge::domain

namespace aiforge::repository {
namespace {

[[nodiscard]] auto failure(const RepositorySnapshotErrorCode code,
                           std::string message)
    -> std::unexpected<RepositorySnapshotError> {
  return std::unexpected(
      RepositorySnapshotError{code, std::move(message), false});
}

[[nodiscard]] auto bounded_text(const std::string_view value,
                                const std::size_t maximum) -> bool {
  if (value.empty() || value.size() > maximum) return false;
  return std::ranges::none_of(value, [](const unsigned char character) {
    return character == 0 || character == 0x7FU;
  });
}

[[nodiscard]] auto valid_digest(const domain::ContentDigest& digest,
                                const std::uint64_t maximum_bytes) -> bool {
  if (!bounded_text(digest.algorithm, 128) ||
      !bounded_text(digest.value, 512) || digest.byte_size > maximum_bytes) {
    return false;
  }
  return std::ranges::all_of(digest.algorithm,
                             [](const unsigned char value) {
                               return std::isalnum(value) != 0 ||
                                      value == '-' || value == '_' ||
                                      value == '.';
                             }) &&
         std::ranges::all_of(digest.value, [](const unsigned char value) {
           return std::isxdigit(value) != 0;
         });
}

[[nodiscard]] auto valid_relative_path(const std::string& value,
                                       const std::size_t maximum) -> bool {
  if (value.empty() || value.size() > maximum ||
      value.find('\0') != std::string::npos) {
    return false;
  }
  const std::filesystem::path path{value};
  if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
    return false;
  }
  for (const auto& part : path) {
    if (part == "." || part == ".." || part.empty()) return false;
  }
  return path.generic_string() == value;
}

[[nodiscard]] auto add_checked(std::uint64_t& total, const std::uint64_t value)
    -> bool {
  if (value > std::numeric_limits<std::uint64_t>::max() - total) return false;
  total += value;
  return true;
}

} // namespace

auto validate_repository_snapshot(const domain::RepositorySnapshot& snapshot,
                                  const RepositorySnapshotLimits& limits)
    -> std::expected<void, RepositorySnapshotError> {
  constexpr RepositorySnapshotLimits maximums;
  if (limits.maximum_entries == 0 || limits.maximum_path_bytes == 0 ||
      limits.maximum_file_bytes == 0 || limits.maximum_total_bytes == 0 ||
      limits.maximum_command_output_bytes == 0 ||
      limits.command_timeout <= std::chrono::milliseconds::zero() ||
      limits.observation_timeout <= std::chrono::milliseconds::zero() ||
      limits.maximum_entries > maximums.maximum_entries ||
      limits.maximum_path_bytes > maximums.maximum_path_bytes ||
      limits.maximum_file_bytes > maximums.maximum_file_bytes ||
      limits.maximum_total_bytes > maximums.maximum_total_bytes ||
      limits.maximum_command_output_bytes >
          maximums.maximum_command_output_bytes ||
      limits.command_timeout > maximums.command_timeout ||
      limits.observation_timeout > maximums.observation_timeout ||
      limits.command_timeout > limits.observation_timeout) {
    return failure(RepositorySnapshotErrorCode::invalid_request,
                   "repository snapshot limits are invalid");
  }

  if (!bounded_text(snapshot.root.canonical_path,
                    limits.maximum_path_bytes * 4U)) {
    return failure(RepositorySnapshotErrorCode::invalid_request,
                   "repository root path is invalid");
  }
  const std::filesystem::path root{snapshot.root.canonical_path};
  if (!root.is_absolute() || root.lexically_normal() != root) {
    return failure(RepositorySnapshotErrorCode::invalid_request,
                   "repository root path must be canonical and absolute");
  }

  if (!valid_digest(snapshot.fingerprint, limits.maximum_total_bytes)) {
    return failure(RepositorySnapshotErrorCode::invalid_request,
                   "repository fingerprint is invalid");
  }

  if (snapshot.vcs) {
    const auto& vcs = *snapshot.vcs;
    if (!bounded_text(vcs.system, 64) || !bounded_text(vcs.object_format, 64)) {
      return failure(RepositorySnapshotErrorCode::invalid_request,
                     "VCS identity is invalid");
    }
    const bool branch_head = vcs.head_kind == domain::VcsHeadKind::branch;
    const bool unborn = vcs.head_kind == domain::VcsHeadKind::unborn;
    if ((branch_head || unborn) != vcs.branch.has_value() ||
        (!unborn) != vcs.revision.has_value() ||
        (vcs.branch && !bounded_text(*vcs.branch, 1024)) ||
        (vcs.revision && !bounded_text(*vcs.revision, 512))) {
      return failure(RepositorySnapshotErrorCode::invalid_request,
                     "VCS head fields are inconsistent");
    }
  }

  if (snapshot.changes.size() > limits.maximum_entries) {
    return failure(RepositorySnapshotErrorCode::resource_exhausted,
                   "repository snapshot contains too many entries");
  }
  std::uint64_t total_bytes{};
  std::string_view previous;
  for (const auto& change : snapshot.changes) {
    if (!valid_relative_path(change.relative_path, limits.maximum_path_bytes) ||
        (!previous.empty() && previous >= change.relative_path)) {
      return failure(RepositorySnapshotErrorCode::invalid_request,
                     "repository paths must be unique and canonically sorted");
    }
    previous = change.relative_path;
    if (!add_checked(total_bytes, change.relative_path.size())) {
      return failure(RepositorySnapshotErrorCode::resource_exhausted,
                     "repository snapshot size overflowed");
    }

    const bool renamed =
        change.change_kind == domain::RepositoryChangeKind::renamed;
    if (renamed != change.previous_path.has_value() ||
        (change.previous_path &&
         !valid_relative_path(*change.previous_path,
                              limits.maximum_path_bytes))) {
      return failure(RepositorySnapshotErrorCode::invalid_request,
                     "repository rename fields are inconsistent");
    }
    if (change.previous_path &&
        !add_checked(total_bytes, change.previous_path->size())) {
      return failure(RepositorySnapshotErrorCode::resource_exhausted,
                     "repository snapshot size overflowed");
    }

    const bool untracked =
        change.change_kind == domain::RepositoryChangeKind::untracked;
    if (untracked !=
            (change.stage == domain::RepositoryChangeStage::untracked) ||
        (untracked && change.index_digest)) {
      return failure(RepositorySnapshotErrorCode::invalid_request,
                     "untracked repository entry is inconsistent");
    }
    const bool deleted =
        change.change_kind == domain::RepositoryChangeKind::deleted;
    if (deleted == change.worktree_digest.has_value()) {
      return failure(RepositorySnapshotErrorCode::invalid_request,
                     "repository entry content state is inconsistent");
    }
    for (const auto* digest :
         {change.index_digest ? &*change.index_digest : nullptr,
          change.worktree_digest ? &*change.worktree_digest : nullptr}) {
      if (digest == nullptr) continue;
      if (!valid_digest(*digest, limits.maximum_file_bytes) ||
          !add_checked(total_bytes, digest->byte_size)) {
        return failure(RepositorySnapshotErrorCode::resource_exhausted,
                       "repository content digest exceeds its budget");
      }
    }
    if (total_bytes > limits.maximum_total_bytes) {
      return failure(RepositorySnapshotErrorCode::resource_exhausted,
                     "repository snapshot exceeds its total byte budget");
    }
  }
  return {};
}

} // namespace aiforge::repository
