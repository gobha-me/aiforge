#include <aiforge/adapters/git_repository_snapshot_source.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace aiforge::adapters {
namespace {

using domain::ContentDigest;
using domain::RepositoryChange;
using domain::RepositoryChangeKind;
using domain::RepositoryChangeStage;
using domain::RepositoryEntryKind;
using domain::RepositorySnapshot;
using domain::VcsHeadKind;
using domain::VcsState;
using repository::RepositorySnapshotError;
using repository::RepositorySnapshotErrorCode;
using repository::RepositorySnapshotLimits;
using ObservationDeadline = std::chrono::steady_clock::time_point;

struct CommandResult {
  int exit_code{};
  std::string output;
  std::string error;
};

[[nodiscard]] auto failure(const RepositorySnapshotErrorCode code,
                           std::string message, const bool retryable = false)
    -> std::unexpected<RepositorySnapshotError> {
  return std::unexpected(
      RepositorySnapshotError{code, std::move(message), retryable});
}

#ifndef _WIN32
auto close_fd(int& descriptor) noexcept -> void {
  if (descriptor >= 0) {
    static_cast<void>(::close(descriptor));
    descriptor = -1;
  }
}

auto terminate_child(const pid_t child) noexcept -> void {
  if (child <= 0) return;
  static_cast<void>(::kill(-child, SIGKILL));
  static_cast<void>(::kill(child, SIGKILL));
  int status{};
  while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
  }
}

[[nodiscard]] auto set_nonblocking(const int descriptor) -> bool {
  const int flags = ::fcntl(descriptor, F_GETFL, 0);
  return flags >= 0 && ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0;
}

[[nodiscard]] auto append_pipe(int& descriptor, std::string& target,
                               const std::size_t maximum) -> int {
  std::array<char, 8192> buffer{};
  while (descriptor >= 0) {
    const auto read_count = ::read(descriptor, buffer.data(), buffer.size());
    if (read_count > 0) {
      if (static_cast<std::size_t>(read_count) > maximum - target.size()) {
        return 1;
      }
      target.append(buffer.data(), static_cast<std::size_t>(read_count));
      continue;
    }
    if (read_count == 0) {
      close_fd(descriptor);
      break;
    }
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
    return -1;
  }
  return 0;
}
#endif

