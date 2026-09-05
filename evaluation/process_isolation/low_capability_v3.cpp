#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "low_capability_v3.hpp"

#include "linux_support.hpp"
#include "probes_v3.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <linux/audit.h>
#include <linux/capability.h>
#include <linux/filter.h>
#include <linux/sched.h>
#include <linux/seccomp.h>
#include <poll.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

namespace aiforge::evaluation::process_isolation::v3 {
namespace {

constexpr auto probe = ProbeId::low_capability_nonescalation;
constexpr auto observation_timeout = std::chrono::seconds{2};
constexpr std::size_t clone_stack_bytes = 64UZ * 1024UZ;
constexpr std::uint32_t wire_magic = 0x33434641U;
constexpr std::uint32_t wire_version = 1;
constexpr int outcome_descriptor = 3;
constexpr int executable_descriptor = 4;
constexpr unsigned int maximum_capability = 63;
constexpr std::uint32_t x32_syscall_bit = 0x40000000U;
#if defined(__x86_64__)
constexpr std::uint32_t filtered_x32_syscall_bit = x32_syscall_bit;
#else
constexpr std::uint32_t filtered_x32_syscall_bit = 0;
#endif

using linux_support::close_descriptors_from;
using linux_support::Descriptor;
using linux_support::write_all;

struct CapabilityObservation {
  bool no_new_privileges{};
  bool capability_sets_empty{};
  bool ambient_empty{};
  bool bounding_subset{};
  bool namespace_creation_denied{};
  bool capability_regain_denied{};
  bool classified{};
};

struct LowCapabilityWire {
  std::uint32_t magic{wire_magic};
  std::uint32_t version{wire_version};
  std::int32_t setup_reason{static_cast<std::int32_t>(ReasonCode::none)};
  std::int32_t pre_exec_verified{};
  std::int32_t descriptor_exec{};
  std::int32_t setup_descriptors_closed{};
  std::int32_t no_new_privileges{};
  std::int32_t capability_sets_empty{};
  std::int32_t ambient_empty{};
  std::int32_t bounding_subset{};
  std::int32_t namespace_creation_denied{};
  std::int32_t capability_regain_denied{};
  std::int32_t fork_descendant_rechecked{};
  std::int32_t clone_descendant_rechecked{};
};

struct LowCapabilityChecks {
  bool pre_exec_verified{};
  bool descriptor_exec{};
  bool setup_descriptors_closed{};
  bool no_new_privileges{};
  bool capability_sets_empty{};
  bool ambient_empty{};
  bool bounding_subset{};
  bool namespace_creation_denied{};
  bool capability_regain_denied{};
  bool fork_descendant_rechecked{};
  bool clone_descendant_rechecked{};
};

static_assert(sizeof(LowCapabilityWire) <= 128);

[[nodiscard]] auto enforced() -> ProbeRecord {
  return {probe, ProbeState::enforced, ReasonCode::none};
}

[[nodiscard]] auto unavailable(const ReasonCode reason) -> ProbeRecord {
  return {probe, ProbeState::unavailable, reason};
}

[[nodiscard]] auto probe_error(const ReasonCode reason) -> ProbeRecord {
  return {probe, ProbeState::probe_error, reason};
}

[[nodiscard]] auto supported_architecture() -> bool {
#if defined(__x86_64__) || defined(__aarch64__)
  return true;
#else
  return false;
#endif
}

[[nodiscard]] auto reason_from_errno(const int error_number) -> ReasonCode {
  switch (error_number) {
    case ENOSYS:
    case EINVAL:
    case E2BIG:
    case EOPNOTSUPP: return ReasonCode::unsupported_kernel;
    case EACCES:
    case EPERM: return ReasonCode::permission_denied;
    case ENOENT:
    case ENODEV:
    case ENOPROTOOPT: return ReasonCode::mechanism_absent;
    case EAGAIN:
    case EBUSY:
    case ENOMEM:
    case ENOSPC:
    case EUSERS: return ReasonCode::prerequisite_unavailable;
    default: return ReasonCode::internal_error;
  }
}

[[nodiscard]] auto low_capability_outcome(const LowCapabilityChecks& checks,
                                          const bool cleanup_complete)
    -> ProbeRecord {
  if (!cleanup_complete) return probe_error(ReasonCode::cleanup_failed);
  const bool complete =
      checks.pre_exec_verified && checks.descriptor_exec &&
      checks.setup_descriptors_closed && checks.no_new_privileges &&
      checks.capability_sets_empty && checks.ambient_empty &&
      checks.bounding_subset && checks.namespace_creation_denied &&
      checks.capability_regain_denied && checks.fork_descendant_rechecked &&
      checks.clone_descendant_rechecked;
  return complete ? enforced() : unavailable(ReasonCode::enforcement_failed);
}

template <typename Value>
[[nodiscard]] auto parse_unsigned(const std::string_view document,
                                  const int base, Value& output) -> bool {
  if (document.empty()) return false;
  const auto* begin = document.data();
  const auto* end = begin + document.size();
  const auto parsed = std::from_chars(begin, end, output, base);
  return parsed.ec == std::errc{} && parsed.ptr == end;
}

[[nodiscard]] auto read_cap_last() -> std::expected<unsigned int, ReasonCode> {
  const Descriptor descriptor{::open("/proc/sys/kernel/cap_last_cap",
                                     O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
  if (descriptor.get() < 0) return std::unexpected(reason_from_errno(errno));
  std::array<char, 16> buffer{};
  ssize_t count{};
  do {
    count = ::read(descriptor.get(), buffer.data(), buffer.size());
  } while (count < 0 && errno == EINTR);
  if (count < 0) return std::unexpected(reason_from_errno(errno));
  if (count < 2 || static_cast<std::size_t>(count) >= buffer.size() ||
      buffer[static_cast<std::size_t>(count) - 1] != '\n')
    return std::unexpected(ReasonCode::unsupported_kernel);
  unsigned int value{};
  const std::string_view document{buffer.data(),
                                  static_cast<std::size_t>(count) - 1};
  if (!parse_unsigned(document, 10, value) || value > maximum_capability)
    return std::unexpected(ReasonCode::unsupported_kernel);
  return value;
}

[[nodiscard]] auto bounding_fingerprint(const unsigned int cap_last)
    -> std::expected<std::uint64_t, ReasonCode> {
  std::uint64_t result{};
  for (unsigned int capability{}; capability <= cap_last; ++capability) {
    errno = 0;
    const auto present = ::prctl(PR_CAPBSET_READ, capability, 0, 0, 0);
    if (present != 0 && present != 1)
      return std::unexpected(reason_from_errno(errno));
    if (present == 1) result |= std::uint64_t{1} << capability;
  }
  return result;
}

[[nodiscard]] auto capability_sets_are_empty() -> std::optional<bool> {
#if defined(SYS_capget)
  __user_cap_header_struct header{_LINUX_CAPABILITY_VERSION_3, 0};
  std::array<__user_cap_data_struct, 2> data{};
  if (::syscall(SYS_capget, &header, data.data()) != 0) return std::nullopt;
  return data[0].effective == 0 && data[0].permitted == 0 &&
         data[0].inheritable == 0 && data[1].effective == 0 &&
         data[1].permitted == 0 && data[1].inheritable == 0;
#else
  return std::nullopt;
#endif
}

[[nodiscard]] auto ambient_set_is_empty(const unsigned int cap_last)
    -> std::optional<bool> {
#if defined(PR_CAP_AMBIENT) && defined(PR_CAP_AMBIENT_IS_SET)
  for (unsigned int capability{}; capability <= cap_last; ++capability) {
    errno = 0;
    const auto present =
        ::prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_IS_SET, capability, 0, 0);
    if (present != 0 && present != 1) return std::nullopt;
    if (present == 1) return false;
  }
  return true;
#else
  static_cast<void>(cap_last);
  return std::nullopt;
#endif
}

enum class Attempt { denied, escaped, unexpected };

[[nodiscard]] auto classify_denial(const long result, const int error_number)
    -> Attempt {
  if (result >= 0) return Attempt::escaped;
  return error_number == EPERM ? Attempt::denied : Attempt::unexpected;
}

[[nodiscard]] auto classify_x32_denial(const long result,
                                       const int error_number) -> Attempt {
  if (result >= 0) return Attempt::escaped;
  if (error_number == EPERM || error_number == ENOSYS) return Attempt::denied;
  return Attempt::unexpected;
}

[[nodiscard]] auto namespace_attempts() -> std::array<Attempt, 5> {
  std::array<Attempt, 5> results{};
  errno = 0;
  const auto unshare_result = ::unshare(CLONE_NEWUSER);
  const auto unshare_error = errno;
  results[0] = classify_denial(unshare_result, unshare_error);

  const Descriptor own_namespace{
      ::open("/proc/self/ns/mnt", O_RDONLY | O_CLOEXEC)};
  if (own_namespace.get() < 0) {
    results[1] = Attempt::unexpected;
  } else {
    errno = 0;
    const auto setns_result = ::setns(own_namespace.get(), CLONE_NEWNS);
    const auto setns_error = errno;
    results[1] = classify_denial(setns_result, setns_error);
  }

#if defined(SYS_clone3)
  clone_args clone3_arguments{};
  clone3_arguments.flags = CLONE_NEWUSER;
  clone3_arguments.exit_signal = SIGCHLD;
  errno = 0;
  const auto clone3_result =
      ::syscall(SYS_clone3, &clone3_arguments, sizeof(clone3_arguments));
  const auto clone3_error = errno;
  if (clone3_result == 0) ::_exit(71);
  if (clone3_result > 0) {
    static_cast<void>(::kill(static_cast<pid_t>(clone3_result), SIGKILL));
    while (::waitpid(static_cast<pid_t>(clone3_result), nullptr, 0) < 0 &&
           errno == EINTR) {
    }
  }
  results[2] = classify_denial(clone3_result, clone3_error);
#else
  results[2] = Attempt::unexpected;
#endif

#if defined(SYS_clone)
  errno = 0;
  const auto clone_result = ::syscall(SYS_clone, CLONE_NEWUSER | SIGCHLD,
                                      nullptr, nullptr, nullptr, 0);
  const auto clone_error = errno;
  if (clone_result == 0) ::_exit(71);
  if (clone_result > 0) {
    static_cast<void>(::kill(static_cast<pid_t>(clone_result), SIGKILL));
    while (::waitpid(static_cast<pid_t>(clone_result), nullptr, 0) < 0 &&
           errno == EINTR) {
    }
  }
  results[3] = classify_denial(clone_result, clone_error);
#else
  results[3] = Attempt::unexpected;
#endif
#if defined(__x86_64__) && defined(SYS_unshare)
  errno = 0;
  const auto x32_result =
      ::syscall(static_cast<long>(static_cast<std::uint32_t>(SYS_unshare) |
                                  x32_syscall_bit),
                CLONE_NEWUSER);
  const auto x32_error = errno;
  results[4] = classify_x32_denial(x32_result, x32_error);
#else
  results[4] = Attempt::denied;
#endif
  return results;
}

[[nodiscard]] auto capability_regain_attempts() -> std::array<Attempt, 2> {
  std::array<Attempt, 2> results{};
#if defined(SYS_capset)
  __user_cap_header_struct header{_LINUX_CAPABILITY_VERSION_3, 0};
  std::array<__user_cap_data_struct, 2> data{};
  data[0].effective = 1U << CAP_CHOWN;
  data[0].permitted = 1U << CAP_CHOWN;
  errno = 0;
  const auto capset_result = ::syscall(SYS_capset, &header, data.data());
  const auto capset_error = errno;
  results[0] = classify_denial(capset_result, capset_error);
  if (capset_result == 0) {
    data = {};
    if (::syscall(SYS_capset, &header, data.data()) != 0)
      results[0] = Attempt::unexpected;
  }
#else
  results[0] = Attempt::unexpected;
#endif

#if defined(PR_CAP_AMBIENT) && defined(PR_CAP_AMBIENT_RAISE) &&                \
    defined(PR_CAP_AMBIENT_CLEAR_ALL)
  errno = 0;
  const auto ambient_result =
      ::prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, CAP_CHOWN, 0, 0);
  const auto ambient_error = errno;
  results[1] = classify_denial(ambient_result, ambient_error);
  if (ambient_result == 0 &&
      ::prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0) != 0)
    results[1] = Attempt::unexpected;
#else
  results[1] = Attempt::unexpected;
#endif
  return results;
}

template <std::size_t Size>
[[nodiscard]] auto all_attempts(const std::array<Attempt, Size>& attempts,
                                const Attempt expected) -> bool {
  return std::ranges::all_of(
      attempts, [expected](const auto attempt) { return attempt == expected; });
}

[[nodiscard]] auto observe_capability_state(const unsigned int cap_last,
                                            const std::uint64_t launch_bounding)
    -> CapabilityObservation {
  CapabilityObservation observation;
  errno = 0;
  const auto no_new_privileges = ::prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0);
  const auto capabilities = capability_sets_are_empty();
  const auto ambient = ambient_set_is_empty(cap_last);
  const auto current_bounding = bounding_fingerprint(cap_last);
  const auto namespace_results = namespace_attempts();
  const auto regain_results = capability_regain_attempts();
  observation.no_new_privileges = no_new_privileges == 1;
  observation.capability_sets_empty = capabilities.value_or(false);
  observation.ambient_empty = ambient.value_or(false);
  observation.bounding_subset =
      current_bounding && ((*current_bounding & ~launch_bounding) == 0);
  observation.namespace_creation_denied =
      all_attempts(namespace_results, Attempt::denied);
  observation.capability_regain_denied =
      all_attempts(regain_results, Attempt::denied);
  observation.classified =
      (no_new_privileges == 0 || no_new_privileges == 1) && capabilities &&
      ambient && current_bounding &&
      std::ranges::none_of(
          namespace_results,
          [](const auto attempt) { return attempt == Attempt::unexpected; }) &&
      std::ranges::none_of(regain_results, [](const auto attempt) {
        return attempt == Attempt::unexpected;
      });
  return observation;
}

