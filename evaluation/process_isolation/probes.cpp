#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "probes.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <dirent.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/openat2.h>
#include <linux/seccomp.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace aiforge::evaluation::process_isolation {
namespace {

constexpr int assertion_enforced = 0;
constexpr int assertion_permission_denied = 20;
constexpr int assertion_mechanism_absent = 21;
constexpr int assertion_failed = 22;
constexpr int assertion_internal_error = 23;

[[nodiscard]] auto enforced(const ProbeId id) -> ProbeRecord {
  return {id, ProbeState::enforced, ReasonCode::none};
}

[[nodiscard]] auto unavailable(const ProbeId id, const ReasonCode reason)
    -> ProbeRecord {
  return {id, ProbeState::unavailable, reason};
}

[[nodiscard]] auto probe_error(const ProbeId id, const ReasonCode reason)
    -> ProbeRecord {
  return {id, ProbeState::probe_error, reason};
}

[[nodiscard]] auto unavailable_from_errno(const ProbeId id,
                                          const int error_number)
    -> ProbeRecord {
  if (error_number == EACCES || error_number == EPERM) {
    return unavailable(id, ReasonCode::permission_denied);
  }
  if (error_number == ENOSYS || error_number == ENODEV ||
      error_number == EOPNOTSUPP || error_number == ENOTSUP ||
      error_number == EINVAL) {
    return unavailable(id, ReasonCode::unsupported_kernel);
  }
  return probe_error(id, ReasonCode::internal_error);
}

[[nodiscard]] auto exit_for_errno(const int error_number) -> int {
  if (error_number == EACCES || error_number == EPERM) {
    return assertion_permission_denied;
  }
  if (error_number == ENOSYS || error_number == ENODEV ||
      error_number == EOPNOTSUPP || error_number == ENOTSUP ||
      error_number == EINVAL) {
    return assertion_mechanism_absent;
  }
  return assertion_internal_error;
}

template <typename Assertion>
[[nodiscard]] auto isolated_assertion(const ProbeId id, Assertion assertion)
    -> ProbeRecord {
  const auto child = ::fork();
  if (child < 0) return probe_error(id, ReasonCode::internal_error);
  if (child == 0) {
    const auto result = assertion();
    ::_exit(result);
  }
  int status{};
  while (::waitpid(child, &status, 0) < 0) {
    if (errno != EINTR) return probe_error(id, ReasonCode::cleanup_failed);
  }
  if (!WIFEXITED(status)) return probe_error(id, ReasonCode::signaled);
  switch (WEXITSTATUS(status)) {
    case assertion_enforced: return enforced(id);
    case assertion_permission_denied:
      return unavailable(id, ReasonCode::permission_denied);
    case assertion_mechanism_absent:
      return unavailable(id, ReasonCode::unsupported_kernel);
    case assertion_failed:
      return unavailable(id, ReasonCode::enforcement_failed);
    default: return probe_error(id, ReasonCode::internal_error);
  }
}

[[nodiscard]] auto valid_state_directory(const std::filesystem::path& path)
    -> bool {
  if (!path.is_absolute()) return false;
  struct stat attributes{};
  return ::lstat(path.c_str(), &attributes) == 0 &&
         S_ISDIR(attributes.st_mode) && !S_ISLNK(attributes.st_mode) &&
         attributes.st_uid == ::geteuid() &&
         (attributes.st_mode & 07777) == S_IRWXU;
}

[[nodiscard]] auto namespace_identity(const char* path, struct stat& result)
    -> bool {
  return ::stat(path, &result) == 0;
}

[[nodiscard]] auto changed_namespace(const char* path, const int flag) -> int {
  struct stat before{};
  struct stat after{};
  if (!namespace_identity(path, before)) return assertion_internal_error;
  if (::unshare(flag) != 0) return exit_for_errno(errno);
  if (!namespace_identity(path, after)) return assertion_internal_error;
  return before.st_dev != after.st_dev || before.st_ino != after.st_ino
             ? assertion_enforced
             : assertion_failed;
}

[[nodiscard]] auto write_all(const int descriptor,
                             const std::span<const std::byte> bytes) -> bool {
  std::size_t offset{};
  while (offset < bytes.size()) {
    const auto count =
        ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
    if (count < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (count == 0) return false;
    offset += static_cast<std::size_t>(count);
  }
  return true;
}

[[nodiscard]] auto copy_executable(const std::filesystem::path& destination)
    -> bool {
  const auto input = ::open("/bin/true", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (input < 0) return false;
  const auto output = ::open(
      destination.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
      S_IRUSR | S_IWUSR | S_IXUSR);
  if (output < 0) {
    static_cast<void>(::close(input));
    return false;
  }
  std::array<std::byte, 8192> buffer{};
  bool okay{true};
  while (okay) {
    const auto count = ::read(input, buffer.data(), buffer.size());
    if (count == 0) break;
    if (count < 0) {
      if (errno == EINTR) continue;
      okay = false;
      break;
    }
    okay = write_all(output,
                     std::span{buffer}.first(static_cast<std::size_t>(count)));
  }
  if (okay) okay = ::fsync(output) == 0;
  if (::close(output) != 0) okay = false;
  if (::close(input) != 0) okay = false;
  return okay;
}

[[nodiscard]] auto execute_open_descriptor(const ProbeId id,
                                           const std::filesystem::path& state,
                                           const bool use_fexecve)
    -> ProbeRecord {
  const auto staged =
      state / (use_fexecve ? "fexecve-image" : "execveat-image");
  if (!copy_executable(staged)) {
    return unavailable(id, ReasonCode::prerequisite_unavailable);
  }
  const auto descriptor =
      ::open(staged.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) return unavailable_from_errno(id, errno);
  if (::unlink(staged.c_str()) != 0) {
    static_cast<void>(::close(descriptor));
    return probe_error(id, ReasonCode::cleanup_failed);
  }
  const auto child = ::fork();
  if (child < 0) {
    static_cast<void>(::close(descriptor));
    return probe_error(id, ReasonCode::internal_error);
  }
  if (child == 0) {
    char name[] = "identity-probe";
    char* arguments[]{name, nullptr};
    char* environment[]{nullptr};
    if (use_fexecve) {
      ::fexecve(descriptor, arguments, environment);
    } else {
#if defined(SYS_execveat)
      static_cast<void>(::syscall(SYS_execveat, descriptor, "", arguments,
                                  environment, AT_EMPTY_PATH));
#else
      ::_exit(assertion_mechanism_absent);
#endif
    }
    ::_exit(exit_for_errno(errno));
  }
  static_cast<void>(::close(descriptor));
  int status{};
  while (::waitpid(child, &status, 0) < 0) {
    if (errno != EINTR) return probe_error(id, ReasonCode::cleanup_failed);
  }
  if (!WIFEXITED(status)) return probe_error(id, ReasonCode::signaled);
  if (WEXITSTATUS(status) == 0) return enforced(id);
  if (WEXITSTATUS(status) == assertion_mechanism_absent) {
    return unavailable(id, ReasonCode::mechanism_absent);
  }
  if (WEXITSTATUS(status) == assertion_permission_denied) {
    return unavailable(id, ReasonCode::permission_denied);
  }
  if (WEXITSTATUS(status) == assertion_failed)
    return unavailable(id, ReasonCode::enforcement_failed);
  return probe_error(id, ReasonCode::internal_error);
}

[[nodiscard]] auto no_new_privileges_probe(const ProbeId id) -> ProbeRecord {
  return isolated_assertion(id, [] {
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
      return exit_for_errno(errno);
    if (::prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1) return assertion_failed;
    errno = 0;
    if (::prctl(PR_SET_NO_NEW_PRIVS, 0, 0, 0, 0) == 0) return assertion_failed;
    return ::prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) == 1 ? assertion_enforced
                                                         : assertion_failed;
  });
}

[[nodiscard]] auto rlimit_cpu_probe(const ProbeId id) -> ProbeRecord {
  const auto child = ::fork();
  if (child < 0) return probe_error(id, ReasonCode::internal_error);
  if (child == 0) {
    const rlimit limit{1, 1};
    if (::setrlimit(RLIMIT_CPU, &limit) != 0) ::_exit(exit_for_errno(errno));
    for (;;) {
      asm volatile("" ::: "memory");
    }
  }
  int status{};
  while (::waitpid(child, &status, 0) < 0) {
    if (errno != EINTR) return probe_error(id, ReasonCode::cleanup_failed);
  }
  if (WIFSIGNALED(status) &&
      (WTERMSIG(status) == SIGXCPU || WTERMSIG(status) == SIGKILL)) {
    return enforced(id);
  }
  if (WIFEXITED(status) && WEXITSTATUS(status) == assertion_permission_denied)
    return unavailable(id, ReasonCode::permission_denied);
  if (WIFEXITED(status) && WEXITSTATUS(status) == assertion_mechanism_absent)
    return unavailable(id, ReasonCode::unsupported_kernel);
  if (WIFEXITED(status) &&
      (WEXITSTATUS(status) == assertion_enforced ||
       WEXITSTATUS(status) == assertion_failed))
    return unavailable(id, ReasonCode::enforcement_failed);
  return probe_error(id, ReasonCode::internal_error);
}

[[nodiscard]] auto rlimit_address_space_probe(const ProbeId id) -> ProbeRecord {
  return isolated_assertion(id, [] {
    const rlimit limit{1, 1};
    if (::setrlimit(RLIMIT_AS, &limit) != 0) return exit_for_errno(errno);
    errno = 0;
    void* memory = ::mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory != MAP_FAILED) {
      static_cast<void>(::munmap(memory, 4096));
      return assertion_failed;
    }
    return errno == ENOMEM ? assertion_enforced : assertion_failed;
  });
}

[[nodiscard]] auto rlimit_process_count_probe(const ProbeId id) -> ProbeRecord {
  return isolated_assertion(id, [] {
    const rlimit limit{0, 0};
    if (::setrlimit(RLIMIT_NPROC, &limit) != 0) return exit_for_errno(errno);
    errno = 0;
    const auto descendant = ::fork();
    if (descendant < 0)
      return errno == EAGAIN ? assertion_enforced : assertion_internal_error;
    if (descendant == 0) ::_exit(0);
    static_cast<void>(::kill(descendant, SIGKILL));
    static_cast<void>(::waitpid(descendant, nullptr, 0));
    return assertion_failed;
  });
}

[[nodiscard]] auto rlimit_descriptor_count_probe(const ProbeId id)
    -> ProbeRecord {
  return isolated_assertion(id, [] {
    const rlimit limit{0, 0};
    if (::setrlimit(RLIMIT_NOFILE, &limit) != 0) return exit_for_errno(errno);
    errno = 0;
    const auto descriptor = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (descriptor >= 0) {
      static_cast<void>(::close(descriptor));
      return assertion_failed;
    }
    return errno == EMFILE ? assertion_enforced : assertion_failed;
  });
}

[[nodiscard]] auto rlimit_file_size_probe(const ProbeId id,
                                          const std::filesystem::path& state)
    -> ProbeRecord {
  return isolated_assertion(id, [&state] {
    const auto path = state / "rlimit-file";
    const auto descriptor = ::open(
        path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
        S_IRUSR | S_IWUSR);
    if (descriptor < 0) return assertion_internal_error;
    const rlimit limit{1, 1};
    if (::setrlimit(RLIMIT_FSIZE, &limit) != 0) {
      static_cast<void>(::close(descriptor));
      return exit_for_errno(errno);
    }
    static_cast<void>(::signal(SIGXFSZ, SIG_IGN));
    const std::array<std::byte, 1> one{std::byte{'a'}};
    if (::write(descriptor, one.data(), one.size()) != 1) {
      static_cast<void>(::close(descriptor));
      return assertion_failed;
    }
    errno = 0;
    const auto second = ::write(descriptor, one.data(), one.size());
    const auto saved_errno = errno;
    static_cast<void>(::close(descriptor));
    return second < 0 && saved_errno == EFBIG ? assertion_enforced
                                              : assertion_failed;
  });
}

[[nodiscard]] auto read_ready_pid(const int descriptor)
    -> std::optional<pid_t> {
  pollfd ready{descriptor, POLLIN | POLLHUP, 0};
  int polled{};
  do {
    polled = ::poll(&ready, 1, 500);
  } while (polled < 0 && errno == EINTR);
  if (polled <= 0 || (ready.revents & POLLIN) == 0) return std::nullopt;
  pid_t value{};
  std::size_t offset{};
  while (offset < sizeof(value)) {
    const auto count = ::read(descriptor,
                              reinterpret_cast<char*>(&value) + offset,
                              sizeof(value) - offset);
    if (count < 0) {
      if (errno == EINTR) continue;
      return std::nullopt;
    }
    if (count == 0) return std::nullopt;
    offset += static_cast<std::size_t>(count);
  }
  return value > 0 ? std::optional<pid_t>{value} : std::nullopt;
}

auto emergency_reap(const pid_t target) -> void {
  if (target <= 0) return;
  static_cast<void>(::kill(target, SIGKILL));
  while (::waitpid(target, nullptr, 0) < 0 && errno == EINTR) {
  }
}

[[nodiscard]] auto pidfd_cleanup_result(const ProbeId id, const pid_t target)
    -> ProbeRecord {
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
  const auto pidfd =
      static_cast<int>(::syscall(SYS_pidfd_open, target, 0U));
  if (pidfd < 0) {
    const auto saved_errno = errno;
    emergency_reap(target);
    return unavailable_from_errno(id, saved_errno);
  }
  const auto signaled =
      ::syscall(SYS_pidfd_send_signal, pidfd, SIGKILL, nullptr, 0U) == 0;
  const auto signal_errno = errno;
  static_cast<void>(::close(pidfd));
  if (!signaled) {
    emergency_reap(target);
    return unavailable_from_errno(id, signal_errno);
  }
  int status{};
  pid_t waited{};
  do {
    waited = ::waitpid(target, &status, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited != target)
    return probe_error(id, ReasonCode::cleanup_failed);
  return WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL
             ? enforced(id)
             : unavailable(id, ReasonCode::enforcement_failed);
#else
  emergency_reap(target);
  return unavailable(id, ReasonCode::mechanism_absent);
#endif
}

[[nodiscard]] auto enable_subreaper(int& previous) -> bool {
  return ::prctl(PR_GET_CHILD_SUBREAPER, &previous) == 0 &&
         (previous != 0 || ::prctl(PR_SET_CHILD_SUBREAPER, 1) == 0);
}

auto restore_subreaper(const int previous) -> bool {
  return previous != 0 || ::prctl(PR_SET_CHILD_SUBREAPER, 0) == 0;
}

[[nodiscard]] auto session_containment_probe(const ProbeId id) -> ProbeRecord {
  int previous{};
  if (!enable_subreaper(previous)) return unavailable_from_errno(id, errno);
  int readiness[2]{};
  if (::pipe2(readiness, O_CLOEXEC) != 0) {
    static_cast<void>(restore_subreaper(previous));
    return probe_error(id, ReasonCode::internal_error);
  }
  const auto intermediate = ::fork();
  if (intermediate == 0) {
    static_cast<void>(::close(readiness[0]));
    if (::setsid() < 0) ::_exit(assertion_internal_error);
    const auto descendant = ::fork();
    if (descendant < 0) ::_exit(assertion_internal_error);
    if (descendant == 0) {
      const auto target = ::getpid();
      if (!write_all(readiness[1], std::as_bytes(std::span{&target, 1})))
        ::_exit(assertion_internal_error);
      static_cast<void>(::close(readiness[1]));
      for (;;) ::pause();
    }
    ::_exit(0);
  }
  static_cast<void>(::close(readiness[1]));
  if (intermediate < 0) {
    static_cast<void>(::close(readiness[0]));
    static_cast<void>(restore_subreaper(previous));
    return probe_error(id, ReasonCode::internal_error);
  }
  int intermediate_status{};
  pid_t waited{};
  do {
    waited = ::waitpid(intermediate, &intermediate_status, 0);
  } while (waited < 0 && errno == EINTR);
  const auto target = read_ready_pid(readiness[0]);
  static_cast<void>(::close(readiness[0]));
  auto result = waited == intermediate && WIFEXITED(intermediate_status) &&
                        WEXITSTATUS(intermediate_status) == 0 && target
                    ? pidfd_cleanup_result(id, *target)
                    : probe_error(id, ReasonCode::malformed_protocol);
  if (target &&
      !(waited == intermediate && WIFEXITED(intermediate_status) &&
        WEXITSTATUS(intermediate_status) == 0))
    emergency_reap(*target);
  if (!restore_subreaper(previous))
    result = probe_error(id, ReasonCode::cleanup_failed);
  return result;
}

[[nodiscard]] auto double_fork_containment_probe(const ProbeId id)
    -> ProbeRecord {
  int previous{};
  if (!enable_subreaper(previous)) return unavailable_from_errno(id, errno);
  int readiness[2]{};
  if (::pipe2(readiness, O_CLOEXEC) != 0) {
    static_cast<void>(restore_subreaper(previous));
    return probe_error(id, ReasonCode::internal_error);
  }
  const auto intermediate = ::fork();
  if (intermediate == 0) {
    static_cast<void>(::close(readiness[0]));
    const auto daemon = ::fork();
    if (daemon < 0) ::_exit(assertion_internal_error);
    if (daemon == 0) {
      const auto target = ::getpid();
      if (::setsid() < 0 ||
          !write_all(readiness[1], std::as_bytes(std::span{&target, 1})))
        ::_exit(assertion_internal_error);
      static_cast<void>(::close(readiness[1]));
      for (;;) ::pause();
    }
    ::_exit(0);
  }
  static_cast<void>(::close(readiness[1]));
  if (intermediate < 0) {
    static_cast<void>(::close(readiness[0]));
    static_cast<void>(restore_subreaper(previous));
    return probe_error(id, ReasonCode::internal_error);
  }
  int intermediate_status{};
  pid_t waited{};
  do {
    waited = ::waitpid(intermediate, &intermediate_status, 0);
  } while (waited < 0 && errno == EINTR);
  const auto target = read_ready_pid(readiness[0]);
  static_cast<void>(::close(readiness[0]));
  auto result = waited == intermediate && WIFEXITED(intermediate_status) &&
                        WEXITSTATUS(intermediate_status) == 0 && target
                    ? pidfd_cleanup_result(id, *target)
                    : probe_error(id, ReasonCode::malformed_protocol);
  if (target &&
      !(waited == intermediate && WIFEXITED(intermediate_status) &&
        WEXITSTATUS(intermediate_status) == 0))
    emergency_reap(*target);
  if (!restore_subreaper(previous))
    result = probe_error(id, ReasonCode::cleanup_failed);
  return result;
}

[[nodiscard]] auto landlock_probe(const ProbeId id,
                                  const std::filesystem::path& state)
    -> ProbeRecord {
#if defined(SYS_landlock_create_ruleset) && defined(SYS_landlock_add_rule) &&  \
    defined(SYS_landlock_restrict_self) &&                                     \
    defined(LANDLOCK_CREATE_RULESET_VERSION)
  return isolated_assertion(id, [&state] {
    const auto allowed_path = state / "landlock-allowed";
    const auto allowed_create =
        ::open(allowed_path.c_str(),
               O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
               S_IRUSR | S_IWUSR);
    if (allowed_create < 0) return assertion_internal_error;
    if (::close(allowed_create) != 0) return assertion_internal_error;
    const auto abi = ::syscall(SYS_landlock_create_ruleset, nullptr, 0,
                               LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 0) return exit_for_errno(errno);
    landlock_ruleset_attr ruleset{};
    ruleset.handled_access_fs =
        LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;
    const auto ruleset_fd = static_cast<int>(
        ::syscall(SYS_landlock_create_ruleset, &ruleset, sizeof(ruleset), 0));
    if (ruleset_fd < 0) return exit_for_errno(errno);
    const auto state_fd =
        ::open(state.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (state_fd < 0) {
      static_cast<void>(::close(ruleset_fd));
      return assertion_internal_error;
    }
    landlock_path_beneath_attr path_rule{};
    path_rule.allowed_access = ruleset.handled_access_fs;
    path_rule.parent_fd = state_fd;
    if (::syscall(SYS_landlock_add_rule, ruleset_fd, LANDLOCK_RULE_PATH_BENEATH,
                  &path_rule, 0) != 0 ||
        ::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
        ::syscall(SYS_landlock_restrict_self, ruleset_fd, 0) != 0) {
      const auto result = exit_for_errno(errno);
      static_cast<void>(::close(state_fd));
      static_cast<void>(::close(ruleset_fd));
      return result;
    }
    static_cast<void>(::close(state_fd));
    static_cast<void>(::close(ruleset_fd));
    const auto allowed =
        ::open(allowed_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (allowed < 0) return assertion_failed;
    static_cast<void>(::close(allowed));
    errno = 0;
    const auto denied = ::open("/etc/passwd", O_RDONLY | O_CLOEXEC);
    if (denied >= 0) {
      static_cast<void>(::close(denied));
      return assertion_failed;
    }
    return errno == EACCES || errno == EPERM ? assertion_enforced
                                             : assertion_failed;
  });
#else
  static_cast<void>(state);
  return unavailable(id, ReasonCode::mechanism_absent);
#endif
}

[[nodiscard]] auto pid_namespace_probe(const ProbeId id) -> ProbeRecord {
  return isolated_assertion(id, [] {
    if (::unshare(CLONE_NEWPID) != 0) return exit_for_errno(errno);
    const auto child = ::fork();
    if (child < 0) return assertion_internal_error;
    if (child == 0) ::_exit(::getpid() == 1 ? 0 : 1);
    int status{};
    if (::waitpid(child, &status, 0) != child) return assertion_internal_error;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? assertion_enforced
                                                         : assertion_failed;
  });
}

[[nodiscard]] auto seccomp_probe(const ProbeId id) -> ProbeRecord {
#if defined(SYS_socket) && (defined(__x86_64__) || defined(__aarch64__))
  return isolated_assertion(id, [] {
#if defined(__x86_64__)
    constexpr std::uint32_t audit_architecture = AUDIT_ARCH_X86_64;
#else
    constexpr std::uint32_t audit_architecture = AUDIT_ARCH_AARCH64;
#endif
    const std::array<sock_filter, 7> filter{{
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                 static_cast<std::uint32_t>(offsetof(seccomp_data, arch))),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, audit_architecture, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                 static_cast<std::uint32_t>(offsetof(seccomp_data, nr))),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_socket, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    }};
    sock_fprog program{static_cast<unsigned short>(filter.size()),
                       const_cast<sock_filter*>(filter.data())};
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
        ::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) != 0) {
      return exit_for_errno(errno);
    }
    errno = 0;
    const auto descriptor = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (descriptor >= 0) {
      static_cast<void>(::close(descriptor));
      return assertion_failed;
    }
    return errno == EPERM && ::getpid() > 0 ? assertion_enforced
                                            : assertion_failed;
  });
#else
  return unavailable(id, ReasonCode::unsupported_architecture);
#endif
}

