#include <aiforge/adapters/linux_process_launcher.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef __linux__
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace aiforge::adapters {
namespace {

constexpr std::string_view none_restriction_policy_identity{
    "aiforge.posix-process.none.v1"};
constexpr int minimum_source_descriptor{5};

[[nodiscard]] auto establishment_failure(
    const LinuxProcessLauncherEstablishmentErrorCode code, std::string message)
    -> std::unexpected<LinuxProcessLauncherEstablishmentError> {
  return std::unexpected(
      LinuxProcessLauncherEstablishmentError{code, std::move(message)});
}

[[nodiscard]] auto launch_failure(const runtime::ProcessLaunchErrorCode code,
                                  const runtime::ProcessLaunchStage stage,
                                  std::string message,
                                  const bool retryable = false)
    -> std::unexpected<runtime::ProcessLaunchError> {
  return std::unexpected(runtime::ProcessLaunchError{
      code, stage, std::nullopt, std::move(message), retryable});
}

[[nodiscard]] auto make_context(
    const LinuxProcessLauncherConfiguration& configuration,
    const bool available)
    -> std::expected<runtime::ApplicationLaunchContext,
                     runtime::ApplicationLaunchContextError> {
  runtime::ApplicationLaunchContextConfiguration context;
  context.selected_restriction = configuration.restriction;
  context.approval_mode = configuration.approval_mode;
  context.matcher_policy_identity = configuration.matcher_policy_identity;
  if (available) {
    context.achieved_restriction = runtime::RestrictionLevel::none;
    context.unavailable_reason.reset();
    context.restriction_policy_identity = none_restriction_policy_identity;
  } else {
    context.achieved_restriction.reset();
    context.unavailable_reason =
        runtime::RestrictionUnavailableReason::mechanism_absent;
    context.restriction_policy_identity.reset();
  }
  return runtime::make_application_launch_context(std::move(context));
}

#ifdef __linux__

class UniqueFd final {
 public:
  UniqueFd() = default;
  explicit UniqueFd(const int value) : m_value(value) {}
  ~UniqueFd() { reset(); }
  UniqueFd(const UniqueFd&) = delete;
  auto operator=(const UniqueFd&) -> UniqueFd& = delete;
  UniqueFd(UniqueFd&& other) noexcept
      : m_value(std::exchange(other.m_value, -1)) {}
  auto operator=(UniqueFd&& other) noexcept -> UniqueFd& {
    if (this != &other) {
      reset();
      m_value = std::exchange(other.m_value, -1);
    }
    return *this;
  }

  [[nodiscard]] auto get() const noexcept -> int { return m_value; }
  [[nodiscard]] auto release() noexcept -> int {
    return std::exchange(m_value, -1);
  }
  auto reset(const int value = -1) noexcept -> void {
    if (m_value >= 0) static_cast<void>(::close(m_value));
    m_value = value;
  }

 private:
  int m_value{-1};
};

struct FileIdentity {
  std::uint64_t device{};
  std::uint64_t inode{};
  std::uint32_t mode{};
  auto operator==(const FileIdentity&) const -> bool = default;
};

// This matches the existing none contract: the path is reopened and its
// identity compared immediately before the exact descriptor is entered or
// executed. This defeats pathname replacement at the effect boundary. It is
// not a content snapshot; an authorized same-inode in-place mutation remains
// visible through that descriptor and none makes no stronger integrity claim.

[[nodiscard]] auto identity(const int descriptor)
    -> std::optional<FileIdentity> {
  struct stat status{};
  if (::fstat(descriptor, &status) != 0) return std::nullopt;
  return FileIdentity{static_cast<std::uint64_t>(status.st_dev),
                      static_cast<std::uint64_t>(status.st_ino),
                      static_cast<std::uint32_t>(status.st_mode)};
}

[[nodiscard]] auto hexadecimal(const std::uint64_t value) -> std::string {
  constexpr std::string_view digits{"0123456789abcdef"};
  std::string result(16U, '0');
  for (std::size_t index{}; index < result.size(); ++index) {
    const auto shift = static_cast<unsigned>((15U - index) * 4U);
    result[index] = digits[(value >> shift) & 0xFU];
  }
  return result;
}

[[nodiscard]] auto identity_text(const FileIdentity& value) -> std::string {
  return "fs-" + hexadecimal(value.device) + "-" + hexadecimal(value.inode) +
         "-" + hexadecimal(value.mode);
}

[[nodiscard]] auto duplicate_descriptor(const int descriptor)
    -> std::expected<UniqueFd, runtime::ProcessLaunchError> {
  // The POSIX fcntl API is variadic for all commands.
  int duplicate{};
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  duplicate = ::fcntl(descriptor, F_DUPFD_CLOEXEC, minimum_source_descriptor);
  if (duplicate < 0) {
    return launch_failure(runtime::ProcessLaunchErrorCode::unavailable,
                          runtime::ProcessLaunchStage::validation,
                          "process descriptor could not be retained", true);
  }
  return UniqueFd{duplicate};
}

[[nodiscard]] auto normalize_source_descriptor(UniqueFd descriptor)
    -> std::expected<UniqueFd, runtime::ProcessLaunchError> {
  if (descriptor.get() >= minimum_source_descriptor) return descriptor;
  return duplicate_descriptor(descriptor.get());
}

[[nodiscard]] auto open_without_symlinks(
    const std::string_view path,
    const runtime::ProcessFilesystemTargetKind kind)
    -> std::expected<UniqueFd, runtime::ProcessLaunchError> {
  // The POSIX open API is variadic even though no mode argument is read here.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  UniqueFd current{::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
  if (current.get() < 0) {
    return launch_failure(runtime::ProcessLaunchErrorCode::unavailable,
                          runtime::ProcessLaunchStage::validation,
                          "process path resolution is unavailable", true);
  }
  std::size_t position{1};
  while (position < path.size()) {
    const auto slash = path.find('/', position);
    const auto end = slash == std::string_view::npos ? path.size() : slash;
    const auto component = path.substr(position, end - position);
    if (component.empty() || component == "." || component == "..") {
      return launch_failure(runtime::ProcessLaunchErrorCode::invalid_request,
                            runtime::ProcessLaunchStage::validation,
                            "process path is ambiguous");
    }
    const bool last = end == path.size();
    const bool directory =
        !last || kind == runtime::ProcessFilesystemTargetKind::directory;
    const std::string component_text{component};
    // The POSIX openat API is variadic even though no mode argument is read.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    UniqueFd next{::openat(current.get(), component_text.c_str(),
                           O_RDONLY | O_CLOEXEC | O_NOFOLLOW |
                               (directory ? O_DIRECTORY : 0))};
    if (next.get() < 0) {
      return launch_failure(runtime::ProcessLaunchErrorCode::unavailable,
                            runtime::ProcessLaunchStage::validation,
                            "process path could not be pinned");
    }
    current = std::move(next);
    if (last) break;
    position = end + 1U;
  }
  const auto current_identity = identity(current.get());
  bool expected_type{};
  if (current_identity) {
    switch (kind) {
      case runtime::ProcessFilesystemTargetKind::regular_executable:
        expected_type = S_ISREG(current_identity->mode) &&
                        (current_identity->mode & 0111U) != 0U;
        break;
      case runtime::ProcessFilesystemTargetKind::directory:
        expected_type = S_ISDIR(current_identity->mode);
        break;
    }
  }
  if (!expected_type) {
    return launch_failure(runtime::ProcessLaunchErrorCode::unavailable,
                          runtime::ProcessLaunchStage::validation,
                          "process path has an invalid filesystem type");
  }
  return normalize_source_descriptor(std::move(current));
}