[[nodiscard]] auto observation_enforced(
    const CapabilityObservation& observation) -> bool {
  return observation.classified && observation.no_new_privileges &&
         observation.capability_sets_empty && observation.ambient_empty &&
         observation.bounding_subset && observation.namespace_creation_denied &&
         observation.capability_regain_denied;
}

[[nodiscard]] auto clear_capability_sets() -> bool {
#if defined(SYS_capset) && defined(PR_CAP_AMBIENT) &&                          \
    defined(PR_CAP_AMBIENT_CLEAR_ALL)
  if (::prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0) != 0)
    return false;
  __user_cap_header_struct header{_LINUX_CAPABILITY_VERSION_3, 0};
  std::array<__user_cap_data_struct, 2> data{};
  return ::syscall(SYS_capset, &header, data.data()) == 0;
#else
  return false;
#endif
}

[[nodiscard]] auto install_namespace_denial() -> bool {
#if defined(SYS_clone) && defined(SYS_clone3) && defined(SYS_setns) &&         \
    defined(SYS_unshare) && (defined(__x86_64__) || defined(__aarch64__))
#if defined(__x86_64__)
  constexpr std::uint32_t audit_architecture = AUDIT_ARCH_X86_64;
#else
  constexpr std::uint32_t audit_architecture = AUDIT_ARCH_AARCH64;
#endif
  constexpr std::uint32_t namespace_flags =
      CLONE_NEWCGROUP | CLONE_NEWIPC | CLONE_NEWNET | CLONE_NEWNS |
      CLONE_NEWPID | CLONE_NEWUSER | CLONE_NEWUTS
#if defined(CLONE_NEWTIME)
      | CLONE_NEWTIME
#endif
      ;
  std::array<sock_filter, 17> filter{{
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
               static_cast<std::uint32_t>(offsetof(seccomp_data, arch))),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, audit_architecture, 1, 0),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
               static_cast<std::uint32_t>(offsetof(seccomp_data, nr))),
      BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, filtered_x32_syscall_bit, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_unshare, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_setns, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_clone3, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_clone, 0, 3),
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
               static_cast<std::uint32_t>(offsetof(seccomp_data, args[0]))),
      BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, namespace_flags, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
  }};
  sock_fprog program{static_cast<unsigned short>(filter.size()), filter.data()};
  return ::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0;
