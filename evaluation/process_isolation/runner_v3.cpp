#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "runner_v3.hpp"

#include "linux_support.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

namespace aiforge::evaluation::process_isolation::v3 {
namespace {

constexpr auto maximum_probe_timeout = std::chrono::seconds{60};
constexpr auto task_cgroup_prefix = std::string_view{"aiforge-evidence-v3-"};

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

class SubreaperGuard final {
 public:
  [[nodiscard]] static auto create() -> std::optional<SubreaperGuard> {
    int previous{};
    if (::prctl(PR_GET_CHILD_SUBREAPER, &previous) != 0 ||
        (previous == 0 && ::prctl(PR_SET_CHILD_SUBREAPER, 1) != 0))
      return std::nullopt;
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
  if (::uname(&identity) != 0)
    return runner_error(RunnerErrorCode::platform_metadata,
                        "platform metadata is unavailable");
  EvidenceReport result{
      std::move(source_sha), "linux", identity.release, identity.machine, {}};
  if (!safe_component(result.platform) || !safe_component(result.kernel) ||
      !safe_component(result.architecture))
    return runner_error(RunnerErrorCode::platform_metadata,
                        "platform metadata is invalid");
  return result;
}

[[nodiscard]] auto make_temporary_root(const std::filesystem::path& parent)
    -> std::optional<std::filesystem::path> {
  auto pattern = (parent / "aiforge-isolation-v3-XXXXXX").string();
  std::vector<char> writable(pattern.begin(), pattern.end());
  writable.push_back('\0');
  const auto* created = ::mkdtemp(writable.data());
  return created == nullptr ? std::nullopt
                            : std::optional{std::filesystem::path{created}};
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

[[nodiscard]] auto direct_descendants() -> std::optional<std::vector<pid_t>> {
  const auto path =
      "/proc/self/task/" + std::to_string(::getpid()) + "/children";
  const Descriptor descriptor{
      ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
  if (descriptor.get() < 0) return std::nullopt;
  const auto document = read_bounded_descriptor(descriptor.get(), 65536);
  if (!document) return std::nullopt;
  std::vector<pid_t> result;
  const char* cursor = document->data();
  const char* end = cursor + document->size();
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

// NOLINTBEGIN(readability-function-cognitive-complexity) -- Bounded reap loop.
[[nodiscard]] auto cleanup_descendants() -> bool {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{2};
  for (;;) {
    int status{};
    for (;;) {
      const auto reaped = ::waitpid(-1, &status, WNOHANG);
      if (reaped > 0) continue;
      if (reaped < 0 && errno != ECHILD && errno != EINTR) return false;
      break;
    }
    const auto children = direct_descendants();
    if (!children) return false;
    if (children->empty()) return true;
    for (const auto child : *children) {
      auto identity = linux_support::pidfd_open(child);
      if (identity.get() < 0 || !linux_support::pidfd_kill(identity.get()))
        return false;
    }
    if (std::chrono::steady_clock::now() >= deadline) return false;
    static_cast<void>(::poll(nullptr, 0, 5));
  }
}
// NOLINTEND(readability-function-cognitive-complexity)

auto terminate_child(const pid_t child, const int pidfd) noexcept -> void {
  if (child <= 0) return;
  static_cast<void>(::kill(-child, SIGKILL));
  if (pidfd >= 0) static_cast<void>(linux_support::pidfd_kill(pidfd));
  static_cast<void>(::kill(child, SIGKILL));
}

// NOLINTBEGIN(readability-function-cognitive-complexity) -- Bounded child IO.
[[nodiscard]] auto launch_probe(const ProbeId probe_id,
                                const std::filesystem::path& state_directory,
                                const RunnerOptions& options,
                                const int executable_descriptor,
                                CgroupBootstrap* const cgroup,
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
    arguments.emplace_back(cgroup != nullptr ? "delegated-root-fd-4"
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
    if (cgroup != nullptr) {
      if (cgroup->descriptor() != 4 && ::dup3(cgroup->descriptor(), 4, 0) < 0)
        ::_exit(126);
      if (cgroup->descriptor() == 4 &&
          ::fcntl(cgroup->descriptor(), F_SETFD, 0) != 0)
        ::_exit(126);
    }
    char* environment[]{nullptr};
    if (!linux_support::close_descriptors_from(5)) ::_exit(126);
    ::fexecve(3, raw_arguments.data(), environment);
    ::_exit(127);
  }

  static_cast<void>(::setpgid(child, child));
  if (cgroup != nullptr) cgroup->remember_task_owner(child);
  static_cast<void>(::close(output_pipe[1]));
  auto child_pidfd = linux_support::pidfd_open(child);
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
        if (amount > options.maximum_child_output_bytes - output.size()) {
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
    if (output_exceeded || read_failed || (child_reaped && pipe_closed)) break;
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
  const bool descendants_cleaned = cleanup_descendants();
  const bool cgroup_cleaned =
      cgroup == nullptr || cgroup->cleanup_task_owner(child);
  if (!descendants_cleaned || !cgroup_cleaned)
    return closed_record(probe_id, ReasonCode::cleanup_failed);
  if (cancelled || timed_out) {
    return cleanup_outcome(closed_record(probe_id, cancelled
                                                       ? ReasonCode::cancelled
                                                       : ReasonCode::timeout),
                           !wait_failed && child_pidfd.get() >= 0);
  }
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
// NOLINTEND(readability-function-cognitive-complexity)

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
    const bool executable_is_regular =
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
        options.child_argument_prefix.size() > 16 || !arguments_are_safe ||
        !linux_support::safe_delegated_root_path(options.delegated_cgroup_root))
      return runner_error(RunnerErrorCode::invalid_options,
                          "process-isolation v3 runner options are invalid");
    auto report = platform_report(std::move(source_sha));
    if (!report) return std::unexpected(std::move(report.error()));
    auto temporary_parent = options.temporary_parent;
    if (temporary_parent.empty()) {
      std::error_code error;
      temporary_parent = std::filesystem::temp_directory_path(error);
      if (error) temporary_parent.clear();
    }
    if (temporary_parent.empty() || !temporary_parent.is_absolute())
      return runner_error(RunnerErrorCode::invalid_options,
                          "temporary parent is unavailable");
    const auto root = make_temporary_root(temporary_parent);
    report->probes.reserve(required_probe_ids().size());
    if (!root) {
      for (const auto id : required_probe_ids())
        report->probes.push_back(closed_record(id, ReasonCode::internal_error));
      return std::move(*report);
    }
    active_root = *root;
    const auto existing_children = direct_descendants();
    if (!existing_children || !existing_children->empty()) {
      const bool root_removed = cleanup_temporary_root(*active_root, options);
      active_root.reset();
      if (!root_removed)
        return runner_error(
            RunnerErrorCode::internal_error,
            "process-isolation v3 temporary root cleanup failed");
      return runner_error(
          RunnerErrorCode::internal_error,
          "process-isolation v3 runner requires no child processes");
    }
    auto subreaper = SubreaperGuard::create();
    if (!subreaper) {
      const bool root_removed = cleanup_temporary_root(*active_root, options);
      active_root.reset();
      if (!root_removed)
        return runner_error(
            RunnerErrorCode::internal_error,
            "process-isolation v3 temporary root cleanup failed");
      return runner_error(RunnerErrorCode::internal_error,
                          "process-isolation v3 cleanup cannot be established");
    }
    active_subreaper.emplace(std::move(*subreaper));
    active_cgroup.emplace(std::string{task_cgroup_prefix});
    const auto cgroup_reason =
        bootstrap_reason(active_cgroup->start(options.delegated_cgroup_root));
    for (const auto id : required_probe_ids()) {
      ProbeRecord record{id, ProbeState::unavailable,
                         ReasonCode::prerequisite_unavailable};
      if (id == ProbeId::direct_process_tree_cgroup_nonescape ||
          id == ProbeId::low_capability_nonescalation ||
          id == ProbeId::private_root_capability_discard) {
        if (stop_token.stop_requested()) {
          record = closed_record(id, ReasonCode::cancelled);
        } else if (id == ProbeId::direct_process_tree_cgroup_nonescape &&
                   cgroup_reason != ReasonCode::none) {
          record = cgroup_reason == ReasonCode::cleanup_failed ||
                           cgroup_reason == ReasonCode::internal_error
                       ? closed_record(id, cgroup_reason)
                       : unavailable_record(id, cgroup_reason);
        } else {
          const auto state = *root / std::string{probe_id_name(id)};
          if (::mkdir(state.c_str(), S_IRWXU) == 0 &&
              ::chmod(state.c_str(), S_IRWXU) == 0) {
            record =
                launch_probe(id, state, options, executable_descriptor.get(),
                             id == ProbeId::direct_process_tree_cgroup_nonescape
                                 ? &*active_cgroup
                                 : nullptr,
                             stop_token);
          } else {
            record = closed_record(id, ReasonCode::internal_error);
          }
          std::error_code cleanup_error;
          static_cast<void>(std::filesystem::remove_all(state, cleanup_error));
          std::error_code existence_error;
          if (cleanup_error ||
              std::filesystem::exists(state, existence_error) ||
              existence_error)
            record = closed_record(id, ReasonCode::cleanup_failed);
        }
      }
      report->probes.push_back(record);
    }
    if (!active_cgroup->cleanup()) mark_cleanup_failure(*report);
    active_cgroup.reset();
    const bool root_removed = cleanup_temporary_root(*root, options);
    const bool subreaper_restored = active_subreaper->restore();
    if (!root_removed || !subreaper_restored) mark_cleanup_failure(*report);
    active_root.reset();
    active_subreaper.reset();
    return std::move(*report);
  } catch (...) {
    bool cleanup_complete{true};
    if (active_cgroup && !active_cgroup->cleanup()) cleanup_complete = false;
    if (active_root && !cleanup_temporary_root(*active_root, options))
      cleanup_complete = false;
    if (active_subreaper && !active_subreaper->restore())
      cleanup_complete = false;
    return runner_error(
        RunnerErrorCode::internal_error,
        cleanup_complete ? "process-isolation v3 evaluation failed internally"
                         : "process-isolation v3 evaluation cleanup failed");
  }
}

#if defined(AIFORGE_PROCESS_ISOLATION_TEST_SUPPORT)
namespace test_support {

auto cleanup_outcome(ProbeRecord record, const bool cleanup_complete)
    -> ProbeRecord {
  return ::aiforge::evaluation::process_isolation::v3::cleanup_outcome(
      record, cleanup_complete);
}

} // namespace test_support
#endif

} // namespace aiforge::evaluation::process_isolation::v3
