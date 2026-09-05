#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "probes_v2.hpp"

#include "probes.hpp"

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
#include <functional>
#include <optional>
#include <ranges>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/sched.h>
#include <linux/seccomp.h>
#include <poll.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace aiforge::evaluation::process_isolation::v2 {
namespace {

constexpr int assertion_enforced = 0;
constexpr int assertion_permission_denied = 20;
constexpr int assertion_mechanism_absent = 21;
constexpr int assertion_failed = 22;
constexpr int assertion_internal_error = 23;
constexpr int assertion_prerequisite_unavailable = 24;
constexpr int assertion_limit_not_triggered = 25;
constexpr auto observation_timeout = std::chrono::seconds{2};
constexpr std::size_t maximum_control_bytes = 64UZ * 1024UZ;

[[nodiscard]] auto apply_landlock(
    const std::filesystem::path& state, std::uint64_t handled,
    std::span<const std::filesystem::path> additional_roots = {}) -> int;

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

[[nodiscard]] auto migration_observation(const bool confinement_applied,
                                         const int parent_error,
                                         const int sibling_error)
    -> ProbeRecord {
  if (!confinement_applied)
    return unavailable(ProbeId::cgroup_self_migration_denial,
                       ReasonCode::unsupported_combination);
  const auto denied = [](const int error_number) {
    return error_number == EACCES || error_number == EPERM;
  };
  if (denied(parent_error) && denied(sibling_error))
    return enforced(ProbeId::cgroup_self_migration_denial);
  if (parent_error == 0 || sibling_error == 0)
    return unavailable(ProbeId::cgroup_self_migration_denial,
                       ReasonCode::enforcement_failed);
  return probe_error(ProbeId::cgroup_self_migration_denial,
                     ReasonCode::internal_error);
}

[[nodiscard]] auto memory_limit_observation(const bool killed,
                                            const bool oom_kill_advanced)
    -> ProbeRecord {
  return killed && oom_kill_advanced
             ? enforced(ProbeId::cgroup_memory_limit_termination)
             : unavailable(ProbeId::cgroup_memory_limit_termination,
                           ReasonCode::limit_not_triggered);
}

[[nodiscard]] auto pids_limit_observation(const bool exhausted,
                                          const bool tree_complete,
                                          const bool cleanup_complete)
    -> ProbeRecord {
  if (!cleanup_complete)
    return probe_error(ProbeId::cgroup_pids_limit_enforcement,
                       ReasonCode::cleanup_failed);
  return exhausted && tree_complete
             ? enforced(ProbeId::cgroup_pids_limit_enforcement)
             : unavailable(ProbeId::cgroup_pids_limit_enforcement,
                           ReasonCode::limit_not_triggered);
}

[[nodiscard]] auto execute_confinement_observation(const bool local_executed,
                                                   const bool outside_denied)
    -> ProbeRecord {
  return local_executed && outside_denied
             ? enforced(ProbeId::landlock_execute_confinement)
             : unavailable(ProbeId::landlock_execute_confinement,
                           ReasonCode::enforcement_failed);
}

[[nodiscard]] auto mount_propagation_observation(
    const bool child_mount_established, const bool visible_in_parent,
    const bool cleanup_complete) -> ProbeRecord {
  if (!cleanup_complete)
    return probe_error(ProbeId::private_mount_propagation,
                       ReasonCode::cleanup_failed);
  return child_mount_established && !visible_in_parent
             ? enforced(ProbeId::private_mount_propagation)
             : unavailable(ProbeId::private_mount_propagation,
                           ReasonCode::enforcement_failed);
}

[[nodiscard]] auto cancellation_cleanup_observation(
    const bool tree_ready, const bool cancellation_requested,
    const bool tree_terminated, const bool cleanup_complete) -> ProbeRecord {
  if (!cleanup_complete)
    return probe_error(ProbeId::cgroup_cancellation_cleanup,
                       ReasonCode::cleanup_failed);
  if (!tree_ready || !cancellation_requested)
    return probe_error(ProbeId::cgroup_cancellation_cleanup,
                       ReasonCode::setup_race);
  return tree_terminated ? enforced(ProbeId::cgroup_cancellation_cleanup)
                         : probe_error(ProbeId::cgroup_cancellation_cleanup,
                                       ReasonCode::cleanup_failed);
}

[[nodiscard]] auto write_confinement_observation(
    const bool allowed_write_succeeded, const bool existing_write_denied,
    const bool truncation_denied, const bool removal_denied,
    const bool creation_denied, const bool rename_denied) -> ProbeRecord {
  return allowed_write_succeeded && existing_write_denied &&
                 truncation_denied && removal_denied && creation_denied &&
                 rename_denied
             ? enforced(ProbeId::landlock_write_confinement)
             : unavailable(ProbeId::landlock_write_confinement,
                           ReasonCode::enforcement_failed);
}

[[nodiscard]] auto setup_order_observation(
    const bool limits_applied, const bool placed, const bool staged_descriptors,
    const bool descriptor_launched, const bool private_root_applied,
    const bool filesystem_applied, const bool internet_denied,
    const bool unix_denied, const bool payload_reached,
    const bool target_waiting_for_cleanup) -> ProbeRecord {
  if (!limits_applied || !placed || !staged_descriptors ||
      !descriptor_launched || !private_root_applied || !filesystem_applied ||
      !internet_denied || !unix_denied)
    return unavailable(ProbeId::combined_setup_order,
                       ReasonCode::unsupported_combination);
  return payload_reached && target_waiting_for_cleanup
             ? enforced(ProbeId::combined_setup_order)
             : probe_error(ProbeId::combined_setup_order,
                           ReasonCode::setup_race);
}

[[nodiscard]] auto confinement_denied(const int error_number) -> bool {
  return error_number == EACCES || error_number == EPERM;
}

[[nodiscard]] auto complete_landlock_access() -> std::optional<std::uint64_t> {
#if defined(LANDLOCK_ACCESS_FS_EXECUTE) &&                                     \
    defined(LANDLOCK_ACCESS_FS_WRITE_FILE) &&                                  \
    defined(LANDLOCK_ACCESS_FS_READ_FILE) &&                                   \
    defined(LANDLOCK_ACCESS_FS_READ_DIR) &&                                    \
    defined(LANDLOCK_ACCESS_FS_REMOVE_DIR) &&                                  \
    defined(LANDLOCK_ACCESS_FS_REMOVE_FILE) &&                                 \
    defined(LANDLOCK_ACCESS_FS_MAKE_CHAR) &&                                   \
    defined(LANDLOCK_ACCESS_FS_MAKE_DIR) &&                                    \
    defined(LANDLOCK_ACCESS_FS_MAKE_REG) &&                                    \
    defined(LANDLOCK_ACCESS_FS_MAKE_SOCK) &&                                   \
    defined(LANDLOCK_ACCESS_FS_MAKE_FIFO) &&                                   \
    defined(LANDLOCK_ACCESS_FS_MAKE_BLOCK) &&                                  \
    defined(LANDLOCK_ACCESS_FS_MAKE_SYM) &&                                    \
    defined(LANDLOCK_ACCESS_FS_REFER) && defined(LANDLOCK_ACCESS_FS_TRUNCATE)
  return LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE |
         LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR |
         LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE |
         LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR |
         LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SOCK |
         LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK |
         LANDLOCK_ACCESS_FS_MAKE_SYM | LANDLOCK_ACCESS_FS_REFER |
         LANDLOCK_ACCESS_FS_TRUNCATE;
#else
  return std::nullopt;
#endif
}

[[nodiscard]] auto unavailable_from_errno(const ProbeId id,
                                          const int error_number)
    -> ProbeRecord {
  switch (error_number) {
    case ENOSYS:
    case EINVAL:
    case EOPNOTSUPP: return unavailable(id, ReasonCode::unsupported_kernel);
    case EACCES:
    case EPERM: return unavailable(id, ReasonCode::permission_denied);
    case ENOENT:
    case ENODEV:
    case ENOPROTOOPT: return unavailable(id, ReasonCode::mechanism_absent);
    case EBUSY:
    case ENOSPC:
    case EUSERS:
    case ENOMEM: return unavailable(id, ReasonCode::prerequisite_unavailable);
    default: return probe_error(id, ReasonCode::internal_error);
  }
}

[[nodiscard]] auto assertion_exit_for_errno(const int error_number) -> int {
  switch (error_number) {
    case ENOSYS:
    case EINVAL:
    case EOPNOTSUPP:
    case ENOENT:
    case ENODEV:
    case ENOPROTOOPT: return assertion_mechanism_absent;
    case EACCES:
    case EPERM: return assertion_permission_denied;
    case EBUSY:
    case ENOSPC:
    case EUSERS:
    case ENOMEM: return assertion_prerequisite_unavailable;
    default: return assertion_internal_error;
  }
}

[[nodiscard]] auto assertion_result(const ProbeId id, const int result)
    -> ProbeRecord {
  switch (result) {
    case assertion_enforced: return enforced(id);
    case assertion_permission_denied:
      return unavailable(id, ReasonCode::permission_denied);
    case assertion_mechanism_absent:
      return unavailable(id, ReasonCode::mechanism_absent);
    case assertion_failed:
      return unavailable(id, ReasonCode::enforcement_failed);
    case assertion_prerequisite_unavailable:
      return unavailable(id, ReasonCode::prerequisite_unavailable);
    case assertion_limit_not_triggered:
      return unavailable(id, ReasonCode::limit_not_triggered);
    default: return probe_error(id, ReasonCode::internal_error);
  }
}

template <typename Assertion>
[[nodiscard]] auto isolated_assertion(const ProbeId id, Assertion assertion)
    -> ProbeRecord {
  const auto child = ::fork();
  if (child < 0) return probe_error(id, ReasonCode::internal_error);
  if (child == 0) ::_exit(assertion());
  int status{};
  pid_t waited{};
  do {
    waited = ::waitpid(child, &status, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited != child) return probe_error(id, ReasonCode::cleanup_failed);
  if (WIFSIGNALED(status)) return probe_error(id, ReasonCode::signaled);
  if (!WIFEXITED(status)) return probe_error(id, ReasonCode::internal_error);
  return assertion_result(id, WEXITSTATUS(status));
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
    const auto value = m_value;
    m_value = -1;
    return value;
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
    if (count == 0) break;
    if (count < 0) {
      if (errno == EINTR) continue;
      return std::nullopt;
    }
    if (result.size() + static_cast<std::size_t>(count) > maximum_control_bytes)
      return std::nullopt;
    result.append(buffer.data(), static_cast<std::size_t>(count));
  }
  return result;
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
  const Descriptor descriptor{
      ::open("/proc/self/cgroup", O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
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
    if (document.size() + static_cast<std::size_t>(count) > 4096)
      return std::nullopt;
    document.append(buffer.data(), static_cast<std::size_t>(count));
  }
  if (!document.starts_with("0::/") ||
      document.find('\n') != document.size() - 1)
    return std::nullopt;
  auto path = std::filesystem::path{document.substr(3, document.size() - 4)};
  if (!path.is_absolute()) return std::nullopt;
  for (const auto& component : path) {
    const auto& value = component.native();
    if (value == "/") continue;
    if (value.empty() || value == "." || value == ".." || value.size() > 255)
      return std::nullopt;
  }
  return path;
}

[[nodiscard]] auto write_process_to(const std::filesystem::path& cgroup,
                                    const std::string_view pid) -> bool {
  const auto control = cgroup / "cgroup.procs";
  const Descriptor descriptor{
      ::open(control.c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW)};
  return descriptor.get() >= 0 && write_all(descriptor.get(), pid);
}

[[nodiscard]] auto parse_pids(const std::string_view document)
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
        value > INT_MAX) {
      return std::nullopt;
    }
    result.push_back(static_cast<pid_t>(value));
    cursor = parsed.ptr;
    if (cursor != end && *cursor != '\n') return std::nullopt;
  }
  std::ranges::sort(result);
  if (std::ranges::adjacent_find(result) != result.end()) return std::nullopt;
  return result;
}

