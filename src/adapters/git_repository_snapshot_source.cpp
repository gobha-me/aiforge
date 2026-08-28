#include <aiforge/adapters/git_exact_source_editor.hpp>
#include <aiforge/adapters/git_project_instruction_source.hpp>
#include <aiforge/adapters/git_repository_snapshot_source.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <limits>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/stat.h>
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

[[nodiscard]] auto project_failure(
    const repository::ProjectInstructionErrorCode code, std::string message,
    std::optional<std::string> path = std::nullopt,
    const bool retryable = false)
    -> std::unexpected<repository::ProjectInstructionError> {
  return std::unexpected(repository::ProjectInstructionError{
      code, std::move(message), std::move(path), retryable});
}

[[nodiscard]] auto project_error(
    const repository::RepositorySnapshotError& error)
    -> repository::ProjectInstructionError {
  using ProjectCode = repository::ProjectInstructionErrorCode;
  using SnapshotCode = repository::RepositorySnapshotErrorCode;
  auto code = ProjectCode::internal_failure;
  switch (error.code) {
    case SnapshotCode::invalid_request:
      code = ProjectCode::invalid_request;
      break;
    case SnapshotCode::not_found: code = ProjectCode::not_found; break;
    case SnapshotCode::not_directory:
      code = ProjectCode::invalid_request;
      break;
    case SnapshotCode::permission_denied:
      code = ProjectCode::permission_denied;
      break;
    case SnapshotCode::unsupported_entry:
      code = ProjectCode::unsupported_entry;
      break;
    case SnapshotCode::unstable: code = ProjectCode::unstable; break;
    case SnapshotCode::resource_exhausted:
      code = ProjectCode::resource_exhausted;
      break;
    case SnapshotCode::vcs_failure:
    case SnapshotCode::io_failure: code = ProjectCode::io_failure; break;
    case SnapshotCode::timed_out: code = ProjectCode::timed_out; break;
    case SnapshotCode::cancelled: code = ProjectCode::cancelled; break;
    case SnapshotCode::internal_failure: break;
  }
  return {code, error.message, std::nullopt, error.retryable};
}

[[nodiscard]] auto valid_utf8_instruction(const std::string_view value)
    -> bool {
  std::size_t index{};
  while (index < value.size()) {
    const auto first = static_cast<unsigned char>(value[index]);
    if (first == 0 || first == 0x7FU ||
        (first < 0x20U && first != '\t' && first != '\n' && first != '\r')) {
      return false;
    }
    if (first <= 0x7FU) {
      ++index;
      continue;
    }
    std::size_t length{};
    std::uint32_t codepoint{};
    if (first >= 0xC2U && first <= 0xDFU) {
      length = 2;
      codepoint = first & 0x1FU;
    } else if (first >= 0xE0U && first <= 0xEFU) {
      length = 3;
      codepoint = first & 0x0FU;
    } else if (first >= 0xF0U && first <= 0xF4U) {
      length = 4;
      codepoint = first & 0x07U;
    } else {
      return false;
    }
    if (length > value.size() - index) return false;
    for (std::size_t offset = 1; offset < length; ++offset) {
      const auto next = static_cast<unsigned char>(value[index + offset]);
      if ((next & 0xC0U) != 0x80U) return false;
      codepoint = (codepoint << 6U) | (next & 0x3FU);
    }
    if ((length == 3 && codepoint < 0x800U) ||
        (length == 4 && codepoint < 0x10000U) ||
        (codepoint >= 0xD800U && codepoint <= 0xDFFFU) ||
        codepoint > 0x10FFFFU) {
      return false;
    }
    index += length;
  }
  return true;
}

[[nodiscard]] auto valid_project_subtree(const std::string& value,
                                         const std::size_t maximum) -> bool {
  if (value.size() > maximum || value.find('\0') != std::string::npos) {
    return false;
  }
  if (value.empty()) return true;
  const std::filesystem::path path{value};
  if (path.is_absolute() || path.has_root_name() || path.has_root_directory() ||
      path.generic_string() != value) {
    return false;
  }
  for (const auto& part : path) {
    if (part.empty() || part == "." || part == "..") return false;
  }
  return true;
}

[[nodiscard]] auto project_path_error(const std::error_code& error,
                                      std::string message, std::string path)
    -> std::unexpected<repository::ProjectInstructionError> {
  if (error == std::errc::no_such_file_or_directory) {
    return project_failure(repository::ProjectInstructionErrorCode::not_found,
                           std::move(message), std::move(path));
  }
  if (error == std::errc::permission_denied) {
    return project_failure(
        repository::ProjectInstructionErrorCode::permission_denied,
        std::move(message), std::move(path));
  }
  return project_failure(repository::ProjectInstructionErrorCode::io_failure,
                         std::move(message), std::move(path), true);
}

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

