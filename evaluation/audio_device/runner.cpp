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
#include <filesystem>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

namespace aiforge::evaluation::audio_device {
namespace {

constexpr auto maximum_probe_timeout = std::chrono::seconds{5};
constexpr std::size_t maximum_child_arguments = 16;
constexpr std::size_t maximum_child_argument_bytes = maximum_child_report_bytes;

enum class ChildKind { contract, rtaudio, miniaudio };
using ChildReport = std::variant<ContractReport, CandidateReport>;

[[nodiscard]] auto runner_error(const RunnerErrorCode code, std::string message)
    -> std::unexpected<RunnerError> {
  return std::unexpected(RunnerError{code, std::move(message)});
}

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
  return value.size() <= maximum_child_argument_bytes &&
         value.find('\0') == std::string_view::npos;
}

[[nodiscard]] auto valid_command(const ChildCommand& command) -> bool {
  const auto executable = command.executable.native();
  return command.executable.is_absolute() && !executable.empty() &&
         safe_argument(executable) &&
         command.argument_prefix.size() <= maximum_child_arguments &&
         std::ranges::all_of(command.argument_prefix, safe_argument);
}

[[nodiscard]] auto platform_report(std::string source_sha)
    -> std::expected<EvidenceReport, RunnerError> {
  struct utsname identity{};
  if (::uname(&identity) != 0)
    return runner_error(RunnerErrorCode::platform_metadata,
                        "audio-device platform metadata is unavailable");
  EvidenceReport report{
      std::move(source_sha), "linux", identity.machine, {}, {}};
  if (!safe_component(report.platform) || !safe_component(report.architecture))
    return runner_error(RunnerErrorCode::platform_metadata,
                        "audio-device platform metadata is invalid");
  return report;
}

class Descriptor {
 public:
  explicit Descriptor(const int value) : m_value{value} {}
  Descriptor(const Descriptor&) = delete;
  auto operator=(const Descriptor&) -> Descriptor& = delete;
  Descriptor(Descriptor&& other) noexcept : m_value{other.m_value} {
    other.m_value = -1;
  }
  auto operator=(Descriptor&&) -> Descriptor& = delete;
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

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Bounded parser.
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
    if (document.size() + static_cast<std::size_t>(count) >
        maximum_report_bytes)
      return std::nullopt;
    document.append(buffer.data(), static_cast<std::size_t>(count));
  }
  std::vector<pid_t> children;
  const char* cursor = document.data();
  const char* const end = cursor + document.size();
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

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Bounded cleanup.
[[nodiscard]] auto cleanup_descendants() noexcept -> DescendantCleanup {
  DescendantCleanup result;
  try {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{1};
    for (;;) {
      int status{};
      for (;;) {
        const auto reaped = ::waitpid(-1, &status, WNOHANG);
        if (reaped > 0) {
          ++result.observed;
          continue;
        }
        if (reaped < 0 && errno == EINTR) continue;
        if (reaped < 0 && errno != ECHILD) return result;
        break;
      }
      const auto children = direct_descendants();
      if (!children) return result;
      if (children->empty()) {
        result.succeeded = true;
        return result;
      }
      result.observed += children->size();
      for (const auto child : *children) {
        if (::kill(child, SIGKILL) != 0 && errno != ESRCH) return result;
      }
      if (std::chrono::steady_clock::now() >= deadline) return result;
      static_cast<void>(::poll(nullptr, 0, 5));
    }
  } catch (...) {
    return result;
  }
}

auto terminate_child(const pid_t child) noexcept -> void {
  if (child <= 0) return;
  static_cast<void>(::kill(-child, SIGKILL));
  static_cast<void>(::kill(child, SIGKILL));
}

[[nodiscard]] auto bounded_reap(const pid_t child, int& status) noexcept
    -> bool {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{1};
  while (std::chrono::steady_clock::now() < deadline) {
    const auto waited = ::waitpid(child, &status, WNOHANG);
    if (waited == child) return true;
    if (waited < 0 && errno == ECHILD) return true;
    if (waited < 0 && errno != EINTR) return false;
    static_cast<void>(::poll(nullptr, 0, 5));
  }
  return false;
}

class ChildCleanupGuard {
 public:
  ChildCleanupGuard(const pid_t child, const int output_descriptor) noexcept
      : m_child{child}, m_output_descriptor{output_descriptor} {}
  ChildCleanupGuard(const ChildCleanupGuard&) = delete;
  auto operator=(const ChildCleanupGuard&) -> ChildCleanupGuard& = delete;
  ~ChildCleanupGuard() {
    if (m_active) static_cast<void>(cleanup());
  }

