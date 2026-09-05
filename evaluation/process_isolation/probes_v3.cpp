#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "probes_v3.hpp"

#include "linux_support.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/sched.h>
#include <linux/seccomp.h>
#include <poll.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace aiforge::evaluation::process_isolation::v3 {
namespace {

constexpr auto probe = ProbeId::direct_process_tree_cgroup_nonescape;
constexpr auto task_cgroup_prefix = std::string_view{"aiforge-evidence-v3-"};
constexpr auto observation_timeout = std::chrono::seconds{2};
constexpr std::uint32_t wire_magic = 0x33464941U;
constexpr std::uint32_t wire_version = 1;
constexpr std::size_t process_count = 5;
constexpr std::size_t path_attempt_count = 4;
constexpr std::size_t descriptor_attempt_count = 8;
constexpr std::size_t clone_attempt_count = 4;
constexpr std::size_t clone_stack_bytes = 64UZ * 1024UZ;

constexpr int outcome_descriptor = 3;
constexpr int executable_descriptor = 4;
constexpr int parent_read_descriptor = 5;
constexpr int parent_path_descriptor = 6;
constexpr int sibling_read_descriptor = 7;
constexpr int sibling_path_descriptor = 8;

using Cgroup = linux_support::TaskCgroup;
using linux_support::close_descriptors_from;
using linux_support::Descriptor;
using linux_support::pidfd_kill;
using linux_support::pidfd_open;
using linux_support::read_control;
using linux_support::write_all;

struct DirectTreeWire {
  std::uint32_t magic{wire_magic};
  std::uint32_t version{wire_version};
  std::int32_t setup_reason{static_cast<std::int32_t>(ReasonCode::none)};
  std::int32_t descriptor_exec{};
  std::int32_t setup_descriptors_closed{};
  std::array<std::int32_t, path_attempt_count> path_errors{};
  std::array<std::int32_t, descriptor_attempt_count> descriptor_errors{};
  std::array<std::int32_t, clone_attempt_count> clone_errors{};
  std::array<std::int32_t, process_count> processes{};
  std::int32_t thread{};
  std::int32_t session_detached{};
  std::int32_t reparented_descendant{};
};

struct DirectTreeChecks {
  bool payload_identity{};
  bool descriptor_exec{};
  bool setup_descriptors_closed{};
  bool path_procs_denied{};
  bool path_threads_denied{};
  bool readable_descriptor_procs_denied{};
  bool readable_descriptor_threads_denied{};
  bool path_descriptor_procs_denied{};
  bool path_descriptor_threads_denied{};
  bool clone_into_parent_denied{};
  bool clone_into_sibling_denied{};
  bool fork_created{};
  bool clone_created{};
  bool thread_created{};
  bool session_detached{};
  bool reparented_descendant_created{};
  bool processes_contained{};
  bool threads_contained{};
};

static_assert(sizeof(DirectTreeWire) <= 256);

[[nodiscard]] auto enforced() -> ProbeRecord {
  return {probe, ProbeState::enforced, ReasonCode::none};
}

[[nodiscard]] auto unavailable(const ReasonCode reason) -> ProbeRecord {
  return {probe, ProbeState::unavailable, reason};
}

[[nodiscard]] auto probe_error(const ReasonCode reason) -> ProbeRecord {
  return {probe, ProbeState::probe_error, reason};
}

[[nodiscard]] auto cgroup_reason(const linux_support::TaskCgroupError error)
    -> ReasonCode {
  switch (error) {
    case linux_support::TaskCgroupError::missing_delegation:
      return ReasonCode::missing_delegation;
    case linux_support::TaskCgroupError::mechanism_absent:
      return ReasonCode::mechanism_absent;
    case linux_support::TaskCgroupError::prerequisite_unavailable:
      return ReasonCode::prerequisite_unavailable;
    case linux_support::TaskCgroupError::internal_error:
      return ReasonCode::internal_error;
  }
  return ReasonCode::internal_error;
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
    case EBUSY:
    case ENOSPC:
    case EUSERS:
    case ENOMEM:
    case EAGAIN: return ReasonCode::prerequisite_unavailable;
    default: return ReasonCode::internal_error;
  }
}