[[nodiscard]] auto path_is_within(const std::string_view parent,
                                  const std::string_view child) -> bool {
  if (parent == "/") return child.starts_with('/');
  return child == parent ||
         (child.size() > parent.size() && child.starts_with(parent) &&
          child[parent.size()] == '/');
}

[[nodiscard]] auto open_descendant_without_symlinks(
    const int root_descriptor, const std::string_view root_path,
    const std::string_view path)
    -> std::expected<UniqueFd, runtime::ProcessLaunchError> {
  if (!path_is_within(root_path, path)) {
    return launch_failure(runtime::ProcessLaunchErrorCode::invalid_request,
                          runtime::ProcessLaunchStage::validation,
                          "process descendant escaped its retained root");
  }
  auto current = duplicate_descriptor(root_descriptor);
  if (!current) return current;
  if (path == root_path) return current;

  std::size_t position = root_path == "/" ? 1U : root_path.size() + 1U;
  while (position < path.size()) {
    const auto slash = path.find('/', position);
    const auto end = slash == std::string_view::npos ? path.size() : slash;
    const std::string component{path.substr(position, end - position)};
    // The POSIX openat API is variadic even though no mode argument is read.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    UniqueFd next{::openat(current->get(), component.c_str(),
                           O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_DIRECTORY)};
    if (next.get() < 0) {
      return launch_failure(runtime::ProcessLaunchErrorCode::contract_drift,
                            runtime::ProcessLaunchStage::validation,
                            "process descendant identity changed before spawn");
    }
    current = normalize_source_descriptor(std::move(next));
    if (!current) return current;
    position = end + 1U;
  }
  return current;
}