[[nodiscard]] auto run_command(
    const std::string& executable, const std::vector<std::string>& arguments,
    const std::string_view input, const RepositorySnapshotLimits& limits,
    const std::stop_token stop_token,
    const ObservationDeadline observation_deadline)
    -> std::expected<CommandResult, RepositorySnapshotError> {
#ifdef _WIN32
  static_cast<void>(executable);
  static_cast<void>(arguments);
  static_cast<void>(input);
  static_cast<void>(limits);
  static_cast<void>(stop_token);
  static_cast<void>(observation_deadline);
  return failure(RepositorySnapshotErrorCode::vcs_failure,
                 "repository command execution is unavailable on this platform");
#else
  int stdin_pipe[2]{-1, -1};
  int stdout_pipe[2]{-1, -1};
  int stderr_pipe[2]{-1, -1};
  if (::pipe(stdin_pipe) != 0 || ::pipe(stdout_pipe) != 0 ||
      ::pipe(stderr_pipe) != 0) {
    close_fd(stdin_pipe[0]);
    close_fd(stdin_pipe[1]);
    close_fd(stdout_pipe[0]);
    close_fd(stdout_pipe[1]);
    close_fd(stderr_pipe[0]);
    close_fd(stderr_pipe[1]);
    return failure(RepositorySnapshotErrorCode::io_failure,
                   "repository command pipes could not be created");
  }

  const pid_t child = ::fork();
  if (child < 0) {
    for (auto* descriptor : {&stdin_pipe[0], &stdin_pipe[1], &stdout_pipe[0],
                             &stdout_pipe[1], &stderr_pipe[0],
                             &stderr_pipe[1]}) {
      close_fd(*descriptor);
    }
    return failure(RepositorySnapshotErrorCode::io_failure,
                   "repository command could not be started");
  }
  if (child == 0) {
    static_cast<void>(::setpgid(0, 0));
    if (::dup2(stdin_pipe[0], STDIN_FILENO) < 0 ||
        ::dup2(stdout_pipe[1], STDOUT_FILENO) < 0 ||
        ::dup2(stderr_pipe[1], STDERR_FILENO) < 0) {
      _exit(126);
    }
    for (const int descriptor : {stdin_pipe[0], stdin_pipe[1], stdout_pipe[0],
                                 stdout_pipe[1], stderr_pipe[0],
                                 stderr_pipe[1]}) {
      static_cast<void>(::close(descriptor));
    }
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 2);
    argv.push_back(const_cast<char*>(executable.c_str()));
    for (const auto& argument : arguments) {
      argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    ::execv(executable.c_str(), argv.data());
    _exit(errno == ENOENT ? 127 : 126);
  }

  static_cast<void>(::setpgid(child, child));
  close_fd(stdin_pipe[0]);
  close_fd(stdout_pipe[1]);
  close_fd(stderr_pipe[1]);
  if (!set_nonblocking(stdin_pipe[1]) || !set_nonblocking(stdout_pipe[0]) ||
      !set_nonblocking(stderr_pipe[0])) {
    close_fd(stdin_pipe[1]);
    close_fd(stdout_pipe[0]);
    close_fd(stderr_pipe[0]);
    terminate_child(child);
    return failure(RepositorySnapshotErrorCode::io_failure,
                   "repository command pipes could not be configured");
  }
  if (input.empty()) close_fd(stdin_pipe[1]);

  CommandResult result;
  std::size_t written{};
  bool child_finished{};
  int child_status{};
  const auto deadline = std::min(std::chrono::steady_clock::now() +
                                     limits.command_timeout,
                                 observation_deadline);
  while (!child_finished || stdout_pipe[0] >= 0 || stderr_pipe[0] >= 0) {
    if (stop_token.stop_requested()) {
      close_fd(stdin_pipe[1]);
      close_fd(stdout_pipe[0]);
      close_fd(stderr_pipe[0]);
      terminate_child(child);
      return failure(RepositorySnapshotErrorCode::cancelled,
                     "repository observation cancelled");
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      close_fd(stdin_pipe[1]);
      close_fd(stdout_pipe[0]);
      close_fd(stderr_pipe[0]);
      terminate_child(child);
      return failure(RepositorySnapshotErrorCode::timed_out,
                     "repository observation timed out", true);
    }

    std::array<pollfd, 3> descriptors{{
        {stdout_pipe[0],
         static_cast<short>(stdout_pipe[0] >= 0 ? POLLIN : 0), 0},
        {stderr_pipe[0],
         static_cast<short>(stderr_pipe[0] >= 0 ? POLLIN : 0), 0},
        {stdin_pipe[1],
         static_cast<short>(stdin_pipe[1] >= 0 ? POLLOUT : 0), 0},
    }};
    const int polled = ::poll(descriptors.data(), descriptors.size(), 20);
    if (polled < 0 && errno != EINTR) {
      close_fd(stdin_pipe[1]);
      close_fd(stdout_pipe[0]);
      close_fd(stderr_pipe[0]);
      terminate_child(child);
      return failure(RepositorySnapshotErrorCode::io_failure,
                     "repository command polling failed");
    }

    if (stdin_pipe[1] >= 0 &&
        (descriptors[2].revents & (POLLOUT | POLLHUP | POLLERR)) != 0) {
      const auto remaining = input.substr(written);
      const auto write_count =
          ::write(stdin_pipe[1], remaining.data(), remaining.size());
      if (write_count > 0) {
        written += static_cast<std::size_t>(write_count);
        if (written == input.size()) close_fd(stdin_pipe[1]);
      } else if (write_count < 0 && errno != EINTR && errno != EAGAIN &&
                 errno != EWOULDBLOCK) {
        close_fd(stdin_pipe[1]);
      }
    }

    const int stdout_result = append_pipe(
        stdout_pipe[0], result.output, limits.maximum_command_output_bytes);
    const int stderr_result = append_pipe(
        stderr_pipe[0], result.error, limits.maximum_command_output_bytes);
    if (stdout_result != 0 || stderr_result != 0) {
      close_fd(stdin_pipe[1]);
      close_fd(stdout_pipe[0]);
      close_fd(stderr_pipe[0]);
      terminate_child(child);
      return failure(stdout_result > 0 || stderr_result > 0
                         ? RepositorySnapshotErrorCode::resource_exhausted
                         : RepositorySnapshotErrorCode::io_failure,
                     stdout_result > 0 || stderr_result > 0
                         ? "repository command output exceeded its budget"
                         : "repository command output could not be read");
    }

    if (!child_finished) {
      const pid_t waited = ::waitpid(child, &child_status, WNOHANG);
      if (waited == child) child_finished = true;
      if (waited < 0 && errno != EINTR) {
        close_fd(stdin_pipe[1]);
        close_fd(stdout_pipe[0]);
        close_fd(stderr_pipe[0]);
        terminate_child(child);
        return failure(RepositorySnapshotErrorCode::io_failure,
                       "repository command status could not be read");
      }
    }
  }
  close_fd(stdin_pipe[1]);
  if (!child_finished) {
    while (::waitpid(child, &child_status, 0) < 0 && errno == EINTR) {
    }
  }
  result.exit_code = WIFEXITED(child_status)
                         ? WEXITSTATUS(child_status)
                         : (WIFSIGNALED(child_status)
                                ? 128 + WTERMSIG(child_status)
                                : 126);
  return result;
#endif
}