[[nodiscard]] auto cgroup_is_populated(const int directory)
    -> std::optional<bool> {
  const auto events = read_control(directory, "cgroup.events");
  if (!events) return std::nullopt;
  if (events->find("populated 0") != std::string::npos) return false;
  if (events->find("populated 1") != std::string::npos) return true;
  return std::nullopt;
}

[[nodiscard]] auto pidfd_open(const pid_t process) -> Descriptor {
#if defined(SYS_pidfd_open)
  return Descriptor{static_cast<int>(::syscall(SYS_pidfd_open, process, 0U))};
#else
  static_cast<void>(process);
  return Descriptor{};
#endif
}

[[nodiscard]] auto pidfd_kill(const int descriptor) -> bool {
#if defined(SYS_pidfd_send_signal)
  return descriptor >= 0 && (::syscall(SYS_pidfd_send_signal, descriptor,
                                       SIGKILL, nullptr, 0U) == 0 ||
                             errno == ESRCH);
#else
  static_cast<void>(descriptor);
  return false;
#endif
}

[[nodiscard]] auto pidfd_dead(const int descriptor) -> bool {
  pollfd value{descriptor, POLLIN, 0};
  int polled{};
  do {
    polled = ::poll(&value, 1, 0);
  } while (polled < 0 && errno == EINTR);
  return polled == 1 && (value.revents & POLLIN) != 0;
}

