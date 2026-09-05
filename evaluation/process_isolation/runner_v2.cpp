#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "runner_v2.hpp"

#include "linux_support.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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
constexpr auto task_cgroup_prefix = std::string_view{"aiforge-evidence-v2-"};

using linux_support::CgroupBootstrap;
using linux_support::Descriptor;

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

[[nodiscard]] auto bootstrap_reason(const linux_support::BootstrapError error)
    -> ReasonCode {
  switch (error) {
    case linux_support::BootstrapError::none: return ReasonCode::none;
    case linux_support::BootstrapError::missing_delegation:
      return ReasonCode::missing_delegation;
    case linux_support::BootstrapError::missing_controller:
      return ReasonCode::missing_controller;
    case linux_support::BootstrapError::cleanup_failed:
      return ReasonCode::cleanup_failed;
    case linux_support::BootstrapError::internal_error:
      return ReasonCode::internal_error;
  }
  return ReasonCode::internal_error;
}

[[nodiscard]] auto cleanup_outcome(ProbeRecord record,
                                   const bool cleanup_complete) -> ProbeRecord {
  return cleanup_complete
             ? record
             : closed_record(record.probe_id, ReasonCode::cleanup_failed);
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
    case ProbeId::private_root_combined_setup_order:
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

[[nodiscard]] auto cleanup_temporary_root(const std::filesystem::path& root,
                                          const RunnerOptions& options) noexcept
    -> bool {
  std::error_code cleanup_error;
  static_cast<void>(std::filesystem::remove_all(root, cleanup_error));
  std::error_code existence_error;
  const bool removed = !cleanup_error &&
                       !std::filesystem::exists(root, existence_error) &&
                       !existence_error;
#if defined(AIFORGE_PROCESS_ISOLATION_TEST_SUPPORT)
  return removed && !options.force_temporary_root_cleanup_failure;
#else
  static_cast<void>(options);
  return removed;
#endif
}

[[nodiscard]] auto direct_descendants() -> std::optional<std::vector<pid_t>>;

[[nodiscard]] auto direct_descendants_for_run(const RunnerOptions& options)
    -> std::optional<std::vector<pid_t>> {
#if defined(AIFORGE_PROCESS_ISOLATION_TEST_SUPPORT)
  if (options.early_failure == EarlyRunnerFailure::descendant_scan)
    return std::nullopt;
#else
  static_cast<void>(options);
#endif
  return direct_descendants();
}

[[nodiscard]] auto subreaper_for_run(const RunnerOptions& options)
    -> std::optional<SubreaperGuard> {
#if defined(AIFORGE_PROCESS_ISOLATION_TEST_SUPPORT)
  if (options.early_failure == EarlyRunnerFailure::subreaper_setup)
    return std::nullopt;
#else
  static_cast<void>(options);
#endif
  return SubreaperGuard::create();
}

[[nodiscard]] auto read_bounded_descriptor(const int descriptor,
                                           const std::size_t maximum)
    -> std::optional<std::string> {
  std::string document;
  std::array<char, 1024> buffer{};
  for (;;) {
    const auto count = ::read(descriptor, buffer.data(), buffer.size());
    if (count == 0) return document;
    if (count < 0) {
      if (errno == EINTR) continue;
      return std::nullopt;
    }
    if (document.size() + static_cast<std::size_t>(count) > maximum)
      return std::nullopt;
    document.append(buffer.data(), static_cast<std::size_t>(count));
  }
}

[[nodiscard]] auto parse_space_separated_processes(
    const std::string_view document) -> std::optional<std::vector<pid_t>> {
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
        value > INT_MAX)
      return std::nullopt;
    result.push_back(static_cast<pid_t>(value));
    cursor = parsed.ptr;
    if (cursor != end && *cursor != ' ') return std::nullopt;
  }
  return result;
}