[[nodiscard]] auto run_command(const std::string& executable,
                               const std::vector<std::string>& arguments,
                               const std::string_view input,
                               const RepositorySnapshotLimits& limits,
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
  return failure(
      RepositorySnapshotErrorCode::vcs_failure,
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
    for (auto* descriptor :
         {&stdin_pipe[0], &stdin_pipe[1], &stdout_pipe[0], &stdout_pipe[1],
          &stderr_pipe[0], &stderr_pipe[1]}) {
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
    for (const int descriptor :
         {stdin_pipe[0], stdin_pipe[1], stdout_pipe[0], stdout_pipe[1],
          stderr_pipe[0], stderr_pipe[1]}) {
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
  const auto deadline =
      std::min(std::chrono::steady_clock::now() + limits.command_timeout,
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
        {stdout_pipe[0], static_cast<short>(stdout_pipe[0] >= 0 ? POLLIN : 0),
         0},
        {stderr_pipe[0], static_cast<short>(stderr_pipe[0] >= 0 ? POLLIN : 0),
         0},
        {stdin_pipe[1], static_cast<short>(stdin_pipe[1] >= 0 ? POLLOUT : 0),
         0},
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

    const int stdout_result = append_pipe(stdout_pipe[0], result.output,
                                          limits.maximum_command_output_bytes);
    const int stderr_result = append_pipe(stderr_pipe[0], result.error,
                                          limits.maximum_command_output_bytes);
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
  result.exit_code =
      WIFEXITED(child_status)
          ? WEXITSTATUS(child_status)
          : (WIFSIGNALED(child_status) ? 128 + WTERMSIG(child_status) : 126);
  return result;
#endif
}

[[nodiscard]] auto trim_record(std::string value) -> std::string {
  while (!value.empty() && (value.back() == '\n' || value.back() == '\r' ||
                            value.back() == '\0')) {
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
    -> std::optional<
        std::pair<std::vector<std::string_view>, std::string_view>> {
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
  return index ? RepositoryChangeStage::index : RepositoryChangeStage::worktree;
}

[[nodiscard]] auto change_from_xy(const std::string_view xy, const bool rename,
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

using ExactError = repository::ExactSourceEditError;
using ExactErrorCode = repository::ExactSourceEditErrorCode;

[[nodiscard]] auto exact_failure(
    const ExactErrorCode code, std::string message,
    std::optional<domain::RepositorySnapshotIdentity> observed_snapshot = {},
    std::optional<domain::RepositorySourceIdentity> observed_source = {},
    const bool retryable = false, const bool may_have_applied = false)
    -> std::unexpected<ExactError> {
  return std::unexpected(
      ExactError{code, std::move(message), std::move(observed_snapshot),
                 std::move(observed_source), retryable, may_have_applied});
}

[[nodiscard]] auto exact_error(const RepositorySnapshotError& error)
    -> ExactError {
  auto code = ExactErrorCode::internal_failure;
  switch (error.code) {
    case RepositorySnapshotErrorCode::invalid_request:
      code = ExactErrorCode::invalid_request;
      break;
    case RepositorySnapshotErrorCode::not_found:
      code = ExactErrorCode::not_found;
      break;
    case RepositorySnapshotErrorCode::not_directory:
    case RepositorySnapshotErrorCode::unsupported_entry:
      code = ExactErrorCode::unsupported_entry;
      break;
    case RepositorySnapshotErrorCode::permission_denied:
      code = ExactErrorCode::permission_denied;
      break;
    case RepositorySnapshotErrorCode::unstable:
      code = ExactErrorCode::concurrent_change;
      break;
    case RepositorySnapshotErrorCode::resource_exhausted:
      code = ExactErrorCode::resource_exhausted;
      break;
    case RepositorySnapshotErrorCode::vcs_failure:
    case RepositorySnapshotErrorCode::io_failure:
      code = ExactErrorCode::io_failure;
      break;
    case RepositorySnapshotErrorCode::timed_out:
      code = ExactErrorCode::timed_out;
      break;
    case RepositorySnapshotErrorCode::cancelled:
      code = ExactErrorCode::cancelled;
      break;
    case RepositorySnapshotErrorCode::internal_failure: break;
  }
  return {code, error.message, {}, {}, error.retryable, false};
}

[[nodiscard]] auto same_repository_state(const RepositorySnapshot& left,
                                         const RepositorySnapshot& right)
    -> bool {
  return left.root == right.root && left.vcs == right.vcs &&
         left.changes == right.changes &&
         domain::same_source_state(left, right);
}

[[nodiscard]] auto exact_snapshot_limits(
    const repository::ExactSourceEditLimits& limits)
    -> RepositorySnapshotLimits {
  RepositorySnapshotLimits result;
  result.maximum_path_bytes = limits.maximum_path_bytes;
  result.maximum_file_bytes = limits.maximum_source_bytes;
  result.observation_timeout = limits.timeout;
  result.command_timeout = std::min(result.command_timeout, limits.timeout);
  return result;
}

#ifndef _WIN32
class OwnedDescriptor final {
 public:
  OwnedDescriptor() = default;
  explicit OwnedDescriptor(const int descriptor) : m_descriptor(descriptor) {}
  ~OwnedDescriptor() { close_fd(m_descriptor); }
  OwnedDescriptor(const OwnedDescriptor&) = delete;
  auto operator=(const OwnedDescriptor&) -> OwnedDescriptor& = delete;
  OwnedDescriptor(OwnedDescriptor&& other) noexcept
      : m_descriptor(std::exchange(other.m_descriptor, -1)) {}
  auto operator=(OwnedDescriptor&& other) noexcept -> OwnedDescriptor& {
    if (this == &other) return *this;
    close_fd(m_descriptor);
    m_descriptor = std::exchange(other.m_descriptor, -1);
    return *this;
  }
  [[nodiscard]] auto get() const noexcept -> int { return m_descriptor; }
  [[nodiscard]] auto release() noexcept -> int {
    return std::exchange(m_descriptor, -1);
  }

 private:
  int m_descriptor{-1};
};

struct ExactFileRead {
  std::string content;
  mode_t mode{};
  dev_t device{};
  ino_t inode{};
};

[[nodiscard]] auto errno_exact_failure(const std::string_view operation,
                                       const bool retryable = false,
                                       const bool may_have_applied = false)
    -> std::unexpected<ExactError> {
  const auto code = errno == EACCES || errno == EPERM
                        ? ExactErrorCode::permission_denied
                        : ExactErrorCode::io_failure;
  return exact_failure(code, std::string{operation} + " failed", {}, {},
                       retryable, may_have_applied);
}

[[nodiscard]] auto secure_source_path(const std::filesystem::path& root,
                                      const std::string& relative)
    -> std::expected<std::filesystem::path, ExactError> {
  auto current = root;
  const std::filesystem::path requested{relative};
  for (auto iterator = requested.begin(); iterator != requested.end();
       ++iterator) {
    current /= *iterator;
    std::error_code error;
    const auto status = std::filesystem::symlink_status(current, error);
    if (error == std::errc::no_such_file_or_directory) {
      return exact_failure(ExactErrorCode::not_found,
                           "exact-source target was not found");
    }
    if (error == std::errc::permission_denied) {
      return exact_failure(ExactErrorCode::permission_denied,
                           "exact-source target could not be inspected");
    }
    if (error) {
      return exact_failure(ExactErrorCode::io_failure,
                           "exact-source target could not be inspected", {}, {},
                           true);
    }
    if (std::filesystem::is_symlink(status)) {
      return exact_failure(
          ExactErrorCode::outside_repository,
          "exact-source target cannot traverse a symbolic link");
    }
    const auto next = std::next(iterator);
    if (next != requested.end() && !std::filesystem::is_directory(status)) {
      return exact_failure(ExactErrorCode::not_found,
                           "exact-source parent is not a directory");
    }
    if (next == requested.end() && !std::filesystem::is_regular_file(status)) {
      return exact_failure(ExactErrorCode::unsupported_entry,
                           "exact-source target is not a regular file");
    }
  }
  return current;
}

[[nodiscard]] auto read_exact_file(
    const std::filesystem::path& root, const std::string& relative,
    const repository::ExactSourceEditLimits& limits,
    const std::stop_token stop_token,
    const std::chrono::steady_clock::time_point deadline)
    -> std::expected<ExactFileRead, ExactError> {
  auto resolved = secure_source_path(root, relative);
  if (!resolved) return std::unexpected(std::move(resolved.error()));
  OwnedDescriptor descriptor{
      ::open(resolved->c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
  if (descriptor.get() < 0) {
    if (errno == ENOENT) {
      return exact_failure(ExactErrorCode::not_found,
                           "exact-source target disappeared");
    }
    if (errno == ELOOP) {
      return exact_failure(ExactErrorCode::outside_repository,
                           "exact-source target became a symbolic link");
    }
    return errno_exact_failure("opening exact-source target", true);
  }
  struct stat before{};
  if (::fstat(descriptor.get(), &before) != 0) {
    return errno_exact_failure("inspecting exact-source target", true);
  }
  if (!S_ISREG(before.st_mode)) {
    return exact_failure(ExactErrorCode::unsupported_entry,
                         "exact-source target is not a regular file");
  }
  if (before.st_nlink != 1) {
    return exact_failure(
        ExactErrorCode::outside_repository,
        "exact-source target has an aliased filesystem identity");
  }
  if (before.st_size < 0 ||
      static_cast<std::uint64_t>(before.st_size) >
          limits.maximum_source_bytes ||
      static_cast<std::uint64_t>(before.st_size) >
          std::numeric_limits<std::size_t>::max()) {
    return exact_failure(ExactErrorCode::resource_exhausted,
                         "exact-source target exceeds its byte budget");
  }
  std::string content;
  content.resize(static_cast<std::size_t>(before.st_size));
  std::size_t offset{};
  while (offset < content.size()) {
    if (stop_token.stop_requested()) {
      return exact_failure(ExactErrorCode::cancelled,
                           "exact-source read cancelled");
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return exact_failure(ExactErrorCode::timed_out,
                           "exact-source read timed out", {}, {}, true);
    }
    const auto count = ::read(descriptor.get(), content.data() + offset,
                              content.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count < 0)
      return errno_exact_failure("reading exact-source target", true);
    if (count == 0) {
      return exact_failure(ExactErrorCode::concurrent_change,
                           "exact-source target changed while being read", {},
                           {}, true);
    }
    offset += static_cast<std::size_t>(count);
  }
  struct stat after{};
  struct stat path_state{};
  if (::fstat(descriptor.get(), &after) != 0 ||
      ::lstat(resolved->c_str(), &path_state) != 0) {
    return errno_exact_failure("rechecking exact-source target", true);
  }
  if (before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
      before.st_size != after.st_size ||
      before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
      before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
      before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
      before.st_ctim.tv_nsec != after.st_ctim.tv_nsec ||
      after.st_dev != path_state.st_dev || after.st_ino != path_state.st_ino ||
      S_ISLNK(path_state.st_mode)) {
    return exact_failure(ExactErrorCode::concurrent_change,
                         "exact-source target changed while being read", {}, {},
                         true);
  }
  return ExactFileRead{std::move(content), before.st_mode, before.st_dev,
                       before.st_ino};
}

class RepositoryWriteLock final {
 public:
  [[nodiscard]] static auto acquire(
      const std::filesystem::path& root, const std::stop_token stop_token,
      const std::chrono::steady_clock::time_point deadline)
      -> std::expected<RepositoryWriteLock, ExactError> {
    OwnedDescriptor descriptor{
        ::open(root.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY)};
    if (descriptor.get() < 0) {
      return errno_exact_failure("opening repository write lock", true);
    }
    while (::flock(descriptor.get(), LOCK_EX | LOCK_NB) != 0) {
      if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) {
        return errno_exact_failure("acquiring repository write lock", true);
      }
      if (stop_token.stop_requested()) {
        return exact_failure(
            ExactErrorCode::cancelled,
            "exact-source edit cancelled while waiting for its lock");
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        return exact_failure(ExactErrorCode::timed_out,
                             "repository write lock timed out", {}, {}, true);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    return RepositoryWriteLock{std::move(descriptor)};
  }

  RepositoryWriteLock(RepositoryWriteLock&&) noexcept = default;
  auto operator=(RepositoryWriteLock&&) noexcept
      -> RepositoryWriteLock& = default;
  ~RepositoryWriteLock() {
    if (m_descriptor.get() >= 0) {
      static_cast<void>(::flock(m_descriptor.get(), LOCK_UN));
    }
  }

 private:
  explicit RepositoryWriteLock(OwnedDescriptor descriptor)
      : m_descriptor(std::move(descriptor)) {}
  OwnedDescriptor m_descriptor;
};

class PreparedReplacement final {
 public:
  [[nodiscard]] static auto create(
      const std::filesystem::path& repository_root,
      const std::filesystem::path& target, const std::string_view content,
      const mode_t mode, const std::stop_token stop_token,
      const std::chrono::steady_clock::time_point deadline)
      -> std::expected<PreparedReplacement, ExactError> {
    auto temporary_directory = repository_root.parent_path();
    std::error_code error;
    const auto git_status =
        std::filesystem::symlink_status(repository_root / ".git", error);
    if (!error && std::filesystem::is_directory(git_status) &&
        !std::filesystem::is_symlink(git_status)) {
      temporary_directory = repository_root / ".git";
    }
    static std::atomic<std::uint64_t> sequence{};
    for (std::size_t attempt{}; attempt < 128; ++attempt) {
      auto path =
          temporary_directory / (".aiforge-edit-" + std::to_string(::getpid()) +
                                 "-" + std::to_string(sequence.fetch_add(1)));
      OwnedDescriptor descriptor{::open(
          path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
          S_IRUSR | S_IWUSR)};
      if (descriptor.get() < 0 && errno == EEXIST) continue;
      if (descriptor.get() < 0) {
        return errno_exact_failure("creating exact-source replacement", true);
      }
      PreparedReplacement replacement{std::move(path), target,
                                      std::move(descriptor)};
      if (::fchmod(replacement.m_descriptor.get(), mode & 0777) != 0) {
        return errno_exact_failure("preserving exact-source permissions", true);
      }
      std::size_t offset{};
      while (offset < content.size()) {
        if (stop_token.stop_requested()) {
          return exact_failure(ExactErrorCode::cancelled,
                               "exact-source edit cancelled before commit");
        }
        if (std::chrono::steady_clock::now() >= deadline) {
          return exact_failure(ExactErrorCode::timed_out,
                               "exact-source replacement timed out", {}, {},
                               true);
        }
        const auto count =
            ::write(replacement.m_descriptor.get(), content.data() + offset,
                    content.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
          return errno_exact_failure("writing exact-source replacement", true);
        }
        if (count == 0) {
          return exact_failure(
              ExactErrorCode::io_failure,
              "writing exact-source replacement made no progress", {}, {},
              true);
        }
        offset += static_cast<std::size_t>(count);
      }
      if (::fsync(replacement.m_descriptor.get()) != 0) {
        return errno_exact_failure("syncing exact-source replacement", true);
      }
      return replacement;
    }
    return exact_failure(ExactErrorCode::io_failure,
                         "exact-source replacement name space is exhausted", {},
                         {}, true);
  }

  PreparedReplacement(PreparedReplacement&& other) noexcept
      : m_path(std::move(other.m_path)), m_target(std::move(other.m_target)),
        m_descriptor(std::move(other.m_descriptor)),
        m_committed(std::exchange(other.m_committed, true)) {}
  auto operator=(PreparedReplacement&&) -> PreparedReplacement& = delete;
  ~PreparedReplacement() {
    if (!m_committed && !m_path.empty()) {
      static_cast<void>(::unlink(m_path.c_str()));
    }
  }

  [[nodiscard]] auto commit() -> std::expected<void, ExactError> {
    if (::rename(m_path.c_str(), m_target.c_str()) != 0) {
      return errno_exact_failure("committing exact-source replacement", true);
    }
    m_committed = true;
    m_path.clear();
    OwnedDescriptor parent{::open(m_target.parent_path().c_str(),
                                  O_RDONLY | O_CLOEXEC | O_DIRECTORY)};
    if (parent.get() < 0 || ::fsync(parent.get()) != 0) {
      return exact_failure(
          ExactErrorCode::durability_failure,
          "exact-source replacement committed but directory sync failed", {},
          {}, true, true);
    }
    return {};
  }

 private:
  PreparedReplacement(std::filesystem::path path, std::filesystem::path target,
                      OwnedDescriptor descriptor)
      : m_path(std::move(path)), m_target(std::move(target)),
        m_descriptor(std::move(descriptor)) {}

  std::filesystem::path m_path;
  std::filesystem::path m_target;
  OwnedDescriptor m_descriptor;
  bool m_committed{};
};
#endif

} // namespace

struct GitRepositorySnapshotSource::Impl {
  explicit Impl(std::string executable)
      : git_executable(std::move(executable)) {}

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
      if (error)
        return error_for_path(error, "repository symlink could not be read");
      const auto target_bytes = target.generic_string();
      bytes = target_bytes.size();
      if (bytes > limits.maximum_file_bytes ||
          bytes > limits.maximum_total_bytes - total_bytes) {
        return failure(RepositorySnapshotErrorCode::resource_exhausted,
                       "repository content exceeds its byte budget");
      }
      auto digest = hash_bytes(target_bytes, root, algorithm, limits,
                               stop_token, deadline);
      if (!digest) return std::unexpected(std::move(digest.error()));
      total_bytes += bytes;
      return digest;
    } else if (kind == RepositoryEntryKind::submodule) {
      auto revision = git({"-C", full_path.string(), "rev-parse", "HEAD"}, {},
                          limits, stop_token, deadline);
      if (!revision) return std::unexpected(std::move(revision.error()));
      if (revision->exit_code != 0) {
        return failure(RepositorySnapshotErrorCode::vcs_failure,
                       "repository submodule revision could not be read");
      }
      auto value = trim_record(std::move(revision->output));
      return ContentDigest{algorithm, std::move(value), 0};
    } else {
      bytes = std::filesystem::file_size(full_path, error);
      if (error)
        return error_for_path(error, "repository file size could not be read");
    }
    if (bytes > limits.maximum_file_bytes ||
        bytes > limits.maximum_total_bytes - total_bytes) {
      return failure(RepositorySnapshotErrorCode::resource_exhausted,
                     "repository content exceeds its byte budget");
    }
    auto result =
        git({"-C", root, "hash-object", "--no-filters", "--", relative_path},
            {}, limits, stop_token, deadline);
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
    auto digest = hash_bytes(canonical_path, root, algorithm, limits,
                             stop_token, deadline);
    if (!digest) return std::unexpected(std::move(digest.error()));
    auto id = domain::RepositoryId::from(algorithm + ":" + digest->value);
    if (!id) {
      return failure(RepositorySnapshotErrorCode::internal_failure,
                     "repository identity could not be represented");
    }
    return domain::RepositoryRootIdentity{std::move(*id), canonical_path};
  }

  [[nodiscard]] auto observe_git_once(const std::string& root,
                                      const std::string& object_format,
                                      const RepositorySnapshotLimits& limits,
                                      const std::stop_token stop_token,
                                      const ObservationDeadline deadline) const
      -> std::expected<RepositorySnapshot, RepositorySnapshotError> {
    const std::string algorithm = "git-" + object_format;
    auto status = git({"-C", root, "status", "--porcelain=v2", "--branch", "-z",
                       "--untracked-files=all"},
                      {}, limits, stop_token, deadline);
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
          vcs.head_kind =
              vcs.revision ? VcsHeadKind::branch : VcsHeadKind::unborn;
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
          (previous_path &&
           previous_path->size() > limits.maximum_path_bytes)) {
        return failure(RepositorySnapshotErrorCode::resource_exhausted,
                       "repository status exceeds its entry or path budget");
      }
      const auto full_path = std::filesystem::path{root} / relative_path;
      std::error_code error;
      const auto status_value =
          std::filesystem::symlink_status(full_path, error);
      if (error && error != std::errc::no_such_file_or_directory) {
        return error_for_path(error, "repository entry could not be inspected");
      }
      const bool exists = !error && std::filesystem::exists(status_value);
      if (!exists && (record.starts_with("? ") ||
                      xy.find('D') == std::string_view::npos)) {
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
        auto digest =
            hash_path(root, change.relative_path, change.entry_kind, algorithm,
                      limits, total_bytes, stop_token, deadline);
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
        append_manifest_field(manifest, "worktree",
                              change.worktree_digest->value);
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
    RepositorySnapshot snapshot{std::move(*identity),
                                std::move(vcs),
                                std::move(changes),
                                std::move(*fingerprint),
                                {}};
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
    if (error)
      return error_for_path(error, "repository root could not be read");
    for (; iterator != end; iterator.increment(error)) {
      if (error)
        return error_for_path(error, "repository entry could not be read");
      if (stop_token.stop_requested()) {
        return failure(RepositorySnapshotErrorCode::cancelled,
                       "repository observation cancelled");
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        return failure(RepositorySnapshotErrorCode::timed_out,
                       "repository observation timed out", true);
      }
      const auto status = iterator->symlink_status(error);
      if (error)
        return error_for_path(error, "repository entry could not be inspected");
      if (iterator->path().filename() == ".git") {
        if (std::filesystem::is_directory(status))
          iterator.disable_recursion_pending();
        continue;
      }
      if (std::filesystem::is_directory(status)) continue;
      const auto kind = entry_kind(status);
      if (!kind || *kind == RepositoryEntryKind::submodule) {
        return failure(RepositorySnapshotErrorCode::unsupported_entry,
                       "repository contains an unsupported entry kind");
      }
      const auto relative =
          iterator->path().lexically_relative(root).generic_string();
      if (changes.size() >= limits.maximum_entries ||
          relative.size() > limits.maximum_path_bytes) {
        return failure(RepositorySnapshotErrorCode::resource_exhausted,
                       "repository scan exceeds its entry or path budget");
      }
      auto digest = hash_path(root, relative, *kind, std::string{algorithm},
                              limits, total_bytes, stop_token, deadline);
      if (!digest) return std::unexpected(std::move(digest.error()));
      changes.push_back(RepositoryChange{
          relative, std::nullopt, *kind, RepositoryChangeKind::untracked,
          RepositoryChangeStage::untracked, std::nullopt, std::move(*digest)});
    }
    std::ranges::sort(changes, {}, &RepositoryChange::relative_path);
    std::string manifest;
    append_manifest_field(manifest, "kind", "filesystem");
    for (const auto& change : changes) {
      append_manifest_field(manifest, "path", change.relative_path);
      append_manifest_field(
          manifest, "kind",
          std::to_string(static_cast<unsigned>(change.entry_kind)));
      append_manifest_field(manifest, "content", change.worktree_digest->value);
    }
    if (manifest.size() > limits.maximum_total_bytes - total_bytes) {
      return failure(RepositorySnapshotErrorCode::resource_exhausted,
                     "repository manifest exceeds its byte budget");
    }
    auto fingerprint =
        hash_bytes(manifest, std::nullopt, std::string{algorithm}, limits,
                   stop_token, deadline);
    if (!fingerprint) return std::unexpected(std::move(fingerprint.error()));
    auto identity = make_root(root, std::nullopt, std::string{algorithm},
                              limits, stop_token, deadline);
    if (!identity) return std::unexpected(std::move(identity.error()));
    RepositorySnapshot snapshot{std::move(*identity),
                                std::nullopt,
                                std::move(changes),
                                std::move(*fingerprint),
                                {}};
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
    return failure(
        RepositorySnapshotErrorCode::vcs_failure,
        "Git repository observation is unavailable on this platform");
#else
    const std::filesystem::path executable{git_executable};
    std::error_code error;
    const auto status = std::filesystem::symlink_status(executable, error);
    if (git_executable.empty() || !executable.is_absolute() || error ||
        !std::filesystem::is_regular_file(status) ||
        ::access(git_executable.c_str(), X_OK) != 0) {
      return failure(
          RepositorySnapshotErrorCode::invalid_request,
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
    : m_impl(std::move(impl)) {
}
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
    const auto deadline =
        std::chrono::steady_clock::now() + request.limits.observation_timeout;
    if (stop_token.stop_requested()) {
      return failure(RepositorySnapshotErrorCode::cancelled,
                     "repository observation cancelled");
    }

    std::error_code error;
    auto root_path = std::filesystem::canonical(request.root, error);
    if (error)
      return error_for_path(error, "repository root could not be resolved");
    if (!std::filesystem::is_directory(root_path, error) || error) {
      return failure(error == std::errc::permission_denied
                         ? RepositorySnapshotErrorCode::permission_denied
                         : RepositorySnapshotErrorCode::not_directory,
                     "repository root is not a readable directory");
    }
    auto root = root_path.generic_string();

    auto top_level = m_impl->git({"-C", root, "rev-parse", "--show-toplevel"},
                                 {}, request.limits, stop_token, deadline);
    if (!top_level) return std::unexpected(std::move(top_level.error()));

    std::optional<std::string> object_format;
    if (top_level->exit_code == 0) {
      auto discovered = trim_record(std::move(top_level->output));
      root_path = std::filesystem::canonical(discovered, error);
      if (error) {
        return error_for_path(error,
                              "Git repository root could not be resolved");
      }
      root = root_path.generic_string();
      auto format =
          m_impl->git({"-C", root, "rev-parse", "--show-object-format"}, {},
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
      return object_format ? m_impl->observe_git_once(root, *object_format,
                                                      request.limits,
                                                      stop_token, deadline)
                           : m_impl->observe_plain_once(root, request.limits,
                                                        stop_token, deadline);
    };
    auto first = observe_once();
    if (!first) return std::unexpected(std::move(first.error()));
    auto second = observe_once();
    if (!second) return std::unexpected(std::move(second.error()));
    if (!domain::same_source_state(*first, *second) ||
        first->vcs != second->vcs || first->changes != second->changes) {
      return failure(RepositorySnapshotErrorCode::unstable,
                     "repository changed while it was being observed", true);
    }
    second->observed_at =
        std::chrono::time_point_cast<std::chrono::milliseconds>(
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

auto GitExactSourceEditor::read(repository::ExactSourceReadRequest request,
                                const std::stop_token stop_token)
    -> std::expected<repository::ExactSourceReadResult,
                     repository::ExactSourceEditError> {
  try {
    const auto valid = repository::validate_exact_source_read_request(request);
    if (!valid) return std::unexpected(valid.error());
    if (stop_token.stop_requested()) {
      return exact_failure(ExactErrorCode::cancelled,
                           "exact-source read cancelled");
    }
#ifdef _WIN32
    static_cast<void>(stop_token);
    return exact_failure(ExactErrorCode::io_failure,
                         "exact-source reads are unavailable on this platform");
#else
    const auto deadline =
        std::chrono::steady_clock::now() + request.limits.timeout;
    const auto snapshot_limits = exact_snapshot_limits(request.limits);
    auto observe = [&]() -> std::expected<RepositorySnapshot, ExactError> {
      auto result = m_snapshot_source.observe(
          {request.baseline.root.canonical_path, snapshot_limits}, stop_token);
      if (!result) return std::unexpected(exact_error(result.error()));
      return std::move(*result);
    };
    auto before = observe();
    if (!before) return std::unexpected(std::move(before.error()));
    if (!same_repository_state(*before, request.baseline)) {
      return exact_failure(ExactErrorCode::stale_snapshot,
                           "repository changed before exact-source read",
                           domain::snapshot_identity(*before), {}, true);
    }
    auto file = read_exact_file(request.baseline.root.canonical_path,
                                request.relative_path, request.limits,
                                stop_token, deadline);
    if (!file) return std::unexpected(std::move(file.error()));

    const auto algorithm = request.baseline.vcs
                               ? "git-" + request.baseline.vcs->object_format
                               : request.baseline.fingerprint.algorithm;
    const std::optional<std::string> hash_root =
        request.baseline.vcs
            ? std::optional<std::string>{request.baseline.root.canonical_path}
            : std::nullopt;
    auto digest = m_snapshot_source.m_impl->hash_bytes(
        file->content, hash_root, algorithm, snapshot_limits, stop_token,
        deadline);
    if (!digest) return std::unexpected(exact_error(digest.error()));

    auto after = observe();
    if (!after) return std::unexpected(std::move(after.error()));
    if (!same_repository_state(*after, request.baseline)) {
      return exact_failure(ExactErrorCode::concurrent_change,
                           "repository changed during exact-source read",
                           domain::snapshot_identity(*after), {}, true);
    }
    const auto changed =
        std::ranges::find(request.baseline.changes, request.relative_path,
                          &RepositoryChange::relative_path);
    if (changed != request.baseline.changes.end() && changed->worktree_digest &&
        *changed->worktree_digest != *digest) {
      domain::RepositorySourceIdentity observed{
          domain::snapshot_identity(*after),
          request.relative_path,
          *digest,
          {}};
      return exact_failure(ExactErrorCode::source_mismatch,
                           "exact-source content does not match its baseline",
                           domain::snapshot_identity(*after), observed, true);
    }
    return repository::ExactSourceReadResult{{domain::snapshot_identity(*after),
                                              request.relative_path,
                                              std::move(*digest),
                                              {}},
                                             std::move(file->content)};
#endif
  } catch (const std::filesystem::filesystem_error&) {
    return exact_failure(ExactErrorCode::io_failure,
                         "exact-source read failed in the filesystem", {}, {},
                         true);
  } catch (...) {
    return exact_failure(ExactErrorCode::internal_failure,
                         "exact-source read failed internally");
  }
}

auto GitExactSourceEditor::apply(repository::ExactSourceEditRequest request,
                                 const std::stop_token stop_token)
    -> std::expected<repository::ExactSourceEditReceipt,
                     repository::ExactSourceEditError> {
  bool effect_committed{};
  try {
    const auto valid = repository::validate_exact_source_edit_request(request);
    if (!valid) return std::unexpected(valid.error());
    if (stop_token.stop_requested()) {
      return exact_failure(ExactErrorCode::cancelled,
                           "exact-source edit cancelled");
    }
#ifdef _WIN32
    static_cast<void>(stop_token);
    return exact_failure(ExactErrorCode::io_failure,
                         "exact-source edits are unavailable on this platform");
#else
    const auto deadline =
        std::chrono::steady_clock::now() + request.limits.timeout;
    auto write_lock = RepositoryWriteLock::acquire(
        request.baseline.root.canonical_path, stop_token, deadline);
    if (!write_lock) return std::unexpected(std::move(write_lock.error()));

    const repository::ExactSourceReadRequest read_request{
        request.baseline, request.expected_source.relative_path,
        request.limits};
    auto current = read(read_request, stop_token);
    if (!current) return std::unexpected(std::move(current.error()));
    if (current->source != request.expected_source) {
      return exact_failure(ExactErrorCode::source_mismatch,
                           "exact-source edit precondition no longer matches",
                           current->source.snapshot, current->source, true);
    }

    const auto prefix_size = static_cast<std::size_t>(request.range.begin);
    const auto suffix_offset = static_cast<std::size_t>(request.range.end);
    std::string replacement;
    replacement.reserve(prefix_size + request.replacement.size() +
                        (current->content.size() - suffix_offset));
    replacement.append(current->content.data(), prefix_size);
    replacement.append(request.replacement);
    replacement.append(current->content.data() + suffix_offset,
                       current->content.size() - suffix_offset);

    auto target_state = read_exact_file(request.baseline.root.canonical_path,
                                        request.expected_source.relative_path,
                                        request.limits, stop_token, deadline);
    if (!target_state) return std::unexpected(std::move(target_state.error()));
    if (target_state->content != current->content) {
      return exact_failure(ExactErrorCode::concurrent_change,
                           "exact-source target changed before replacement",
                           current->source.snapshot, {}, true);
    }
    const auto target =
        std::filesystem::path{request.baseline.root.canonical_path} /
        request.expected_source.relative_path;
    auto prepared = PreparedReplacement::create(
        request.baseline.root.canonical_path, target, replacement,
        target_state->mode, stop_token, deadline);
    if (!prepared) return std::unexpected(std::move(prepared.error()));

    // The prepared file is outside the observed worktree (or within .git), so
    // this final exact read checks both repository and target preconditions at
    // the effect boundary without making its own temporary file look dirty.
    auto final_check = read(read_request, stop_token);
    if (!final_check) return std::unexpected(std::move(final_check.error()));
    if (final_check->source != request.expected_source ||
        final_check->content != current->content) {
      return exact_failure(ExactErrorCode::concurrent_change,
                           "exact-source target changed before commit",
                           final_check->source.snapshot, final_check->source,
                           true);
    }
    if (stop_token.stop_requested()) {
      return exact_failure(ExactErrorCode::cancelled,
                           "exact-source edit cancelled before commit");
    }
    auto committed = prepared->commit();
    if (!committed) return std::unexpected(std::move(committed.error()));
    effect_committed = true;

    const auto snapshot_limits = exact_snapshot_limits(request.limits);
    auto after = m_snapshot_source.observe(
        {request.baseline.root.canonical_path, snapshot_limits}, stop_token);
    if (!after) {
      auto error = exact_error(after.error());
      error.message =
          "exact-source edit committed but postcondition observation failed";
      error.may_have_applied = true;
      return std::unexpected(std::move(error));
    }
    repository::ExactSourceReadRequest after_request{
        *after, request.expected_source.relative_path, request.limits};
    auto resulting = read(std::move(after_request), stop_token);
    if (!resulting) {
      auto error = std::move(resulting.error());
      error.message =
          "exact-source edit committed but resulting source could not be read";
      error.may_have_applied = true;
      return std::unexpected(std::move(error));
    }
    const auto replacement_end =
        request.range.begin + request.replacement.size();
    repository::ExactSourceEditReceipt receipt{
        request.expected_source,
        resulting->source,
        request.range,
        {request.range.begin, replacement_end},
        request.expected_source.snapshot,
        resulting->source.snapshot};
    auto receipt_valid =
        repository::validate_exact_source_edit_receipt(request, receipt);
    if (!receipt_valid) {
      auto error = receipt_valid.error();
      error.message =
          "exact-source edit committed but its receipt is inconsistent";
      error.observed_snapshot = resulting->source.snapshot;
      error.observed_source = resulting->source;
      error.may_have_applied = true;
      return std::unexpected(std::move(error));
    }
    return receipt;
#endif
  } catch (const std::filesystem::filesystem_error&) {
    return exact_failure(ExactErrorCode::io_failure,
                         "exact-source edit failed in the filesystem", {}, {},
                         true, effect_committed);
  } catch (...) {
    return exact_failure(ExactErrorCode::internal_failure,
                         "exact-source edit failed internally", {}, {}, false,
                         effect_committed);
  }
}

auto GitProjectInstructionSource::discover(
    repository::ProjectInstructionRequest request,
    const std::stop_token stop_token)
    -> std::expected<domain::ProjectInstructionDiscovery,
                     repository::ProjectInstructionError> {
  using ProjectCode = repository::ProjectInstructionErrorCode;
  try {
    constexpr repository::ProjectInstructionLimits maximums;
    if (request.limits.maximum_documents == 0 ||
        request.limits.maximum_path_bytes == 0 ||
        request.limits.maximum_document_bytes == 0 ||
        request.limits.maximum_total_bytes == 0 ||
        request.limits.timeout <= std::chrono::milliseconds::zero() ||
        request.limits.maximum_documents > maximums.maximum_documents ||
        request.limits.maximum_path_bytes > maximums.maximum_path_bytes ||
        request.limits.maximum_document_bytes >
            maximums.maximum_document_bytes ||
        request.limits.maximum_total_bytes > maximums.maximum_total_bytes ||
        request.limits.timeout > maximums.timeout ||
        request.limits.maximum_document_bytes >
            request.limits.maximum_total_bytes ||
        !valid_project_subtree(request.target_subtree,
                               request.limits.maximum_path_bytes)) {
      return project_failure(ProjectCode::invalid_request,
                             "project instruction request is invalid");
    }
    const auto validated =
        repository::validate_repository_snapshot(request.baseline);
    if (!validated) {
      return project_failure(ProjectCode::invalid_request,
                             "project instruction baseline is invalid");
    }
    if (stop_token.stop_requested()) {
      return project_failure(ProjectCode::cancelled,
                             "project instruction discovery cancelled");
    }

    const auto deadline =
        std::chrono::steady_clock::now() + request.limits.timeout;
    auto observe = [&]() -> std::expected<domain::RepositorySnapshot,
                                          repository::ProjectInstructionError> {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        return project_failure(ProjectCode::timed_out,
                               "project instruction discovery timed out", {},
                               true);
      }
      auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
      if (remaining <= std::chrono::milliseconds::zero()) {
        remaining = std::chrono::milliseconds{1};
      }
      repository::RepositorySnapshotLimits limits;
      limits.maximum_path_bytes = request.limits.maximum_path_bytes;
      limits.observation_timeout =
          std::min(remaining, limits.observation_timeout);
      limits.command_timeout =
          std::min(limits.command_timeout, limits.observation_timeout);
      auto snapshot = m_snapshot_source.observe(
          {request.baseline.root.canonical_path, limits}, stop_token);
      if (!snapshot) return std::unexpected(project_error(snapshot.error()));
      return std::move(*snapshot);
    };

    const std::filesystem::path root{request.baseline.root.canonical_path};
    std::filesystem::path target = root;
    if (!request.target_subtree.empty()) {
      for (const auto& part : std::filesystem::path{request.target_subtree}) {
        target /= part;
        std::error_code error;
        const auto status = std::filesystem::symlink_status(target, error);
        if (error) {
          return project_path_error(
              error, "project instruction target could not be inspected",
              request.target_subtree);
        }
        if (std::filesystem::is_symlink(status)) {
          return project_failure(
              ProjectCode::outside_repository,
              "project instruction target cannot traverse a symbolic link",
              request.target_subtree);
        }
        if (!std::filesystem::is_directory(status)) {
          return project_failure(
              ProjectCode::not_found,
              "project instruction target is not a directory",
              request.target_subtree);
        }
      }
    }

    auto before = observe();
    if (!before) return std::unexpected(std::move(before.error()));
    if (before->root != request.baseline.root ||
        before->vcs != request.baseline.vcs ||
        before->changes != request.baseline.changes ||
        !domain::same_source_state(*before, request.baseline)) {
      return project_failure(ProjectCode::stale_snapshot,
                             "repository changed before instruction discovery",
                             {}, true);
    }

    struct Scope {
      std::string relative_path;
      std::uint32_t specificity{};
    };
    std::vector<Scope> scopes{{"", 0}};
    std::string subtree;
    std::uint32_t specificity{};
    if (!request.target_subtree.empty()) {
      for (const auto& part : std::filesystem::path{request.target_subtree}) {
        if (!subtree.empty()) subtree.push_back('/');
        subtree.append(part.generic_string());
        ++specificity;
        scopes.push_back({subtree, specificity});
      }
    }
    if (scopes.size() > request.limits.maximum_documents) {
      return project_failure(ProjectCode::resource_exhausted,
                             "project instruction ancestry is too deep");
    }

    std::vector<domain::ProjectInstructionDocument> documents;
    std::uint64_t total_bytes{};
    std::uint64_t discovery_order{};
    for (const auto& scope : scopes) {
      if (stop_token.stop_requested()) {
        return project_failure(ProjectCode::cancelled,
                               "project instruction discovery cancelled");
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        return project_failure(ProjectCode::timed_out,
                               "project instruction discovery timed out", {},
                               true);
      }
      const auto relative_path = scope.relative_path.empty()
                                     ? std::string{"AGENTS.md"}
                                     : scope.relative_path + "/AGENTS.md";

#ifdef _WIN32
      return project_failure(
          ProjectCode::io_failure,
          "project instruction reads are unavailable on this platform",
          relative_path);
#else
      int directory_descriptor =
          ::open(root.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
      if (directory_descriptor < 0) {
        return project_path_error(
            std::error_code{errno, std::generic_category()},
            "project instruction root could not be opened", relative_path);
      }
      bool directory_failed{};
      int directory_error{};
      if (!scope.relative_path.empty()) {
        for (const auto& part : std::filesystem::path{scope.relative_path}) {
          const int child =
              ::openat(directory_descriptor, part.c_str(),
                       O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
          if (child < 0) {
            directory_failed = true;
            directory_error = errno;
            break;
          }
          static_cast<void>(::close(directory_descriptor));
          directory_descriptor = child;
        }
      }
      if (directory_failed) {
        static_cast<void>(::close(directory_descriptor));
        if (directory_error == ELOOP) {
          return project_failure(
              ProjectCode::outside_repository,
              "project instruction target cannot traverse a symbolic link",
              scope.relative_path);
        }
        return project_path_error(
            std::error_code{directory_error, std::generic_category()},
            "project instruction target could not be opened",
            scope.relative_path);
      }
      const int descriptor = ::openat(directory_descriptor, "AGENTS.md",
                                      O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
      const int open_error = errno;
      static_cast<void>(::close(directory_descriptor));
      if (descriptor < 0) {
        if (open_error == ENOENT) continue;
        if (open_error == ELOOP) {
          return project_failure(
              ProjectCode::unsupported_entry,
              "project instruction file cannot be a symbolic link",
              relative_path);
        }
        return project_path_error(
            std::error_code{open_error, std::generic_category()},
            "project instruction file could not be opened", relative_path);
      }
      struct stat before_read{};
      if (::fstat(descriptor, &before_read) != 0) {
        const auto saved_errno = errno;
        static_cast<void>(::close(descriptor));
        return project_path_error(
            std::error_code{saved_errno, std::generic_category()},
            "project instruction file could not be identified", relative_path);
      }
      if (!S_ISREG(before_read.st_mode)) {
        static_cast<void>(::close(descriptor));
        return project_failure(ProjectCode::unsupported_entry,
                               "project instruction path is not a regular file",
                               relative_path);
      }
      if (before_read.st_size < 0 ||
          static_cast<std::uint64_t>(before_read.st_size) >
              request.limits.maximum_document_bytes ||
          static_cast<std::uint64_t>(before_read.st_size) >
              request.limits.maximum_total_bytes - total_bytes) {
        static_cast<void>(::close(descriptor));
        return project_failure(ProjectCode::resource_exhausted,
                               "project instruction content exceeds its budget",
                               relative_path);
      }
      std::string content;
      content.reserve(static_cast<std::size_t>(before_read.st_size));
      std::array<char, 8192> buffer{};
      while (true) {
        if (stop_token.stop_requested()) {
          static_cast<void>(::close(descriptor));
          return project_failure(ProjectCode::cancelled,
                                 "project instruction discovery cancelled",
                                 relative_path);
        }
        if (std::chrono::steady_clock::now() >= deadline) {
          static_cast<void>(::close(descriptor));
          return project_failure(ProjectCode::timed_out,
                                 "project instruction discovery timed out",
                                 relative_path, true);
        }
        const auto count = ::read(descriptor, buffer.data(), buffer.size());
        if (count == 0) break;
        if (count < 0) {
          if (errno == EINTR) continue;
          const auto error = std::error_code{errno, std::generic_category()};
          static_cast<void>(::close(descriptor));
          return project_path_error(
              error, "project instruction file could not be read",
              relative_path);
        }
        const auto bytes = static_cast<std::size_t>(count);
        if (bytes > request.limits.maximum_document_bytes - content.size() ||
            bytes > request.limits.maximum_total_bytes - total_bytes -
                        content.size()) {
          static_cast<void>(::close(descriptor));
          return project_failure(
              ProjectCode::resource_exhausted,
              "project instruction content exceeds its budget", relative_path);
        }
        content.append(buffer.data(), bytes);
      }
      struct stat after_read{};
      const bool stable =
          ::fstat(descriptor, &after_read) == 0 &&
          before_read.st_dev == after_read.st_dev &&
          before_read.st_ino == after_read.st_ino &&
          before_read.st_size == after_read.st_size &&
          before_read.st_mtim.tv_sec == after_read.st_mtim.tv_sec &&
          before_read.st_mtim.tv_nsec == after_read.st_mtim.tv_nsec;
      static_cast<void>(::close(descriptor));
      if (!stable) {
        return project_failure(ProjectCode::unstable,
                               "project instruction changed while being read",
                               relative_path, true);
      }
      if (content.empty()) continue;
      if (!valid_utf8_instruction(content)) {
        return project_failure(ProjectCode::malformed_text,
                               "project instruction is not bounded UTF-8 text",
                               relative_path);
      }

      repository::RepositorySnapshotLimits hash_limits;
      hash_limits.maximum_path_bytes = request.limits.maximum_path_bytes;
      hash_limits.maximum_file_bytes = request.limits.maximum_document_bytes;
      hash_limits.maximum_total_bytes = request.limits.maximum_total_bytes;
      hash_limits.command_timeout =
          std::min(hash_limits.command_timeout,
                   std::chrono::duration_cast<std::chrono::milliseconds>(
                       deadline - std::chrono::steady_clock::now()));
      if (hash_limits.command_timeout <= std::chrono::milliseconds::zero()) {
        return project_failure(ProjectCode::timed_out,
                               "project instruction discovery timed out", {},
                               true);
      }
      auto digest = m_snapshot_source.m_impl->hash_bytes(
          content, request.baseline.root.canonical_path,
          request.baseline.fingerprint.algorithm, hash_limits, stop_token,
          deadline);
      if (!digest) {
        return std::unexpected(project_error(digest.error()));
      }
      total_bytes += content.size();
      ++discovery_order;
      auto instruction_id = domain::ProjectInstructionId::from(
          "project:" + std::to_string(scope.specificity) + ":" + digest->value);
      if (!instruction_id) {
        return project_failure(ProjectCode::internal_failure,
                               "project instruction identity is invalid",
                               relative_path);
      }
      documents.push_back(domain::ProjectInstructionDocument{
          std::move(*instruction_id),
          domain::RepositorySourceIdentity{
              domain::snapshot_identity(request.baseline), relative_path,
              std::move(*digest), std::nullopt},
          scope.relative_path, std::move(content), scope.specificity,
          discovery_order});
#endif
    }

    auto after = observe();
    if (!after) return std::unexpected(std::move(after.error()));
    if (after->root != before->root || after->vcs != before->vcs ||
        after->changes != before->changes ||
        !domain::same_source_state(*after, *before)) {
      return project_failure(ProjectCode::unstable,
                             "repository changed during instruction discovery",
                             {}, true);
    }
    return domain::ProjectInstructionDiscovery{
        domain::snapshot_identity(request.baseline),
        std::move(request.target_subtree), std::move(documents)};
  } catch (const std::filesystem::filesystem_error&) {
    return project_failure(
        ProjectCode::io_failure,
        "project instruction discovery failed in the filesystem", {}, true);
  } catch (...) {
    return project_failure(ProjectCode::internal_failure,
                           "project instruction discovery failed internally");
  }
}

} // namespace aiforge::adapters
