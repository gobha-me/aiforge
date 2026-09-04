#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "runner_v2.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <linux/magic.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

namespace aiforge::evaluation::process_isolation::v2 {
namespace {

constexpr auto maximum_probe_timeout = std::chrono::seconds{60};

[[nodiscard]] auto runner_error(const RunnerErrorCode code, std::string message)
    -> std::unexpected<RunnerError> {
  return std::unexpected(RunnerError{code, std::move(message)});
}

[[nodiscard]] auto closed_record(const ProbeId id, const ReasonCode reason)
    -> ProbeRecord {
  return {id, ProbeState::probe_error, reason};
}

[[nodiscard]] auto unavailable_record(const ProbeId id, const ReasonCode reason)
    -> ProbeRecord {
  return {id, ProbeState::unavailable, reason};
}

[[nodiscard]] auto requires_delegated_cgroup(const ProbeId id) -> bool {
  switch (id) {
    case ProbeId::cgroup_v2_delegation:
    case ProbeId::cgroup_required_controllers:
    case ProbeId::cgroup_atomic_child_placement:
    case ProbeId::cgroup_self_migration_denial:
    case ProbeId::cgroup_whole_tree_enumeration:
    case ProbeId::cgroup_kill:
    case ProbeId::cgroup_populated_zero:
    case ProbeId::cgroup_setsid_containment:
    case ProbeId::cgroup_double_fork_containment:
    case ProbeId::cgroup_daemon_containment:
    case ProbeId::cgroup_clone_fork_fanout:
    case ProbeId::cgroup_leader_exit_containment:
    case ProbeId::cgroup_cancellation_cleanup:
    case ProbeId::cgroup_cpu_limit_enforcement:
    case ProbeId::cgroup_memory_limit_termination:
    case ProbeId::cgroup_pids_limit_enforcement:
    case ProbeId::combined_setup_order:
    case ProbeId::partial_setup_cleanup: return true;
    case ProbeId::landlock_read_confinement:
    case ProbeId::landlock_write_confinement:
    case ProbeId::landlock_execute_confinement:
    case ProbeId::seccomp_internet_socket_family_denial:
    case ProbeId::seccomp_unix_socket_denial:
    case ProbeId::private_root_construction:
    case ProbeId::private_mount_propagation:
    case ProbeId::descriptor_relative_launch:
    case ProbeId::staged_input_identity:
    case ProbeId::staged_output_identity: return false;
  }
  return false;
}

class Descriptor {
 public:
  explicit Descriptor(const int value = -1) : m_value(value) {}
  Descriptor(const Descriptor&) = delete;
  auto operator=(const Descriptor&) -> Descriptor& = delete;
  Descriptor(Descriptor&& other) noexcept : m_value(other.release()) {}
  auto operator=(Descriptor&& other) noexcept -> Descriptor& {
    if (this == &other) return *this;
    reset(other.release());
    return *this;
  }
  ~Descriptor() { reset(); }
  [[nodiscard]] auto get() const noexcept -> int { return m_value; }
  [[nodiscard]] auto release() noexcept -> int {
    const auto result = m_value;
    m_value = -1;
    return result;
  }
  auto reset(const int value = -1) noexcept -> void {
    if (m_value >= 0) static_cast<void>(::close(m_value));
    m_value = value;
  }