  void close_output() noexcept {
    if (m_output_descriptor < 0) return;
    static_cast<void>(::close(m_output_descriptor));
    m_output_descriptor = -1;
  }
  void mark_reaped() noexcept { m_reaped = true; }
  [[nodiscard]] auto cleanup() noexcept -> bool {
    close_output();
    if (!m_reaped) {
      terminate_child(m_child);
      int status{};
      m_reaped = bounded_reap(m_child, status);
    }
    const auto descendants = cleanup_descendants();
    m_active = false;
    return m_reaped && descendants.succeeded && descendants.observed == 0;
  }
  void release() noexcept { m_active = false; }

 private:
  pid_t m_child{};
  int m_output_descriptor{-1};
  bool m_reaped{};
  bool m_active{true};
};

[[nodiscard]] auto error_record(const ProbeKey key, const ReasonCode reason,
                                const bool cleanup_complete = true)
    -> ProbeRecord {
  return {
      key.probe_id, key.direction,   ProbeState::probe_error, reason, 0, 0, 0,
      false,        cleanup_complete};
}

[[nodiscard]] auto failed_contract(const ReasonCode reason,
                                   const bool cleanup_complete = true)
    -> ContractReport {
  ContractReport result;
  result.probes.reserve(required_contract_probe_keys().size());
  for (const auto key : required_contract_probe_keys())
    result.probes.push_back(error_record(key, reason, cleanup_complete));
  return result;
}

[[nodiscard]] auto failed_candidate(const CandidateId candidate,
                                    const ReasonCode reason,
                                    const bool cleanup_complete = true)
    -> CandidateReport {
  CandidateReport result;
  result.candidate_id = candidate;
  result.candidate_version = std::string{candidate_version(candidate)};
  result.runtime_backend = candidate == CandidateId::rtaudio
                               ? RuntimeBackend::dummy
                               : RuntimeBackend::null_backend;
  result.probes.reserve(required_candidate_probe_keys().size());
  for (const auto key : required_candidate_probe_keys())
    result.probes.push_back(error_record(key, reason, cleanup_complete));
  return result;
}

[[nodiscard]] auto failed_child(const ChildKind kind, const ReasonCode reason,
                                const bool cleanup_complete = true)
    -> ChildReport {
  switch (kind) {
    case ChildKind::contract: return failed_contract(reason, cleanup_complete);
    case ChildKind::rtaudio:
      return failed_candidate(CandidateId::rtaudio, reason, cleanup_complete);
    case ChildKind::miniaudio:
      return failed_candidate(CandidateId::miniaudio, reason, cleanup_complete);
  }
  return failed_contract(ReasonCode::internal_error);
}

[[nodiscard]] auto parse_child(const ChildKind kind,
                               const std::string_view output)
    -> std::optional<ChildReport> {
  if (kind == ChildKind::contract) {
    auto parsed = parse_contract_report(output);
    if (!parsed) return std::nullopt;
    return ChildReport{std::move(*parsed)};
  }
  auto parsed = parse_candidate_report(output);
  const auto expected = kind == ChildKind::rtaudio ? CandidateId::rtaudio
                                                   : CandidateId::miniaudio;
  if (!parsed || parsed->candidate_id != expected) return std::nullopt;
  return ChildReport{std::move(*parsed)};
}

struct PreparedCommand {
  Descriptor executable;
  std::vector<std::string> arguments;
  std::vector<char*> raw_arguments;
};

[[nodiscard]] auto prepare_command(const ChildCommand& command)
    -> std::optional<PreparedCommand> {
  Descriptor descriptor{
      ::open(command.executable.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
  struct stat attributes{};
  if (descriptor.get() < 0 || ::fstat(descriptor.get(), &attributes) != 0 ||
      !S_ISREG(attributes.st_mode))
    return std::nullopt;
  std::vector<std::string> arguments;
  arguments.reserve(command.argument_prefix.size() + 1);
  arguments.push_back(command.executable.string());
  arguments.insert(arguments.end(), command.argument_prefix.begin(),
                   command.argument_prefix.end());
  std::vector<char*> raw_arguments;
  raw_arguments.reserve(arguments.size() + 1);
  for (auto& argument : arguments)
    raw_arguments.push_back(argument.data());
  raw_arguments.push_back(nullptr);
  return PreparedCommand{std::move(descriptor), std::move(arguments),
                         std::move(raw_arguments)};
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Bounded launch.
[[nodiscard]] auto launch_child(const ChildKind kind,
                                const ChildCommand& command,
                                const RunnerOptions& options) -> ChildReport {
  auto prepared = prepare_command(command);
  if (!prepared) return failed_child(kind, ReasonCode::internal_error);
  int output_pipe[2]{};
  if (::pipe2(output_pipe, O_CLOEXEC | O_NONBLOCK) != 0)
    return failed_child(kind, ReasonCode::internal_error);
  const auto child = ::fork();
  if (child < 0) {
    static_cast<void>(::close(output_pipe[0]));
    static_cast<void>(::close(output_pipe[1]));
    return failed_child(kind, ReasonCode::internal_error);
  }
  if (child == 0) {
    const rlimit no_core{0, 0};
    if (::setrlimit(RLIMIT_CORE, &no_core) != 0 ||
        ::prctl(PR_SET_DUMPABLE, 0) != 0)
      ::_exit(126);
    if (::setpgid(0, 0) != 0) ::_exit(126);
    const auto input = ::open("/dev/null", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    const auto errors = ::open("/dev/null", O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (input < 0 || errors < 0 || ::dup2(input, STDIN_FILENO) < 0 ||
        ::dup2(output_pipe[1], STDOUT_FILENO) < 0 ||
        ::dup2(errors, STDERR_FILENO) < 0)
      ::_exit(126);
    static_cast<void>(::close(output_pipe[0]));
    static_cast<void>(::close(output_pipe[1]));
    if (input != STDIN_FILENO) static_cast<void>(::close(input));
    if (errors != STDERR_FILENO) static_cast<void>(::close(errors));
    if (prepared->executable.get() != 3 &&
        ::dup3(prepared->executable.get(), 3, O_CLOEXEC) < 0)
      ::_exit(126);
    if (prepared->executable.get() == 3 &&
        ::fcntl(prepared->executable.get(), F_SETFD, FD_CLOEXEC) != 0)
      ::_exit(126);
    char language[] = "LANG=C";
    char locale[] = "LC_ALL=C";
    char* environment[]{language, locale, nullptr};
    if (!close_unrelated_descriptors(4)) ::_exit(126);
    ::fexecve(3, prepared->raw_arguments.data(), environment);
    ::_exit(127);
  }

  static_cast<void>(::setpgid(child, child));
  static_cast<void>(::close(output_pipe[1]));
  auto cleanup_guard = ChildCleanupGuard{child, output_pipe[0]};
  try {
    std::string output;
    output.reserve(options.maximum_child_output_bytes);
    bool output_exceeded{};
    bool pipe_read_failed{};
    bool timed_out{};
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
          pipe_read_failed = true;
          terminate_child(child);
        }
        break;
      }
      if (!child_reaped) {
        const auto waited = ::waitpid(child, &status, WNOHANG);
        if (waited == child) child_reaped = true;
        if (waited < 0 && errno != EINTR) {
          wait_failed = true;
          terminate_child(child);
        }
      }
      if (output_exceeded || pipe_read_failed || wait_failed) break;
      if (child_reaped && pipe_closed) break;
      if (std::chrono::steady_clock::now() >= deadline) {
        timed_out = true;
        terminate_child(child);
        break;
      }
      pollfd descriptor{output_pipe[0], POLLIN | POLLHUP, 0};
      static_cast<void>(::poll(&descriptor, 1, 10));
    }
    cleanup_guard.close_output();
    if (!child_reaped) {
      terminate_child(child);
      child_reaped = bounded_reap(child, status);
      if (!child_reaped) wait_failed = true;
    }
    if (child_reaped) cleanup_guard.mark_reaped();
    const auto cleanup = cleanup_descendants();
    cleanup_guard.release();
    if (!cleanup.succeeded || cleanup.observed != 0)
      return failed_child(kind, ReasonCode::cleanup_failed, false);
    if (wait_failed || pipe_read_failed)
      return failed_child(kind, ReasonCode::internal_error);
    if (output_exceeded) return failed_child(kind, ReasonCode::output_limit);
    if (timed_out) return failed_child(kind, ReasonCode::timeout);
    if (WIFSIGNALED(status)) return failed_child(kind, ReasonCode::signaled);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
      return failed_child(kind, ReasonCode::nonzero_exit);
    auto parsed = parse_child(kind, output);
    if (!parsed) return failed_child(kind, ReasonCode::malformed_protocol);
    return std::move(*parsed);
  } catch (...) {
    const auto cleaned = cleanup_guard.cleanup();
    return failed_child(
        kind, cleaned ? ReasonCode::internal_error : ReasonCode::cleanup_failed,
        cleaned);
  }
}

auto mark_cleanup_failure(EvidenceReport& report) -> void {
  report.contract = failed_contract(ReasonCode::cleanup_failed, false);
  report.candidates = {
      failed_candidate(CandidateId::rtaudio, ReasonCode::cleanup_failed, false),
      failed_candidate(CandidateId::miniaudio, ReasonCode::cleanup_failed,
                       false),
  };
}

} // namespace

auto run_evaluation(std::string source_sha, const RunnerOptions& options)
    -> std::expected<EvidenceReport, RunnerError> {
  try {
    if (!valid_source_sha(source_sha) || !valid_command(options.contract) ||
        !valid_command(options.rtaudio) || !valid_command(options.miniaudio) ||
        options.child_timeout <= std::chrono::milliseconds::zero() ||
        options.child_timeout > maximum_probe_timeout ||
        options.maximum_child_output_bytes == 0 ||
        options.maximum_child_output_bytes > maximum_child_report_bytes) {
      return runner_error(RunnerErrorCode::invalid_options,
                          "audio-device runner options are invalid");
    }
    const auto existing_children = direct_descendants();
    if (!existing_children || !existing_children->empty())
      return runner_error(RunnerErrorCode::cleanup_unavailable,
                          "audio-device runner requires no child processes");
    auto subreaper = SubreaperGuard::create();
    if (!subreaper)
      return runner_error(
          RunnerErrorCode::cleanup_unavailable,
          "audio-device descendant cleanup cannot be established");
    auto report = platform_report(std::move(source_sha));
    if (!report) return std::unexpected(std::move(report.error()));

    auto contract =
        launch_child(ChildKind::contract, options.contract, options);
    auto rtaudio = launch_child(ChildKind::rtaudio, options.rtaudio, options);
    auto miniaudio =
        launch_child(ChildKind::miniaudio, options.miniaudio, options);
    report->contract = std::get<ContractReport>(std::move(contract));
    report->candidates.push_back(std::get<CandidateReport>(std::move(rtaudio)));
    report->candidates.push_back(
        std::get<CandidateReport>(std::move(miniaudio)));
    if (!subreaper->restore()) mark_cleanup_failure(*report);
    return std::move(*report);
  } catch (...) {
    return runner_error(RunnerErrorCode::internal_error,
                        "audio-device evaluation failed internally");
  }
}

} // namespace aiforge::evaluation::audio_device