[[nodiscard]] auto disposable_workspace_probe(
    const ProbeId id, const std::filesystem::path& state)
    -> ProbeRecord {
  const auto file = state / "workspace-prerequisite";
  const auto descriptor =
      ::open(file.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
             S_IRUSR | S_IWUSR);
  if (descriptor < 0) return unavailable_from_errno(id, errno);
  const std::array<std::byte, 1> content{std::byte{'x'}};
  const bool written = write_all(descriptor, content);
  const bool synchronized = written && ::fsync(descriptor) == 0;
  const bool closed = ::close(descriptor) == 0;
  if (!written || !synchronized || !closed)
    return probe_error(id, ReasonCode::internal_error);
  struct stat attributes{};
  if (::lstat(file.c_str(), &attributes) != 0)
    return probe_error(id, ReasonCode::internal_error);
  const bool safe = S_ISREG(attributes.st_mode) &&
                    attributes.st_uid == ::geteuid() &&
                    (attributes.st_mode & (S_IRWXG | S_IRWXO)) == 0;
  return safe ? enforced(id)
              : unavailable(id, ReasonCode::enforcement_failed);
}

[[nodiscard]] auto openat2_probe(const ProbeId id,
                                 const std::filesystem::path& state)
    -> ProbeRecord {
#if defined(SYS_openat2)
  const auto confined = state / "confined";
  std::error_code error;
  if (!std::filesystem::create_directory(confined, error) || error)
    return probe_error(id, ReasonCode::internal_error);
  const auto inside = confined / "inside";
  const auto outside = state / "outside";
  for (const auto& path : {inside, outside}) {
    const auto descriptor = ::open(
        path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
        S_IRUSR | S_IWUSR);
    if (descriptor < 0) return probe_error(id, ReasonCode::internal_error);
    static_cast<void>(::close(descriptor));
  }
  const auto directory =
      ::open(confined.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
  if (directory < 0) return unavailable_from_errno(id, errno);
  open_how how{};
  how.flags = O_RDONLY | O_CLOEXEC;
  how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS;
  const auto allowed = static_cast<int>(
      ::syscall(SYS_openat2, directory, "inside", &how, sizeof(how)));
  const auto allowed_errno = errno;
  errno = 0;
  const auto escaped = static_cast<int>(
      ::syscall(SYS_openat2, directory, "../outside", &how, sizeof(how)));
  const auto escape_errno = errno;
  if (allowed >= 0) static_cast<void>(::close(allowed));
  if (escaped >= 0) static_cast<void>(::close(escaped));
  static_cast<void>(::close(directory));
  if (allowed < 0) return unavailable_from_errno(id, allowed_errno);
  return escaped < 0 && (escape_errno == EXDEV || escape_errno == ELOOP)
             ? enforced(id)
             : unavailable(id, ReasonCode::enforcement_failed);
#else
  static_cast<void>(state);
  return unavailable(id, ReasonCode::mechanism_absent);
#endif
}

[[nodiscard]] auto fchdir_probe(const ProbeId id,
                                const std::filesystem::path& state)
    -> ProbeRecord {
  const auto original = state / "cwd-original";
  const auto renamed = state / "cwd-renamed";
  std::error_code error;
  if (!std::filesystem::create_directory(original, error) || error)
    return probe_error(id, ReasonCode::internal_error);
  const auto descriptor =
      ::open(original.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) return unavailable_from_errno(id, errno);
  struct stat opened{};
  if (::fstat(descriptor, &opened) != 0 ||
      ::rename(original.c_str(), renamed.c_str()) != 0) {
    static_cast<void>(::close(descriptor));
    return probe_error(id, ReasonCode::internal_error);
  }
  const auto previous = ::open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (previous < 0 || ::fchdir(descriptor) != 0) {
    if (previous >= 0) static_cast<void>(::close(previous));
    static_cast<void>(::close(descriptor));
    return unavailable_from_errno(id, errno);
  }
  struct stat current{};
  const bool same = ::stat(".", &current) == 0 &&
                    current.st_dev == opened.st_dev &&
                    current.st_ino == opened.st_ino;
  const bool restored = ::fchdir(previous) == 0;
  static_cast<void>(::close(previous));
  static_cast<void>(::close(descriptor));
  if (!restored) return probe_error(id, ReasonCode::cleanup_failed);
  return same ? enforced(id) : unavailable(id, ReasonCode::enforcement_failed);
}

[[nodiscard]] auto staged_input_probe(const ProbeId id,
                                      const std::filesystem::path& state)
    -> ProbeRecord {
  const auto original = state / "input";
  const auto moved = state / "input-opened";
  const std::array<std::byte, 1> first{std::byte{'A'}};
  const std::array<std::byte, 1> replacement{std::byte{'B'}};
  auto write_file = [](const std::filesystem::path& path,
                       const std::span<const std::byte> content) {
    const auto descriptor = ::open(
        path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
        S_IRUSR | S_IWUSR);
    if (descriptor < 0) return false;
    const bool result = write_all(descriptor, content);
    return ::close(descriptor) == 0 && result;
  };
  if (!write_file(original, first))
    return probe_error(id, ReasonCode::internal_error);
  const auto descriptor =
      ::open(original.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) return unavailable_from_errno(id, errno);
  if (::rename(original.c_str(), moved.c_str()) != 0 ||
      !write_file(original, replacement)) {
    static_cast<void>(::close(descriptor));
    return probe_error(id, ReasonCode::internal_error);
  }
  std::byte observed{};
  struct stat opened{};
  struct stat current{};
  const bool read_original =
      ::read(descriptor, &observed, 1) == 1 && observed == first.front();
  const bool identities_differ =
      ::fstat(descriptor, &opened) == 0 &&
      ::stat(original.c_str(), &current) == 0 &&
      (opened.st_dev != current.st_dev || opened.st_ino != current.st_ino);
  static_cast<void>(::close(descriptor));
  return read_original && identities_differ
             ? enforced(id)
             : unavailable(id, ReasonCode::enforcement_failed);
}

} // namespace