#else
  return false;
#endif
}

[[nodiscard]] auto write_wire(const LowCapabilityWire& wire) -> bool {
  return write_all(outcome_descriptor,
                   {reinterpret_cast<const char*>(&wire), sizeof(wire)});
}

[[noreturn]] auto setup_failed(const ReasonCode reason) -> void {
  LowCapabilityWire wire;
  wire.setup_reason = static_cast<std::int32_t>(reason);
  static_cast<void>(write_wire(wire));
  ::_exit(0);
}

[[nodiscard]] auto setup_restrictions() -> ReasonCode {
  if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
    return reason_from_errno(errno);
  if (!clear_capability_sets()) return reason_from_errno(errno);
  if (!install_namespace_denial()) {
#if defined(SYS_clone) && defined(SYS_clone3) && defined(SYS_setns) &&         \
    defined(SYS_unshare) && (defined(__x86_64__) || defined(__aarch64__))
    return reason_from_errno(errno);
#else
    return supported_architecture() ? ReasonCode::unsupported_kernel
                                    : ReasonCode::unsupported_architecture;
#endif
  }
  return ReasonCode::none;
}

[[nodiscard]] auto read_wire(const int descriptor)
    -> std::optional<LowCapabilityWire> {
  LowCapabilityWire wire;
  auto* output = reinterpret_cast<std::byte*>(&wire);
  std::size_t offset{};
  const auto deadline = std::chrono::steady_clock::now() + observation_timeout;
  while (offset < sizeof(wire)) {
    const auto count =
        ::read(descriptor, output + offset, sizeof(wire) - offset);
    if (count > 0) {
      offset += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
      return std::nullopt;
    if (std::chrono::steady_clock::now() >= deadline) return std::nullopt;
    pollfd value{descriptor, POLLIN | POLLHUP, 0};
    static_cast<void>(::poll(&value, 1, 5));
  }
  return wire.magic == wire_magic && wire.version == wire_version
             ? std::optional{wire}
             : std::nullopt;
}

struct DescendantArguments {
  unsigned int cap_last{};
  std::uint64_t launch_bounding{};
  int result_descriptor{-1};
};

[[nodiscard]] auto descendant_body(void* opaque) -> int {
  const auto& arguments = *static_cast<DescendantArguments*>(opaque);
  const auto observation =
      observe_capability_state(arguments.cap_last, arguments.launch_bounding);
  const char result = !observation.classified
                          ? 'E'
                          : (observation_enforced(observation) ? 'R' : 'F');
  return ::write(arguments.result_descriptor, &result, 1) == 1 && result == 'R'
             ? 0
             : 1;
}

[[nodiscard]] auto read_descendant_result(const int descriptor) -> char {
  pollfd value{descriptor, POLLIN | POLLHUP, 0};
  int polled{};
  do {
    polled = ::poll(
        &value, 1,
        static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                             observation_timeout)
                             .count()));
  } while (polled < 0 && errno == EINTR);
  char result{'E'};
  return polled > 0 && (value.revents & POLLIN) != 0 &&
                 ::read(descriptor, &result, 1) == 1
             ? result
             : 'E';
}