[[nodiscard]] auto close_descriptors_from(const unsigned int first) -> bool {
#if defined(SYS_close_range)
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

class Cgroup final {
 public:
  [[nodiscard]] static auto create(const std::string_view suffix = {})
      -> std::expected<Cgroup, ReasonCode> {
    Descriptor parent{::fcntl(4, F_DUPFD_CLOEXEC, 5)};
    if (parent.get() < 0)
      return std::unexpected(ReasonCode::missing_delegation);
    const auto marker = read_control(parent.get(), "cgroup.controllers");
    if (!marker) return std::unexpected(ReasonCode::mechanism_absent);
    const auto name = "aiforge-evidence-v2-" + std::to_string(::getpid()) +
                      std::string{suffix};
    if (::mkdirat(parent.get(), name.c_str(), S_IRWXU) != 0) {
      return std::unexpected(errno == EACCES || errno == EPERM || errno == EROFS
                                 ? ReasonCode::missing_delegation
                                 : ReasonCode::prerequisite_unavailable);
    }
    Descriptor child{::openat(parent.get(), name.c_str(),
                              O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    if (child.get() < 0) {
      static_cast<void>(::unlinkat(parent.get(), name.c_str(), AT_REMOVEDIR));
      return std::unexpected(ReasonCode::internal_error);
    }
    return Cgroup{std::move(parent), std::move(child), name};
  }

  Cgroup(const Cgroup&) = delete;
  auto operator=(const Cgroup&) -> Cgroup& = delete;
  Cgroup(Cgroup&& other) noexcept
      : m_parent(std::move(other.m_parent)), m_child(std::move(other.m_child)),
        m_name(std::move(other.m_name)), m_cleaned(other.m_cleaned) {
    other.m_cleaned = true;
  }
  auto operator=(Cgroup&& other) noexcept -> Cgroup& {
    if (this == &other) return *this;
    if (!m_cleaned) static_cast<void>(cleanup());
    m_parent = std::move(other.m_parent);
    m_child = std::move(other.m_child);
    m_name = std::move(other.m_name);
    m_cleaned = other.m_cleaned;
    other.m_cleaned = true;
    return *this;
  }
  ~Cgroup() {
    if (!m_cleaned) static_cast<void>(cleanup());
  }

  [[nodiscard]] auto parent() const noexcept -> int { return m_parent.get(); }
  [[nodiscard]] auto child() const noexcept -> int { return m_child.get(); }
  [[nodiscard]] auto name() const noexcept -> std::string_view {
    return m_name;
  }

  [[nodiscard]] auto has_required_controllers() const -> std::optional<bool> {
    for (const auto* control : {"cpu.max", "memory.max", "pids.max"}) {
      struct stat attributes{};
      if (::fstatat(m_child.get(), control, &attributes, AT_SYMLINK_NOFOLLOW) !=
          0) {
        if (errno == ENOENT) return false;
        return std::nullopt;
      }
      if (!S_ISREG(attributes.st_mode)) return std::nullopt;
    }
    return true;
  }

  [[nodiscard]] auto processes() const -> std::optional<std::vector<pid_t>> {
    const auto value = read_control(m_child.get(), "cgroup.procs");
    return value ? parse_pids(*value) : std::nullopt;
  }

  [[nodiscard]] auto cleanup() -> bool {
    if (m_cleaned) return true;
    bool trustworthy{true};
    if (!write_control(m_child.get(), "cgroup.kill", "1")) {
      const auto processes = this->processes();
      if (!processes) {
        trustworthy = false;
      } else {
        for (const auto process : *processes) {
          auto pidfd = pidfd_open(process);
          if (pidfd.get() < 0 || !pidfd_kill(pidfd.get())) trustworthy = false;
        }
      }
    }
    const auto deadline =
        std::chrono::steady_clock::now() + observation_timeout;
    bool empty{};
    do {
      const auto populated = cgroup_is_populated(m_child.get());
      if (populated && !*populated) {
        empty = true;
        break;
      }
      static_cast<void>(::poll(nullptr, 0, 5));
    } while (std::chrono::steady_clock::now() < deadline);
    m_child.reset();
    const bool removed =
        ::unlinkat(m_parent.get(), m_name.c_str(), AT_REMOVEDIR) == 0;
    m_cleaned = true;
    return trustworthy && empty && removed;
  }

 private:
  Cgroup(Descriptor parent, Descriptor child, std::string name)
      : m_parent(std::move(parent)), m_child(std::move(child)),
        m_name(std::move(name)) {}
  Descriptor m_parent;
  Descriptor m_child;
  std::string m_name;
  bool m_cleaned{};
};

[[nodiscard]] auto cgroup_or_record(const ProbeId id)
    -> std::expected<Cgroup, ProbeRecord> {
  auto cgroup = Cgroup::create();
  if (!cgroup) {
    const auto reason = cgroup.error();
    if (reason == ReasonCode::internal_error)
      return std::unexpected(probe_error(id, reason));
    return std::unexpected(unavailable(id, reason));
  }
  return std::move(*cgroup);
}

[[nodiscard]] auto required_cgroup_or_record(const ProbeId id)
    -> std::expected<Cgroup, ProbeRecord> {
  auto cgroup = cgroup_or_record(id);
  if (!cgroup) return std::unexpected(cgroup.error());
  const auto controllers = cgroup->has_required_controllers();
  if (!controllers) {
    return std::unexpected(probe_error(id, ReasonCode::internal_error));
  }
  if (!*controllers) {
    return std::unexpected(unavailable(id, ReasonCode::missing_controller));
  }
  return std::move(*cgroup);
}

[[nodiscard]] auto atomic_child(const int cgroup_descriptor,
                                const std::function<int()>& body,
                                const int preserved_descriptor = -1)
    -> std::expected<pid_t, ReasonCode> {
#if defined(SYS_clone3) && defined(CLONE_INTO_CGROUP)
  clone_args arguments{};
  arguments.flags = CLONE_INTO_CGROUP;
  arguments.exit_signal = SIGCHLD;
  arguments.cgroup = static_cast<std::uint64_t>(cgroup_descriptor);
  const auto result =
      static_cast<pid_t>(::syscall(SYS_clone3, &arguments, sizeof(arguments)));
  if (result == 0) {
    if (preserved_descriptor >= 0) {
      if (preserved_descriptor != 3 && ::dup3(preserved_descriptor, 3, 0) < 0)
        ::_exit(assertion_internal_error);
      if (preserved_descriptor == 3 && ::fcntl(3, F_SETFD, 0) != 0)
        ::_exit(assertion_internal_error);
      if (!close_descriptors_from(4)) ::_exit(assertion_internal_error);
    } else if (!close_descriptors_from(3)) {
      ::_exit(assertion_internal_error);
    }
    ::_exit(body());
  }
  if (result > 0) return result;
  if (errno == ENOSYS || errno == EINVAL || errno == E2BIG)
    return std::unexpected(ReasonCode::unsupported_kernel);
  if (errno == EACCES || errno == EPERM)
    return std::unexpected(ReasonCode::permission_denied);
  return std::unexpected(ReasonCode::prerequisite_unavailable);
#else
  static_cast<void>(cgroup_descriptor);
  static_cast<void>(body);
  static_cast<void>(preserved_descriptor);
  return std::unexpected(ReasonCode::unsupported_kernel);
#endif
}

[[nodiscard]] auto atomic_combined_payload(const int cgroup_descriptor,
                                           const int readiness_descriptor,
                                           const int executable_descriptor,
                                           const int input_descriptor,
                                           const int output_descriptor,
                                           const std::filesystem::path& state)
    -> std::expected<pid_t, ReasonCode> {
#if defined(SYS_clone3) && defined(CLONE_INTO_CGROUP)
  const std::array sources{readiness_descriptor, executable_descriptor,
                           input_descriptor, output_descriptor};
  std::array<Descriptor, 4> pinned;
  for (std::size_t index{}; index < sources.size(); ++index) {
    pinned[index].reset(
        ::fcntl(sources[index], F_DUPFD_CLOEXEC, 16 + static_cast<int>(index)));
    if (pinned[index].get() < 0)
      return std::unexpected(ReasonCode::internal_error);
  }
  std::array<std::string, 3> arguments{
      std::string{"aiforge_process_isolation_probe_v2"},
      std::string{"--combined-setup-payload"}, state.string()};
  std::array<char*, 4> raw_arguments{arguments[0].data(), arguments[1].data(),
                                     arguments[2].data(), nullptr};
  clone_args clone_arguments{};
  clone_arguments.flags = CLONE_INTO_CGROUP;
  clone_arguments.exit_signal = SIGCHLD;
  clone_arguments.cgroup = static_cast<std::uint64_t>(cgroup_descriptor);
  const auto result = static_cast<pid_t>(
      ::syscall(SYS_clone3, &clone_arguments, sizeof(clone_arguments)));
  if (result == 0) {
    for (std::size_t index{}; index < pinned.size(); ++index) {
      const auto target = 3 + static_cast<int>(index);
      const auto flags = index == 1 ? O_CLOEXEC : 0;
      if (::dup3(pinned[index].get(), target, flags) < 0)
        ::_exit(assertion_internal_error);
    }
    if (!close_descriptors_from(7)) ::_exit(assertion_internal_error);
    char* environment[]{nullptr};
    ::fexecve(4, raw_arguments.data(), environment);
    ::_exit(assertion_exit_for_errno(errno));
  }
  if (result > 0) return result;
  if (errno == ENOSYS || errno == EINVAL || errno == E2BIG)
    return std::unexpected(ReasonCode::unsupported_kernel);
  if (errno == EACCES || errno == EPERM)
    return std::unexpected(ReasonCode::permission_denied);
  return std::unexpected(ReasonCode::prerequisite_unavailable);
#else
  static_cast<void>(cgroup_descriptor);
  static_cast<void>(readiness_descriptor);
  static_cast<void>(executable_descriptor);
  static_cast<void>(input_descriptor);
  static_cast<void>(output_descriptor);
  static_cast<void>(state);
  return std::unexpected(ReasonCode::unsupported_kernel);
#endif
}

[[nodiscard]] auto wait_for_byte(const int descriptor) -> bool {
  pollfd ready{descriptor, POLLIN | POLLHUP, 0};
  int result{};
  do {
    result = ::poll(
        &ready, 1,
        static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                             observation_timeout)
                             .count()));
  } while (result < 0 && errno == EINTR);
  if (result <= 0 || (ready.revents & POLLIN) == 0) return false;
  char marker{};
  return ::read(descriptor, &marker, 1) == 1 && marker == 'R';
}

auto reap_all_children() -> bool {
  bool okay{true};
  const auto deadline = std::chrono::steady_clock::now() + observation_timeout;
  for (;;) {
    int status{};
    const auto waited = ::waitpid(-1, &status, WNOHANG);
    if (waited > 0) continue;
    if (waited == 0) {
      if (std::chrono::steady_clock::now() >= deadline) return false;
      static_cast<void>(::poll(nullptr, 0, 5));
      continue;
    }
    if (errno == EINTR) continue;
    if (errno == ECHILD) break;
    okay = false;
    break;
  }
  return okay;
}

enum class TreeShape {
  simple,
  setsid,
  double_fork,
  daemon,
  fanout,
  leader_exit
};

[[noreturn]] auto pause_forever() -> void {
  for (;;)
    ::pause();
}

[[nodiscard]] auto signal_tree_ready() -> bool {
  constexpr int write_descriptor = 3;
  const bool result = write_all(write_descriptor, "R");
  static_cast<void>(::close(write_descriptor));
  return result;
}

[[nodiscard]] auto run_detached_tree_child(const TreeShape shape) -> int {
  const auto intermediate = ::fork();
  if (intermediate < 0) return assertion_internal_error;
  if (intermediate > 0) return assertion_enforced;
  if (::setsid() < 0) return assertion_internal_error;
  const auto daemon = ::fork();
  if (daemon < 0) return assertion_internal_error;
  if (daemon > 0) return assertion_enforced;
  if (shape == TreeShape::daemon) {
    static_cast<void>(::chdir("/"));
    ::umask(0);
  }
  if (!signal_tree_ready()) return assertion_internal_error;
  pause_forever();
}

[[nodiscard]] auto run_fanout_tree_child() -> int {
  constexpr int write_descriptor = 3;
  for (int index{}; index < 4; ++index) {
    const auto descendant = ::fork();
    if (descendant < 0) return assertion_internal_error;
    if (descendant == 0) {
      static_cast<void>(::close(write_descriptor));
      pause_forever();
    }
  }
  return assertion_enforced;
}

[[nodiscard]] auto run_tree_child(const TreeShape shape) -> int {
  static_cast<void>(::close(STDIN_FILENO));
  if (shape == TreeShape::setsid && ::setsid() < 0)
    return assertion_internal_error;
  if (shape == TreeShape::double_fork || shape == TreeShape::daemon)
    return run_detached_tree_child(shape);
  if (shape == TreeShape::fanout &&
      run_fanout_tree_child() != assertion_enforced)
    return assertion_internal_error;
  if (shape == TreeShape::leader_exit) {
    const auto descendant = ::fork();
    if (descendant < 0) return assertion_internal_error;
    if (descendant > 0) return assertion_enforced;
  }
  if (!signal_tree_ready()) return assertion_internal_error;
  pause_forever();
}

[[nodiscard]] auto spawn_tree(Cgroup& cgroup, const TreeShape shape,
                              int& readiness_descriptor)
    -> std::expected<pid_t, ReasonCode> {
  int readiness[2]{};
  if (::pipe2(readiness, O_CLOEXEC) != 0)
    return std::unexpected(ReasonCode::internal_error);
  readiness_descriptor = readiness[0];
  const auto child = atomic_child(
      cgroup.child(), [shape] { return run_tree_child(shape); }, readiness[1]);
  static_cast<void>(::close(readiness[1]));
  if (!child) {
    static_cast<void>(::close(readiness[0]));
    readiness_descriptor = -1;
  }
  return child;
}

[[nodiscard]] auto await_cgroup_tree_death(
    const std::vector<Descriptor>& identities, const int cgroup_descriptor)
    -> std::optional<bool> {
  const auto deadline = std::chrono::steady_clock::now() + observation_timeout;
  bool all_dead{};
  bool empty{};
  do {
    all_dead = std::ranges::all_of(identities, [](const Descriptor& value) {
      return pidfd_dead(value.get());
    });
    const auto populated = cgroup_is_populated(cgroup_descriptor);
    if (!populated) return std::nullopt;
    empty = !*populated;
    if (all_dead && empty) break;
    static_cast<void>(::poll(nullptr, 0, 5));
  } while (std::chrono::steady_clock::now() < deadline);
  return all_dead && empty;
}

[[nodiscard]] auto terminate_cgroup_tree(const ProbeId id, Cgroup& cgroup,
                                         const bool require_kill_file,
                                         const std::size_t minimum_processes)
    -> ProbeRecord {
  const auto processes = cgroup.processes();
  if (!processes) return probe_error(id, ReasonCode::malformed_protocol);
  if (processes->size() < minimum_processes)
    return unavailable(id, ReasonCode::enforcement_failed);
  std::vector<Descriptor> identities;
  identities.reserve(processes->size());
  for (const auto process : *processes) {
    auto pidfd = pidfd_open(process);
    if (pidfd.get() < 0) return probe_error(id, ReasonCode::pid_reuse);
    identities.push_back(std::move(pidfd));
  }
  const bool killed = write_control(cgroup.child(), "cgroup.kill", "1");
  if (!killed && require_kill_file) return unavailable_from_errno(id, errno);
  if (!killed) {
    for (const auto& identity : identities) {
      if (!pidfd_kill(identity.get()))
        return probe_error(id, ReasonCode::cleanup_failed);
    }
  }
  const auto tree_dead = await_cgroup_tree_death(identities, cgroup.child());
  if (!tree_dead) return probe_error(id, ReasonCode::malformed_protocol);
  if (!reap_all_children()) return probe_error(id, ReasonCode::cleanup_failed);
  if (!*tree_dead) return probe_error(id, ReasonCode::cleanup_failed);
  return enforced(id);
}

[[nodiscard]] auto tree_probe(const ProbeId id, const TreeShape shape,
                              const std::size_t minimum_processes = 1,
                              const bool require_kill_file = true,
                              const bool cancellation_probe = false)
    -> ProbeRecord {
  auto cgroup = required_cgroup_or_record(id);
  if (!cgroup) return cgroup.error();
  int previous_subreaper{};
  if (::prctl(PR_GET_CHILD_SUBREAPER, &previous_subreaper) != 0 ||
      (previous_subreaper == 0 && ::prctl(PR_SET_CHILD_SUBREAPER, 1) != 0)) {
    return unavailable_from_errno(id, errno);
  }
  int readiness{-1};
  const auto leader = spawn_tree(*cgroup, shape, readiness);
  if (!leader) {
    const bool cgroup_cleanup_complete = cgroup->cleanup();
    const bool subreaper_restored =
        previous_subreaper != 0 || ::prctl(PR_SET_CHILD_SUBREAPER, 0) == 0;
    return cgroup_cleanup_complete && subreaper_restored
               ? unavailable(id, leader.error())
               : probe_error(id, ReasonCode::cleanup_failed);
  }
  const Descriptor ready{readiness};
  const bool tree_ready = wait_for_byte(ready.get());
  std::stop_source cancellation;
  if (tree_ready && cancellation_probe) cancellation.request_stop();
  auto termination =
      tree_ready && (!cancellation_probe || cancellation.stop_requested())
          ? terminate_cgroup_tree(id, *cgroup, require_kill_file,
                                  minimum_processes)
          : probe_error(id, ReasonCode::setup_race);
  const bool cgroup_cleanup_complete = cgroup->cleanup();
  const bool subreaper_restored =
      previous_subreaper != 0 || ::prctl(PR_SET_CHILD_SUBREAPER, 0) == 0;
  const bool cleanup_complete = cgroup_cleanup_complete && subreaper_restored;
  auto result =
      cancellation_probe
          ? cancellation_cleanup_observation(
                tree_ready, cancellation.stop_requested(),
                termination.state == ProbeState::enforced, cleanup_complete)
          : termination;
  if (!cancellation_probe && !cleanup_complete)
    result = probe_error(id, ReasonCode::cleanup_failed);
  result.probe_id = id;
  return result;
}

[[nodiscard]] auto map_v1_record(
    const ProbeId id,
    const aiforge::evaluation::process_isolation::ProbeRecord& record)
    -> ProbeRecord {
  if (record.state == ProbeState::enforced) return enforced(id);
  if (record.state == ProbeState::probe_error) {
    return probe_error(id, record.reason ==
                                   aiforge::evaluation::process_isolation::
                                       ReasonCode::cleanup_failed
                               ? ReasonCode::cleanup_failed
                               : ReasonCode::internal_error);
  }
  switch (record.reason) {
    case aiforge::evaluation::process_isolation::ReasonCode::unsupported_kernel:
      return unavailable(id, ReasonCode::unsupported_kernel);
    case aiforge::evaluation::process_isolation::ReasonCode::
        unsupported_architecture:
      return unavailable(id, ReasonCode::unsupported_architecture);
    case aiforge::evaluation::process_isolation::ReasonCode::permission_denied:
      return unavailable(id, ReasonCode::permission_denied);
    case aiforge::evaluation::process_isolation::ReasonCode::mechanism_absent:
      return unavailable(id, ReasonCode::mechanism_absent);
    case aiforge::evaluation::process_isolation::ReasonCode::enforcement_failed:
      return unavailable(id, ReasonCode::enforcement_failed);
    case aiforge::evaluation::process_isolation::ReasonCode::
        prerequisite_unavailable:
      return unavailable(id, ReasonCode::prerequisite_unavailable);
    default: return probe_error(id, ReasonCode::internal_error);
  }
}

[[nodiscard]] auto cgroup_delegation_probe(const ProbeId id) -> ProbeRecord {
  auto cgroup = cgroup_or_record(id);
  if (!cgroup) return cgroup.error();
  return cgroup->cleanup() ? enforced(id)
                           : probe_error(id, ReasonCode::cleanup_failed);
}

[[nodiscard]] auto cgroup_controller_probe(const ProbeId id) -> ProbeRecord {
  auto cgroup = cgroup_or_record(id);
  if (!cgroup) return cgroup.error();
  const auto controllers = cgroup->has_required_controllers();
  auto result =
      !controllers
          ? probe_error(id, ReasonCode::internal_error)
          : (*controllers ? enforced(id)
                          : unavailable(id, ReasonCode::missing_controller));
  if (!cgroup->cleanup()) result = probe_error(id, ReasonCode::cleanup_failed);
  return result;
}

[[nodiscard]] auto atomic_placement_probe(const ProbeId id) -> ProbeRecord {
  auto cgroup = required_cgroup_or_record(id);
  if (!cgroup) return cgroup.error();
  int readiness[2]{};
  if (::pipe2(readiness, O_CLOEXEC) != 0) {
    return cgroup->cleanup() ? probe_error(id, ReasonCode::internal_error)
                             : probe_error(id, ReasonCode::cleanup_failed);
  }
  const auto child = atomic_child(
      cgroup->child(),
      [] {
        constexpr int write = 3;
        const bool sent = write_all(write, "R");
        static_cast<void>(::close(write));
        if (!sent) return assertion_internal_error;
        for (;;)
          ::pause();
      },
      readiness[1]);
  static_cast<void>(::close(readiness[1]));
  const Descriptor ready{readiness[0]};
  if (!child) {
    return cgroup->cleanup() ? unavailable(id, child.error())
                             : probe_error(id, ReasonCode::cleanup_failed);
  }
  auto result = probe_error(id, ReasonCode::setup_race);
  if (wait_for_byte(ready.get())) {
    const auto processes = cgroup->processes();
    result =
        processes && std::ranges::find(*processes, *child) != processes->end()
            ? terminate_cgroup_tree(id, *cgroup, false, 1)
            : unavailable(id, ReasonCode::enforcement_failed);
  }
  if (!cgroup->cleanup()) result = probe_error(id, ReasonCode::cleanup_failed);
  return result;
}

[[nodiscard]] auto run_migration_child(const std::filesystem::path& state,
                                       const std::string& sibling_name) -> int {
  constexpr int write_descriptor = 3;
#if defined(LANDLOCK_ACCESS_FS_WRITE_FILE)
  const auto setup = apply_landlock(state, LANDLOCK_ACCESS_FS_WRITE_FILE);
#else
  const auto setup = assertion_mechanism_absent;
#endif
  if (setup != assertion_enforced) {
    const std::array results{setup, -1, -1};
    static_cast<void>(
        ::write(write_descriptor, results.data(), sizeof(results)));
    static_cast<void>(::close(write_descriptor));
    return setup;
  }
  const auto own_cgroup = current_cgroup_path();
  if (!own_cgroup || !own_cgroup->has_parent_path())
    return assertion_internal_error;
  const auto delegated = std::filesystem::path{"/sys/fs/cgroup"} /
                         own_cgroup->parent_path().relative_path();
  const auto sibling_path = delegated / sibling_name;
  const auto pid = std::to_string(::getpid());
  errno = 0;
  const bool escaped_sibling = write_process_to(sibling_path, pid);
  const int sibling_error = escaped_sibling ? 0 : errno;
  errno = 0;
  const bool escaped_parent = write_process_to(delegated, pid);
  const int parent_error = escaped_parent ? 0 : errno;
  const std::array results{assertion_enforced, parent_error, sibling_error};
  const bool sent =
      ::write(write_descriptor, results.data(), sizeof(results)) ==
      static_cast<ssize_t>(sizeof(results));
  static_cast<void>(::close(write_descriptor));
  if (!sent) return assertion_internal_error;
  pause_forever();
}

[[nodiscard]] auto cleanup_cgroup_pair(const ProbeId id, Cgroup& first,
                                       Cgroup& second, ProbeRecord result)
    -> ProbeRecord {
  if (!first.cleanup()) result = probe_error(id, ReasonCode::cleanup_failed);
  if (!second.cleanup()) result = probe_error(id, ReasonCode::cleanup_failed);
  return result;
}

[[nodiscard]] auto migration_denial_probe(const ProbeId id,
                                          const std::filesystem::path& state)
    -> ProbeRecord {
  auto cgroup = required_cgroup_or_record(id);
  if (!cgroup) return cgroup.error();
  auto sibling = Cgroup::create("-sibling");
  if (!sibling) {
    auto result = sibling.error() == ReasonCode::internal_error
                      ? probe_error(id, sibling.error())
                      : unavailable(id, sibling.error());
    if (!cgroup->cleanup())
      result = probe_error(id, ReasonCode::cleanup_failed);
    return result;
  }
  int outcome[2]{};
  if (::pipe2(outcome, O_CLOEXEC) != 0) {
    const bool sibling_cleanup_complete = sibling->cleanup();
    const bool cgroup_cleanup_complete = cgroup->cleanup();
    return sibling_cleanup_complete && cgroup_cleanup_complete
               ? probe_error(id, ReasonCode::internal_error)
               : probe_error(id, ReasonCode::cleanup_failed);
  }
  const auto sibling_name = std::string{sibling->name()};
  const auto child = atomic_child(
      cgroup->child(),
      [sibling_name, &state] {
        return run_migration_child(state, sibling_name);
      },
      outcome[1]);
  static_cast<void>(::close(outcome[1]));
  const Descriptor observed{outcome[0]};
  if (!child) {
    const bool sibling_cleanup_complete = sibling->cleanup();
    const bool cgroup_cleanup_complete = cgroup->cleanup();
    return sibling_cleanup_complete && cgroup_cleanup_complete
               ? unavailable(id, child.error())
               : probe_error(id, ReasonCode::cleanup_failed);
  }
  pollfd ready{observed.get(), POLLIN | POLLHUP, 0};
  std::array error_numbers{-1, -1, -1};
  const auto polled = ::poll(
      &ready, 1,
      static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                           observation_timeout)
                           .count()));
  const bool read =
      polled > 0 &&
      ::read(observed.get(), error_numbers.data(), sizeof(error_numbers)) ==
          static_cast<ssize_t>(sizeof(error_numbers));
  auto identity = pidfd_open(*child);
  if (identity.get() >= 0)
    static_cast<void>(pidfd_kill(identity.get()));
  else
    static_cast<void>(::kill(*child, SIGKILL));
  while (::waitpid(*child, nullptr, 0) < 0 && errno == EINTR) {
  }
  auto result =
      !read ? probe_error(id, ReasonCode::setup_race)
            : migration_observation(error_numbers[0] == assertion_enforced,
                                    error_numbers[1], error_numbers[2]);
  return cleanup_cgroup_pair(id, *sibling, *cgroup, result);
}