[[nodiscard]] auto setup_reason(const DirectTreeWire& wire)
    -> std::optional<ReasonCode> {
  const auto value = static_cast<ReasonCode>(wire.setup_reason);
  switch (value) {
    case ReasonCode::none: return std::nullopt;
    case ReasonCode::unsupported_kernel:
    case ReasonCode::unsupported_architecture:
    case ReasonCode::permission_denied:
    case ReasonCode::mechanism_absent:
    case ReasonCode::missing_delegation:
    case ReasonCode::missing_controller:
    case ReasonCode::enforcement_failed:
    case ReasonCode::prerequisite_unavailable:
    case ReasonCode::unsupported_combination: return value;
    case ReasonCode::timeout:
    case ReasonCode::cancelled:
    case ReasonCode::pid_reuse:
    case ReasonCode::setup_race:
    case ReasonCode::signaled:
    case ReasonCode::nonzero_exit:
    case ReasonCode::malformed_protocol:
    case ReasonCode::output_limit:
    case ReasonCode::cleanup_failed:
    case ReasonCode::internal_error: return ReasonCode::internal_error;
  }
  return ReasonCode::internal_error;
}

[[nodiscard]] auto valid_state_directory(const std::filesystem::path& path)
    -> bool {
  struct stat attributes{};
  return path.is_absolute() && ::lstat(path.c_str(), &attributes) == 0 &&
         S_ISDIR(attributes.st_mode) && !S_ISLNK(attributes.st_mode) &&
         attributes.st_uid == ::geteuid() &&
         (attributes.st_mode & (S_IRWXG | S_IRWXO)) == 0;
}