[[nodiscard]] auto covering_root_index(
    const std::span<const runtime::ProcessFilesystemRoot> roots,
    const std::string_view path,
    const runtime::ProcessFilesystemAccess required_access)
    -> std::optional<std::size_t> {
  std::optional<std::size_t> result;
  for (std::size_t index{}; index < roots.size(); ++index) {
    const auto& root = roots[index];
    const bool access_covers =
        root.access == runtime::ProcessFilesystemAccess::read_write ||
        required_access == runtime::ProcessFilesystemAccess::read_only;
    if (access_covers && path_is_within(root.path, path) &&
        (!result || root.path.size() > roots[*result].path.size())) {
      result = index;
    }
  }
  return result;
}

[[nodiscard]] auto descriptor_matches(const int descriptor,
                                      const std::string_view expected) -> bool {
  const auto current = identity(descriptor);
  return current && identity_text(*current) == expected;
}

[[noreturn]] auto child_failure(const int descriptor,
                                const int error_number) noexcept -> void {
  const auto bytes =
      std::bit_cast<std::array<std::byte, sizeof(error_number)>>(error_number);
  std::size_t written{};
  while (written < sizeof(error_number)) {
    const auto remaining = std::span{bytes}.subspan(written);
    const auto count = ::write(descriptor, remaining.data(), remaining.size());
    if (count > 0) {
      written += static_cast<std::size_t>(count);
    } else if (count < 0 && errno == EINTR) {
      continue;
    } else {
      break;
    }
  }
  _exit(127);
}

auto close_extra_descriptors(const int descriptor_limit) noexcept -> void {
#if defined(SYS_close_range)
  // The raw Linux syscall boundary is necessarily variadic.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  if (::syscall(SYS_close_range, 5U, ~0U, 0U) == 0) return;
#endif
  for (int descriptor = 5; descriptor < descriptor_limit; ++descriptor) {
    static_cast<void>(::close(descriptor));
  }
}

[[nodiscard]] auto set_nonblocking(const int descriptor) -> bool {
  // The POSIX fcntl API is variadic for all commands.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  const auto flags = ::fcntl(descriptor, F_GETFL);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  return flags >= 0 && ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0;
}

[[nodiscard]] auto set_close_on_exec(const int descriptor, const bool enabled)
    -> bool {
  // The POSIX fcntl API is variadic for all commands.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  return ::fcntl(descriptor, F_SETFD, enabled ? FD_CLOEXEC : 0) == 0;
}

[[nodiscard]] auto make_pipe(std::array<int, 2>& descriptors) -> bool {
  if (::pipe2(descriptors.data(), O_CLOEXEC) != 0) return false;
  for (auto& descriptor : descriptors) {
    if (descriptor >= minimum_source_descriptor) continue;
    auto normalized = duplicate_descriptor(descriptor);
    if (!normalized) {
      for (const auto opened : descriptors) {
        if (opened >= 0) static_cast<void>(::close(opened));
      }
      descriptors = {-1, -1};
      return false;
    }
    static_cast<void>(::close(descriptor));
    descriptor = normalized->release();
  }
  return true;
}

class LinuxProcessStream final : public runtime::ProcessLaunchStream {
 public:
  LinuxProcessStream(pid_t process, UniqueFd standard_output,
                     UniqueFd standard_error, UniqueFd exec_error,
                     runtime::ProcessLaunchLimits limits)
      : m_process(process), m_standard_output(std::move(standard_output)),
        m_standard_error(std::move(standard_error)),
        m_exec_error(std::move(exec_error)), m_limits(limits),
        m_started(std::chrono::steady_clock::now()) {}