[[nodiscard]] auto parse_named_counter(const std::string_view document,
                                       const std::string_view name)
    -> std::optional<std::uint64_t> {
  const auto start = document.find(name);
  if (start == std::string_view::npos ||
      (start != 0 && document[start - 1] != '\n'))
    return std::nullopt;
  const auto value_start = start + name.size();
  if (value_start >= document.size() || document[value_start] != ' ')
    return std::nullopt;
  std::uint64_t value{};
  const auto parsed = std::from_chars(document.data() + value_start + 1,
                                      document.data() + document.size(), value);
  if (parsed.ec != std::errc{} ||
      (parsed.ptr != document.data() + document.size() && *parsed.ptr != '\n'))
    return std::nullopt;
  return value;
}

[[nodiscard]] auto await_counter_advance(const int cgroup_descriptor,
                                         const char* control,
                                         const std::string_view counter,
                                         const std::uint64_t before,
                                         const std::chrono::milliseconds wait)
    -> std::optional<std::uint64_t> {
  const auto deadline = std::chrono::steady_clock::now() + wait;
  std::optional<std::uint64_t> after;
  do {
    const auto document = read_control(cgroup_descriptor, control);
    if (document) after = parse_named_counter(*document, counter);
    if (after && *after > before) break;
    static_cast<void>(::poll(nullptr, 0, 10));
  } while (std::chrono::steady_clock::now() < deadline);
  return after;
}