[[nodiscard]] auto current_cgroup_path()
    -> std::optional<std::filesystem::path> {
  const Descriptor proc{
      ::open("/proc", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
  if (proc.get() < 0) return std::nullopt;
  const auto own_process = std::to_string(::getpid());
  const Descriptor proc_self{
      ::openat(proc.get(), own_process.c_str(),
               O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
  if (proc_self.get() < 0) return std::nullopt;
  const auto document =
      linux_support::read_control(proc_self.get(), "cgroup", 4096);
  if (!document || !document->starts_with("0::/") ||
      document->find('\n') != document->size() - 1) {
    return std::nullopt;
  }
  auto path = std::filesystem::path{document->substr(3, document->size() - 4)};
  if (!path.is_absolute()) return std::nullopt;
  for (const auto& component : path) {
    const auto& value = component.native();
    if (value == "/") continue;
    if (value.empty() || value == "." || value == ".." || value.size() > 255)
      return std::nullopt;
  }
  return path;
}

[[nodiscard]] auto apply_write_confinement(const std::filesystem::path& state)
    -> ReasonCode {
#if defined(SYS_landlock_create_ruleset) && defined(SYS_landlock_add_rule) &&  \
    defined(SYS_landlock_restrict_self) &&                                     \
    defined(LANDLOCK_CREATE_RULESET_VERSION) &&                                \
    defined(LANDLOCK_ACCESS_FS_WRITE_FILE)
  const auto abi = ::syscall(SYS_landlock_create_ruleset, nullptr, 0,
                             LANDLOCK_CREATE_RULESET_VERSION);
  if (abi < 0) return reason_from_errno(errno);
  landlock_ruleset_attr ruleset{};
  ruleset.handled_access_fs = LANDLOCK_ACCESS_FS_WRITE_FILE;
  const Descriptor ruleset_descriptor{static_cast<int>(
      ::syscall(SYS_landlock_create_ruleset, &ruleset, sizeof(ruleset), 0))};
  const Descriptor state_descriptor{
      ::open(state.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
  if (ruleset_descriptor.get() < 0 || state_descriptor.get() < 0)
    return reason_from_errno(errno);
  landlock_path_beneath_attr rule{};
  rule.allowed_access = LANDLOCK_ACCESS_FS_WRITE_FILE;
  rule.parent_fd = state_descriptor.get();
  if (::syscall(SYS_landlock_add_rule, ruleset_descriptor.get(),
                LANDLOCK_RULE_PATH_BENEATH, &rule, 0) != 0 ||
      ::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
      ::syscall(SYS_landlock_restrict_self, ruleset_descriptor.get(), 0) != 0)
    return reason_from_errno(errno);
  return ReasonCode::none;
#else
  static_cast<void>(state);
  return ReasonCode::mechanism_absent;
#endif
}

[[nodiscard]] auto install_clone3_denial() -> ReasonCode {
#if defined(SYS_clone3) && (defined(__x86_64__) || defined(__aarch64__))
#if defined(__x86_64__)
  constexpr std::uint32_t audit_architecture = AUDIT_ARCH_X86_64;
#else
  constexpr std::uint32_t audit_architecture = AUDIT_ARCH_AARCH64;
#endif
  std::array<sock_filter, 7> filter{{
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
               static_cast<std::uint32_t>(offsetof(seccomp_data, arch))),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, audit_architecture, 1, 0),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
               static_cast<std::uint32_t>(offsetof(seccomp_data, nr))),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_clone3, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
  }};
  sock_fprog program{static_cast<unsigned short>(filter.size()), filter.data()};
  if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
      ::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) != 0)
    return reason_from_errno(errno);
  return ReasonCode::none;
#else
  return supported_architecture() ? ReasonCode::unsupported_kernel
                                  : ReasonCode::unsupported_architecture;
#endif
}

[[nodiscard]] auto write_wire(const DirectTreeWire& wire) -> bool {
  return write_all(outcome_descriptor,
                   {reinterpret_cast<const char*>(&wire), sizeof(wire)});
}

[[noreturn]] auto setup_failed(const ReasonCode reason) -> void {
  DirectTreeWire wire;
  wire.setup_reason = static_cast<std::int32_t>(reason);
  static_cast<void>(write_wire(wire));
  ::_exit(0);
}

[[nodiscard]] auto duplicate_sources(const std::array<int, 6>& sources)
    -> std::optional<std::array<Descriptor, 6>> {
  std::array<Descriptor, 6> pinned;
  for (std::size_t index{}; index < sources.size(); ++index) {
    pinned[index].reset(
        ::fcntl(sources[index], F_DUPFD_CLOEXEC, 16 + static_cast<int>(index)));
    if (pinned[index].get() < 0) return std::nullopt;
  }
  return pinned;
}

[[nodiscard]] auto spawn_payload(const Cgroup& task, const Cgroup& sibling,
                                 const std::filesystem::path& state,
                                 const int outcome, const int executable)
    -> std::expected<pid_t, ReasonCode> {
#if defined(SYS_clone3) && defined(CLONE_INTO_CGROUP)
  const Descriptor parent_path{
      ::openat(task.parent(), ".", O_PATH | O_DIRECTORY | O_CLOEXEC)};
  const Descriptor sibling_path{
      ::openat(sibling.child(), ".", O_PATH | O_DIRECTORY | O_CLOEXEC)};
  if (parent_path.get() < 0 || sibling_path.get() < 0)
    return std::unexpected(reason_from_errno(errno));
  const std::array sources{outcome,         executable,
                           task.parent(),   parent_path.get(),
                           sibling.child(), sibling_path.get()};
  auto pinned = duplicate_sources(sources);
  if (!pinned) return std::unexpected(ReasonCode::internal_error);
  clone_args arguments{};
  arguments.flags = CLONE_INTO_CGROUP;
  arguments.exit_signal = SIGCHLD;
  arguments.cgroup = static_cast<std::uint64_t>(task.child());
  const auto child =
      static_cast<pid_t>(::syscall(SYS_clone3, &arguments, sizeof(arguments)));
  if (child > 0) return child;
  if (child < 0) return std::unexpected(reason_from_errno(errno));

  const std::array targets{outcome_descriptor,      executable_descriptor,
                           parent_read_descriptor,  parent_path_descriptor,
                           sibling_read_descriptor, sibling_path_descriptor};
  for (std::size_t index{}; index < pinned->size(); ++index) {
    const auto flags = targets[index] == executable_descriptor ? O_CLOEXEC : 0;
    if (::dup3((*pinned)[index].get(), targets[index], flags) < 0)
      setup_failed(ReasonCode::internal_error);
  }
  if (!close_descriptors_from(9)) setup_failed(ReasonCode::internal_error);
  if (const auto reason = apply_write_confinement(state);
      reason != ReasonCode::none)
    setup_failed(reason);
  if (const auto reason = install_clone3_denial(); reason != ReasonCode::none)
    setup_failed(reason);
  std::array<std::string, 3> arguments_storage{
      "aiforge_process_isolation_probe_v3", "--direct-tree-payload",
      std::string{sibling.name()}};
  std::array<char*, 4> raw_arguments{arguments_storage[0].data(),
                                     arguments_storage[1].data(),
                                     arguments_storage[2].data(), nullptr};
  char* environment[]{nullptr};
  ::fexecve(executable_descriptor, raw_arguments.data(), environment);
  setup_failed(ReasonCode::internal_error);
#else
  static_cast<void>(task);
  static_cast<void>(sibling);
  static_cast<void>(state);
  static_cast<void>(outcome);
  static_cast<void>(executable);
  return std::unexpected(ReasonCode::unsupported_kernel);
#endif
}

[[nodiscard]] auto control_write_errno(const std::filesystem::path& directory,
                                       const char* control,
                                       const std::string_view value) -> int {
  const auto path = directory / control;
  const Descriptor descriptor{
      ::open(path.c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW)};
  if (descriptor.get() < 0) return errno;
  errno = 0;
  return write_all(descriptor.get(), value) ? 0 : errno;
}

[[nodiscard]] auto control_write_errno(const int directory, const char* control,
                                       const std::string_view value) -> int {
  const Descriptor descriptor{
      ::openat(directory, control, O_WRONLY | O_CLOEXEC | O_NOFOLLOW)};
  if (descriptor.get() < 0) return errno;
  errno = 0;
  return write_all(descriptor.get(), value) ? 0 : errno;
}

[[nodiscard]] auto clone_into_errno(const int cgroup) -> int {
#if defined(SYS_clone3) && defined(CLONE_INTO_CGROUP)
  clone_args arguments{};
  arguments.flags = CLONE_INTO_CGROUP;
  arguments.exit_signal = SIGCHLD;
  arguments.cgroup = static_cast<std::uint64_t>(cgroup);
  errno = 0;
  const auto child =
      static_cast<pid_t>(::syscall(SYS_clone3, &arguments, sizeof(arguments)));
  if (child == 0) ::_exit(0);
  if (child > 0) {
    static_cast<void>(::kill(child, SIGKILL));
    while (::waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
    }
    return 0;
  }
  return errno;
#else
  static_cast<void>(cgroup);
  return ENOSYS;
#endif
}

auto record_escape_attempts(DirectTreeWire& wire,
                            const std::filesystem::path& parent,
                            const std::filesystem::path& sibling) -> void {
  const auto process = std::to_string(::getpid());
#if defined(SYS_gettid)
  const auto thread = std::to_string(::syscall(SYS_gettid));
#else
  const auto thread = process;
#endif
  wire.path_errors = {
      control_write_errno(parent, "cgroup.procs", process),
      control_write_errno(parent, "cgroup.threads", thread),
      control_write_errno(sibling, "cgroup.procs", process),
      control_write_errno(sibling, "cgroup.threads", thread),
  };
  const std::array borrowed{parent_read_descriptor, parent_path_descriptor,
                            sibling_read_descriptor, sibling_path_descriptor};
  for (std::size_t index{}; index < borrowed.size(); ++index) {
    wire.descriptor_errors[index * 2] =
        control_write_errno(borrowed[index], "cgroup.procs", process);
    wire.descriptor_errors[(index * 2) + 1] =
        control_write_errno(borrowed[index], "cgroup.threads", thread);
    wire.clone_errors[index] = clone_into_errno(borrowed[index]);
  }
}

[[nodiscard]] auto all_denied(const DirectTreeWire& wire) -> bool;

[[noreturn]] auto pause_forever() -> void {
  for (;;)
    ::pause();
}

[[nodiscard]] auto allocate_stack() -> void* {
  const auto allocation =
      ::mmap(nullptr, clone_stack_bytes, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
  return allocation == MAP_FAILED ? nullptr : allocation;
}

[[nodiscard]] auto wait_for_marker(const int descriptor) -> bool {
  pollfd value{descriptor, POLLIN | POLLHUP, 0};
  int polled{};
  do {
    polled = ::poll(
        &value, 1,
        static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                             observation_timeout)
                             .count()));
  } while (polled < 0 && errno == EINTR);
  char marker{};
  return polled > 0 && (value.revents & POLLIN) != 0 &&
         ::read(descriptor, &marker, 1) == 1 && marker == 'R';
}