  ~LinuxProcessStream() override {
    if (!m_terminal_emitted) {
      m_discard_output = true;
      static_cast<void>(terminate());
    }
  }

  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  auto next(const std::stop_token stop_token) noexcept
      -> std::expected<std::optional<runtime::ProcessLaunchEvent>,
                       runtime::ProcessLaunchError> override {
    try {
      if (m_terminal_emitted) {
        return std::optional<runtime::ProcessLaunchEvent>{};
      }
      for (;;) {
        if (stop_token.stop_requested()) {
          m_discard_output = true;
          if (!terminate()) {
            return launch_failure(
                runtime::ProcessLaunchErrorCode::cleanup_failed,
                runtime::ProcessLaunchStage::cleanup,
                "cancelled process cleanup could not be proven");
          }
          return launch_failure(runtime::ProcessLaunchErrorCode::cancelled,
                                runtime::ProcessLaunchStage::termination,
                                "process execution cancelled");
        }
        if (!m_forced_terminal &&
            std::chrono::steady_clock::now() - m_started >=
                m_limits.wall_time) {
          m_forced_terminal = runtime::ProcessTerminalKind::timed_out;
          if (!terminate()) {
            return launch_failure(
                runtime::ProcessLaunchErrorCode::cleanup_failed,
                runtime::ProcessLaunchStage::cleanup,
                "timed out process cleanup could not be proven");
          }
        }
        pump(25);
        if (m_io_failure) {
          m_discard_output = true;
          if (!terminate()) {
            return launch_failure(
                runtime::ProcessLaunchErrorCode::cleanup_failed,
                runtime::ProcessLaunchStage::cleanup,
                "failed process output cleanup could not be proven");
          }
          return launch_failure(runtime::ProcessLaunchErrorCode::unavailable,
                                runtime::ProcessLaunchStage::execution,
                                "process output could not be collected", true);
        }
        if (m_output_limit && !m_forced_terminal) {
          m_forced_terminal = runtime::ProcessTerminalKind::output_limit;
          if (!terminate()) {
            return launch_failure(
                runtime::ProcessLaunchErrorCode::cleanup_failed,
                runtime::ProcessLaunchStage::cleanup,
                "overbound process cleanup could not be proven");
          }
        }
        if (auto progress = next_progress()) return progress;
        if (finished()) return terminal();
      }
    } catch (...) {
      m_discard_output = true;
      const bool cleaned = terminate();
      return launch_failure(
          cleaned ? runtime::ProcessLaunchErrorCode::internal_failure
                  : runtime::ProcessLaunchErrorCode::cleanup_failed,
          cleaned ? runtime::ProcessLaunchStage::execution
                  : runtime::ProcessLaunchStage::cleanup,
          cleaned ? "process stream failed internally"
                  : "process stream cleanup could not be proven");
    }
  }

 private:
  [[nodiscard]] auto finished() const noexcept -> bool {
    return m_reaped && m_standard_output.get() < 0 &&
           m_standard_error.get() < 0 && m_exec_error.get() < 0;
  }

  [[nodiscard]] auto next_progress()
      -> std::optional<runtime::ProcessLaunchEvent> {
    const auto limit = static_cast<std::size_t>(
        std::min<std::uint64_t>(m_limits.maximum_progress_chunk_bytes,
                                std::numeric_limits<std::size_t>::max()));
    const auto make_progress =
        [&](const runtime::ProcessOutputStream stream,
            const std::vector<std::byte>& bytes,
            std::size_t& offset) -> std::optional<runtime::ProcessLaunchEvent> {
      if (offset >= bytes.size()) return std::nullopt;
      const auto count = std::min(limit, bytes.size() - offset);
      runtime::ProcessLaunchProgress progress;
      progress.stream = stream;
      progress.content.assign(
          bytes.begin() + static_cast<std::ptrdiff_t>(offset),
          bytes.begin() + static_cast<std::ptrdiff_t>(offset + count));
      offset += count;
      return runtime::ProcessLaunchEvent{std::move(progress)};
    };
    if (auto progress =
            make_progress(runtime::ProcessOutputStream::standard_output,
                          m_stdout, m_stdout_emitted)) {
      return progress;
    }
    return make_progress(runtime::ProcessOutputStream::standard_error, m_stderr,
                         m_stderr_emitted);
  }

  auto collect(UniqueFd& descriptor, std::vector<std::byte>& destination)
      -> void {
    if (m_discard_output) return;
    std::array<std::byte, 4096> buffer{};
    for (;;) {
      const auto count = ::read(descriptor.get(), buffer.data(), buffer.size());
      if (count > 0) {
        const auto size = static_cast<std::size_t>(count);
        const auto remaining =
            m_observed_output < m_limits.maximum_output_bytes
                ? m_limits.maximum_output_bytes - m_observed_output
                : 0U;
        const auto accepted =
            static_cast<std::size_t>(std::min<std::uint64_t>(size, remaining));
        destination.insert(destination.end(), buffer.begin(),
                           buffer.begin() +
                               static_cast<std::ptrdiff_t>(accepted));
        m_observed_output += accepted;
        if (accepted != size) {
          m_output_limit = true;
          return;
        }
        continue;
      }
      if (count == 0) {
        descriptor.reset();
        return;
      }
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      descriptor.reset();
      m_io_failure = true;
      return;
    }
  }

  auto collect_exec_error() -> void {
    int value{};
    for (;;) {
      const auto count = ::read(m_exec_error.get(), &value, sizeof(value));
      if (std::cmp_equal(count, sizeof(value))) {
        m_exec_errno = value;
        continue;
      }
      if (count == 0) {
        m_exec_error.reset();
        return;
      }
      if (count < 0 && errno == EINTR) continue;
      if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
      m_exec_error.reset();
      m_io_failure = true;
      return;
    }
  }