[[nodiscard]] auto direct_descendants() -> std::optional<std::vector<pid_t>> {
  const auto path =
      "/proc/self/task/" + std::to_string(::getpid()) + "/children";
  const Descriptor descriptor{
      ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
  if (descriptor.get() < 0) return std::nullopt;
  const auto document = read_bounded_descriptor(descriptor.get(), 65536);
  return document ? parse_space_separated_processes(*document) : std::nullopt;
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
                                CgroupBootstrap* cgroup_bootstrap,
                                const std::stop_token stop_token)
    -> ProbeRecord {
  const auto delegated_root_descriptor =
      cgroup_bootstrap != nullptr ? cgroup_bootstrap->descriptor() : -1;
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
    if (!linux_support::close_descriptors_from(
            delegated_root_descriptor >= 0 ? 5U : 4U))
      ::_exit(126);
    ::fexecve(3, raw_arguments.data(), environment);
    ::_exit(127);
  }

  static_cast<void>(::setpgid(child, child));
  if (cgroup_bootstrap != nullptr) cgroup_bootstrap->remember_task_owner(child);
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
  const auto cgroup_cleanup = cgroup_bootstrap == nullptr ||
                              cgroup_bootstrap->cleanup_task_owner(child);
  const bool interrupted = cancelled || timed_out;
  if (interrupted) {
    const auto reason = cancelled ? ReasonCode::cancelled : ReasonCode::timeout;
    return cleanup_outcome(closed_record(probe_id, reason),
                           cleanup.complete && cleanup.pidfd_verified &&
                               cgroup_cleanup && !wait_failed &&
                               child_pidfd.get() >= 0);
  }
  if (!cleanup.complete || !cleanup.pidfd_verified || !cgroup_cleanup ||
      cleanup.observed != 0)
    return closed_record(probe_id, ReasonCode::cleanup_failed);
  if (wait_failed || child_pidfd.get() < 0)
    return closed_record(probe_id, ReasonCode::pid_reuse);
  if (output_exceeded) return closed_record(probe_id, ReasonCode::output_limit);
  if (read_failed) return closed_record(probe_id, ReasonCode::internal_error);
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
  std::optional<SubreaperGuard> active_subreaper;
  std::optional<CgroupBootstrap> active_cgroup;
  std::optional<std::filesystem::path> active_root;
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
    if (!linux_support::safe_delegated_root_path(
            options.delegated_cgroup_root)) {
      return runner_error(RunnerErrorCode::invalid_options,
                          "delegated cgroup root path is invalid");
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
    const auto root = make_temporary_root(temporary_parent);
    report->probes.reserve(required_probe_ids().size());
    if (!root) {
      for (const auto probe_id : required_probe_ids())
        report->probes.push_back(
            closed_record(probe_id, ReasonCode::internal_error));
      return std::move(*report);
    }
    active_root = *root;
    const auto existing_children = direct_descendants_for_run(options);
    if (!existing_children || !existing_children->empty()) {
      const bool root_removed = cleanup_temporary_root(*active_root, options);
      active_root.reset();
      if (!root_removed) {
        return runner_error(
            RunnerErrorCode::internal_error,
            "process-isolation v2 temporary root cleanup failed");
      }
      return runner_error(
          RunnerErrorCode::internal_error,
          "process-isolation v2 runner requires no child processes");
    }
    auto subreaper = subreaper_for_run(options);
    if (!subreaper) {
      const bool root_removed = cleanup_temporary_root(*active_root, options);
      active_root.reset();
      if (!root_removed) {
        return runner_error(
            RunnerErrorCode::internal_error,
            "process-isolation v2 temporary root cleanup failed");
      }
      return runner_error(RunnerErrorCode::internal_error,
                          "process-isolation v2 cleanup cannot be established");
    }
    active_subreaper.emplace(std::move(*subreaper));
    active_cgroup.emplace(std::string{task_cgroup_prefix});
    const auto cgroup_reason =
        bootstrap_reason(active_cgroup->start(options.delegated_cgroup_root));
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
            requires_delegated_cgroup(probe_id) ? &*active_cgroup : nullptr,
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
    if (!active_cgroup->cleanup()) mark_cleanup_failure(*report);
    active_cgroup.reset();
    const bool root_removed = cleanup_temporary_root(*root, options);
    const bool subreaper_restored = active_subreaper->restore();
    if (!root_removed || !subreaper_restored) {
      mark_cleanup_failure(*report);
    }
    active_root.reset();
    active_subreaper.reset();
    return std::move(*report);
  } catch (...) {
    bool cleanup_complete{true};
    if (active_cgroup && !active_cgroup->cleanup()) cleanup_complete = false;
    if (active_root) {
      if (!cleanup_temporary_root(*active_root, options))
        cleanup_complete = false;
    }
    if (active_subreaper && !active_subreaper->restore())
      cleanup_complete = false;
    return runner_error(
        RunnerErrorCode::internal_error,
        cleanup_complete ? "process-isolation v2 evaluation failed internally"
                         : "process-isolation v2 evaluation cleanup failed");
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

auto cleanup_outcome(ProbeRecord record, const bool cleanup_complete)
    -> ProbeRecord {
  return ::aiforge::evaluation::process_isolation::v2::cleanup_outcome(
      record, cleanup_complete);
}

auto owns_task_cgroup(const int process, const std::string_view name) -> bool {
  return linux_support::task_cgroup_owned_by(static_cast<pid_t>(process),
                                             task_cgroup_prefix, name);
}

} // namespace test_support
#endif

} // namespace aiforge::evaluation::process_isolation::v2