struct DescendantArguments {
  const std::filesystem::path* parent{};
  const std::filesystem::path* sibling{};
  int readiness{-1};
  bool detach_session{};
  bool verify_confinement{true};
};

[[nodiscard]] auto confined_descendant_body(void* opaque) -> int {
  auto& arguments = *static_cast<DescendantArguments*>(opaque);
  DirectTreeWire observation;
  const bool detached = !arguments.detach_session || ::setsid() >= 0;
  if (arguments.verify_confinement)
    record_escape_attempts(observation, *arguments.parent, *arguments.sibling);
  const char marker =
      detached && (!arguments.verify_confinement || all_denied(observation))
          ? 'R'
          : 'F';
  const bool sent = ::write(arguments.readiness, &marker, 1) == 1;
  static_cast<void>(::close(arguments.readiness));
  if (!sent || marker != 'R') return 1;
  pause_forever();
}

[[nodiscard]] auto spawn_legacy_clone(const int flags,
                                      const std::filesystem::path& parent,
                                      const std::filesystem::path& sibling,
                                      const bool detach_session = false,
                                      const bool verify_confinement = true)
    -> pid_t {
  int readiness[2]{};
  if (::pipe2(readiness, O_CLOEXEC) != 0) return -1;
  auto* allocation = allocate_stack();
  if (allocation == nullptr) {
    static_cast<void>(::close(readiness[0]));
    static_cast<void>(::close(readiness[1]));
    return -1;
  }
  auto* top = static_cast<std::byte*>(allocation) + clone_stack_bytes;
  DescendantArguments arguments{&parent, &sibling, readiness[1], detach_session,
                                verify_confinement};
  const auto child = ::clone(confined_descendant_body, top, flags, &arguments);
  static_cast<void>(::close(readiness[1]));
  const Descriptor observed{readiness[0]};
  return child > 0 && wait_for_marker(observed.get()) ? child : -1;
}