  auto reap(const int options) -> void {
    if (m_reaped) return;
    int status{};
    const auto waited = ::waitpid(m_process, &status, options);
    if (waited == m_process) {
      m_wait_status = status;
      m_reaped = true;
      // `none` provides best-effort process-group lifecycle only. This does not
      // claim containment of hostile descendants that escape the group.
      static_cast<void>(::kill(-m_process, SIGKILL));
    } else if (waited < 0 && errno == ECHILD) {
      m_reaped = true;
    } else if (waited < 0 && errno != EINTR) {
      m_io_failure = true;
    }
  }

  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  auto pump(const int timeout_ms) -> void {
    std::array<struct pollfd, 3> descriptors{};
    std::array<UniqueFd*, 3> owners{&m_standard_output, &m_standard_error,
                                    &m_exec_error};
    nfds_t count{};
    for (auto* owner : owners) {
      if (owner->get() >= 0) {
        descriptors[count++] = {owner->get(), POLLIN | POLLHUP | POLLERR, 0};
      }
    }
    if (count != 0) {
      int result{};
      for (;;) {
        result = ::poll(descriptors.data(), count, timeout_ms);
        if (result >= 0 || errno != EINTR) break;
      }
      if (result < 0) m_io_failure = true;
    } else if (!m_reaped && timeout_ms > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds{timeout_ms});
    }
    if (m_standard_output.get() >= 0) collect(m_standard_output, m_stdout);
    if (m_standard_error.get() >= 0) collect(m_standard_error, m_stderr);
    if (m_exec_error.get() >= 0) collect_exec_error();
    reap(WNOHANG);
    if (m_reaped) {
      if (!m_reaped_at) m_reaped_at = std::chrono::steady_clock::now();
      if (std::chrono::steady_clock::now() - *m_reaped_at >=
          m_limits.termination_grace) {
        m_standard_output.reset();
        m_standard_error.reset();
        m_exec_error.reset();
      }
    }
  }

  [[nodiscard]] auto terminate() noexcept -> bool {
    if (m_process <= 0 || m_reaped) return true;
    static_cast<void>(::kill(-m_process, SIGTERM));
    const auto deadline =
        std::chrono::steady_clock::now() + m_limits.termination_grace;
    while (!m_reaped && std::chrono::steady_clock::now() < deadline)
      pump(10);
    static_cast<void>(::kill(-m_process, SIGKILL));
    while (!m_reaped) {
      reap(0);
      if (!m_reaped && errno == EINTR) continue;
      if (!m_reaped) return false;
    }
    for (int attempt{}; attempt < 4; ++attempt)
      pump(0);
    m_standard_output.reset();
    m_standard_error.reset();
    m_exec_error.reset();
    return m_reaped;
  }

  [[nodiscard]] auto terminal() -> std::optional<runtime::ProcessLaunchEvent> {
    runtime::ProcessLaunchTerminal result;
    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - m_started);
    result.standard_output = std::move(m_stdout);
    result.standard_error = std::move(m_stderr);
    if (m_forced_terminal) {
      result.kind = *m_forced_terminal;
    } else if (m_exec_errno) {
      result.kind = runtime::ProcessTerminalKind::spawn_failed;
      switch (*m_exec_errno) {
        case ENOENT:
          result.spawn_error = runtime::ProcessSpawnError::not_found;
          break;
        case EACCES:
          result.spawn_error = runtime::ProcessSpawnError::permission_denied;
          break;
        case ENOEXEC:
          result.spawn_error = runtime::ProcessSpawnError::invalid_format;
          break;
        default:
          result.spawn_error =
              runtime::ProcessSpawnError::operating_system_error;
          break;
      }
    } else if (WIFEXITED(m_wait_status)) {
      result.kind = runtime::ProcessTerminalKind::exited;
      result.exit_code = WEXITSTATUS(m_wait_status);
    } else if (WIFSIGNALED(m_wait_status)) {
      result.kind = runtime::ProcessTerminalKind::signaled;
      result.signal = WTERMSIG(m_wait_status);
    } else {
      result.kind = runtime::ProcessTerminalKind::spawn_failed;
      result.spawn_error = runtime::ProcessSpawnError::operating_system_error;
    }
    m_terminal_emitted = true;
    return runtime::ProcessLaunchEvent{std::move(result)};
  }

  pid_t m_process{-1};
  UniqueFd m_standard_output;
  UniqueFd m_standard_error;
  UniqueFd m_exec_error;
  runtime::ProcessLaunchLimits m_limits;
  std::chrono::steady_clock::time_point m_started;
  std::optional<std::chrono::steady_clock::time_point> m_reaped_at;
  std::vector<std::byte> m_stdout;
  std::vector<std::byte> m_stderr;
  std::size_t m_stdout_emitted{};
  std::size_t m_stderr_emitted{};
  std::uint64_t m_observed_output{};
  int m_wait_status{};
  std::optional<int> m_exec_errno;
  std::optional<runtime::ProcessTerminalKind> m_forced_terminal;
  bool m_output_limit{};
  bool m_io_failure{};
  bool m_reaped{};
  bool m_terminal_emitted{};
  bool m_discard_output{};
};