auto child_process_is_sanitized() noexcept -> bool {
  try {
    if (::environ != nullptr && ::environ[0] != nullptr) return false;
    errno = 0;
    if (::fcntl(STDIN_FILENO, F_GETFD) != -1 || errno != EBADF) return false;
    struct stat output{};
    struct stat error{};
    if (::fstat(STDOUT_FILENO, &output) != 0 ||
        ::fstat(STDERR_FILENO, &error) != 0 || !S_ISFIFO(output.st_mode) ||
        !S_ISCHR(error.st_mode)) {
      return false;
    }
    DIR* directory = ::opendir("/proc/self/fd");
    if (directory == nullptr) return false;
    const auto inspection_descriptor = ::dirfd(directory);
    bool safe{true};
    while (const auto* entry = ::readdir(directory)) {
      if (entry->d_name[0] == '.') continue;
      char* end{};
      errno = 0;
      const auto parsed = std::strtol(entry->d_name, &end, 10);
      if (errno != 0 || end == entry->d_name || *end != '\0') {
        safe = false;
        break;
      }
      const auto descriptor = static_cast<int>(parsed);
      if (descriptor != STDOUT_FILENO && descriptor != STDERR_FILENO &&
          descriptor != inspection_descriptor) {
        safe = false;
        break;
      }
    }
    static_cast<void>(::closedir(directory));
    return safe;
  } catch (...) {
    return false;
  }
}

