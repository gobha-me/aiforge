#pragma once

#include <expected>
#include <stop_token>
#include <string>

#include <aiforge/adapters/git_repository_snapshot_source.hpp>
#include <aiforge/domain/repository.hpp>

namespace aiforge::adapters {

[[nodiscard]] auto open_process_repository_source()
    -> std::expected<GitRepositorySnapshotSource, std::string>;

[[nodiscard]] auto open_process_repository_source(GitCommandPolicy policy)
    -> std::expected<GitRepositorySnapshotSource, std::string>;

[[nodiscard]] auto observe_process_repository(std::stop_token stop_token = {})
    -> std::expected<domain::RepositorySnapshot, std::string>;

} // namespace aiforge::adapters