class LinuxProcessLauncher final : public runtime::ProcessLauncher {
 public:
  LinuxProcessLauncher(runtime::ProcessLaunchBounds bounds,
                       runtime::ProcessLauncherContract contract)
      : ProcessLauncher(std::move(contract)), m_bounds(bounds) {}

 private:
  auto do_pin_path(std::string path,
                   const runtime::ProcessFilesystemTargetKind kind) noexcept
      -> std::expected<std::string, runtime::ProcessLaunchError> override {
    try {
      if (path.size() > m_bounds.maximum_path_bytes) {
        return launch_failure(runtime::ProcessLaunchErrorCode::invalid_request,
                              runtime::ProcessLaunchStage::validation,
                              "process path exceeds configured bounds");
      }
      auto pinned = open_without_symlinks(path, kind);
      if (!pinned) return std::unexpected(std::move(pinned.error()));
      const auto pinned_identity = identity(pinned->get());
      if (!pinned_identity) {
        return launch_failure(runtime::ProcessLaunchErrorCode::unavailable,
                              runtime::ProcessLaunchStage::validation,
                              "process path identity is unavailable");
      }
      auto result = identity_text(*pinned_identity);
      if (result.size() > m_bounds.maximum_identity_bytes) {
        return launch_failure(
            runtime::ProcessLaunchErrorCode::invalid_request,
            runtime::ProcessLaunchStage::validation,
            "process path identity exceeds configured bounds");
      }
      return result;
    } catch (...) {
      return launch_failure(runtime::ProcessLaunchErrorCode::internal_failure,
                            runtime::ProcessLaunchStage::validation,
                            "process path pinning failed internally");
    }
  }

  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  auto do_launch(runtime::ProcessLaunchRequest request,
                 const std::stop_token stop_token) noexcept
      -> std::expected<std::unique_ptr<runtime::ProcessLaunchStream>,
                       runtime::ProcessLaunchError> override {
    std::array<int, 2> output{-1, -1};
    std::array<int, 2> error{-1, -1};
    std::array<int, 2> exec_error{-1, -1};
    pid_t spawned_process{-1};
    const auto close_pipes = [&]() noexcept {
      for (const auto descriptor : {output[0], output[1], error[0], error[1],
                                    exec_error[0], exec_error[1]}) {
        if (descriptor >= 0) static_cast<void>(::close(descriptor));
      }
      output = {-1, -1};
      error = {-1, -1};
      exec_error = {-1, -1};
    };
    try {
      if (stop_token.stop_requested()) {
        return launch_failure(runtime::ProcessLaunchErrorCode::cancelled,
                              runtime::ProcessLaunchStage::validation,
                              "process launch cancelled");
      }
      if (auto valid =
              runtime::validate_process_launch_request(request, m_bounds);
          !valid) {
        return std::unexpected(std::move(valid.error()));
      }
      auto executable = checked_path(
          request.executable, request.executable_identity,
          runtime::ProcessFilesystemTargetKind::regular_executable);
      if (!executable) return std::unexpected(std::move(executable.error()));

      std::vector<UniqueFd> configured_descriptors;
      configured_descriptors.reserve(request.configured_roots.size());
      for (const auto& root : request.configured_roots) {
        auto pinned =
            checked_path(root.path, root.identity,
                         runtime::ProcessFilesystemTargetKind::directory);
        if (!pinned) return std::unexpected(std::move(pinned.error()));
        configured_descriptors.push_back(std::move(*pinned));
      }

      std::vector<UniqueFd> requested_descriptors;
      requested_descriptors.reserve(request.requested_roots.size());
      for (const auto& requested : request.requested_roots) {
        const auto configured_index = covering_root_index(
            std::span<const runtime::ProcessFilesystemRoot>{
                request.configured_roots},
            requested.path, requested.access);
        if (!configured_index) {
          return launch_failure(
              runtime::ProcessLaunchErrorCode::invalid_request,
              runtime::ProcessLaunchStage::validation,
              "requested process root is not configured");
        }
        auto pinned =
            checked_descendant(configured_descriptors[*configured_index].get(),
                               request.configured_roots[*configured_index].path,
                               requested.path, requested.identity);
        if (!pinned) return std::unexpected(std::move(pinned.error()));
        requested_descriptors.push_back(std::move(*pinned));
      }

      const auto working_root_index = covering_root_index(
          std::span<const runtime::ProcessFilesystemRoot>{
              request.requested_roots},
          request.working_directory,
          runtime::ProcessFilesystemAccess::read_only);
      if (!working_root_index) {
        return launch_failure(runtime::ProcessLaunchErrorCode::invalid_request,
                              runtime::ProcessLaunchStage::validation,
                              "working directory is outside requested roots");
      }
      auto working_directory = checked_descendant(
          requested_descriptors[*working_root_index].get(),
          request.requested_roots[*working_root_index].path,
          request.working_directory, request.working_directory_identity);
      if (!working_directory)
        return std::unexpected(std::move(working_directory.error()));

      std::vector<std::string> argv_storage;
      argv_storage.reserve(request.arguments.size() + 1U);
      argv_storage.push_back(request.executable);
      argv_storage.insert(argv_storage.end(), request.arguments.begin(),
                          request.arguments.end());
      std::vector<char*> arguments;
      arguments.reserve(argv_storage.size() + 1U);
      for (auto& value : argv_storage)
        arguments.push_back(value.data());
      arguments.push_back(nullptr);

      std::vector<std::string> environment_storage;
      environment_storage.reserve(request.environment.size());
      for (const auto& variable : request.environment) {
        environment_storage.push_back(variable.name + "=" + variable.value);
      }
      std::vector<char*> environment;
      environment.reserve(environment_storage.size() + 1U);
      for (auto& value : environment_storage)
        environment.push_back(value.data());
      environment.push_back(nullptr);

      if (!make_pipe(output) || !make_pipe(error) || !make_pipe(exec_error)) {
        close_pipes();
        return launch_failure(runtime::ProcessLaunchErrorCode::unavailable,
                              runtime::ProcessLaunchStage::spawn,
                              "process pipes could not be created", true);
      }
      if (!set_nonblocking(output[0]) || !set_nonblocking(error[0]) ||
          !set_nonblocking(exec_error[0])) {
        close_pipes();
        return launch_failure(runtime::ProcessLaunchErrorCode::unavailable,
                              runtime::ProcessLaunchStage::spawn,
                              "process pipes could not be bounded", true);
      }
      const auto maximum_descriptor = ::sysconf(_SC_OPEN_MAX);
      if (maximum_descriptor < 0) {
        close_pipes();
        return launch_failure(runtime::ProcessLaunchErrorCode::unavailable,
                              runtime::ProcessLaunchStage::spawn,
                              "process descriptor limit is unavailable", true);
      }
      const auto descriptor_limit = static_cast<int>(
          std::min<long>(maximum_descriptor, static_cast<long>(INT_MAX)));

      const auto process = ::fork();
      if (process < 0) {
        close_pipes();
        return launch_failure(runtime::ProcessLaunchErrorCode::unavailable,
                              runtime::ProcessLaunchStage::spawn,
                              "process could not be spawned", true);
      }
      if (process == 0) {
        const auto fail = [&](const int error_number) noexcept {
          child_failure(exec_error[1], error_number);
        };
        if (::setpgid(0, 0) != 0 || ::dup2(output[1], STDOUT_FILENO) < 0 ||
            ::dup2(error[1], STDERR_FILENO) < 0) {
          fail(errno);
        }
        // The POSIX open API is variadic even though no mode argument is read.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        const auto input = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (input < 0 || ::dup2(input, STDIN_FILENO) < 0 ||
            ::fchdir(working_directory->get()) != 0) {
          fail(errno);
        }
        if (input != STDIN_FILENO) static_cast<void>(::close(input));

        sigset_t mask;
        if (::sigemptyset(&mask) != 0) fail(errno);
        // This runs in the single surviving thread after fork, where the
        // async-signal-safe sigprocmask is the required pre-exec primitive.
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        if (::sigprocmask(SIG_SETMASK, &mask, nullptr) != 0) fail(errno);
        struct sigaction action{};
        action.sa_handler = SIG_DFL;
        if (::sigemptyset(&action.sa_mask) != 0) fail(errno);
        for (int signal_number = 1; signal_number < NSIG; ++signal_number) {
          if (signal_number == SIGKILL || signal_number == SIGSTOP) continue;
          if (::sigaction(signal_number, &action, nullptr) != 0 &&
              errno != EINVAL) {
            fail(errno);
          }
        }

        if (::dup2(executable->get(), 3) < 0 || ::dup2(exec_error[1], 4) < 0 ||
            !set_close_on_exec(3, true) || !set_close_on_exec(4, true)) {
          fail(errno);
        }
        close_extra_descriptors(descriptor_limit);
        ::fexecve(3, arguments.data(), environment.data());
        auto failure = errno;
        if (failure == ENOENT) {
          if (!set_close_on_exec(3, false)) child_failure(4, errno);
          ::fexecve(3, arguments.data(), environment.data());
          failure = errno;
        }
        child_failure(4, failure);
      }

      spawned_process = process;
      static_cast<void>(::setpgid(process, process));
      static_cast<void>(::close(output[1]));
      output[1] = -1;
      static_cast<void>(::close(error[1]));
      error[1] = -1;
      static_cast<void>(::close(exec_error[1]));
      exec_error[1] = -1;
      auto stream = std::make_unique<LinuxProcessStream>(
          process, UniqueFd{std::exchange(output[0], -1)},
          UniqueFd{std::exchange(error[0], -1)},
          UniqueFd{std::exchange(exec_error[0], -1)}, request.limits);
      spawned_process = -1;
      return stream;
    } catch (...) {
      if (spawned_process > 0) {
        static_cast<void>(::kill(-spawned_process, SIGKILL));
        static_cast<void>(::kill(spawned_process, SIGKILL));
        int status{};
        while (::waitpid(spawned_process, &status, 0) < 0 && errno == EINTR) {
        }
      }
      close_pipes();
      return launch_failure(runtime::ProcessLaunchErrorCode::internal_failure,
                            runtime::ProcessLaunchStage::spawn,
                            "process launch failed internally");
    }
  }

  [[nodiscard]] auto checked_path(
      const std::string& path, const std::string_view expected_identity,
      const runtime::ProcessFilesystemTargetKind kind)
      -> std::expected<UniqueFd, runtime::ProcessLaunchError> {
    auto current = open_without_symlinks(path, kind);
    if (!current) {
      return launch_failure(runtime::ProcessLaunchErrorCode::contract_drift,
                            runtime::ProcessLaunchStage::validation,
                            "process path identity changed before spawn");
    }
    if (!descriptor_matches(current->get(), expected_identity)) {
      return launch_failure(runtime::ProcessLaunchErrorCode::contract_drift,
                            runtime::ProcessLaunchStage::validation,
                            "process path identity changed before spawn");
    }
    return current;
  }

  [[nodiscard]] auto checked_descendant(
      const int root_descriptor, const std::string_view root_path,
      const std::string_view path, const std::string_view expected_identity)
      -> std::expected<UniqueFd, runtime::ProcessLaunchError> {
    auto current =
        open_descendant_without_symlinks(root_descriptor, root_path, path);
    if (!current) return std::unexpected(std::move(current.error()));
    if (!descriptor_matches(current->get(), expected_identity)) {
      return launch_failure(runtime::ProcessLaunchErrorCode::contract_drift,
                            runtime::ProcessLaunchStage::validation,
                            "process descendant identity changed before spawn");
    }
    return current;
  }

  runtime::ProcessLaunchBounds m_bounds;
};