[[nodiscard]] auto trim_record(std::string value) -> std::string {
  while (!value.empty() &&
         (value.back() == '\n' || value.back() == '\r' || value.back() == '\0')) {
    value.pop_back();
  }
  return value;
}

[[nodiscard]] auto split_nul(const std::string& value)
    -> std::vector<std::string_view> {
  std::vector<std::string_view> records;
  std::size_t begin{};
  while (begin < value.size()) {
    const auto end = value.find('\0', begin);
    if (end == std::string::npos) {
      records.emplace_back(value.data() + begin, value.size() - begin);
      break;
    }
    records.emplace_back(value.data() + begin, end - begin);
    begin = end + 1;
  }
  return records;
}

[[nodiscard]] auto fields_with_path(const std::string_view record,
                                    const std::size_t field_count)
    -> std::optional<std::pair<std::vector<std::string_view>, std::string_view>> {
  std::vector<std::string_view> fields;
  fields.reserve(field_count);
  std::size_t begin{};
  for (std::size_t index{}; index < field_count; ++index) {
    const auto end = record.find(' ', begin);
    if (end == std::string_view::npos || end == begin) return std::nullopt;
    fields.push_back(record.substr(begin, end - begin));
    begin = end + 1;
  }
  if (begin >= record.size()) return std::nullopt;
  return std::pair{std::move(fields), record.substr(begin)};
}

auto append_manifest_field(std::string& manifest, const std::string_view label,
                           const std::string_view value) -> void {
  manifest.append(std::to_string(label.size()));
  manifest.push_back(':');
  manifest.append(label);
  manifest.append(std::to_string(value.size()));
  manifest.push_back(':');
  manifest.append(value);
}

[[nodiscard]] auto error_for_path(const std::error_code& error,
                                  std::string message)
    -> std::unexpected<RepositorySnapshotError> {
  if (error == std::errc::no_such_file_or_directory) {
    return failure(RepositorySnapshotErrorCode::not_found, std::move(message));
  }
  if (error == std::errc::permission_denied) {
    return failure(RepositorySnapshotErrorCode::permission_denied,
                   std::move(message));
  }
  return failure(RepositorySnapshotErrorCode::io_failure, std::move(message));
}

[[nodiscard]] auto has_git_marker(std::filesystem::path path) -> bool {
  std::error_code error;
  while (!path.empty()) {
    const auto marker = std::filesystem::symlink_status(path / ".git", error);
    if (error == std::errc::no_such_file_or_directory) {
      error.clear();
    } else if (error) {
      return true;
    }
    if (std::filesystem::exists(marker)) return true;
    const auto parent = path.parent_path();
    if (parent == path) break;
    path = parent;
  }
  return false;
}

[[nodiscard]] auto entry_kind(const std::filesystem::file_status status)
    -> std::optional<RepositoryEntryKind> {
  if (std::filesystem::is_symlink(status)) {
    return RepositoryEntryKind::symbolic_link;
  }
  if (std::filesystem::is_regular_file(status)) {
    return RepositoryEntryKind::regular_file;
  }
  if (std::filesystem::is_directory(status)) {
    return RepositoryEntryKind::submodule;
  }
  return std::nullopt;
}

[[nodiscard]] auto kind_from_mode(const std::string_view mode)
    -> RepositoryEntryKind {
  if (mode == "120000") return RepositoryEntryKind::symbolic_link;
  if (mode == "160000") return RepositoryEntryKind::submodule;
  return RepositoryEntryKind::regular_file;
}

[[nodiscard]] auto stage_from_xy(const std::string_view xy)
    -> RepositoryChangeStage {
  const bool index = xy.size() == 2 && xy[0] != '.';
  const bool worktree = xy.size() == 2 && xy[1] != '.';
  if (index && worktree) return RepositoryChangeStage::index_and_worktree;
  return index ? RepositoryChangeStage::index
               : RepositoryChangeStage::worktree;
}