[[nodiscard]] auto spawn_fork_child(const std::filesystem::path& parent,
                                    const std::filesystem::path& sibling,
                                    const bool detach_session, bool& ready)
    -> pid_t {
  int readiness[2]{};
  if (::pipe2(readiness, O_CLOEXEC) != 0) return -1;
  const auto child = ::fork();
  if (child == 0) {
    static_cast<void>(::close(readiness[0]));
    DescendantArguments arguments{&parent, &sibling, readiness[1],
                                  detach_session, true};
    ::_exit(confined_descendant_body(&arguments));
  }
  static_cast<void>(::close(readiness[1]));
  const Descriptor observed{readiness[0]};
  ready = child > 0 && wait_for_marker(observed.get());
  return ready ? child : -1;
}

[[nodiscard]] auto spawn_reparented_descendant(
    const std::filesystem::path& parent, const std::filesystem::path& sibling,
    bool& ready) -> pid_t {
  int readiness[2]{};
  if (::pipe2(readiness, O_CLOEXEC) != 0) return -1;
  const auto intermediate = ::fork();
  if (intermediate < 0) {
    static_cast<void>(::close(readiness[0]));
    static_cast<void>(::close(readiness[1]));
    return -1;
  }
  if (intermediate == 0) {
    static_cast<void>(::close(readiness[0]));
    const auto descendant = ::fork();
    if (descendant < 0) ::_exit(1);
    if (descendant > 0) ::_exit(0);
    if (::setsid() < 0) ::_exit(1);
    DirectTreeWire observation;
    record_escape_attempts(observation, parent, sibling);
    if (!all_denied(observation)) ::_exit(1);
    const auto own_pid = static_cast<std::int32_t>(::getpid());
    if (::write(readiness[1], &own_pid, sizeof(own_pid)) != sizeof(own_pid))
      ::_exit(1);
    static_cast<void>(::close(readiness[1]));
    pause_forever();
  }
  static_cast<void>(::close(readiness[1]));
  const Descriptor observed{readiness[0]};
  std::int32_t descendant{-1};
  pollfd value{observed.get(), POLLIN | POLLHUP, 0};
  int polled{};
  do {
    polled = ::poll(
        &value, 1,
        static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                             observation_timeout)
                             .count()));
  } while (polled < 0 && errno == EINTR);
  const bool observed_pid = polled > 0 && (value.revents & POLLIN) != 0 &&
                            ::read(observed.get(), &descendant,
                                   sizeof(descendant)) == sizeof(descendant) &&
                            descendant > 0;
  int status{};
  pid_t waited{};
  do {
    waited = ::waitpid(intermediate, &status, 0);
  } while (waited < 0 && errno == EINTR);
  ready = intermediate > 0 && observed_pid && waited == intermediate &&
          WIFEXITED(status) && WEXITSTATUS(status) == 0;
  return ready ? static_cast<pid_t>(descendant) : -1;
}

[[nodiscard]] auto denied(const int error_number) -> bool {
  return error_number == EACCES || error_number == EPERM;
}

[[nodiscard]] auto all_denied(const DirectTreeWire& wire) -> bool {
  return std::ranges::all_of(wire.path_errors, denied) &&
         std::ranges::all_of(wire.descriptor_errors, denied) &&
         std::ranges::all_of(wire.clone_errors,
                             [](const int value) { return value == EPERM; });
}

[[nodiscard]] auto classified_attempt(const int error_number) -> bool {
  return error_number == 0 || denied(error_number);
}

[[nodiscard]] auto observations_are_classified(const DirectTreeWire& wire)
    -> bool {
  return std::ranges::all_of(wire.path_errors, classified_attempt) &&
         std::ranges::all_of(wire.descriptor_errors, classified_attempt) &&
         std::ranges::all_of(wire.clone_errors, [](const int value) {
           return value == 0 || value == EPERM;
         });
}

[[nodiscard]] auto escape_attempt_outcome(const DirectTreeWire& wire)
    -> ProbeRecord {
  if (!observations_are_classified(wire))
    return probe_error(ReasonCode::internal_error);
  return all_denied(wire) ? enforced()
                          : unavailable(ReasonCode::enforcement_failed);
}