auto run_probe(const ProbeId probe_id,
               const std::filesystem::path& state_directory) -> ProbeRecord {
  try {
    if (!valid_state_directory(state_directory))
      return probe_error(probe_id, ReasonCode::internal_error);
    switch (probe_id) {
      case ProbeId::no_new_privileges: return no_new_privileges_probe(probe_id);
      case ProbeId::rlimit_cpu: return rlimit_cpu_probe(probe_id);
      case ProbeId::rlimit_address_space:
        return rlimit_address_space_probe(probe_id);
      case ProbeId::rlimit_process_count:
        return rlimit_process_count_probe(probe_id);
      case ProbeId::rlimit_descriptor_count:
        return rlimit_descriptor_count_probe(probe_id);
      case ProbeId::rlimit_file_size:
        return rlimit_file_size_probe(probe_id, state_directory);
      case ProbeId::inherited_descriptors:
        return child_process_is_sanitized()
                   ? enforced(probe_id)
                   : unavailable(probe_id, ReasonCode::enforcement_failed);
      case ProbeId::subreaper_session_cleanup:
        return session_containment_probe(probe_id);
      case ProbeId::subreaper_double_fork_cleanup:
        return double_fork_containment_probe(probe_id);
      case ProbeId::landlock_read_confinement:
        return landlock_probe(probe_id, state_directory);
      case ProbeId::user_namespace:
        return isolated_assertion(probe_id, [] {
          return changed_namespace("/proc/self/ns/user", CLONE_NEWUSER);
        });
      case ProbeId::mount_namespace:
        return isolated_assertion(probe_id, [] {
          return changed_namespace("/proc/self/ns/mnt", CLONE_NEWNS);
        });
      case ProbeId::pid_namespace: return pid_namespace_probe(probe_id);
      case ProbeId::network_namespace:
        return isolated_assertion(probe_id, [] {
          return changed_namespace("/proc/self/ns/net", CLONE_NEWNET);
        });
      case ProbeId::seccomp_socket_creation_denial:
        return seccomp_probe(probe_id);
      case ProbeId::disposable_workspace:
        return disposable_workspace_probe(probe_id, state_directory);
      case ProbeId::openat2_resolution:
        return openat2_probe(probe_id, state_directory);
      case ProbeId::fexecve_identity:
        return execute_open_descriptor(probe_id, state_directory, true);
      case ProbeId::execveat_identity:
        return execute_open_descriptor(probe_id, state_directory, false);
      case ProbeId::fchdir_identity:
        return fchdir_probe(probe_id, state_directory);
      case ProbeId::staged_input_identity:
        return staged_input_probe(probe_id, state_directory);
    }
    return probe_error(probe_id, ReasonCode::internal_error);
  } catch (...) {
    return probe_error(probe_id, ReasonCode::internal_error);
  }
}