[[nodiscard]] auto change_from_xy(const std::string_view xy,
                                  const bool rename,
                                  const bool exists) -> RepositoryChangeKind {
  if (xy.find('U') != std::string_view::npos ||
      (xy.size() == 2 && xy[0] == 'A' && xy[1] == 'A') ||
      (xy.size() == 2 && xy[0] == 'D' && xy[1] == 'D')) {
    return RepositoryChangeKind::conflicted;
  }
  if (rename || xy.find('R') != std::string_view::npos) {
    return RepositoryChangeKind::renamed;
  }
  if (!exists && xy.find('D') != std::string_view::npos) {
    return RepositoryChangeKind::deleted;
  }
  if (xy.find('A') != std::string_view::npos) {
    return RepositoryChangeKind::added;
  }
  if (xy.find('T') != std::string_view::npos) {
    return RepositoryChangeKind::type_changed;
  }
  return RepositoryChangeKind::modified;
}

}  // namespace

struct GitRepositorySnapshotSource::Impl {
  explicit Impl(std::string executable) : git_executable(std::move(executable)) {}

  std::string git_executable;

  [[nodiscard]] auto git(const std::vector<std::string>& arguments,
                         const std::string_view input,
                         const RepositorySnapshotLimits& limits,
                         const std::stop_token stop_token,
                         const ObservationDeadline deadline) const
      -> std::expected<CommandResult, RepositorySnapshotError> {
    return run_command(git_executable, arguments, input, limits, stop_token,
                       deadline);
  }

  [[nodiscard]] auto hash_bytes(const std::string_view bytes,
                                const std::optional<std::string>& root,
                                const std::string& algorithm,
                                const RepositorySnapshotLimits& limits,
                                const std::stop_token stop_token,
                                const ObservationDeadline deadline) const
      -> std::expected<ContentDigest, RepositorySnapshotError> {
    std::vector<std::string> arguments;
    if (root) {
      arguments.insert(arguments.end(), {"-C", *root});
    }
    arguments.insert(arguments.end(), {"hash-object", "--stdin"});
    auto result = git(arguments, bytes, limits, stop_token, deadline);
    if (!result) return std::unexpected(std::move(result.error()));
    if (result->exit_code != 0) {
      return failure(RepositorySnapshotErrorCode::vcs_failure,
                     "Git could not hash repository content");
    }
    auto value = trim_record(std::move(result->output));
    if (value.empty()) {
      return failure(RepositorySnapshotErrorCode::vcs_failure,
                     "Git returned an empty content hash");
    }
    return ContentDigest{algorithm, std::move(value), bytes.size()};
  }

  [[nodiscard]] auto hash_path(const std::string& root,
                               const std::string& relative_path,
                               const RepositoryEntryKind kind,
                               const std::string& algorithm,
                               const RepositorySnapshotLimits& limits,
                               std::uint64_t& total_bytes,
                               const std::stop_token stop_token,
                               const ObservationDeadline deadline) const
      -> std::expected<ContentDigest, RepositorySnapshotError> {
    if (stop_token.stop_requested()) {
      return failure(RepositorySnapshotErrorCode::cancelled,
                     "repository observation cancelled");
    }
    const auto full_path = std::filesystem::path{root} / relative_path;
    std::error_code error;
    std::uint64_t bytes{};
    if (kind == RepositoryEntryKind::symbolic_link) {
      const auto target = std::filesystem::read_symlink(full_path, error);
      if (error) return error_for_path(error, "repository symlink could not be read");
      bytes = target.native().size();
    } else if (kind == RepositoryEntryKind::submodule) {
      auto revision = git({"-C", full_path.string(), "rev-parse", "HEAD"},
                          {}, limits, stop_token, deadline);
      if (!revision) return std::unexpected(std::move(revision.error()));
      if (revision->exit_code != 0) {
        return failure(RepositorySnapshotErrorCode::vcs_failure,
                       "repository submodule revision could not be read");
      }
      auto value = trim_record(std::move(revision->output));
      return ContentDigest{algorithm, std::move(value), 0};
    } else {
      bytes = std::filesystem::file_size(full_path, error);
      if (error) return error_for_path(error, "repository file size could not be read");
    }
    if (bytes > limits.maximum_file_bytes ||
        bytes > limits.maximum_total_bytes - total_bytes) {
      return failure(RepositorySnapshotErrorCode::resource_exhausted,
                     "repository content exceeds its byte budget");
    }
    auto result = git({"-C", root, "hash-object", "--no-filters", "--",
                       relative_path}, {}, limits, stop_token, deadline);
    if (!result) return std::unexpected(std::move(result.error()));
    if (result->exit_code != 0) {
      return failure(RepositorySnapshotErrorCode::io_failure,
                     "repository content could not be hashed", true);
    }
    auto value = trim_record(std::move(result->output));
    if (value.empty()) {
      return failure(RepositorySnapshotErrorCode::vcs_failure,
                     "Git returned an empty repository content hash");
    }
    total_bytes += bytes;
    return ContentDigest{algorithm, std::move(value), bytes};
  }