[[nodiscard]] auto reap_descendant(const pid_t child) -> bool {
  int status{};
  pid_t waited{};
  do {
    waited = ::waitpid(child, &status, 0);
  } while (waited < 0 && errno == EINTR);
  return waited == child && WIFEXITED(status) &&
         (WEXITSTATUS(status) == 0 || WEXITSTATUS(status) == 1);
}

[[nodiscard]] auto verify_fork_descendant(const unsigned int cap_last,
                                          const std::uint64_t launch_bounding)
    -> std::optional<bool> {
  int result_pipe[2]{};
  if (::pipe2(result_pipe, O_CLOEXEC) != 0) return std::nullopt;
  const auto child = ::fork();
  if (child == 0) {
    static_cast<void>(::close(result_pipe[0]));
    DescendantArguments arguments{cap_last, launch_bounding, result_pipe[1]};
    ::_exit(descendant_body(&arguments));
  }
  static_cast<void>(::close(result_pipe[1]));
  const Descriptor observed{result_pipe[0]};
  if (child < 0) return std::nullopt;
  const auto marker = read_descendant_result(observed.get());
  if (marker == 'E') static_cast<void>(::kill(child, SIGKILL));
  const bool reaped = reap_descendant(child);
  if (marker == 'E' || !reaped) return std::nullopt;
  return marker == 'R';
}