#if defined(AIFORGE_PROCESS_ISOLATION_TEST_SUPPORT)
namespace test_support {

auto callable_assertion_failure() -> ProbeRecord {
  return isolated_assertion(ProbeId::no_new_privileges,
                            [] { return assertion_failed; });
}

auto runtime_permission_denial() -> ProbeRecord {
#if defined(SYS_unshare) && (defined(__x86_64__) || defined(__aarch64__))
  return isolated_assertion(ProbeId::user_namespace, [] {
#if defined(__x86_64__)
    constexpr std::uint32_t audit_architecture = AUDIT_ARCH_X86_64;
#else
    constexpr std::uint32_t audit_architecture = AUDIT_ARCH_AARCH64;
#endif
    const std::array<sock_filter, 7> filter{{
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                 static_cast<std::uint32_t>(offsetof(seccomp_data, arch))),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, audit_architecture, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                 static_cast<std::uint32_t>(offsetof(seccomp_data, nr))),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_unshare, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    }};
    sock_fprog program{static_cast<unsigned short>(filter.size()),
                       const_cast<sock_filter*>(filter.data())};
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
        ::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) != 0)
      return assertion_internal_error;
    return changed_namespace("/proc/self/ns/user", CLONE_NEWUSER);
  });
#else
  return unavailable(ProbeId::user_namespace,
                     ReasonCode::unsupported_architecture);
#endif
}

} // namespace test_support
#endif

} // namespace aiforge::evaluation::process_isolation