[[nodiscard]] auto cpu_limit_probe(const ProbeId id) -> ProbeRecord {
  auto cgroup = required_cgroup_or_record(id);
  if (!cgroup) return cgroup.error();
  const auto before = read_control(cgroup->child(), "cpu.stat");
  const auto before_throttled =
      before ? parse_named_counter(*before, "nr_throttled") : std::nullopt;
  if (!before_throttled ||
      !write_control(cgroup->child(), "cpu.max", "1000 100000")) {
    auto result = unavailable_from_errno(id, errno);
    if (!cgroup->cleanup())
      result = probe_error(id, ReasonCode::cleanup_failed);
    return result;
  }
  const auto child = atomic_child(cgroup->child(), []() -> int {
    for (;;)
      asm volatile("" ::: "memory");
  });
  if (!child) {
    return cgroup->cleanup() ? unavailable(id, child.error())
                             : probe_error(id, ReasonCode::cleanup_failed);
  }
  const auto after_throttled =
      await_counter_advance(cgroup->child(), "cpu.stat", "nr_throttled",
                            *before_throttled, std::chrono::milliseconds{500});
  auto result = terminate_cgroup_tree(id, *cgroup, false, 1);
  if (result.state == ProbeState::enforced &&
      (!after_throttled || *after_throttled <= *before_throttled))
    result = unavailable(id, ReasonCode::limit_not_triggered);
  if (!cgroup->cleanup()) result = probe_error(id, ReasonCode::cleanup_failed);
  return result;
}

[[nodiscard]] auto allocate_until_memory_limit() -> int {
  constexpr std::size_t allocation = 256UZ * 1024UZ * 1024UZ;
  auto* memory = static_cast<volatile std::byte*>(
      ::mmap(nullptr, allocation, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (memory == MAP_FAILED) return assertion_prerequisite_unavailable;
  for (std::size_t offset{}; offset < allocation; offset += 4096)
    memory[offset] = std::byte{1};
  return assertion_limit_not_triggered;
}

[[nodiscard]] auto await_child(const pid_t child, int& status) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + observation_timeout;
  do {
    const auto waited = ::waitpid(child, &status, WNOHANG);
    if (waited == child) return true;
    if (waited < 0 && errno != EINTR) return false;
    static_cast<void>(::poll(nullptr, 0, 10));
  } while (std::chrono::steady_clock::now() < deadline);
  return false;
}

auto kill_and_reap(const pid_t child, int& status) -> void {
  static_cast<void>(::kill(child, SIGKILL));
  while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
  }
}

[[nodiscard]] auto memory_limit_probe(const ProbeId id) -> ProbeRecord {
  auto cgroup = required_cgroup_or_record(id);
  if (!cgroup) return cgroup.error();
  const auto before = read_control(cgroup->child(), "memory.events");
  const auto before_oom_kill =
      before ? parse_named_counter(*before, "oom_kill") : std::nullopt;
  if (!before_oom_kill) {
    auto result = probe_error(id, ReasonCode::malformed_protocol);
    if (!cgroup->cleanup())
      result = probe_error(id, ReasonCode::cleanup_failed);
    return result;
  }
  if (!write_control(cgroup->child(), "memory.max", "33554432") ||
      !write_control(cgroup->child(), "memory.swap.max", "0") ||
      !write_control(cgroup->child(), "memory.oom.group", "1")) {
    auto result = unavailable_from_errno(id, errno);
    if (!cgroup->cleanup())
      result = probe_error(id, ReasonCode::cleanup_failed);
    return result;
  }
  const auto child = atomic_child(cgroup->child(), allocate_until_memory_limit);
  if (!child) {
    return cgroup->cleanup() ? unavailable(id, child.error())
                             : probe_error(id, ReasonCode::cleanup_failed);
  }
  int status{};
  const bool reaped = await_child(*child, status);
  if (!reaped) kill_and_reap(*child, status);
  const auto after = read_control(cgroup->child(), "memory.events");
  const auto after_oom_kill =
      after ? parse_named_counter(*after, "oom_kill") : std::nullopt;
  auto result = memory_limit_observation(
      reaped && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
      after_oom_kill && *after_oom_kill > *before_oom_kill);
  if (!cgroup->cleanup()) result = probe_error(id, ReasonCode::cleanup_failed);
  return result;
}

[[nodiscard]] auto run_pids_limit_child() -> int {
  constexpr int write_descriptor = 3;
  int error_number{};
  for (;;) {
    const auto descendant = ::fork();
    if (descendant < 0) {
      error_number = errno;
      break;
    }
    if (descendant == 0) pause_forever();
  }
  const bool sent = ::write(write_descriptor, &error_number,
                            sizeof(error_number)) == sizeof(error_number);
  static_cast<void>(::close(write_descriptor));
  if (!sent) return assertion_internal_error;
  pause_forever();
}

[[nodiscard]] auto finish_subreaper_probe(const ProbeId id, Cgroup& cgroup,
                                          const int previous_subreaper,
                                          ProbeRecord result) -> ProbeRecord {
  const bool cgroup_cleanup_complete = cgroup.cleanup();
  const bool subreaper_restored =
      previous_subreaper != 0 || ::prctl(PR_SET_CHILD_SUBREAPER, 0) == 0;
  return cgroup_cleanup_complete && subreaper_restored
             ? result
             : probe_error(id, ReasonCode::cleanup_failed);
}

[[nodiscard]] auto pids_limit_probe(const ProbeId id) -> ProbeRecord {
  auto cgroup = required_cgroup_or_record(id);
  if (!cgroup) return cgroup.error();
  int previous_subreaper{};
  if (::prctl(PR_GET_CHILD_SUBREAPER, &previous_subreaper) != 0 ||
      (previous_subreaper == 0 && ::prctl(PR_SET_CHILD_SUBREAPER, 1) != 0)) {
    auto result = unavailable_from_errno(id, errno);
    if (!cgroup->cleanup())
      result = probe_error(id, ReasonCode::cleanup_failed);
    return result;
  }
  if (!write_control(cgroup->child(), "pids.max", "4")) {
    return finish_subreaper_probe(id, *cgroup, previous_subreaper,
                                  unavailable_from_errno(id, errno));
  }
  int outcome[2]{};
  if (::pipe2(outcome, O_CLOEXEC) != 0)
    return finish_subreaper_probe(id, *cgroup, previous_subreaper,
                                  probe_error(id, ReasonCode::internal_error));
  const auto child =
      atomic_child(cgroup->child(), run_pids_limit_child, outcome[1]);
  static_cast<void>(::close(outcome[1]));
  const Descriptor observed{outcome[0]};
  if (!child) {
    return finish_subreaper_probe(id, *cgroup, previous_subreaper,
                                  unavailable(id, child.error()));
  }
  pollfd ready{observed.get(), POLLIN | POLLHUP, 0};
  int error_number{};
  const auto polled = ::poll(
      &ready, 1,
      static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                           observation_timeout)
                           .count()));
  const bool triggered = polled > 0 &&
                         ::read(observed.get(), &error_number,
                                sizeof(error_number)) == sizeof(error_number) &&
                         error_number == EAGAIN;
  const auto termination = terminate_cgroup_tree(id, *cgroup, false, 4);
  const auto result =
      termination.state == ProbeState::probe_error
          ? termination
          : pids_limit_observation(
                triggered, termination.state == ProbeState::enforced, true);
  return finish_subreaper_probe(id, *cgroup, previous_subreaper, result);
}

[[nodiscard]] auto apply_landlock(
    const std::filesystem::path& state, const std::uint64_t handled,
    const std::span<const std::filesystem::path> additional_roots) -> int {
#if defined(SYS_landlock_create_ruleset) && defined(SYS_landlock_add_rule) &&  \
    defined(SYS_landlock_restrict_self) &&                                     \
    defined(LANDLOCK_CREATE_RULESET_VERSION)
  const auto abi = ::syscall(SYS_landlock_create_ruleset, nullptr, 0,
                             LANDLOCK_CREATE_RULESET_VERSION);
  if (abi < 0) return assertion_exit_for_errno(errno);
  landlock_ruleset_attr ruleset{};
  ruleset.handled_access_fs = handled;
  const Descriptor ruleset_fd{static_cast<int>(
      ::syscall(SYS_landlock_create_ruleset, &ruleset, sizeof(ruleset), 0))};
  if (ruleset_fd.get() < 0) return assertion_exit_for_errno(errno);
  std::vector<Descriptor> roots;
  roots.reserve(additional_roots.size() + 1);
  roots.emplace_back(::open(state.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC));
  if (roots.back().get() < 0) return assertion_internal_error;
  for (const auto& root : additional_roots) {
    Descriptor descriptor{
        ::open(root.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC)};
    if (descriptor.get() >= 0) roots.push_back(std::move(descriptor));
  }
  for (const auto& root : roots) {
    landlock_path_beneath_attr rule{};
    rule.allowed_access = handled;
    rule.parent_fd = root.get();
    if (::syscall(SYS_landlock_add_rule, ruleset_fd.get(),
                  LANDLOCK_RULE_PATH_BENEATH, &rule, 0) != 0)
      return assertion_exit_for_errno(errno);
  }
  if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
      ::syscall(SYS_landlock_restrict_self, ruleset_fd.get(), 0) != 0)
    return assertion_exit_for_errno(errno);
  return assertion_enforced;
