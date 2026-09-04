#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "runner.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <cstring>
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
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

namespace aiforge::evaluation::process_isolation {
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

[[nodiscard]] auto safe_component(const std::string_view value) -> bool {
  return !value.empty() && value.size() <= maximum_platform_metadata_bytes &&
         std::ranges::all_of(value, [](const unsigned char character) {
           return (character >= 'a' && character <= 'z') ||
                  (character >= 'A' && character <= 'Z') ||
                  (character >= '0' && character <= '9') || character == '.' ||
                  character == '_' || character == '-' || character == '+';
         });
}

[[nodiscard]] auto valid_source_sha(const std::string_view value) -> bool {
  return value.size() == 40 &&
         std::ranges::all_of(value, [](const unsigned char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
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
  EvidenceReport report{
      std::move(source_sha), "linux", identity.release, identity.machine, {}};
  if (!safe_component(report.platform) || !safe_component(report.kernel) ||
      !safe_component(report.architecture)) {
    return runner_error(RunnerErrorCode::platform_metadata,
                        "platform metadata is invalid");
  }
  return report;
}

[[nodiscard]] auto make_temporary_root(const std::filesystem::path& parent)
    -> std::optional<std::filesystem::path> {
  auto pattern = (parent / "aiforge-isolation-evidence-XXXXXX").string();
  std::vector<char> writable(pattern.begin(), pattern.end());
  writable.push_back('\0');
  const auto* created = ::mkdtemp(writable.data());
  if (created == nullptr) return std::nullopt;
  return std::filesystem::path{created};
}

class Descriptor {
 public:
  explicit Descriptor(const int value) : m_value{value} {}
  Descriptor(const Descriptor&) = delete;
  auto operator=(const Descriptor&) -> Descriptor& = delete;
  ~Descriptor() {
    if (m_value >= 0) static_cast<void>(::close(m_value));
  }
  [[nodiscard]] auto get() const noexcept -> int { return m_value; }

 private:
  int m_value{-1};
};

class SubreaperGuard {
 public:
  [[nodiscard]] static auto create() -> std::optional<SubreaperGuard> {
    int original{};
    if (::prctl(PR_GET_CHILD_SUBREAPER, &original) != 0) return std::nullopt;
    if (original == 0 && ::prctl(PR_SET_CHILD_SUBREAPER, 1) != 0)
      return std::nullopt;
    return SubreaperGuard{original};
  }
  SubreaperGuard(const SubreaperGuard&) = delete;
  auto operator=(const SubreaperGuard&) -> SubreaperGuard& = delete;
  SubreaperGuard(SubreaperGuard&& other) noexcept
      : m_original{other.m_original}, m_active{other.m_active} {
    other.m_active = false;
  }
  ~SubreaperGuard() { static_cast<void>(restore()); }
  [[nodiscard]] auto restore() noexcept -> bool {
    if (!m_active) return true;
    if (m_original == 0 && ::prctl(PR_SET_CHILD_SUBREAPER, 0) != 0)
      return false;
    m_active = false;
    return true;
  }

 private:
  explicit SubreaperGuard(const int original) : m_original{original} {}
  int m_original{};
  bool m_active{true};
};

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

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Proc parsing.
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
  std::vector<pid_t> children;
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
    children.push_back(static_cast<pid_t>(value));
    cursor = parsed.ptr;
    if (cursor != end && *cursor != ' ') return std::nullopt;
  }
  return children;
}

struct DescendantCleanup {
  bool succeeded{};
  std::size_t observed{};
};

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Cleanup loop.
[[nodiscard]] auto cleanup_descendants() -> DescendantCleanup {
  DescendantCleanup result;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{1};
  bool exceeded_deadline{};
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
      result.succeeded = !exceeded_deadline;
      return result;
    }
    result.observed += children->size();
    for (const auto child : *children) {
      if (::kill(child, SIGKILL) != 0 && errno != ESRCH) return result;
    }
    if (std::chrono::steady_clock::now() >= deadline) exceeded_deadline = true;
    static_cast<void>(::poll(nullptr, 0, 5));
  }
}