[[nodiscard]] auto read_wire(const int descriptor)
    -> std::optional<DirectTreeWire> {
  DirectTreeWire wire;
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

[[nodiscard]] auto contains_exactly(std::vector<pid_t> actual,
                                    std::span<const std::int32_t> expected)
    -> bool {
  if (actual.size() != expected.size() ||
      std::ranges::any_of(expected,
                          [](const auto value) { return value <= 0; }))
    return false;
  std::ranges::sort(actual);
  std::vector<pid_t> wanted;
  wanted.reserve(expected.size());
  std::ranges::transform(
      expected, std::back_inserter(wanted),
      [](const auto value) { return static_cast<pid_t>(value); });
  std::ranges::sort(wanted);
  return actual == wanted && std::ranges::adjacent_find(wanted) == wanted.end();
}

[[nodiscard]] auto direct_checks(const DirectTreeWire& wire, const Cgroup& task,
                                 const pid_t payload) -> DirectTreeChecks {
  const auto processes = task.processes();
  const auto threads_document = read_control(task.child(), "cgroup.threads");
  const auto threads = threads_document
                           ? linux_support::parse_processes(*threads_document)
                           : std::nullopt;
  std::array<std::int32_t, process_count + 1> expected_threads{};
  std::ranges::copy(wire.processes, expected_threads.begin());
  expected_threads.back() = wire.thread;
  return {
      wire.processes[0] == payload,
      wire.descriptor_exec == 1,
      wire.setup_descriptors_closed == 1,
      denied(wire.path_errors[0]) && denied(wire.path_errors[2]),
      denied(wire.path_errors[1]) && denied(wire.path_errors[3]),
      denied(wire.descriptor_errors[0]) && denied(wire.descriptor_errors[4]),
      denied(wire.descriptor_errors[1]) && denied(wire.descriptor_errors[5]),
      denied(wire.descriptor_errors[2]) && denied(wire.descriptor_errors[6]),
      denied(wire.descriptor_errors[3]) && denied(wire.descriptor_errors[7]),
      wire.clone_errors[0] == EPERM && wire.clone_errors[1] == EPERM,
      wire.clone_errors[2] == EPERM && wire.clone_errors[3] == EPERM,
      wire.processes[1] > 0,
      wire.processes[2] > 0,
      wire.thread > 0,
      wire.session_detached == 1,
      wire.reparented_descendant == 1,
      processes && contains_exactly(*processes, wire.processes),
      threads && contains_exactly(*threads, expected_threads),
  };
}

[[nodiscard]] auto direct_tree_outcome(const DirectTreeChecks& checks,
                                       const bool cleanup_complete)
    -> ProbeRecord {
  if (!cleanup_complete) return probe_error(ReasonCode::cleanup_failed);
  const bool complete =
      checks.payload_identity && checks.descriptor_exec &&
      checks.setup_descriptors_closed && checks.path_procs_denied &&
      checks.path_threads_denied && checks.readable_descriptor_procs_denied &&
      checks.readable_descriptor_threads_denied &&
      checks.path_descriptor_procs_denied &&
      checks.path_descriptor_threads_denied &&
      checks.clone_into_parent_denied && checks.clone_into_sibling_denied &&
      checks.fork_created && checks.clone_created && checks.thread_created &&
      checks.session_detached && checks.reparented_descendant_created &&
      checks.processes_contained && checks.threads_contained;
  return complete ? enforced() : unavailable(ReasonCode::enforcement_failed);
}

[[nodiscard]] auto reap_all_children() -> bool {
  const auto deadline = std::chrono::steady_clock::now() + observation_timeout;
  for (;;) {
    int status{};
    const auto child = ::waitpid(-1, &status, WNOHANG);
    if (child > 0) continue;
    if (child < 0 && errno == ECHILD) return true;
    if (child < 0 && errno != EINTR) return false;
    if (std::chrono::steady_clock::now() >= deadline) return false;
    static_cast<void>(::poll(nullptr, 0, 5));
  }
}

// NOLINTBEGIN(readability-function-cognitive-complexity) -- Lifecycle
// finalization.
[[nodiscard]] auto run_direct_tree_probe(const std::filesystem::path& state)
    -> ProbeRecord {
  if (!supported_architecture())
    return unavailable(ReasonCode::unsupported_architecture);
  auto task = Cgroup::create(task_cgroup_prefix);
  if (!task) {
    const auto reason = cgroup_reason(task.error());
    return reason == ReasonCode::internal_error ? probe_error(reason)
                                                : unavailable(reason);
  }
  const auto controllers = task->has_required_controllers();
  if (!controllers || !*controllers) {
    auto result = controllers ? unavailable(ReasonCode::missing_controller)
                              : probe_error(ReasonCode::internal_error);
    if (!task->cleanup()) result = probe_error(ReasonCode::cleanup_failed);
    return result;
  }
  auto sibling = Cgroup::create(task_cgroup_prefix, "-sibling");
  if (!sibling) {
    const auto reason = cgroup_reason(sibling.error());
    auto result = reason == ReasonCode::internal_error ? probe_error(reason)
                                                       : unavailable(reason);
    if (!task->cleanup()) result = probe_error(ReasonCode::cleanup_failed);
    return result;
  }
  int previous_subreaper{};
  if (::prctl(PR_GET_CHILD_SUBREAPER, &previous_subreaper) != 0 ||
      (previous_subreaper == 0 && ::prctl(PR_SET_CHILD_SUBREAPER, 1) != 0)) {
    auto result = unavailable(reason_from_errno(errno));
    const bool task_cleaned = task->cleanup();
    const bool sibling_cleaned = sibling->cleanup();
    if (!task_cleaned || !sibling_cleaned)
      result = probe_error(ReasonCode::cleanup_failed);
    return result;
  }
  int outcome[2]{};
  const Descriptor executable{::open("/proc/self/exe", O_PATH | O_CLOEXEC)};
  if (::pipe2(outcome, O_CLOEXEC | O_NONBLOCK) != 0) {
    auto result = probe_error(ReasonCode::internal_error);
    const bool task_cleaned = task->cleanup();
    const bool sibling_cleaned = sibling->cleanup();
    const bool subreaper_restored =
        previous_subreaper != 0 || ::prctl(PR_SET_CHILD_SUBREAPER, 0) == 0;
    if (!task_cleaned || !sibling_cleaned || !subreaper_restored)
      result = probe_error(ReasonCode::cleanup_failed);
    return result;
  }
  const Descriptor observed{outcome[0]};
  Descriptor outcome_writer{outcome[1]};
  if (executable.get() < 0) {
    auto result = probe_error(ReasonCode::internal_error);
    const bool task_cleaned = task->cleanup();
    const bool sibling_cleaned = sibling->cleanup();
    const bool subreaper_restored =
        previous_subreaper != 0 || ::prctl(PR_SET_CHILD_SUBREAPER, 0) == 0;
    if (!task_cleaned || !sibling_cleaned || !subreaper_restored)
      result = probe_error(ReasonCode::cleanup_failed);
    return result;
  }
  const auto payload = spawn_payload(*task, *sibling, state,
                                     outcome_writer.get(), executable.get());
  outcome_writer.reset();
  if (!payload) {
    auto result = unavailable(payload.error());
    const bool task_cleaned = task->cleanup();
    const bool sibling_cleaned = sibling->cleanup();
    const bool reaped = reap_all_children();
    const bool subreaper_restored =
        previous_subreaper != 0 || ::prctl(PR_SET_CHILD_SUBREAPER, 0) == 0;
    if (!task_cleaned || !sibling_cleaned || !reaped || !subreaper_restored)
      result = probe_error(ReasonCode::cleanup_failed);
    return result;
  }
  auto identity = pidfd_open(*payload);
  const auto identity_reason = identity.get() < 0
                                   ? std::optional{reason_from_errno(errno)}
                                   : std::nullopt;
  const auto wire = read_wire(observed.get());
  ProbeRecord result = probe_error(ReasonCode::setup_race);
  if (wire) {
    if (const auto reason = setup_reason(*wire)) {
      result = *reason == ReasonCode::internal_error ? probe_error(*reason)
                                                     : unavailable(*reason);
    } else {
      const auto attempts = escape_attempt_outcome(*wire);
      result =
          attempts.state == ProbeState::enforced
              ? direct_tree_outcome(direct_checks(*wire, *task, *payload), true)
              : attempts;
    }
  }
  const bool identity_killed = identity.get() < 0 || pidfd_kill(identity.get());
  const bool task_cleaned = task->cleanup();
  const bool sibling_cleaned = sibling->cleanup();
  const bool reaped = reap_all_children();
  const bool subreaper_restored =
      previous_subreaper != 0 || ::prctl(PR_SET_CHILD_SUBREAPER, 0) == 0;
  if (!identity_killed || !task_cleaned || !sibling_cleaned || !reaped ||
      !subreaper_restored)
    return probe_error(ReasonCode::cleanup_failed);
  if (identity_reason) {
    return *identity_reason == ReasonCode::internal_error
               ? probe_error(*identity_reason)
               : unavailable(*identity_reason);
  }
  return result;
}
// NOLINTEND(readability-function-cognitive-complexity)

} // namespace