[[nodiscard]] auto verify_clone_descendant(const unsigned int cap_last,
                                           const std::uint64_t launch_bounding)
    -> std::optional<bool> {
  int result_pipe[2]{};
  if (::pipe2(result_pipe, O_CLOEXEC) != 0) return std::nullopt;
  const auto allocation =
      ::mmap(nullptr, clone_stack_bytes, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
  if (allocation == MAP_FAILED) {
    static_cast<void>(::close(result_pipe[0]));
    static_cast<void>(::close(result_pipe[1]));
    return std::nullopt;
  }
  DescendantArguments arguments{cap_last, launch_bounding, result_pipe[1]};
  auto* stack = static_cast<std::byte*>(allocation) + clone_stack_bytes;
  const auto child = ::clone(descendant_body, stack, SIGCHLD, &arguments);
  static_cast<void>(::close(result_pipe[1]));
  const Descriptor observed{result_pipe[0]};
  if (child < 0) {
    static_cast<void>(::munmap(allocation, clone_stack_bytes));
    return std::nullopt;
  }
  const auto marker = read_descendant_result(observed.get());
  if (marker == 'E') static_cast<void>(::kill(child, SIGKILL));
  const bool reaped = reap_descendant(child);
  const bool unmapped = ::munmap(allocation, clone_stack_bytes) == 0;
  if (marker == 'E' || !reaped || !unmapped) return std::nullopt;
  return marker == 'R';
}

[[nodiscard]] auto checks_from_wire(const LowCapabilityWire& wire)
    -> LowCapabilityChecks {
  return {
      wire.pre_exec_verified == 1,
      wire.descriptor_exec == 1,
      wire.setup_descriptors_closed == 1,
      wire.no_new_privileges == 1,
      wire.capability_sets_empty == 1,
      wire.ambient_empty == 1,
      wire.bounding_subset == 1,
      wire.namespace_creation_denied == 1,
      wire.capability_regain_denied == 1,
      wire.fork_descendant_rechecked == 1,
      wire.clone_descendant_rechecked == 1,
  };
}

[[nodiscard]] auto result_from_wire(const LowCapabilityWire& wire)
    -> ProbeRecord {
  const auto reason = static_cast<ReasonCode>(wire.setup_reason);
  switch (reason) {
    case ReasonCode::none:
      return low_capability_outcome(checks_from_wire(wire), true);
    case ReasonCode::unsupported_kernel:
    case ReasonCode::unsupported_architecture:
    case ReasonCode::permission_denied:
    case ReasonCode::mechanism_absent:
    case ReasonCode::prerequisite_unavailable:
    case ReasonCode::unsupported_combination: return unavailable(reason);
    case ReasonCode::missing_delegation:
    case ReasonCode::missing_controller:
    case ReasonCode::enforcement_failed:
    case ReasonCode::timeout:
    case ReasonCode::cancelled:
    case ReasonCode::pid_reuse:
    case ReasonCode::setup_race:
    case ReasonCode::signaled:
    case ReasonCode::nonzero_exit:
    case ReasonCode::malformed_protocol:
    case ReasonCode::output_limit:
    case ReasonCode::cleanup_failed:
    case ReasonCode::internal_error:
      return probe_error(ReasonCode::internal_error);
  }
  return probe_error(ReasonCode::internal_error);
}

} // namespace