#else
  static_cast<void>(state);
  static_cast<void>(handled);
  static_cast<void>(additional_roots);
  return assertion_mechanism_absent;
#endif
}

[[nodiscard]] auto landlock_access_probe(const ProbeId id,
                                         const std::filesystem::path& state,
                                         const std::uint64_t handled,
                                         const std::function<int()>& verify)
    -> ProbeRecord {
  return isolated_assertion(id, [&] {
    const auto setup = apply_landlock(state, handled);
    return setup == assertion_enforced ? verify() : setup;
  });
}

[[nodiscard]] auto landlock_write_probe(const ProbeId id,
                                        const std::filesystem::path& state)
    -> ProbeRecord {
#if defined(LANDLOCK_ACCESS_FS_WRITE_FILE) &&                                  \
    defined(LANDLOCK_ACCESS_FS_MAKE_REG) &&                                    \
    defined(LANDLOCK_ACCESS_FS_REMOVE_FILE) &&                                 \
    defined(LANDLOCK_ACCESS_FS_MAKE_DIR) &&                                    \
    defined(LANDLOCK_ACCESS_FS_REFER) && defined(LANDLOCK_ACCESS_FS_TRUNCATE)
  const auto access = complete_landlock_access();
  if (!access) return unavailable(id, ReasonCode::mechanism_absent);
  const auto outside_existing =
      state.parent_path() / "landlock-v2-existing-write";
  const auto outside_created =
      state.parent_path() / "landlock-v2-denied-create";
  const auto outside_directory =
      state.parent_path() / "landlock-v2-denied-directory";
  const auto outside_renamed =
      state.parent_path() / "landlock-v2-denied-rename";
  const auto inside_rename = state / "rename-source";
  {
    const Descriptor existing{
        ::open(outside_existing.c_str(),
               O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
               S_IRUSR | S_IWUSR)};
    const Descriptor rename_source{
        ::open(inside_rename.c_str(),
               O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
               S_IRUSR | S_IWUSR)};
    if (existing.get() < 0 || rename_source.get() < 0)
      return probe_error(id, ReasonCode::internal_error);
  }
  return landlock_access_probe(id, state, *access, [&] {
    const auto allowed = state / "allowed-write";
    const Descriptor accepted{::open(
        allowed.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR)};

    errno = 0;
    const Descriptor existing{
        ::open(outside_existing.c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW)};
    const auto existing_error = errno;
    errno = 0;
    const Descriptor truncated{::open(
        outside_existing.c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC | O_NOFOLLOW)};
    const auto truncation_error = errno;
    errno = 0;
    const bool removed = ::unlink(outside_existing.c_str()) == 0;
    const auto removal_error = errno;
    errno = 0;
    const Descriptor created{
        ::open(outside_created.c_str(),
               O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, S_IRUSR)};
    const auto creation_error = errno;
    errno = 0;
    const bool directory_created =
        ::mkdir(outside_directory.c_str(), S_IRWXU) == 0;
    const auto directory_error = errno;
    errno = 0;
    const bool renamed =
        ::rename(inside_rename.c_str(), outside_renamed.c_str()) == 0;
    const auto rename_error = errno;

    const auto observation = write_confinement_observation(
        accepted.get() >= 0,
        existing.get() < 0 && confinement_denied(existing_error),
        truncated.get() < 0 && confinement_denied(truncation_error),
        !removed && confinement_denied(removal_error),
        created.get() < 0 && confinement_denied(creation_error) &&
            !directory_created && confinement_denied(directory_error),
        !renamed &&
            (confinement_denied(rename_error) || rename_error == EXDEV));
    return observation.state == ProbeState::enforced ? assertion_enforced
                                                     : assertion_failed;
  });
#else
  static_cast<void>(state);
  return unavailable(id, ReasonCode::mechanism_absent);
#endif
}

[[nodiscard]] auto copy_file(const std::filesystem::path& source,
                             const std::filesystem::path& destination,
                             const mode_t mode) -> bool {
  const Descriptor input{
      ::open(source.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
  const Descriptor output{
      ::open(destination.c_str(),
             O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, mode)};
  if (input.get() < 0 || output.get() < 0) return false;
  std::array<char, 8192> buffer{};
  for (;;) {
    const auto count = ::read(input.get(), buffer.data(), buffer.size());
    if (count == 0) return true;
    if (count < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (!write_all(
            output.get(),
            std::string_view{buffer.data(), static_cast<std::size_t>(count)}))
      return false;
  }
}

[[nodiscard]] auto execute_path(std::string path) -> int {
  const auto child = ::fork();
  if (child < 0) return -1;
  if (child == 0) {
    char* arguments[]{path.data(), nullptr};
    char* environment[]{nullptr};
    ::execve(path.c_str(), arguments, environment);
    ::_exit(errno == EACCES || errno == EPERM ? 126 : 127);
  }
  int status{};
  while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

[[nodiscard]] auto landlock_execute_probe(const ProbeId id,
                                          const std::filesystem::path& state)
    -> ProbeRecord {
#if defined(LANDLOCK_ACCESS_FS_EXECUTE)
  const auto local = state / "allowed-executable";
  if (!copy_file("/bin/true", local, S_IRUSR | S_IWUSR | S_IXUSR))
    return unavailable(id, ReasonCode::prerequisite_unavailable);
  const std::array runtime_roots{std::filesystem::path{"/lib"},
                                 std::filesystem::path{"/lib64"},
                                 std::filesystem::path{"/usr/lib"}};
  return isolated_assertion(id, [&] {
    const auto setup =
        apply_landlock(state, LANDLOCK_ACCESS_FS_EXECUTE, runtime_roots);
    if (setup != assertion_enforced) return setup;
    const auto observation = execute_confinement_observation(
        execute_path(local.string()) == 0,
        execute_path(std::string{"/bin/true"}) == 126);
    return observation.state == ProbeState::enforced ? assertion_enforced
                                                     : assertion_failed;
  });
#else
  static_cast<void>(state);
  return unavailable(id, ReasonCode::mechanism_absent);
#endif
}

[[nodiscard]] auto install_seccomp_family_filter(const bool internet) -> int {
#if defined(SYS_socket) && (defined(__x86_64__) || defined(__aarch64__))
#if defined(__x86_64__)
  constexpr std::uint32_t audit_architecture = AUDIT_ARCH_X86_64;
#else
  constexpr std::uint32_t audit_architecture = AUDIT_ARCH_AARCH64;
#endif
  const auto first_family = internet ? AF_INET : AF_UNIX;
  const auto second_family = internet ? AF_INET6 : -1;
  std::array<sock_filter, 11> filter{{
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
               static_cast<std::uint32_t>(offsetof(seccomp_data, arch))),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, audit_architecture, 1, 0),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
               static_cast<std::uint32_t>(offsetof(seccomp_data, nr))),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_socket, 0, 5),
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
               static_cast<std::uint32_t>(offsetof(seccomp_data, args[0]))),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
               static_cast<std::uint32_t>(first_family), 2, 0),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
               static_cast<std::uint32_t>(second_family), 1, 0),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
  }};
  sock_fprog program{static_cast<unsigned short>(filter.size()), filter.data()};
  if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
      ::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) != 0)
    return assertion_exit_for_errno(errno);
  return assertion_enforced;
#else
  static_cast<void>(internet);
  return assertion_mechanism_absent;
#endif
}

[[nodiscard]] auto socket_is_denied(const int family) -> bool {
  errno = 0;
  const Descriptor descriptor{::socket(family, SOCK_STREAM | SOCK_CLOEXEC, 0)};
  return descriptor.get() < 0 && errno == EPERM;
}

[[nodiscard]] auto seccomp_family_probe(const ProbeId id, const bool internet)
    -> ProbeRecord {
#if defined(SYS_socket) && (defined(__x86_64__) || defined(__aarch64__))
  return isolated_assertion(id, [internet] {
    const auto setup = install_seccomp_family_filter(internet);
    if (setup != assertion_enforced) return setup;
    const auto first_family = internet ? AF_INET : AF_UNIX;
    if (!socket_is_denied(first_family)) return assertion_failed;
    if (internet) {
      if (!socket_is_denied(AF_INET6)) return assertion_failed;
      const Descriptor local{::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)};
      if (local.get() < 0) return assertion_failed;
    }
    return assertion_enforced;
  });
#else
  static_cast<void>(internet);
  return unavailable(id, ReasonCode::unsupported_architecture);
#endif
}

[[nodiscard]] auto establish_private_mount_namespace() -> int {
  if (::unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0)
    return assertion_exit_for_errno(errno);
  static_cast<void>(write_control(AT_FDCWD, "/proc/self/setgroups", "deny"));
  const auto uid_map = "0 " + std::to_string(::getuid()) + " 1";
  const auto gid_map = "0 " + std::to_string(::getgid()) + " 1";
  if (!write_control(AT_FDCWD, "/proc/self/uid_map", uid_map) ||
      !write_control(AT_FDCWD, "/proc/self/gid_map", gid_map))
    return assertion_exit_for_errno(errno);
  if (::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0)
    return assertion_exit_for_errno(errno);
  return assertion_enforced;
}