auto run_direct_tree_payload(const std::string_view sibling_name) -> int {
  try {
    DirectTreeWire wire;
    wire.descriptor_exec = 1;
    wire.setup_descriptors_closed = 1;
    const auto own_cgroup = current_cgroup_path();
    if (!own_cgroup || !own_cgroup->has_parent_path() || sibling_name.empty() ||
        sibling_name.size() > 255) {
      wire.setup_reason = static_cast<std::int32_t>(ReasonCode::internal_error);
      return write_wire(wire) ? 0 : 70;
    }
    const auto parent = std::filesystem::path{"/sys/fs/cgroup"} /
                        own_cgroup->parent_path().relative_path();
    const auto sibling = parent / sibling_name;
    record_escape_attempts(wire, parent, sibling);
    if (!all_denied(wire)) {
      static_cast<void>(write_wire(wire));
      pause_forever();
    }

    wire.processes[0] = static_cast<std::int32_t>(::getpid());
    bool fork_ready{};
    wire.processes[1] = static_cast<std::int32_t>(
        spawn_fork_child(parent, sibling, false, fork_ready));
    wire.processes[2] =
        static_cast<std::int32_t>(spawn_legacy_clone(SIGCHLD, parent, sibling));
    bool session_ready{};
    wire.processes[3] = static_cast<std::int32_t>(
        spawn_fork_child(parent, sibling, true, session_ready));
    bool reparented_ready{};
    wire.processes[4] = static_cast<std::int32_t>(
        spawn_reparented_descendant(parent, sibling, reparented_ready));
    wire.session_detached = session_ready ? 1 : 0;
    wire.reparented_descendant = reparented_ready ? 1 : 0;
    const bool process_tree_ready = fork_ready && wire.processes[2] > 0 &&
                                    session_ready && reparented_ready;
    // The raw shared-VM thread is last: after it exists this process performs
    // only the bounded outcome write and waits for evaluator cleanup.
    wire.thread = process_tree_ready
                      ? static_cast<std::int32_t>(spawn_legacy_clone(
                            CLONE_VM | CLONE_SIGHAND | CLONE_THREAD, parent,
                            sibling, false, false))
                      : -1;
    if (!write_wire(wire)) return 70;
    pause_forever();
  } catch (...) {
    return 70;
  }
}