  [[nodiscard]] auto make_root(const std::string& canonical_path,
                               const std::optional<std::string>& root,
                               const std::string& algorithm,
                               const RepositorySnapshotLimits& limits,
                               const std::stop_token stop_token,
                               const ObservationDeadline deadline) const
      -> std::expected<domain::RepositoryRootIdentity,
                       RepositorySnapshotError> {
    auto digest = hash_bytes(canonical_path, root, algorithm, limits, stop_token,
                             deadline);
    if (!digest) return std::unexpected(std::move(digest.error()));
    auto id = domain::RepositoryId::from(algorithm + ":" + digest->value);
    if (!id) {
      return failure(RepositorySnapshotErrorCode::internal_failure,
                     "repository identity could not be represented");
    }
    return domain::RepositoryRootIdentity{std::move(*id), canonical_path};
  }

  [[nodiscard]] auto observe_git_once(
      const std::string& root, const std::string& object_format,
      const RepositorySnapshotLimits& limits,
      const std::stop_token stop_token,
      const ObservationDeadline deadline) const
      -> std::expected<RepositorySnapshot, RepositorySnapshotError> {
    const std::string algorithm = "git-" + object_format;
    auto status = git({"-C", root, "status", "--porcelain=v2", "--branch",
                       "-z", "--untracked-files=all"}, {}, limits, stop_token,
                      deadline);
    if (!status) return std::unexpected(std::move(status.error()));
    if (status->exit_code != 0) {
      return failure(RepositorySnapshotErrorCode::vcs_failure,
                     "Git could not observe repository status", true);
    }

    VcsState vcs{"git", object_format, VcsHeadKind::detached, std::nullopt,
                 std::nullopt};
    std::vector<RepositoryChange> changes;
    const auto records = split_nul(status->output);
    std::uint64_t total_bytes{};
    for (std::size_t index{}; index < records.size(); ++index) {
      const auto record = records[index];
      if (record.starts_with("# branch.oid ")) {
        const auto value = record.substr(13);
        if (value != "(initial)") vcs.revision = std::string{value};
        continue;
      }
      if (record.starts_with("# branch.head ")) {
        const auto value = record.substr(14);
        if (value == "(detached)") {
          vcs.head_kind = VcsHeadKind::detached;
        } else {
          vcs.branch = std::string{value};
          vcs.head_kind = vcs.revision ? VcsHeadKind::branch
                                       : VcsHeadKind::unborn;
        }
        continue;
      }
      if (record.empty() || record[0] == '#') continue;

      std::string relative_path;
      std::optional<std::string> previous_path;
      std::string_view xy;
      std::string_view worktree_mode;
      bool rename{};
      if (record.starts_with("? ")) {
        relative_path = std::string{record.substr(2)};
      } else if (record.starts_with("1 ")) {
        const auto parsed = fields_with_path(record, 8);
        if (!parsed) {
          return failure(RepositorySnapshotErrorCode::vcs_failure,
                         "Git returned malformed ordinary status data");
        }
        xy = parsed->first[1];
        worktree_mode = parsed->first[5];
        relative_path = std::string{parsed->second};
      } else if (record.starts_with("2 ")) {
        const auto parsed = fields_with_path(record, 9);
        if (!parsed || index + 1 >= records.size()) {
          return failure(RepositorySnapshotErrorCode::vcs_failure,
                         "Git returned malformed rename status data");
        }
        xy = parsed->first[1];
        worktree_mode = parsed->first[5];
        relative_path = std::string{parsed->second};
        previous_path = std::string{records[++index]};
        rename = true;
      } else if (record.starts_with("u ")) {
        const auto parsed = fields_with_path(record, 10);
        if (!parsed) {
          return failure(RepositorySnapshotErrorCode::vcs_failure,
                         "Git returned malformed conflict status data");
        }
        xy = parsed->first[1];
        worktree_mode = parsed->first[6];
        relative_path = std::string{parsed->second};
      } else {
        return failure(RepositorySnapshotErrorCode::vcs_failure,
                       "Git returned an unsupported status record");
      }

      while (relative_path.size() > 1 && relative_path.ends_with('/')) {
        relative_path.pop_back();
      }
      if (previous_path) {
        while (previous_path->size() > 1 && previous_path->ends_with('/')) {
          previous_path->pop_back();
        }
      }

      if (changes.size() >= limits.maximum_entries ||
          relative_path.size() > limits.maximum_path_bytes ||
          (previous_path && previous_path->size() > limits.maximum_path_bytes)) {
        return failure(RepositorySnapshotErrorCode::resource_exhausted,
                       "repository status exceeds its entry or path budget");
      }
      const auto full_path = std::filesystem::path{root} / relative_path;
      std::error_code error;
      const auto status_value = std::filesystem::symlink_status(full_path, error);
      if (error && error != std::errc::no_such_file_or_directory) {
        return error_for_path(error, "repository entry could not be inspected");
      }
      const bool exists = !error && std::filesystem::exists(status_value);
      if (!exists &&
          (record.starts_with("? ") || xy.find('D') == std::string_view::npos)) {
        return failure(RepositorySnapshotErrorCode::unstable,
                       "repository entry disappeared during observation", true);
      }
      auto kind = exists ? entry_kind(status_value)
                         : std::optional<RepositoryEntryKind>{
                               kind_from_mode(worktree_mode)};
      if (!kind) {
        return failure(RepositorySnapshotErrorCode::unsupported_entry,
                       "repository contains an unsupported entry kind");
      }

      RepositoryChange change;
      change.relative_path = std::move(relative_path);
      change.previous_path = std::move(previous_path);
      change.entry_kind = *kind;
      if (record.starts_with("? ")) {
        change.change_kind = RepositoryChangeKind::untracked;
        change.stage = RepositoryChangeStage::untracked;
      } else {
        change.change_kind = change_from_xy(xy, rename, exists);
        change.stage = stage_from_xy(xy);
      }
      if (exists) {
        auto digest = hash_path(root, change.relative_path, change.entry_kind,
                                algorithm, limits, total_bytes, stop_token,
                                deadline);
        if (!digest) return std::unexpected(std::move(digest.error()));
        change.worktree_digest = std::move(*digest);
      }
      changes.push_back(std::move(change));
    }
    std::ranges::sort(changes, {}, &RepositoryChange::relative_path);

    std::string manifest;
    append_manifest_field(manifest, "kind", "git");
    append_manifest_field(manifest, "object-format", object_format);
    append_manifest_field(manifest, "status", status->output);
    for (const auto& change : changes) {
      append_manifest_field(manifest, "path", change.relative_path);
      if (change.worktree_digest) {
        append_manifest_field(manifest, "worktree", change.worktree_digest->value);
      }
    }
    if (manifest.size() > limits.maximum_total_bytes - total_bytes) {
      return failure(RepositorySnapshotErrorCode::resource_exhausted,
                     "repository manifest exceeds its byte budget");
    }
    auto fingerprint =
        hash_bytes(manifest, root, algorithm, limits, stop_token, deadline);
    if (!fingerprint) return std::unexpected(std::move(fingerprint.error()));
    auto identity =
        make_root(root, root, algorithm, limits, stop_token, deadline);
    if (!identity) return std::unexpected(std::move(identity.error()));
    RepositorySnapshot snapshot{std::move(*identity), std::move(vcs),
                                std::move(changes), std::move(*fingerprint), {}};
    auto valid = repository::validate_repository_snapshot(snapshot, limits);
    if (!valid) return std::unexpected(std::move(valid.error()));
    return snapshot;
  }