auto run_low_capability_payload(const std::string_view cap_last_document,
                                const std::string_view bounding_document)
    -> int {
  unsigned int cap_last{};
  std::uint64_t launch_bounding{};
  if (!parse_unsigned(cap_last_document, 10, cap_last) ||
      cap_last > maximum_capability ||
      !parse_unsigned(bounding_document, 16, launch_bounding))
    return 70;
  const auto observation = observe_capability_state(cap_last, launch_bounding);
  LowCapabilityWire wire;
  wire.pre_exec_verified = 1;
  wire.descriptor_exec = 1;
  wire.setup_descriptors_closed = 1;
  if (!observation.classified) {
    wire.setup_reason = static_cast<std::int32_t>(ReasonCode::internal_error);
    return write_wire(wire) ? 0 : 70;
  }
  wire.no_new_privileges = observation.no_new_privileges ? 1 : 0;
  wire.capability_sets_empty = observation.capability_sets_empty ? 1 : 0;
  wire.ambient_empty = observation.ambient_empty ? 1 : 0;
  wire.bounding_subset = observation.bounding_subset ? 1 : 0;
  wire.namespace_creation_denied =
      observation.namespace_creation_denied ? 1 : 0;
  wire.capability_regain_denied = observation.capability_regain_denied ? 1 : 0;
  const auto fork_recheck = verify_fork_descendant(cap_last, launch_bounding);
  const auto clone_recheck = verify_clone_descendant(cap_last, launch_bounding);
  if (!fork_recheck || !clone_recheck) {
    wire.setup_reason = static_cast<std::int32_t>(ReasonCode::internal_error);
  } else {
    wire.fork_descendant_rechecked = *fork_recheck ? 1 : 0;
    wire.clone_descendant_rechecked = *clone_recheck ? 1 : 0;
  }
  return write_wire(wire) ? 0 : 70;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Bounded setup.
auto run_low_capability_probe(const std::filesystem::path& state_directory)
    -> ProbeRecord {
  static_cast<void>(state_directory);
  if (!supported_architecture())
    return unavailable(ReasonCode::unsupported_architecture);
  const auto cap_last = read_cap_last();
  if (!cap_last) {
    return cap_last.error() == ReasonCode::internal_error
               ? probe_error(cap_last.error())
               : unavailable(cap_last.error());
  }
  const auto launch_bounding = bounding_fingerprint(*cap_last);
  if (!launch_bounding) {
    return launch_bounding.error() == ReasonCode::internal_error
               ? probe_error(launch_bounding.error())
               : unavailable(launch_bounding.error());
  }
  const Descriptor executable{::open("/proc/self/exe", O_PATH | O_CLOEXEC)};
  int outcome[2]{};
  if (executable.get() < 0 || ::pipe2(outcome, O_CLOEXEC | O_NONBLOCK) != 0)
    return probe_error(ReasonCode::internal_error);
  const Descriptor observed{outcome[0]};
  Descriptor writer{outcome[1]};
  const auto child = ::fork();
  if (child < 0) return probe_error(ReasonCode::internal_error);
  if (child == 0) {
    const Descriptor pinned_executable{
        ::fcntl(executable.get(), F_DUPFD_CLOEXEC, 8)};
    const Descriptor pinned_writer{::fcntl(writer.get(), F_DUPFD_CLOEXEC, 9)};
    if (pinned_executable.get() < 0 || pinned_writer.get() < 0 ||
        ::dup3(pinned_writer.get(), outcome_descriptor, 0) < 0 ||
        ::dup3(pinned_executable.get(), executable_descriptor, O_CLOEXEC) < 0 ||
        !close_descriptors_from(5))
      setup_failed(ReasonCode::internal_error);
    if (const auto reason = setup_restrictions(); reason != ReasonCode::none)
      setup_failed(reason);
    const auto pre_exec = observe_capability_state(*cap_last, *launch_bounding);
    if (!pre_exec.classified) setup_failed(ReasonCode::internal_error);
    if (!observation_enforced(pre_exec)) {
      LowCapabilityWire wire;
      static_cast<void>(write_wire(wire));
      ::_exit(0);
    }
    std::array<char, 4> cap_last_argument{};
    std::array<char, 17> bounding_argument{};
    const auto cap_last_encoded = std::to_chars(
        cap_last_argument.data(),
        cap_last_argument.data() + cap_last_argument.size(), *cap_last);
    const auto bounding_encoded =
        std::to_chars(bounding_argument.data(),
                      bounding_argument.data() + bounding_argument.size(),
                      *launch_bounding, 16);
    if (cap_last_encoded.ec != std::errc{} ||
        bounding_encoded.ec != std::errc{})
      setup_failed(ReasonCode::internal_error);
    *cap_last_encoded.ptr = '\0';
    *bounding_encoded.ptr = '\0';
    std::array<char, 39> executable_name{"aiforge_process_isolation_probe_v3"};
    std::array<char, 25> mode{"--low-capability-payload"};
    std::array<char*, 5> arguments{executable_name.data(), mode.data(),
                                   cap_last_argument.data(),
                                   bounding_argument.data(), nullptr};
    char* environment[]{nullptr};
    ::fexecve(executable_descriptor, arguments.data(), environment);
    setup_failed(ReasonCode::internal_error);
  }
  writer.reset();
  const auto wire = read_wire(observed.get());
  if (!wire) static_cast<void>(::kill(child, SIGKILL));
  int status{};
  pid_t waited{};
  do {
    waited = ::waitpid(child, &status, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited != child) return probe_error(ReasonCode::cleanup_failed);
  if (!wire) return probe_error(ReasonCode::setup_race);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    return probe_error(ReasonCode::internal_error);
  return result_from_wire(*wire);
}

#if defined(AIFORGE_PROCESS_ISOLATION_TEST_SUPPORT)
namespace test_support {

auto low_capability_outcome(const LowCapabilityChecks& checks,
                            const bool cleanup_complete) -> ProbeRecord {
  const ::aiforge::evaluation::process_isolation::v3::LowCapabilityChecks value{
      checks.pre_exec_verified,
      checks.descriptor_exec,
      checks.setup_descriptors_closed,
      checks.no_new_privileges,
      checks.capability_sets_empty,
      checks.ambient_empty,
      checks.bounding_subset,
      checks.namespace_creation_denied,
      checks.capability_regain_denied,
      checks.fork_descendant_rechecked,
      checks.clone_descendant_rechecked,
  };
  return ::aiforge::evaluation::process_isolation::v3::low_capability_outcome(
      value, cleanup_complete);
}

auto capability_prerequisite_outcome(const bool architecture,
                                     const bool cap_last_readable,
                                     const bool cap_last_bounded)
    -> ProbeRecord {
  if (!architecture) return unavailable(ReasonCode::unsupported_architecture);
  if (!cap_last_readable || !cap_last_bounded)
    return unavailable(ReasonCode::unsupported_kernel);
  return enforced();
}

auto cap_last_outcome(const std::string_view document) -> ProbeRecord {
  unsigned int cap_last{};
  return parse_unsigned(document, 10, cap_last) &&
                 cap_last <= maximum_capability
             ? enforced()
             : unavailable(ReasonCode::unsupported_kernel);
}

auto bounding_subset_outcome(const std::uint64_t launch,
                             const std::uint64_t current) -> ProbeRecord {
  return (current & ~launch) == 0 ? enforced()
                                  : unavailable(ReasonCode::enforcement_failed);
}

auto bounding_read_outcome(const int error_number) -> ProbeRecord {
  if (error_number == 0) return enforced();
  const auto reason = reason_from_errno(error_number);
  return reason == ReasonCode::internal_error ? probe_error(reason)
                                              : unavailable(reason);
}

auto x32_namespace_outcome(const long result, const int error_number)
    -> ProbeRecord {
  const auto attempt = classify_x32_denial(result, error_number);
  if (attempt == Attempt::denied) return enforced();
  return attempt == Attempt::escaped
             ? unavailable(ReasonCode::enforcement_failed)
             : probe_error(ReasonCode::internal_error);
}

} // namespace test_support
#endif

} // namespace aiforge::evaluation::process_isolation::v3