auto run_probe(const ProbeId probe_id,
               const std::filesystem::path& state_directory,
               const bool has_delegated_cgroup_root) -> ProbeRecord {
  try {
    if (!valid_state_directory(state_directory))
      return {probe_id, ProbeState::probe_error, ReasonCode::internal_error};
    if (probe_id != probe)
      return {probe_id, ProbeState::unavailable,
              ReasonCode::prerequisite_unavailable};
    if (!has_delegated_cgroup_root)
      return unavailable(ReasonCode::missing_delegation);
    struct stat root_attributes{};
    if (::fstat(4, &root_attributes) != 0 || !S_ISDIR(root_attributes.st_mode))
      return probe_error(ReasonCode::internal_error);
    return run_direct_tree_probe(state_directory);
  } catch (...) {
    return {probe_id, ProbeState::probe_error, ReasonCode::internal_error};
  }
}

#if defined(AIFORGE_PROCESS_ISOLATION_TEST_SUPPORT)
namespace test_support {

auto direct_tree_outcome(const DirectTreeChecks& checks,
                         const bool cleanup_complete) -> ProbeRecord {
  const ::aiforge::evaluation::process_isolation::v3::DirectTreeChecks value{
      checks.payload_identity,
      checks.descriptor_exec,
      checks.setup_descriptors_closed,
      checks.path_procs_denied,
      checks.path_threads_denied,
      checks.readable_descriptor_procs_denied,
      checks.readable_descriptor_threads_denied,
      checks.path_descriptor_procs_denied,
      checks.path_descriptor_threads_denied,
      checks.clone_into_parent_denied,
      checks.clone_into_sibling_denied,
      checks.fork_created,
      checks.clone_created,
      checks.thread_created,
      checks.session_detached,
      checks.reparented_descendant_created,
      checks.processes_contained,
      checks.threads_contained,
  };
  return ::aiforge::evaluation::process_isolation::v3::direct_tree_outcome(
      value, cleanup_complete);
}

auto prerequisite_outcome(const bool architecture, const bool delegated,
                          const bool controllers_available) -> ProbeRecord {
  if (!architecture) return unavailable(ReasonCode::unsupported_architecture);
  if (!delegated) return unavailable(ReasonCode::missing_delegation);
  if (!controllers_available)
    return unavailable(ReasonCode::missing_controller);
  return enforced();
}

auto escape_attempt_outcome(const std::array<int, 4>& path_errors,
                            const std::array<int, 8>& descriptor_errors,
                            const std::array<int, 4>& clone_errors)
    -> ProbeRecord {
  DirectTreeWire wire;
  wire.path_errors = path_errors;
  wire.descriptor_errors = descriptor_errors;
  wire.clone_errors = clone_errors;
  return ::aiforge::evaluation::process_isolation::v3::escape_attempt_outcome(
      wire);
}

} // namespace test_support
#endif

} // namespace aiforge::evaluation::process_isolation::v3