  [[nodiscard]] auto observe_plain_once(
      const std::string& root, const RepositorySnapshotLimits& limits,
      const std::stop_token stop_token,
      const ObservationDeadline deadline) const
      -> std::expected<RepositorySnapshot, RepositorySnapshotError> {
    constexpr std::string_view algorithm = "git-sha1";
    std::vector<RepositoryChange> changes;
    std::uint64_t total_bytes{};
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator{
        root, std::filesystem::directory_options::none, error};
    const std::filesystem::recursive_directory_iterator end;
    if (error) return error_for_path(error, "repository root could not be read");
    for (; iterator != end; iterator.increment(error)) {
      if (error) return error_for_path(error, "repository entry could not be read");
      if (stop_token.stop_requested()) {
        return failure(RepositorySnapshotErrorCode::cancelled,
                       "repository observation cancelled");
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        return failure(RepositorySnapshotErrorCode::timed_out,
                       "repository observation timed out", true);
      }
      const auto status = iterator->symlink_status(error);
      if (error) return error_for_path(error, "repository entry could not be inspected");
      if (iterator->path().filename() == ".git") {
        if (std::filesystem::is_directory(status)) iterator.disable_recursion_pending();
        continue;
      }
      if (std::filesystem::is_directory(status)) continue;
      const auto kind = entry_kind(status);
      if (!kind || *kind == RepositoryEntryKind::submodule) {
        return failure(RepositorySnapshotErrorCode::unsupported_entry,
                       "repository contains an unsupported entry kind");
      }
      const auto relative = iterator->path().lexically_relative(root).generic_string();
      if (changes.size() >= limits.maximum_entries ||
          relative.size() > limits.maximum_path_bytes) {
        return failure(RepositorySnapshotErrorCode::resource_exhausted,
                       "repository scan exceeds its entry or path budget");
      }
      auto digest = hash_path(root, relative, *kind, std::string{algorithm},
                              limits, total_bytes, stop_token, deadline);
      if (!digest) return std::unexpected(std::move(digest.error()));
      changes.push_back(RepositoryChange{relative, std::nullopt, *kind,
                                         RepositoryChangeKind::untracked,
                                         RepositoryChangeStage::untracked,
                                         std::nullopt, std::move(*digest)});
    }
    std::ranges::sort(changes, {}, &RepositoryChange::relative_path);
    std::string manifest;
    append_manifest_field(manifest, "kind", "filesystem");
    for (const auto& change : changes) {
      append_manifest_field(manifest, "path", change.relative_path);
      append_manifest_field(manifest, "kind", std::to_string(
          static_cast<unsigned>(change.entry_kind)));
      append_manifest_field(manifest, "content", change.worktree_digest->value);
    }
    if (manifest.size() > limits.maximum_total_bytes - total_bytes) {
      return failure(RepositorySnapshotErrorCode::resource_exhausted,
                     "repository manifest exceeds its byte budget");
    }
    auto fingerprint = hash_bytes(manifest, std::nullopt, std::string{algorithm},
                                  limits, stop_token, deadline);
    if (!fingerprint) return std::unexpected(std::move(fingerprint.error()));
    auto identity = make_root(root, std::nullopt, std::string{algorithm}, limits,
                              stop_token, deadline);
    if (!identity) return std::unexpected(std::move(identity.error()));
    RepositorySnapshot snapshot{std::move(*identity), std::nullopt,
                                std::move(changes), std::move(*fingerprint), {}};
    auto valid = repository::validate_repository_snapshot(snapshot, limits);
    if (!valid) return std::unexpected(std::move(valid.error()));
    return snapshot;
  }
};