[[nodiscard]] auto establish_private_root(const std::filesystem::path& state)
    -> int {
  const auto setup = establish_private_mount_namespace();
  if (setup != assertion_enforced) return setup;
  const auto root = state / "root";
  const auto old = root / "old";
  if (::mkdir(root.c_str(), S_IRWXU) != 0 ||
      ::mount("tmpfs", root.c_str(), "tmpfs", MS_NODEV | MS_NOSUID,
              "size=1048576,mode=0700") != 0 ||
      ::mkdir(old.c_str(), S_IRWXU) != 0 || ::chdir(root.c_str()) != 0)
    return assertion_exit_for_errno(errno);
#if defined(SYS_pivot_root)
  if (::syscall(SYS_pivot_root, ".", "old") != 0 || ::chdir("/") != 0 ||
      ::umount2("/old", MNT_DETACH) != 0 || ::rmdir("/old") != 0)
    return assertion_exit_for_errno(errno);
#else
  return assertion_mechanism_absent;
#endif
  const Descriptor escaped{::open("/etc/passwd", O_RDONLY | O_CLOEXEC)};
  return escaped.get() < 0 && errno == ENOENT ? assertion_enforced
                                              : assertion_failed;
}

[[nodiscard]] auto private_root_probe(const ProbeId id,
                                      const std::filesystem::path& state)
    -> ProbeRecord {
  return isolated_assertion(id, [&] { return establish_private_root(state); });
}

[[noreturn]] auto run_mount_propagation_child(
    const std::filesystem::path& target, const std::filesystem::path& marker,
    const int readiness_descriptor) -> void {
  const auto setup = establish_private_mount_namespace();
  if (setup != assertion_enforced) ::_exit(setup);
  if (::mount("tmpfs", target.c_str(), "tmpfs", MS_NODEV | MS_NOSUID,
              "size=1048576,mode=0700") != 0)
    ::_exit(assertion_exit_for_errno(errno));
  const Descriptor mounted_marker{
      ::open(marker.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
             S_IRUSR | S_IWUSR)};
  if (mounted_marker.get() < 0 || !write_all(readiness_descriptor, "R"))
    ::_exit(assertion_internal_error);
  static_cast<void>(::close(readiness_descriptor));
  pause_forever();
}

[[nodiscard]] auto mount_propagation_result(
    const ProbeId id, const std::filesystem::path& target,
    const std::filesystem::path& marker, const struct stat& before,
    const bool child_mounted, const bool termination_sent, const pid_t child,
    const pid_t waited, const int status) -> ProbeRecord {
  struct stat after{};
  errno = 0;
  const bool marker_visible = ::lstat(marker.c_str(), &after) == 0;
  const auto marker_error = errno;
  const bool target_observed = ::lstat(target.c_str(), &after) == 0;
  const bool target_changed =
      target_observed &&
      (before.st_dev != after.st_dev || before.st_ino != after.st_ino);
  const bool visible_in_parent = marker_visible || target_changed;
  bool cleanup_complete{true};
  if (visible_in_parent && ::umount2(target.c_str(), MNT_DETACH) != 0)
    cleanup_complete = false;
  if (::rmdir(target.c_str()) != 0) cleanup_complete = false;
  if (waited != child || (!marker_visible && marker_error != ENOENT) ||
      !target_observed)
    cleanup_complete = false;
  if (!child_mounted) {
    if (!cleanup_complete) return probe_error(id, ReasonCode::cleanup_failed);
    if (WIFEXITED(status)) return assertion_result(id, WEXITSTATUS(status));
    return termination_sent ? probe_error(id, ReasonCode::timeout)
                            : probe_error(id, ReasonCode::signaled);
  }
  if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL)
    cleanup_complete = false;
  auto result = mount_propagation_observation(child_mounted, visible_in_parent,
                                              cleanup_complete);
  result.probe_id = id;
  return result;
}

[[nodiscard]] auto mount_propagation_probe(const ProbeId id,
                                           const std::filesystem::path& state)
    -> ProbeRecord {
  const auto target = state / "propagation-target";
  const auto marker = target / "child-mount";
  struct stat before{};
  if (::mkdir(target.c_str(), S_IRWXU) != 0 ||
      ::lstat(target.c_str(), &before) != 0)
    return probe_error(id, ReasonCode::internal_error);

  int readiness[2]{};
  if (::pipe2(readiness, O_CLOEXEC) != 0) {
    static_cast<void>(::rmdir(target.c_str()));
    return probe_error(id, ReasonCode::internal_error);
  }
  const auto child = ::fork();
  if (child < 0) {
    static_cast<void>(::close(readiness[0]));
    static_cast<void>(::close(readiness[1]));
    static_cast<void>(::rmdir(target.c_str()));
    return probe_error(id, ReasonCode::internal_error);
  }
  if (child == 0) {
    static_cast<void>(::close(readiness[0]));
    run_mount_propagation_child(target, marker, readiness[1]);
  }

  static_cast<void>(::close(readiness[1]));
  const Descriptor ready{readiness[0]};
  const bool child_mounted = wait_for_byte(ready.get());
  const bool termination_sent = ::kill(child, SIGKILL) == 0;
  int status{};
  pid_t waited{};
  do {
    waited = ::waitpid(child, &status, 0);
  } while (waited < 0 && errno == EINTR);
  return mount_propagation_result(id, target, marker, before, child_mounted,
                                  termination_sent, child, waited, status);
}

[[nodiscard]] auto staged_output_probe(const ProbeId id,
                                       const std::filesystem::path& state)
    -> ProbeRecord {
  const auto original = state / "output";
  const auto moved = state / "output-opened";
  const Descriptor descriptor{::open(
      original.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
      S_IRUSR | S_IWUSR)};
  if (descriptor.get() < 0) return probe_error(id, ReasonCode::internal_error);
  if (::rename(original.c_str(), moved.c_str()) != 0)
    return probe_error(id, ReasonCode::internal_error);
  const Descriptor replacement{::open(
      original.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
      S_IRUSR | S_IWUSR)};
  if (replacement.get() < 0 || !write_all(descriptor.get(), "A") ||
      !write_all(replacement.get(), "B"))
    return probe_error(id, ReasonCode::internal_error);
  struct stat opened{};
  struct stat current{};
  return ::fstat(descriptor.get(), &opened) == 0 &&
                 ::stat(original.c_str(), &current) == 0 &&
                 (opened.st_dev != current.st_dev ||
                  opened.st_ino != current.st_ino)
             ? enforced(id)
             : unavailable(id, ReasonCode::enforcement_failed);
}

[[nodiscard]] auto finish_cgroup_probe(const ProbeId id, Cgroup& cgroup,
                                       ProbeRecord result) -> ProbeRecord {
  return cgroup.cleanup() ? result
                          : probe_error(id, ReasonCode::cleanup_failed);
}

[[nodiscard]] auto apply_combined_limits(const int cgroup_descriptor) -> bool {
  return write_control(cgroup_descriptor, "cpu.max", "50000 100000") &&
         write_control(cgroup_descriptor, "memory.max", "67108864") &&
         write_control(cgroup_descriptor, "memory.swap.max", "0") &&
         write_control(cgroup_descriptor, "memory.oom.group", "1") &&
         write_control(cgroup_descriptor, "pids.max", "16");
}