auto terminate_child(const pid_t child) noexcept -> void {
  if (child <= 0) return;
  static_cast<void>(::kill(-child, SIGKILL));
  static_cast<void>(::kill(child, SIGKILL));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Child process.
[[nodiscard]] auto launch_probe(const ProbeId probe_id,
                                const std::filesystem::path& state_directory,
                                const RunnerOptions& options,
                                const int executable_descriptor)
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
    arguments.reserve(options.child_argument_prefix.size() + 3);
    arguments.push_back(options.child_executable.string());
    arguments.insert(arguments.end(), options.child_argument_prefix.begin(),
                     options.child_argument_prefix.end());
    arguments.emplace_back(probe_id_name(probe_id));
    arguments.push_back(state_directory.string());
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
    char* environment[]{nullptr};
    if (!close_unrelated_descriptors(4)) ::_exit(126);
    ::fexecve(3, raw_arguments.data(), environment);
    ::_exit(127);
  }

  static_cast<void>(::setpgid(child, child));
  static_cast<void>(::close(output_pipe[1]));
  std::string output;
  output.reserve(
      std::min(options.maximum_child_output_bytes, maximum_child_record_bytes));
  bool output_exceeded{};
  bool pipe_read_failed{};
  bool timed_out{};
  bool pipe_closed{};
  bool child_reaped{};
  bool wait_failed{};
  int status{};
  const auto timeout = probe_id == ProbeId::rlimit_cpu
                           ? options.cpu_limit_probe_timeout
                           : options.child_timeout;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!child_reaped || !pipe_closed) {
    std::array<char, 1024> buffer{};
    for (;;) {
      const auto count = ::read(output_pipe[0], buffer.data(), buffer.size());
      if (count > 0) {
        const auto amount = static_cast<std::size_t>(count);
        if (amount >
            options.maximum_child_output_bytes -
                std::min(output.size(), options.maximum_child_output_bytes)) {
          output_exceeded = true;
          terminate_child(child);
          break;
        }
        output.append(buffer.data(), amount);
        continue;
      }
      if (count == 0) pipe_closed = true;
      if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
          errno != EINTR) {
        terminate_child(child);
        pipe_read_failed = true;
      }
      break;
    }
    if (!child_reaped) {
      const auto waited = ::waitpid(child, &status, WNOHANG);
      if (waited == child) child_reaped = true;
      if (waited < 0 && errno != EINTR) {
        terminate_child(child);
        child_reaped = true;
        wait_failed = true;
      }
    }
    if (output_exceeded || pipe_read_failed) break;
    if (child_reaped && pipe_closed) break;
    if (std::chrono::steady_clock::now() >= deadline) {
      timed_out = true;
      terminate_child(child);
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
  const auto descendant_cleanup = cleanup_descendants();
  if (!descendant_cleanup.succeeded)
    return closed_record(probe_id, ReasonCode::cleanup_failed);
  if (wait_failed) return closed_record(probe_id, ReasonCode::internal_error);
  if (output_exceeded) return closed_record(probe_id, ReasonCode::output_limit);
  if (pipe_read_failed)
    return closed_record(probe_id, ReasonCode::internal_error);
  if (timed_out) return closed_record(probe_id, ReasonCode::timeout);
  if (WIFSIGNALED(status)) return closed_record(probe_id, ReasonCode::signaled);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    return closed_record(probe_id, ReasonCode::nonzero_exit);
  auto parsed = parse_child_record(output);
  if (!parsed || parsed->probe_id != probe_id)
    return closed_record(probe_id, ReasonCode::malformed_protocol);
  if (descendant_cleanup.observed != 0)
    return closed_record(probe_id, ReasonCode::cleanup_failed);
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
auto run_evaluation(std::string source_sha, const RunnerOptions& options)
    -> std::expected<EvidenceReport, RunnerError> {
  try {
    const auto executable = options.child_executable.native();
    const auto arguments_are_safe =
        std::ranges::all_of(options.child_argument_prefix, safe_argument);
    const Descriptor executable_descriptor{
        ::open(options.child_executable.c_str(), O_RDONLY | O_CLOEXEC)};
    struct stat executable_attributes{};
    const auto executable_is_runnable =
        executable_descriptor.get() >= 0 &&
        ::fstat(executable_descriptor.get(), &executable_attributes) == 0 &&
        S_ISREG(executable_attributes.st_mode);
    if (!valid_source_sha(source_sha) ||
        !options.child_executable.is_absolute() || !safe_argument(executable) ||
        executable.empty() || !executable_is_runnable ||
        options.child_timeout <= std::chrono::milliseconds::zero() ||
        options.child_timeout > maximum_probe_timeout ||
        options.cpu_limit_probe_timeout <= std::chrono::milliseconds::zero() ||
        options.cpu_limit_probe_timeout > maximum_probe_timeout ||
        options.maximum_child_output_bytes == 0 ||
        options.maximum_child_output_bytes > maximum_child_record_bytes ||
        options.child_argument_prefix.size() > 16 || !arguments_are_safe) {
      return runner_error(RunnerErrorCode::invalid_options,
                          "process-isolation runner options are invalid");
    }
    const auto existing_children = direct_descendants();
    if (!existing_children || !existing_children->empty()) {
      return runner_error(
          RunnerErrorCode::internal_error,
          "process-isolation runner requires no child processes");
    }
    auto subreaper = SubreaperGuard::create();
    if (!subreaper) {
      return runner_error(RunnerErrorCode::internal_error,
                          "process-isolation cleanup cannot be established");
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

    for (const auto probe_id : required_probe_ids()) {
      const auto state = *root / std::string{probe_id_name(probe_id)};
      if (::mkdir(state.c_str(), S_IRWXU) != 0 ||
          ::chmod(state.c_str(), S_IRWXU) != 0) {
        report->probes.push_back(
            closed_record(probe_id, ReasonCode::internal_error));
      } else {
        auto record =
            launch_probe(probe_id, state, options, executable_descriptor.get());
        std::error_code cleanup_error;
        static_cast<void>(std::filesystem::remove_all(state, cleanup_error));
        std::error_code existence_error;
        const bool state_remains =
            std::filesystem::exists(state, existence_error);
        if (cleanup_error || existence_error || state_remains) {
          record = closed_record(probe_id, ReasonCode::cleanup_failed);
        }
        report->probes.push_back(record);
      }
    }
    std::error_code cleanup_error;
    static_cast<void>(std::filesystem::remove_all(*root, cleanup_error));
    std::error_code existence_error;
    const bool root_remains = std::filesystem::exists(*root, existence_error);
    if (cleanup_error || existence_error || root_remains)
      mark_cleanup_failure(*report);
    if (!subreaper->restore()) mark_cleanup_failure(*report);
    return std::move(*report);
  } catch (...) {
    return runner_error(RunnerErrorCode::internal_error,
                        "process-isolation evaluation failed internally");
  }
}

} // namespace aiforge::evaluation::process_isolation