auto GitRepositorySnapshotSource::open(std::string git_executable)
    -> std::expected<GitRepositorySnapshotSource, RepositorySnapshotError> {
  try {
#ifdef _WIN32
    static_cast<void>(git_executable);
    return failure(RepositorySnapshotErrorCode::vcs_failure,
                   "Git repository observation is unavailable on this platform");
#else
    const std::filesystem::path executable{git_executable};
    std::error_code error;
    const auto status = std::filesystem::symlink_status(executable, error);
    if (git_executable.empty() || !executable.is_absolute() || error ||
        !std::filesystem::is_regular_file(status) ||
        ::access(git_executable.c_str(), X_OK) != 0) {
      return failure(RepositorySnapshotErrorCode::invalid_request,
                     "Git executable must be an executable absolute regular file");
    }
    return GitRepositorySnapshotSource{
        std::make_unique<Impl>(std::move(git_executable))};
#endif
  } catch (...) {
    return failure(RepositorySnapshotErrorCode::internal_failure,
                   "Git repository source could not be created");
  }
}

GitRepositorySnapshotSource::GitRepositorySnapshotSource(
    std::unique_ptr<Impl> impl)
    : m_impl(std::move(impl)) {}
GitRepositorySnapshotSource::GitRepositorySnapshotSource(
    GitRepositorySnapshotSource&&) noexcept = default;
auto GitRepositorySnapshotSource::operator=(
    GitRepositorySnapshotSource&&) noexcept
    -> GitRepositorySnapshotSource& = default;
GitRepositorySnapshotSource::~GitRepositorySnapshotSource() = default;

