#include <aiforge/adapters/process_repository.hpp>

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string_view>
#include <utility>

#include <aiforge/adapters/git_repository_snapshot_source.hpp>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace aiforge::adapters {
namespace {

#ifndef _WIN32
[[nodiscard]] auto executable_at(const std::filesystem::path& path)
    -> std::optional<std::string> {
  std::error_code error;
  const auto canonical = std::filesystem::canonical(path, error);
  if (error || !canonical.is_absolute()) return std::nullopt;
  const auto status = std::filesystem::status(canonical, error);
  if (error || !std::filesystem::is_regular_file(status) ||
      ::access(canonical.c_str(), X_OK) != 0) {
    return std::nullopt;
  }
  return canonical.string();
}

[[nodiscard]] auto resolve_git() -> std::expected<std::string, std::string> {
  const char* raw_path = std::getenv("PATH");
  if (raw_path == nullptr) {
    return std::unexpected("Git executable could not be resolved");
  }
  const std::string search{raw_path};
  std::size_t start{};
  while (start <= search.size()) {
    const auto end = search.find(':', start);
    const auto part = std::string_view{search}.substr(
        start, end == std::string::npos ? end : end - start);
    const std::filesystem::path directory{part};
    if (!part.empty() && directory.is_absolute()) {
      if (auto resolved = executable_at(directory / "git")) return *resolved;
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return std::unexpected("Git executable could not be resolved");
}
#endif

} // namespace

auto open_process_repository_source()
    -> std::expected<GitRepositorySnapshotSource, std::string> {
  return open_process_repository_source(GitCommandPolicy::standard);
}

auto open_process_repository_source(const GitCommandPolicy policy)
    -> std::expected<GitRepositorySnapshotSource, std::string> {
#ifdef _WIN32
  static_cast<void>(policy);
  return std::unexpected(
      "Git repository observation is unavailable on this platform");
#else
  auto git = resolve_git();
  if (!git) return std::unexpected(std::move(git.error()));
  auto source = GitRepositorySnapshotSource::open(std::move(*git), policy);
  if (!source) return std::unexpected(source.error().message);
  return std::move(*source);
#endif
}

auto observe_process_repository(const std::stop_token stop_token)
    -> std::expected<domain::RepositorySnapshot, std::string> {
#ifdef _WIN32
  static_cast<void>(stop_token);
  return std::unexpected(
      "Git repository observation is unavailable on this platform");
#else
  auto source = open_process_repository_source();
  if (!source) return std::unexpected(std::move(source.error()));
  std::error_code path_error;
  auto root = std::filesystem::current_path(path_error);
  if (path_error) {
    return std::unexpected("Current repository path is unavailable");
  }
  auto snapshot = source->observe({root.string(), {}}, stop_token);
  if (!snapshot) return std::unexpected(snapshot.error().message);
  return std::move(*snapshot);
#endif
}

} // namespace aiforge::adapters