[[nodiscard]] auto combined_setup_probe(const ProbeId id,
                                        const std::filesystem::path& state)
    -> ProbeRecord {
  auto cgroup = required_cgroup_or_record(id);
  if (!cgroup) return cgroup.error();
  const bool limits_applied = apply_combined_limits(cgroup->child());
  if (!limits_applied)
    return finish_cgroup_probe(id, *cgroup, unavailable_from_errno(id, errno));

  const auto input_path = state / "combined-input";
  const auto opened_input_path = state / "combined-input-opened";
  const auto output_path = state / "combined-output";
  const auto opened_output_path = state / "combined-output-opened";
  const Descriptor input{::open(
      input_path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
      S_IRUSR | S_IWUSR)};
  const Descriptor output{::open(
      output_path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
      S_IRUSR | S_IWUSR)};
  const Descriptor executable{::open("/proc/self/exe", O_PATH | O_CLOEXEC)};
  if (input.get() < 0 || output.get() < 0 || executable.get() < 0 ||
      !write_all(input.get(), "I") || ::lseek(input.get(), 0, SEEK_SET) != 0 ||
      ::rename(input_path.c_str(), opened_input_path.c_str()) != 0 ||
      ::rename(output_path.c_str(), opened_output_path.c_str()) != 0) {
    return finish_cgroup_probe(id, *cgroup,
                               probe_error(id, ReasonCode::internal_error));
  }
  const Descriptor replacement_input{::open(
      input_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
      S_IRUSR | S_IWUSR)};
  const Descriptor replacement_output{::open(
      output_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
      S_IRUSR | S_IWUSR)};
  if (replacement_input.get() < 0 || replacement_output.get() < 0 ||
      !write_all(replacement_input.get(), "X")) {
    return finish_cgroup_probe(id, *cgroup,
                               probe_error(id, ReasonCode::internal_error));
  }

  int readiness[2]{};
  if (::pipe2(readiness, O_CLOEXEC) != 0)
    return finish_cgroup_probe(id, *cgroup,
                               probe_error(id, ReasonCode::internal_error));
  const auto child =
      atomic_combined_payload(cgroup->child(), readiness[1], executable.get(),
                              input.get(), output.get(), state);
  static_cast<void>(::close(readiness[1]));
  const Descriptor ready{readiness[0]};
  if (!child) {
    return finish_cgroup_probe(id, *cgroup, unavailable(id, child.error()));
  }
  const bool payload_reached = wait_for_byte(ready.get());
  if (!payload_reached) {
    const bool cgroup_cleanup = cgroup->cleanup();
    int status{};
    pid_t waited{};
    do {
      waited = ::waitpid(*child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    const bool reaped = reap_all_children();
    if (!cgroup_cleanup || waited != *child || !reaped)
      return probe_error(id, ReasonCode::cleanup_failed);
    return WIFEXITED(status) ? assertion_result(id, WEXITSTATUS(status))
                             : probe_error(id, ReasonCode::setup_race);
  }

  char output_marker{};
  const bool staged_descriptors =
      ::lseek(output.get(), 0, SEEK_SET) == 0 &&
      ::read(output.get(), &output_marker, 1) == 1 && output_marker == 'O';
  const auto processes = cgroup->processes();
  const bool target_waiting_for_cleanup =
      processes && std::ranges::find(*processes, *child) != processes->end();
  auto result = setup_order_observation(
      limits_applied, true, staged_descriptors, true, true, true, true, true,
      payload_reached, target_waiting_for_cleanup);
  const auto termination = terminate_cgroup_tree(id, *cgroup, false, 1);
  if (termination.state != ProbeState::enforced) result = termination;
  return finish_cgroup_probe(id, *cgroup, result);
}

[[nodiscard]] auto partial_setup_cleanup_probe(const ProbeId id)
    -> ProbeRecord {
  auto cgroup = required_cgroup_or_record(id);
  if (!cgroup) return cgroup.error();
  if (!write_control(cgroup->child(), "pids.max", "4")) {
    auto result = unavailable_from_errno(id, errno);
    if (!cgroup->cleanup())
      result = probe_error(id, ReasonCode::cleanup_failed);
    return result;
  }
  errno = 0;
  if (write_control(cgroup->child(), "memory.max", "invalid")) {
    return cgroup->cleanup() ? unavailable(id, ReasonCode::enforcement_failed)
                             : probe_error(id, ReasonCode::cleanup_failed);
  }
  return cgroup->cleanup() ? enforced(id)
                           : probe_error(id, ReasonCode::cleanup_failed);
}

} // namespace

auto run_combined_setup_payload(const std::filesystem::path& state_directory)
    -> int {
  if (!valid_state_directory(state_directory)) return assertion_internal_error;
  char input{};
  char trailing{};
  if (::read(5, &input, 1) != 1 || input != 'I' || ::read(5, &trailing, 1) != 0)
    return assertion_internal_error;
  const auto root = establish_private_root(state_directory);
  if (root != assertion_enforced) return root;
  const auto filesystem_access = complete_landlock_access();
  if (!filesystem_access) return assertion_mechanism_absent;
  if (apply_landlock("/", *filesystem_access) != assertion_enforced)
    return assertion_prerequisite_unavailable;
  if (install_seccomp_family_filter(true) != assertion_enforced ||
      install_seccomp_family_filter(false) != assertion_enforced)
    return assertion_prerequisite_unavailable;
  for (const auto family : {AF_INET, AF_INET6, AF_UNIX}) {
    errno = 0;
    const Descriptor denied{::socket(family, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    if (denied.get() >= 0 || errno != EPERM) return assertion_failed;
  }
  if (!write_all(6, "O") || !write_all(3, "R")) return assertion_internal_error;
  static_cast<void>(::close(3));
  static_cast<void>(::close(5));
  static_cast<void>(::close(6));
  for (;;)
    ::pause();
}

auto run_probe(const ProbeId probe_id,
               const std::filesystem::path& state_directory,
               const bool has_delegated_cgroup_root) -> ProbeRecord {
  try {
    if (!valid_state_directory(state_directory))
      return probe_error(probe_id, ReasonCode::internal_error);
    struct stat root_attributes{};
    if (has_delegated_cgroup_root && (::fstat(4, &root_attributes) != 0 ||
                                      !S_ISDIR(root_attributes.st_mode)))
      return probe_error(probe_id, ReasonCode::internal_error);
    namespace v1 = aiforge::evaluation::process_isolation;
    switch (probe_id) {
      case ProbeId::cgroup_v2_delegation:
        return cgroup_delegation_probe(probe_id);
      case ProbeId::cgroup_required_controllers:
        return cgroup_controller_probe(probe_id);
      case ProbeId::cgroup_atomic_child_placement:
        return atomic_placement_probe(probe_id);
      case ProbeId::cgroup_self_migration_denial:
        return migration_denial_probe(probe_id, state_directory);
      case ProbeId::cgroup_whole_tree_enumeration:
        return tree_probe(probe_id, TreeShape::fanout, 5, false);
      case ProbeId::cgroup_kill:
        return tree_probe(probe_id, TreeShape::fanout, 5, true);
      case ProbeId::cgroup_populated_zero:
        return tree_probe(probe_id, TreeShape::simple, 1, false);
      case ProbeId::cgroup_setsid_containment:
        return tree_probe(probe_id, TreeShape::setsid, 1, false);
      case ProbeId::cgroup_double_fork_containment:
        return tree_probe(probe_id, TreeShape::double_fork, 1, false);
      case ProbeId::cgroup_daemon_containment:
        return tree_probe(probe_id, TreeShape::daemon, 1, false);
      case ProbeId::cgroup_clone_fork_fanout:
        return tree_probe(probe_id, TreeShape::fanout, 5, false);
      case ProbeId::cgroup_leader_exit_containment:
        return tree_probe(probe_id, TreeShape::leader_exit, 1, false);
      case ProbeId::cgroup_cancellation_cleanup:
        return tree_probe(probe_id, TreeShape::fanout, 5, false, true);
      case ProbeId::cgroup_cpu_limit_enforcement:
        return cpu_limit_probe(probe_id);
      case ProbeId::cgroup_memory_limit_termination:
        return memory_limit_probe(probe_id);
      case ProbeId::cgroup_pids_limit_enforcement:
        return pids_limit_probe(probe_id);
      case ProbeId::landlock_read_confinement:
        return map_v1_record(
            probe_id, v1::run_probe(v1::ProbeId::landlock_read_confinement,
                                    state_directory));
      case ProbeId::landlock_write_confinement:
        return landlock_write_probe(probe_id, state_directory);
      case ProbeId::landlock_execute_confinement:
        return landlock_execute_probe(probe_id, state_directory);
      case ProbeId::seccomp_internet_socket_family_denial:
        return seccomp_family_probe(probe_id, true);
      case ProbeId::seccomp_unix_socket_denial:
        return seccomp_family_probe(probe_id, false);
      case ProbeId::private_root_construction:
        return private_root_probe(probe_id, state_directory);
      case ProbeId::private_mount_propagation:
        return mount_propagation_probe(probe_id, state_directory);
      case ProbeId::descriptor_relative_launch:
        return map_v1_record(
            probe_id,
            v1::run_probe(v1::ProbeId::fexecve_identity, state_directory));
      case ProbeId::staged_input_identity:
        return map_v1_record(
            probe_id,
            v1::run_probe(v1::ProbeId::staged_input_identity, state_directory));
      case ProbeId::staged_output_identity:
        return staged_output_probe(probe_id, state_directory);
      case ProbeId::combined_setup_order:
        return combined_setup_probe(probe_id, state_directory);
      case ProbeId::partial_setup_cleanup:
        return partial_setup_cleanup_probe(probe_id);
    }
  } catch (...) {
    return probe_error(probe_id, ReasonCode::internal_error);
  }
  return probe_error(probe_id, ReasonCode::internal_error);
}

#if defined(AIFORGE_PROCESS_ISOLATION_TEST_SUPPORT)
namespace test_support {

auto cgroup_prerequisite_outcome(const bool unified, const bool delegated,
                                 const bool cpu, const bool memory,
                                 const bool pids) -> ProbeRecord {
  if (!unified)
    return unavailable(ProbeId::cgroup_v2_delegation,
                       ReasonCode::mechanism_absent);
  if (!delegated)
    return unavailable(ProbeId::cgroup_v2_delegation,
                       ReasonCode::missing_delegation);
  if (!cpu || !memory || !pids)
    return unavailable(ProbeId::cgroup_required_controllers,
                       ReasonCode::missing_controller);
  return enforced(ProbeId::cgroup_required_controllers);
}

auto migration_attempt_outcome(const bool confinement_applied,
                               const int parent_error, const int sibling_error)
    -> ProbeRecord {
  return migration_observation(confinement_applied, parent_error,
                               sibling_error);
}

auto memory_limit_outcome(const bool killed, const bool oom_kill_advanced)
    -> ProbeRecord {
  return memory_limit_observation(killed, oom_kill_advanced);
}

auto pids_limit_outcome(const bool exhausted, const bool tree_complete,
                        const bool cleanup_complete) -> ProbeRecord {
  return pids_limit_observation(exhausted, tree_complete, cleanup_complete);
}

auto execute_confinement_outcome(const bool local_executed,
                                 const bool outside_denied) -> ProbeRecord {
  return execute_confinement_observation(local_executed, outside_denied);
}

auto mount_propagation_outcome(const bool child_mount_established,
                               const bool visible_in_parent,
                               const bool cleanup_complete) -> ProbeRecord {
  return mount_propagation_observation(child_mount_established,
                                       visible_in_parent, cleanup_complete);
}

auto cancellation_cleanup_outcome(const bool tree_ready,
                                  const bool cancellation_requested,
                                  const bool tree_terminated,
                                  const bool cleanup_complete) -> ProbeRecord {
  return cancellation_cleanup_observation(tree_ready, cancellation_requested,
                                          tree_terminated, cleanup_complete);
}

auto write_confinement_outcome(const bool allowed_write_succeeded,
                               const bool existing_write_denied,
                               const bool truncation_denied,
                               const bool removal_denied,
                               const bool creation_denied,
                               const bool rename_denied) -> ProbeRecord {
  return write_confinement_observation(
      allowed_write_succeeded, existing_write_denied, truncation_denied,
      removal_denied, creation_denied, rename_denied);
}

auto pid_identity_outcome(const bool pidfd_opened, const bool identity_stable)
    -> ProbeRecord {
  return pidfd_opened && identity_stable
             ? enforced(ProbeId::cgroup_whole_tree_enumeration)
             : probe_error(ProbeId::cgroup_whole_tree_enumeration,
                           ReasonCode::pid_reuse);
}

auto setup_order_outcome(const bool limits_applied, const bool placed,
                         const bool staged_descriptors,
                         const bool descriptor_launched,
                         const bool private_root_applied,
                         const bool filesystem_applied,
                         const bool internet_denied, const bool unix_denied,
                         const bool payload_reached,
                         const bool target_waiting_for_cleanup) -> ProbeRecord {
  return setup_order_observation(
      limits_applied, placed, staged_descriptors, descriptor_launched,
      private_root_applied, filesystem_applied, internet_denied, unix_denied,
      payload_reached, target_waiting_for_cleanup);
}

} // namespace test_support
#endif

} // namespace aiforge::evaluation::process_isolation::v2