auto GitRepositorySnapshotSource::observe(
    repository::RepositorySnapshotRequest request,
    const std::stop_token stop_token)
    -> std::expected<RepositorySnapshot, RepositorySnapshotError> {
  try {
    if (request.root.empty() || request.root.find('\0') != std::string::npos) {
      return failure(RepositorySnapshotErrorCode::invalid_request,
                     "repository root is invalid");
    }
    constexpr repository::RepositorySnapshotLimits maximums;
    if (request.limits.maximum_entries == 0 ||
        request.limits.maximum_path_bytes == 0 ||
        request.limits.maximum_file_bytes == 0 ||
        request.limits.maximum_total_bytes == 0 ||
        request.limits.maximum_command_output_bytes == 0 ||
        request.limits.command_timeout <= std::chrono::milliseconds::zero() ||
        request.limits.observation_timeout <=
            std::chrono::milliseconds::zero() ||
        request.limits.maximum_entries > maximums.maximum_entries ||
        request.limits.maximum_path_bytes > maximums.maximum_path_bytes ||
        request.limits.maximum_file_bytes > maximums.maximum_file_bytes ||
        request.limits.maximum_total_bytes > maximums.maximum_total_bytes ||
        request.limits.maximum_command_output_bytes >
            maximums.maximum_command_output_bytes ||
        request.limits.command_timeout > maximums.command_timeout ||
        request.limits.observation_timeout > maximums.observation_timeout ||
        request.limits.command_timeout > request.limits.observation_timeout) {
      return failure(RepositorySnapshotErrorCode::invalid_request,
                     "repository observation limits must be positive");
    }
    const auto deadline = std::chrono::steady_clock::now() +
                          request.limits.observation_timeout;
    if (stop_token.stop_requested()) {
      return failure(RepositorySnapshotErrorCode::cancelled,
                     "repository observation cancelled");
    }

    std::error_code error;
    auto root_path = std::filesystem::canonical(request.root, error);
    if (error) return error_for_path(error, "repository root could not be resolved");
    if (!std::filesystem::is_directory(root_path, error) || error) {
      return failure(error == std::errc::permission_denied
                         ? RepositorySnapshotErrorCode::permission_denied
                         : RepositorySnapshotErrorCode::not_directory,
                     "repository root is not a readable directory");
    }
    auto root = root_path.generic_string();

    auto top_level = m_impl->git(
        {"-C", root, "rev-parse", "--show-toplevel"}, {}, request.limits,
        stop_token, deadline);
    if (!top_level) return std::unexpected(std::move(top_level.error()));

    std::optional<std::string> object_format;
    if (top_level->exit_code == 0) {
      auto discovered = trim_record(std::move(top_level->output));
      root_path = std::filesystem::canonical(discovered, error);
      if (error) {
        return error_for_path(error, "Git repository root could not be resolved");
      }
      root = root_path.generic_string();
      auto format = m_impl->git(
          {"-C", root, "rev-parse", "--show-object-format"}, {},
          request.limits, stop_token, deadline);
      if (!format) return std::unexpected(std::move(format.error()));
      if (format->exit_code != 0) {
        return failure(RepositorySnapshotErrorCode::vcs_failure,
                       "Git object format could not be determined");
      }
      object_format = trim_record(std::move(format->output));
      if (*object_format != "sha1" && *object_format != "sha256") {
        return failure(RepositorySnapshotErrorCode::vcs_failure,
                       "Git repository uses an unsupported object format");
      }
    } else if (has_git_marker(root_path)) {
      return failure(RepositorySnapshotErrorCode::vcs_failure,
                     "Git repository root could not be discovered", true);
    }

    auto observe_once = [&]() {
      return object_format
                 ? m_impl->observe_git_once(root, *object_format, request.limits,
                                             stop_token, deadline)
                 : m_impl->observe_plain_once(root, request.limits, stop_token,
                                              deadline);
    };
    auto first = observe_once();
    if (!first) return std::unexpected(std::move(first.error()));
    auto second = observe_once();
    if (!second) return std::unexpected(std::move(second.error()));
    if (!domain::same_source_state(*first, *second) || first->vcs != second->vcs ||
        first->changes != second->changes) {
      return failure(RepositorySnapshotErrorCode::unstable,
                     "repository changed while it was being observed", true);
    }
    second->observed_at = std::chrono::time_point_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now());
    return second;
  } catch (const std::filesystem::filesystem_error&) {
    return failure(RepositorySnapshotErrorCode::io_failure,
                   "repository observation failed in the filesystem", true);
  } catch (...) {
    return failure(RepositorySnapshotErrorCode::internal_failure,
                   "repository observation failed internally");
  }
}

}  // namespace aiforge::adapters