#endif

} // namespace

auto linux_process_unavailable_conjunct_name(
    const LinuxProcessUnavailableConjunct conjunct) noexcept
    -> std::string_view {
  switch (conjunct) {
    case LinuxProcessUnavailableConjunct::same_uid_broker_execution_confinement:
      return "same_uid_broker_execution_confinement";
  }
  return "invalid";
}

auto establish_linux_process_launcher(
    LinuxProcessLauncherConfiguration configuration) noexcept
    -> std::expected<LinuxProcessLauncherEstablishment,
                     LinuxProcessLauncherEstablishmentError> {
  try {
    const bool available =
        configuration.restriction == runtime::RestrictionLevel::none;
    auto context = make_context(configuration, available);
    if (!context) {
      return establishment_failure(
          LinuxProcessLauncherEstablishmentErrorCode::invalid_configuration,
          "Linux process launch context is invalid");
    }
    if (!available) {
      return LinuxProcessLauncherEstablishment{LinuxProcessLauncherUnavailable{
          std::move(*context), LinuxProcessUnavailableConjunct::
                                   same_uid_broker_execution_confinement}};
    }
    if (auto valid =
            runtime::validate_process_launch_bounds(configuration.bounds);
        !valid) {
      return establishment_failure(
          LinuxProcessLauncherEstablishmentErrorCode::invalid_configuration,
          "Linux process launcher bounds are invalid");
    }
#ifdef __linux__
    runtime::ProcessLauncherContract contract{
        runtime::RestrictionLevel::none, context->mechanism(),
        std::string{none_restriction_policy_identity}};
    auto launcher =
        std::make_shared<LinuxProcessLauncher>(configuration.bounds, contract);
    auto bound = runtime::bind_process_launcher(std::move(*context),
                                                std::move(launcher));
    if (!bound) {
      return establishment_failure(
          LinuxProcessLauncherEstablishmentErrorCode::binding_failed,
          "Linux process launcher could not be bound to its launch context");
    }
    return LinuxProcessLauncherEstablishment{std::move(*bound)};
#else
    return establishment_failure(
        LinuxProcessLauncherEstablishmentErrorCode::unsupported_platform,
        "native Linux process launching is unavailable on this platform");
#endif
  } catch (...) {
    return establishment_failure(
        LinuxProcessLauncherEstablishmentErrorCode::internal_failure,
        "Linux process launcher establishment failed internally");
  }
}

} // namespace aiforge::adapters