 private:
  int m_value{-1};
};

[[nodiscard]] auto write_all(const int descriptor, const std::string_view value)
    -> bool {
  std::size_t offset{};
  while (offset < value.size()) {
    const auto count =
        ::write(descriptor, value.data() + offset, value.size() - offset);
    if (count < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (count == 0) return false;
    offset += static_cast<std::size_t>(count);
  }
  return true;
}

[[nodiscard]] auto write_control(const int directory, const char* name,
                                 const std::string_view value) -> bool {
  const Descriptor descriptor{
      ::openat(directory, name, O_WRONLY | O_CLOEXEC | O_NOFOLLOW)};
  return descriptor.get() >= 0 && write_all(descriptor.get(), value);
}

[[nodiscard]] auto read_control(const int directory, const char* name)
    -> std::optional<std::string> {
  const Descriptor descriptor{
      ::openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
  if (descriptor.get() < 0) return std::nullopt;
  std::string result;
  std::array<char, 1024> buffer{};
  for (;;) {
    const auto count = ::read(descriptor.get(), buffer.data(), buffer.size());
    if (count == 0) return result;
    if (count < 0) {
      if (errno == EINTR) continue;
      return std::nullopt;
    }
    if (result.size() + static_cast<std::size_t>(count) > 65536)
      return std::nullopt;
    result.append(buffer.data(), static_cast<std::size_t>(count));
  }
}

[[nodiscard]] auto parse_tokens(const std::string_view document)
    -> std::optional<std::set<std::string>> {
  std::set<std::string> result;
  std::size_t offset{};
  while (offset < document.size()) {
    while (offset < document.size() &&
           (document[offset] == ' ' || document[offset] == '\n'))
      ++offset;
    if (offset == document.size()) break;
    const auto end = document.find_first_of(" \n", offset);
    const auto token = document.substr(offset, end == std::string_view::npos
                                                   ? document.size() - offset
                                                   : end - offset);
    if (token.empty() || token.size() > 128 ||
        !std::ranges::all_of(token, [](const unsigned char character) {
          return (character >= 'a' && character <= 'z') || character == '_';
        }))
      return std::nullopt;
    result.emplace(token);
    if (end == std::string_view::npos) break;
    offset = end + 1;
  }
  return result;
}

[[nodiscard]] auto parse_processes(const std::string_view document)
    -> std::optional<std::vector<pid_t>> {
  std::vector<pid_t> result;
  const char* cursor = document.data();
  const char* end = cursor + document.size();
  while (cursor != end) {
    while (cursor != end && (*cursor == ' ' || *cursor == '\n'))
      ++cursor;
    if (cursor == end) break;
    long value{};
    const auto parsed = std::from_chars(cursor, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr == cursor || value <= 0 ||
        value > INT_MAX)
      return std::nullopt;
    result.push_back(static_cast<pid_t>(value));
    cursor = parsed.ptr;
    if (cursor != end && *cursor != '\n') return std::nullopt;
  }
  std::ranges::sort(result);
  if (std::ranges::adjacent_find(result) != result.end()) return std::nullopt;
  return result;
}

[[nodiscard]] auto safe_delegated_root_path(const std::filesystem::path& path)
    -> bool {
  if (path.empty()) return true;
  const auto text = path.native();
  if (!path.is_absolute() || text.size() > 4096 || text == "/" ||
      text.back() == '/')
    return false;
  for (const auto& component : path) {
    const auto value = component.native();
    if (value == "/") continue;
    if (value.empty() || value == "." || value == ".." || value.size() > 255 ||
        !std::ranges::all_of(value, [](const unsigned char character) {
          return character >= 0x21U && character != 0x7fU;
        }))
      return false;
  }
  return true;
}

[[nodiscard]] auto open_pinned_directory(const std::filesystem::path& path)
    -> Descriptor {
  auto current =
      Descriptor{::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
  if (current.get() < 0) return current;
  for (const auto& component : path) {
    const auto value = component.native();
    if (value == "/") continue;
    Descriptor next{::openat(current.get(), value.c_str(),
                             O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    if (next.get() < 0) return Descriptor{};
    current = std::move(next);
  }
  return current;
}

class CgroupBootstrap final {
 public:
  CgroupBootstrap() = default;
  CgroupBootstrap(const CgroupBootstrap&) = delete;
  auto operator=(const CgroupBootstrap&) -> CgroupBootstrap& = delete;
  ~CgroupBootstrap() { static_cast<void>(cleanup()); }

  [[nodiscard]] auto start(const std::filesystem::path& path) -> ReasonCode {
    if (path.empty()) return ReasonCode::missing_delegation;
    m_root = open_pinned_directory(path);
    struct stat attributes{};
    struct statfs filesystem{};
    if (m_root.get() < 0 || ::fstat(m_root.get(), &attributes) != 0 ||
        ::fstatfs(m_root.get(), &filesystem) != 0 ||
        !S_ISDIR(attributes.st_mode) || attributes.st_uid != ::geteuid() ||
        filesystem.f_type != CGROUP2_SUPER_MAGIC)
      return ReasonCode::missing_delegation;
    const auto controllers = read_control(m_root.get(), "cgroup.controllers");
    const auto available =
        controllers ? parse_tokens(*controllers) : std::nullopt;
    if (!available) return ReasonCode::internal_error;
    for (const auto* required : {"cpu", "memory", "pids"}) {
      if (!available->contains(required)) return ReasonCode::missing_controller;
    }
    const auto processes_document = read_control(m_root.get(), "cgroup.procs");
    const auto processes = processes_document
                               ? parse_processes(*processes_document)
                               : std::nullopt;
    if (!processes) return ReasonCode::internal_error;
    if (processes->size() != 1 || processes->front() != ::getpid())
      return ReasonCode::missing_delegation;
    m_supervisor_name =
        "aiforge-evidence-v2-supervisor-" + std::to_string(::getpid());
    if (::mkdirat(m_root.get(), m_supervisor_name.c_str(), S_IRWXU) != 0)
      return errno == EACCES || errno == EPERM || errno == EROFS
                 ? ReasonCode::missing_delegation
                 : ReasonCode::internal_error;
    m_created = true;
    m_supervisor =
        Descriptor{::openat(m_root.get(), m_supervisor_name.c_str(),
                            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    if (m_supervisor.get() < 0) return fail(ReasonCode::internal_error);
    const auto own_pid = std::to_string(::getpid());
    if (!write_control(m_supervisor.get(), "cgroup.procs", own_pid))
      return fail(ReasonCode::missing_delegation);
    m_moved = true;
    const auto supervisor_document =
        read_control(m_supervisor.get(), "cgroup.procs");
    const auto supervisor_processes =
        supervisor_document ? parse_processes(*supervisor_document)
                            : std::nullopt;
    if (!supervisor_processes || supervisor_processes->size() != 1 ||
        supervisor_processes->front() != ::getpid())
      return fail(supervisor_processes ? ReasonCode::missing_delegation
                                       : ReasonCode::internal_error);
    const auto remaining_document = read_control(m_root.get(), "cgroup.procs");
    const auto remaining = remaining_document
                               ? parse_processes(*remaining_document)
                               : std::nullopt;
    if (!remaining || !remaining->empty())
      return fail(remaining ? ReasonCode::missing_delegation
                            : ReasonCode::internal_error);
    if (!write_control(m_root.get(), "cgroup.subtree_control",
                       "+cpu +memory +pids")) {
      const auto enable_error = errno;
      const auto partially_enabled =
          read_control(m_root.get(), "cgroup.subtree_control");
      const auto partial_tokens =
          partially_enabled ? parse_tokens(*partially_enabled) : std::nullopt;
      if (!partial_tokens) return fail(ReasonCode::internal_error);
      m_enabled = partial_tokens->contains("cpu") ||
                  partial_tokens->contains("memory") ||
                  partial_tokens->contains("pids");
      return fail(enable_error == EACCES || enable_error == EPERM ||
                          enable_error == EBUSY
                      ? ReasonCode::missing_delegation
                      : ReasonCode::internal_error);
    }
    m_enabled = true;
    const auto enabled = read_control(m_root.get(), "cgroup.subtree_control");
    const auto enabled_tokens = enabled ? parse_tokens(*enabled) : std::nullopt;
    if (!enabled_tokens || !enabled_tokens->contains("cpu") ||
        !enabled_tokens->contains("memory") ||
        !enabled_tokens->contains("pids"))
      return fail(enabled_tokens ? ReasonCode::missing_controller
                                 : ReasonCode::internal_error);
    m_ready = true;
    return ReasonCode::none;
  }

  [[nodiscard]] auto descriptor() const noexcept -> int {
    return m_ready ? m_root.get() : -1;
  }

  [[nodiscard]] auto cleanup() noexcept -> bool {
    if (m_cleaned) return m_cleanup_okay;
    try {
      bool okay{true};
      m_ready = false;
      if (m_enabled && !write_control(m_root.get(), "cgroup.subtree_control",
                                      "-cpu -memory -pids"))
        okay = false;
      m_enabled = false;
      if (m_moved && !write_control(m_root.get(), "cgroup.procs",
                                    std::to_string(::getpid())))
        okay = false;
      m_moved = false;
      if (m_supervisor.get() >= 0) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds{2};
        bool empty{};
        do {
          const auto events = read_control(m_supervisor.get(), "cgroup.events");
          if (events && events->find("populated 0") != std::string::npos) {
            empty = true;
            break;
          }
          static_cast<void>(::poll(nullptr, 0, 5));
        } while (std::chrono::steady_clock::now() < deadline);
        if (!empty) okay = false;
        m_supervisor.reset();
      }
      if (m_created && ::unlinkat(m_root.get(), m_supervisor_name.c_str(),
                                  AT_REMOVEDIR) != 0)
        okay = false;
      m_created = false;
      m_cleaned = true;
      m_cleanup_okay = okay;
      return okay;
    } catch (...) {
      m_ready = false;
      m_cleaned = true;
      m_cleanup_okay = false;
      return false;
    }
  }

 private:
  [[nodiscard]] auto fail(const ReasonCode reason) -> ReasonCode {
    return cleanup() ? reason : ReasonCode::cleanup_failed;
  }
  Descriptor m_root;
  Descriptor m_supervisor;
  std::string m_supervisor_name;
  bool m_created{};
  bool m_moved{};
  bool m_enabled{};
  bool m_ready{};
  bool m_cleaned{};
  bool m_cleanup_okay{true};
};

class SubreaperGuard {
 public:
  [[nodiscard]] static auto create() -> std::optional<SubreaperGuard> {
    int previous{};
    if (::prctl(PR_GET_CHILD_SUBREAPER, &previous) != 0 ||
        (previous == 0 && ::prctl(PR_SET_CHILD_SUBREAPER, 1) != 0)) {
      return std::nullopt;
    }
    return SubreaperGuard{previous};
  }
  SubreaperGuard(const SubreaperGuard&) = delete;
  auto operator=(const SubreaperGuard&) -> SubreaperGuard& = delete;
  SubreaperGuard(SubreaperGuard&& other) noexcept
      : m_previous(other.m_previous), m_active(other.m_active) {
    other.m_active = false;
  }
  ~SubreaperGuard() { static_cast<void>(restore()); }
  [[nodiscard]] auto restore() noexcept -> bool {
    if (!m_active) return true;
    if (m_previous == 0 && ::prctl(PR_SET_CHILD_SUBREAPER, 0) != 0)
      return false;
    m_active = false;
    return true;
  }

 private:
  explicit SubreaperGuard(const int previous) : m_previous(previous) {}
  int m_previous{};
  bool m_active{true};
};

[[nodiscard]] auto valid_source_sha(const std::string_view value) -> bool {
  return value.size() == 40 &&
         std::ranges::all_of(value, [](const unsigned char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

[[nodiscard]] auto safe_component(const std::string_view value) -> bool {
  return !value.empty() && value.size() <= maximum_platform_metadata_bytes &&
         std::ranges::all_of(value, [](const unsigned char character) {
           return (character >= 'a' && character <= 'z') ||
                  (character >= 'A' && character <= 'Z') ||
                  (character >= '0' && character <= '9') || character == '.' ||
                  character == '_' || character == '-' || character == '+';
         });
}

[[nodiscard]] auto safe_argument(const std::string_view value) -> bool {
  return value.size() <= 4096 && value.find('\0') == std::string_view::npos;
}

[[nodiscard]] auto platform_report(std::string source_sha)
    -> std::expected<EvidenceReport, RunnerError> {
  struct utsname identity{};
  if (::uname(&identity) != 0) {
    return runner_error(RunnerErrorCode::platform_metadata,
                        "platform metadata is unavailable");
  }
  EvidenceReport result{
      std::move(source_sha), "linux", identity.release, identity.machine, {}};
  if (!safe_component(result.platform) || !safe_component(result.kernel) ||
      !safe_component(result.architecture)) {
    return runner_error(RunnerErrorCode::platform_metadata,
                        "platform metadata is invalid");
  }
  return result;
}

[[nodiscard]] auto make_temporary_root(const std::filesystem::path& parent)
    -> std::optional<std::filesystem::path> {
  auto pattern = (parent / "aiforge-isolation-v2-XXXXXX").string();
  std::vector<char> writable(pattern.begin(), pattern.end());
  writable.push_back('\0');
  const auto* created = ::mkdtemp(writable.data());
  if (created == nullptr) return std::nullopt;
  return std::filesystem::path{created};
}

[[nodiscard]] auto close_unrelated_descriptors(
    const unsigned int first) noexcept -> bool {
#ifdef SYS_close_range
  if (::syscall(SYS_close_range, first, UINT_MAX, 0U) == 0) return true;
  if (errno != ENOSYS && errno != EINVAL) return false;
#endif
  const auto maximum = ::sysconf(_SC_OPEN_MAX);
  if (maximum <= 0 || maximum > INT_MAX) return false;
  for (int descriptor = static_cast<int>(first); descriptor < maximum;
       ++descriptor) {
    if (::close(descriptor) != 0 && errno != EBADF && errno != EINTR)
      return false;
  }
  return true;
}

[[nodiscard]] auto direct_descendants() -> std::optional<std::vector<pid_t>> {
  const auto path =
      "/proc/self/task/" + std::to_string(::getpid()) + "/children";
  const Descriptor descriptor{
      ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
  if (descriptor.get() < 0) return std::nullopt;
  std::string document;
  std::array<char, 1024> buffer{};
  for (;;) {
    const auto count = ::read(descriptor.get(), buffer.data(), buffer.size());
    if (count == 0) break;
    if (count < 0) {
      if (errno == EINTR) continue;
      return std::nullopt;
    }
    if (document.size() + static_cast<std::size_t>(count) > 65536)
      return std::nullopt;
    document.append(buffer.data(), static_cast<std::size_t>(count));
  }
  std::vector<pid_t> result;
  const char* cursor = document.data();
  const char* end = cursor + document.size();
  while (cursor != end) {
    while (cursor != end && *cursor == ' ')
      ++cursor;
    if (cursor == end) break;
    long value{};
    const auto parsed = std::from_chars(cursor, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr == cursor || value <= 0 ||
        value > INT_MAX) {
      return std::nullopt;
    }
    result.push_back(static_cast<pid_t>(value));
    cursor = parsed.ptr;
    if (cursor != end && *cursor != ' ') return std::nullopt;
  }
  return result;
}

struct CleanupResult {
  bool complete{};
  bool pidfd_verified{true};
  std::size_t observed{};
};

auto signal_pidfd(const pid_t process) -> bool {
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
  const Descriptor pidfd{
      static_cast<int>(::syscall(SYS_pidfd_open, process, 0U))};
  if (pidfd.get() < 0) return errno == ESRCH;
  return ::syscall(SYS_pidfd_send_signal, pidfd.get(), SIGKILL, nullptr, 0U) ==
             0 ||
         errno == ESRCH;
#else
  static_cast<void>(process);
  return false;
#endif
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Cleanup loop.
[[nodiscard]] auto cleanup_descendants() -> CleanupResult {
  CleanupResult result;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{2};
  for (;;) {
    int status{};
    for (;;) {
      const auto reaped = ::waitpid(-1, &status, WNOHANG);
      if (reaped > 0) {
        ++result.observed;
        continue;
      }
      if (reaped < 0 && errno != ECHILD && errno != EINTR) return result;
      break;
    }
    const auto children = direct_descendants();
    if (!children) return result;
    if (children->empty()) {
      result.complete = true;
      return result;
    }
    result.observed += children->size();
    for (const auto child : *children) {
      if (!signal_pidfd(child)) {
        result.pidfd_verified = false;
        static_cast<void>(::kill(child, SIGKILL));
      }
    }
    if (std::chrono::steady_clock::now() >= deadline) return result;
    static_cast<void>(::poll(nullptr, 0, 5));
  }
}

auto terminate_child(const pid_t child, const int pidfd) noexcept -> void {
  if (child <= 0) return;
  static_cast<void>(::kill(-child, SIGKILL));
#if defined(SYS_pidfd_send_signal)
  if (pidfd >= 0) {
    static_cast<void>(
        ::syscall(SYS_pidfd_send_signal, pidfd, SIGKILL, nullptr, 0U));
    return;
  }
#else
  static_cast<void>(pidfd);
#endif
  static_cast<void>(::kill(child, SIGKILL));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Child process.
[[nodiscard]] auto launch_probe(const ProbeId probe_id,
                                const std::filesystem::path& state_directory,
                                const RunnerOptions& options,
                                const int executable_descriptor,
                                const int delegated_root_descriptor,
                                const std::stop_token stop_token)
    -> ProbeRecord {
  int output_pipe[2]{};
  if (::pipe2(output_pipe, O_CLOEXEC | O_NONBLOCK) != 0)
    return closed_record(probe_id, ReasonCode::internal_error);
  const auto child = ::fork();
  if (child < 0) {
    static_cast<void>(::close(output_pipe[0]));
    static_cast<void>(::close(output_pipe[1]));
    return closed_record(probe_id, ReasonCode::internal_error);
  }
  if (child == 0) {
    static_cast<void>(::setpgid(0, 0));
    static_cast<void>(::close(STDIN_FILENO));
    if (::dup2(output_pipe[1], STDOUT_FILENO) < 0) ::_exit(126);
    const auto null_descriptor =
        ::open("/dev/null", O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (null_descriptor < 0 || ::dup2(null_descriptor, STDERR_FILENO) < 0)
      ::_exit(126);
    static_cast<void>(::close(output_pipe[0]));
    static_cast<void>(::close(output_pipe[1]));
    if (null_descriptor != STDERR_FILENO)
      static_cast<void>(::close(null_descriptor));
    std::vector<std::string> arguments;
    arguments.reserve(options.child_argument_prefix.size() + 4);
    arguments.push_back(options.child_executable.string());
    arguments.insert(arguments.end(), options.child_argument_prefix.begin(),
                     options.child_argument_prefix.end());
    arguments.emplace_back(probe_id_name(probe_id));
    arguments.push_back(state_directory.string());
    arguments.emplace_back(delegated_root_descriptor >= 0
                               ? "delegated-root-fd-4"
                               : "no-delegated-root");
    std::vector<char*> raw_arguments;
    raw_arguments.reserve(arguments.size() + 1);
    for (auto& argument : arguments)
      raw_arguments.push_back(argument.data());
    raw_arguments.push_back(nullptr);
    if (executable_descriptor != 3 &&
        ::dup3(executable_descriptor, 3, O_CLOEXEC) < 0)
      ::_exit(126);
    if (executable_descriptor == 3 &&
        ::fcntl(executable_descriptor, F_SETFD, FD_CLOEXEC) != 0)
      ::_exit(126);
    if (delegated_root_descriptor >= 0) {
      if (delegated_root_descriptor != 4 &&
          ::dup3(delegated_root_descriptor, 4, 0) < 0)
        ::_exit(126);
      if (delegated_root_descriptor == 4 &&
          ::fcntl(delegated_root_descriptor, F_SETFD, 0) != 0)
        ::_exit(126);
    }
    char* environment[]{nullptr};
    if (!close_unrelated_descriptors(delegated_root_descriptor >= 0 ? 5U : 4U))
      ::_exit(126);
    ::fexecve(3, raw_arguments.data(), environment);
    ::_exit(127);
  }

  static_cast<void>(::setpgid(child, child));
  static_cast<void>(::close(output_pipe[1]));
#if defined(SYS_pidfd_open)
  const Descriptor child_pidfd{
      static_cast<int>(::syscall(SYS_pidfd_open, child, 0U))};
#else
  const Descriptor child_pidfd;
#endif
  std::string output;
  output.reserve(
      std::min(options.maximum_child_output_bytes, maximum_child_record_bytes));
  bool output_exceeded{};
  bool read_failed{};
  bool timed_out{};
  bool cancelled{};
  bool pipe_closed{};
  bool child_reaped{};
  bool wait_failed{};
  int status{};
  const auto deadline =
      std::chrono::steady_clock::now() + options.child_timeout;
  while (!child_reaped || !pipe_closed) {
    std::array<char, 1024> buffer{};
    for (;;) {
      const auto count = ::read(output_pipe[0], buffer.data(), buffer.size());
      if (count > 0) {
        const auto amount = static_cast<std::size_t>(count);
        const auto retained =
            std::min(output.size(), options.maximum_child_output_bytes);
        if (amount > options.maximum_child_output_bytes - retained) {
          output_exceeded = true;
          terminate_child(child, child_pidfd.get());
          break;
        }
        output.append(buffer.data(), amount);
        continue;
      }
      if (count == 0) pipe_closed = true;
      if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
          errno != EINTR) {
        read_failed = true;
        terminate_child(child, child_pidfd.get());
      }
      break;
    }
    if (!child_reaped) {
      const auto waited = ::waitpid(child, &status, WNOHANG);
      if (waited == child) child_reaped = true;
      if (waited < 0 && errno != EINTR) {
        child_reaped = true;
        wait_failed = true;
      }
    }
    if (output_exceeded || read_failed) break;
    if (child_reaped && pipe_closed) break;
    if (stop_token.stop_requested()) {
      cancelled = true;
      terminate_child(child, child_pidfd.get());
      break;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      timed_out = true;
      terminate_child(child, child_pidfd.get());
      break;
    }
    pollfd descriptor{output_pipe[0], POLLIN | POLLHUP, 0};
    static_cast<void>(::poll(&descriptor, 1, 10));
  }
  static_cast<void>(::close(output_pipe[0]));
  if (!child_reaped) {
    pid_t waited{};
    do {
      waited = ::waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != child) wait_failed = true;
  }
  const auto cleanup = cleanup_descendants();
  if (!cleanup.complete || !cleanup.pidfd_verified || cleanup.observed != 0)
    return closed_record(probe_id, ReasonCode::cleanup_failed);
  if (wait_failed || child_pidfd.get() < 0)
    return closed_record(probe_id, ReasonCode::pid_reuse);
  if (output_exceeded) return closed_record(probe_id, ReasonCode::output_limit);
  if (read_failed) return closed_record(probe_id, ReasonCode::internal_error);
  if (cancelled) return closed_record(probe_id, ReasonCode::cancelled);
  if (timed_out) return closed_record(probe_id, ReasonCode::timeout);
  if (WIFSIGNALED(status)) return closed_record(probe_id, ReasonCode::signaled);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    return closed_record(probe_id, ReasonCode::nonzero_exit);
  auto parsed = parse_child_record(output);
  if (!parsed || parsed->probe_id != probe_id)
    return closed_record(probe_id, ReasonCode::malformed_protocol);
  return *parsed;
}

auto mark_cleanup_failure(EvidenceReport& report) -> void {
  for (auto& record : report.probes) {
    record.state = ProbeState::probe_error;
    record.reason = ReasonCode::cleanup_failed;
  }
}

} // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Evaluation flow.
auto run_evaluation(std::string source_sha, const RunnerOptions& options,
                    const std::stop_token stop_token)
    -> std::expected<EvidenceReport, RunnerError> {
  try {
    const auto executable = options.child_executable.native();
    const auto arguments_are_safe =
        std::ranges::all_of(options.child_argument_prefix, safe_argument);
    const Descriptor executable_descriptor{
        ::open(options.child_executable.c_str(), O_RDONLY | O_CLOEXEC)};
    struct stat executable_attributes{};
    const auto executable_is_regular =
        executable_descriptor.get() >= 0 &&
        ::fstat(executable_descriptor.get(), &executable_attributes) == 0 &&
        S_ISREG(executable_attributes.st_mode);
    if (!valid_source_sha(source_sha) ||
        !options.child_executable.is_absolute() || executable.empty() ||
        !safe_argument(executable) || !executable_is_regular ||
        options.child_timeout <= std::chrono::milliseconds::zero() ||
        options.child_timeout > maximum_probe_timeout ||
        options.maximum_child_output_bytes == 0 ||
        options.maximum_child_output_bytes > maximum_child_record_bytes ||
        options.child_argument_prefix.size() > 16 || !arguments_are_safe) {
      return runner_error(RunnerErrorCode::invalid_options,
                          "process-isolation v2 runner options are invalid");
    }
    if (!safe_delegated_root_path(options.delegated_cgroup_root)) {
      return runner_error(RunnerErrorCode::invalid_options,
                          "delegated cgroup root path is invalid");
    }
    const auto existing_children = direct_descendants();
    if (!existing_children || !existing_children->empty()) {
      return runner_error(
          RunnerErrorCode::internal_error,
          "process-isolation v2 runner requires no child processes");
    }
    auto subreaper = SubreaperGuard::create();
    if (!subreaper) {
      return runner_error(RunnerErrorCode::internal_error,
                          "process-isolation v2 cleanup cannot be established");
    }
    auto report = platform_report(std::move(source_sha));
    if (!report) return std::unexpected(std::move(report.error()));
    auto temporary_parent = options.temporary_parent;
    if (temporary_parent.empty()) {
      std::error_code error;
      temporary_parent = std::filesystem::temp_directory_path(error);
      if (error) temporary_parent.clear();
    }
    if (temporary_parent.empty() || !temporary_parent.is_absolute()) {
      return runner_error(RunnerErrorCode::invalid_options,
                          "temporary parent is unavailable");
    }
    CgroupBootstrap cgroup_bootstrap;
    const auto cgroup_reason =
        cgroup_bootstrap.start(options.delegated_cgroup_root);
    const auto root = make_temporary_root(temporary_parent);
    report->probes.reserve(required_probe_ids().size());
    if (!root) {
      for (const auto probe_id : required_probe_ids())
        report->probes.push_back(
            closed_record(probe_id, ReasonCode::internal_error));
      if (!cgroup_bootstrap.cleanup()) mark_cleanup_failure(*report);
      return std::move(*report);
    }
    for (const auto probe_id : required_probe_ids()) {
      const auto state = *root / std::string{probe_id_name(probe_id)};
      ProbeRecord record = closed_record(probe_id, ReasonCode::internal_error);
      if (stop_token.stop_requested()) {
        record = closed_record(probe_id, ReasonCode::cancelled);
      } else if (requires_delegated_cgroup(probe_id) &&
                 cgroup_reason != ReasonCode::none) {
        record = cgroup_reason == ReasonCode::cleanup_failed ||
                         cgroup_reason == ReasonCode::internal_error
                     ? closed_record(probe_id, cgroup_reason)
                     : unavailable_record(probe_id, cgroup_reason);
      } else if (::mkdir(state.c_str(), S_IRWXU) == 0 &&
                 ::chmod(state.c_str(), S_IRWXU) == 0) {
        record = launch_probe(
            probe_id, state, options, executable_descriptor.get(),
            requires_delegated_cgroup(probe_id) ? cgroup_bootstrap.descriptor()
                                                : -1,
            stop_token);
      }
      std::error_code cleanup_error;
      static_cast<void>(std::filesystem::remove_all(state, cleanup_error));
      std::error_code existence_error;
      if (cleanup_error || std::filesystem::exists(state, existence_error) ||
          existence_error) {
        record = closed_record(probe_id, ReasonCode::cleanup_failed);
      }
      report->probes.push_back(record);
    }
    if (!cgroup_bootstrap.cleanup()) mark_cleanup_failure(*report);
    std::error_code cleanup_error;
    static_cast<void>(std::filesystem::remove_all(*root, cleanup_error));
    std::error_code existence_error;
    if (cleanup_error || std::filesystem::exists(*root, existence_error) ||
        existence_error || !subreaper->restore()) {
      mark_cleanup_failure(*report);
    }
    return std::move(*report);
  } catch (...) {
    return runner_error(RunnerErrorCode::internal_error,
                        "process-isolation v2 evaluation failed internally");
  }
}

#if defined(AIFORGE_PROCESS_ISOLATION_TEST_SUPPORT)
namespace test_support {

auto bootstrap_failure_outcome(const BootstrapFailurePhase phase,
                               const bool rollback_complete) -> ProbeRecord {
  if (!rollback_complete)
    return closed_record(ProbeId::cgroup_v2_delegation,
                         ReasonCode::cleanup_failed);
  switch (phase) {
    case BootstrapFailurePhase::verify_controllers:
      return unavailable_record(ProbeId::cgroup_required_controllers,
                                ReasonCode::missing_controller);
    case BootstrapFailurePhase::disable_controllers:
    case BootstrapFailurePhase::move_to_root:
    case BootstrapFailurePhase::await_empty:
    case BootstrapFailurePhase::remove_supervisor:
      return closed_record(ProbeId::cgroup_v2_delegation,
                           ReasonCode::cleanup_failed);
    case BootstrapFailurePhase::pin_root:
    case BootstrapFailurePhase::verify_ownership:
    case BootstrapFailurePhase::create_supervisor:
    case BootstrapFailurePhase::move_to_supervisor:
    case BootstrapFailurePhase::verify_supervisor:
    case BootstrapFailurePhase::verify_root_empty:
    case BootstrapFailurePhase::enable_controllers:
    case BootstrapFailurePhase::verify_enabled_controllers:
      return unavailable_record(ProbeId::cgroup_v2_delegation,
                                ReasonCode::missing_delegation);
  }
  return closed_record(ProbeId::cgroup_v2_delegation,
                       ReasonCode::internal_error);
}

} // namespace test_support
#endif

} // namespace aiforge::evaluation::process_isolation::v2
