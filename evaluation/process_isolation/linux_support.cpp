#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

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
#include <cstring>
#include <ranges>
#include <set>
#include <utility>

#include <fcntl.h>
#include <linux/magic.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace aiforge::evaluation::process_isolation::linux_support {
namespace {

constexpr std::size_t maximum_cgroup_children{64};
constexpr std::size_t maximum_cgroup_depth{8};
constexpr auto observation_timeout = std::chrono::seconds{2};

struct LinuxDirectoryEntry {
  std::uint64_t inode;
  std::int64_t offset;
  unsigned short record_length;
  unsigned char type;
  char name;
};

template <typename Visitor>
[[nodiscard]] auto visit_directory_entries(const int directory,
                                           const Visitor& visitor) -> bool {
#if defined(SYS_getdents64)
  alignas(LinuxDirectoryEntry) std::array<std::byte, 4096> buffer{};
  for (;;) {
    const auto count =
        ::syscall(SYS_getdents64, directory, buffer.data(), buffer.size());
    if (count == 0) return true;
    if (count < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    std::size_t position{};
    const auto length = static_cast<std::size_t>(count);
    while (position < length) {
      const auto* entry = reinterpret_cast<const LinuxDirectoryEntry*>(
          buffer.data() + position);
      constexpr auto name_offset = offsetof(LinuxDirectoryEntry, name);
      if (entry->record_length < name_offset + 1U ||
          entry->record_length > length - position)
        return false;
      const auto maximum_name = entry->record_length - name_offset;
      const auto* terminator = static_cast<const char*>(
          std::memchr(&entry->name, '\0', maximum_name));
      if (terminator == nullptr ||
          !visitor(std::string_view{
              &entry->name,
              static_cast<std::size_t>(terminator - &entry->name)}))
        return false;
      position += entry->record_length;
    }
  }
#else
  static_cast<void>(directory);
  static_cast<void>(visitor);
  return false;
#endif
}

[[nodiscard]] auto cgroup_directories(const int directory)
    -> std::optional<std::vector<std::string>> {
  const Descriptor scan{::fcntl(directory, F_DUPFD_CLOEXEC, 5)};
  if (scan.get() < 0) return std::nullopt;
  std::vector<std::string> result;
  const bool valid = visit_directory_entries(scan.get(), [&](const auto name) {
    if (name == "." || name == "..") return true;
    struct stat attributes{};
    const std::string owned_name{name};
    if (::fstatat(directory, owned_name.c_str(), &attributes,
                  AT_SYMLINK_NOFOLLOW) != 0)
      return false;
    if (!S_ISDIR(attributes.st_mode)) return true;
    const bool safe_name =
        !name.empty() && name.size() <= 255 &&
        std::ranges::all_of(name, [](const unsigned char character) {
          return (character >= 'a' && character <= 'z') ||
                 (character >= 'A' && character <= 'Z') ||
                 (character >= '0' && character <= '9') || character == '-' ||
                 character == '_';
        });
    if (!safe_name || result.size() == maximum_cgroup_children) return false;
    result.push_back(owned_name);
    return true;
  });
  if (!valid) return std::nullopt;
  std::ranges::sort(result);
  return result;
}

[[nodiscard]] auto await_cgroup_empty(const int directory) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + observation_timeout;
  do {
    const auto events = read_control(directory, "cgroup.events");
    if (events && events->find("populated 0") != std::string::npos) return true;
    static_cast<void>(::poll(nullptr, 0, 5));
  } while (std::chrono::steady_clock::now() < deadline);
  return false;
}

struct CgroupCleanupFrame {
  int parent{};
  std::string name;
  std::size_t depth{};
  Descriptor child;
  std::vector<std::string> descendants;
  std::size_t next_descendant{};
  bool complete{};
};

[[nodiscard]] auto make_cleanup_frame(const int parent, std::string name,
                                      const std::size_t depth,
                                      std::size_t& remaining)
    -> std::expected<CgroupCleanupFrame, bool> {
  if (depth > maximum_cgroup_depth || remaining == 0)
    return std::unexpected(false);
  --remaining;
  Descriptor child{::openat(parent, name.c_str(),
                            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
  if (child.get() < 0) return std::unexpected(errno == ENOENT);
  bool complete = write_control(child.get(), "cgroup.kill", "1") &&
                  await_cgroup_empty(child.get());
  auto descendants = cgroup_directories(child.get());
  if (!descendants) complete = false;
  return CgroupCleanupFrame{parent,
                            std::move(name),
                            depth,
                            std::move(child),
                            descendants ? std::move(*descendants)
                                        : std::vector<std::string>{},
                            0,
                            complete};
}

[[nodiscard]] auto cleanup_cgroup_tree(const int parent, std::string name,
                                       const std::size_t depth,
                                       std::size_t& remaining) -> bool {
  auto initial = make_cleanup_frame(parent, std::move(name), depth, remaining);
  if (!initial) return initial.error();
  std::vector<CgroupCleanupFrame> pending;
  pending.push_back(std::move(*initial));
  for (;;) {
    auto& current = pending.back();
    if (current.next_descendant < current.descendants.size()) {
      auto descendant = make_cleanup_frame(
          current.child.get(), current.descendants[current.next_descendant++],
          current.depth + 1, remaining);
      if (descendant) {
        pending.push_back(std::move(*descendant));
      } else if (!descendant.error()) {
        current.complete = false;
      }
      continue;
    }
    current.child.reset();
    if (::unlinkat(current.parent, current.name.c_str(), AT_REMOVEDIR) != 0 &&
        errno != ENOENT)
      current.complete = false;
    const bool complete = current.complete;
    pending.pop_back();
    if (pending.empty()) return complete;
    if (!complete) pending.back().complete = false;
  }
}

[[nodiscard]] auto cleanup_task_cgroups(const int root, const pid_t owner,
                                        const std::string_view prefix) -> bool {
  const auto children = cgroup_directories(root);
  if (!children) return false;
  bool complete{true};
  std::size_t remaining{maximum_cgroup_children};
  for (const auto& child : *children) {
    if (task_cgroup_owned_by(owner, prefix, child) &&
        !cleanup_cgroup_tree(root, child, 1, remaining)) {
      complete = false;
    }
  }
  return complete;
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

} // namespace

Descriptor::Descriptor(const int value) : m_value(value) {
}

Descriptor::Descriptor(Descriptor&& other) noexcept : m_value(other.release()) {
}

auto Descriptor::operator=(Descriptor&& other) noexcept -> Descriptor& {
  if (this == &other) return *this;
  reset(other.release());
  return *this;
}

Descriptor::~Descriptor() {
  reset();
}

auto Descriptor::get() const noexcept -> int {
  return m_value;
}

auto Descriptor::release() noexcept -> int {
  const auto value = m_value;
  m_value = -1;
  return value;
}

auto Descriptor::reset(const int value) noexcept -> void {
  if (m_value >= 0) static_cast<void>(::close(m_value));
  m_value = value;
}

auto write_all(const int descriptor, const std::string_view value) -> bool {
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

auto write_control(const int directory, const char* name,
                   const std::string_view value) -> bool {
  const Descriptor descriptor{
      ::openat(directory, name, O_WRONLY | O_CLOEXEC | O_NOFOLLOW)};
  return descriptor.get() >= 0 && write_all(descriptor.get(), value);
}

auto read_control(const int directory, const char* name,
                  const std::size_t maximum) -> std::optional<std::string> {
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
    if (result.size() + static_cast<std::size_t>(count) > maximum)
      return std::nullopt;
    result.append(buffer.data(), static_cast<std::size_t>(count));
  }
}

auto parse_processes(const std::string_view document)
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

auto cgroup_is_populated(const int directory) -> std::optional<bool> {
  const auto events = read_control(directory, "cgroup.events");
  if (!events) return std::nullopt;
  if (events->find("populated 0") != std::string::npos) return false;
  if (events->find("populated 1") != std::string::npos) return true;
  return std::nullopt;
}

auto pidfd_open(const pid_t process) -> Descriptor {
#if defined(SYS_pidfd_open)
  return Descriptor{static_cast<int>(::syscall(SYS_pidfd_open, process, 0U))};
#else
  static_cast<void>(process);
  return Descriptor{};
#endif
}

auto pidfd_kill(const int descriptor) -> bool {
#if defined(SYS_pidfd_send_signal)
  return descriptor >= 0 && (::syscall(SYS_pidfd_send_signal, descriptor,
                                       SIGKILL, nullptr, 0U) == 0 ||
                             errno == ESRCH);
#else
  static_cast<void>(descriptor);
  return false;
#endif
}

auto pidfd_dead(const int descriptor) -> bool {
  pollfd value{descriptor, POLLIN, 0};
  int polled{};
  do {
    polled = ::poll(&value, 1, 0);
  } while (polled < 0 && errno == EINTR);
  return polled == 1 && (value.revents & POLLIN) != 0;
}

auto close_descriptors_from(const unsigned int first) -> bool {
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

auto safe_delegated_root_path(const std::filesystem::path& path) -> bool {
  if (path.empty()) return true;
  const auto& text = path.native();
  if (!path.is_absolute() || text.size() > 4096 || text == "/" ||
      text.back() == '/')
    return false;
  for (const auto& component : path) {
    const auto& value = component.native();
    if (value == "/") continue;
    if (value.empty() || value == "." || value == ".." || value.size() > 255 ||
        !std::ranges::all_of(value, [](const unsigned char character) {
          return character >= 0x21U && character != 0x7fU;
        }))
      return false;
  }
  return true;
}

auto open_pinned_directory(const std::filesystem::path& path) -> Descriptor {
  auto current =
      Descriptor{::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
  if (current.get() < 0) return current;
  for (const auto& component : path) {
    const auto& value = component.native();
    if (value == "/") continue;
    Descriptor next{::openat(current.get(), value.c_str(),
                             O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    if (next.get() < 0) return Descriptor{};
    current = std::move(next);
  }
  return current;
}

auto task_cgroup_owned_by(const pid_t process, const std::string_view prefix,
                          const std::string_view name) -> bool {
  if (process <= 0) return false;
  const auto base = std::string{prefix} + std::to_string(process);
  return name == base || (name.starts_with(base) && name.size() > base.size() &&
                          name[base.size()] == '-');
}

CgroupBootstrap::CgroupBootstrap(std::string task_prefix)
    : m_task_prefix(std::move(task_prefix)) {
}

CgroupBootstrap::~CgroupBootstrap() {
  static_cast<void>(cleanup());
}

auto CgroupBootstrap::start(const std::filesystem::path& path)
    -> BootstrapError {
  if (path.empty()) return BootstrapError::missing_delegation;
  m_root = open_pinned_directory(path);
  if (const auto validation = validate_root();
      validation != BootstrapError::none)
    return validation;
  if (const auto validation = validate_controllers();
      validation != BootstrapError::none)
    return validation;
  if (const auto validation = validate_root_ownership();
      validation != BootstrapError::none)
    return validation;
  if (const auto creation = create_supervisor();
      creation != BootstrapError::none)
    return creation;
  if (const auto migration = migrate_to_supervisor();
      migration != BootstrapError::none)
    return migration;
  if (const auto enabling = enable_controllers();
      enabling != BootstrapError::none)
    return enabling;
  m_ready = true;
  return BootstrapError::none;
}

auto CgroupBootstrap::descriptor() const noexcept -> int {
  return m_ready ? m_root.get() : -1;
}

auto CgroupBootstrap::remember_task_owner(const pid_t process) -> void {
  if (process > 0) m_task_owners.push_back(process);
}

auto CgroupBootstrap::cleanup_task_owner(const pid_t process) -> bool {
  return m_root.get() >= 0 &&
         cleanup_task_cgroups(m_root.get(), process, m_task_prefix);
}

auto CgroupBootstrap::cleanup() noexcept -> bool {
  if (m_cleaned) return m_cleanup_okay;
  try {
    bool okay{true};
    m_ready = false;
    for (const auto process : m_task_owners) {
      if (!cleanup_task_owner(process)) okay = false;
    }
    m_task_owners.clear();
    if (m_enabled && !write_control(m_root.get(), "cgroup.subtree_control",
                                    "-cpu -memory -pids"))
      okay = false;
    m_enabled = false;
    if (m_moved && !write_control(m_root.get(), "cgroup.procs",
                                  std::to_string(::getpid())))
      okay = false;
    m_moved = false;
    if (m_supervisor.get() >= 0 && !await_cgroup_empty(m_supervisor.get()))
      okay = false;
    m_supervisor.reset();
    if (m_created &&
        ::unlinkat(m_root.get(), m_supervisor_name.c_str(), AT_REMOVEDIR) != 0)
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

auto CgroupBootstrap::validate_root() const -> BootstrapError {
  struct stat attributes{};
  struct statfs filesystem{};
  if (m_root.get() < 0 || ::fstat(m_root.get(), &attributes) != 0 ||
      ::fstatfs(m_root.get(), &filesystem) != 0 ||
      !S_ISDIR(attributes.st_mode) || attributes.st_uid != ::geteuid() ||
      filesystem.f_type != CGROUP2_SUPER_MAGIC)
    return BootstrapError::missing_delegation;
  return BootstrapError::none;
}

auto CgroupBootstrap::validate_controllers() const -> BootstrapError {
  const auto controllers = read_control(m_root.get(), "cgroup.controllers");
  const auto available =
      controllers ? parse_tokens(*controllers) : std::nullopt;
  if (!available) return BootstrapError::internal_error;
  for (const auto* required : {"cpu", "memory", "pids"}) {
    if (!available->contains(required))
      return BootstrapError::missing_controller;
  }
  return BootstrapError::none;
}

auto CgroupBootstrap::validate_root_ownership() const -> BootstrapError {
  const auto processes_document = read_control(m_root.get(), "cgroup.procs");
  const auto processes =
      processes_document ? parse_processes(*processes_document) : std::nullopt;
  if (!processes) return BootstrapError::internal_error;
  if (processes->size() != 1 || processes->front() != ::getpid())
    return BootstrapError::missing_delegation;
  const auto children = cgroup_directories(m_root.get());
  if (!children || !children->empty())
    return children ? BootstrapError::missing_delegation
                    : BootstrapError::internal_error;
  return BootstrapError::none;
}

auto CgroupBootstrap::create_supervisor() -> BootstrapError {
  m_supervisor_name =
      m_task_prefix + "supervisor-" + std::to_string(::getpid());
  if (::mkdirat(m_root.get(), m_supervisor_name.c_str(), S_IRWXU) != 0)
    return errno == EACCES || errno == EPERM || errno == EROFS
               ? BootstrapError::missing_delegation
               : BootstrapError::internal_error;
  m_created = true;
  m_supervisor =
      Descriptor{::openat(m_root.get(), m_supervisor_name.c_str(),
                          O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
  if (m_supervisor.get() < 0) return fail(BootstrapError::internal_error);
  return BootstrapError::none;
}

auto CgroupBootstrap::migrate_to_supervisor() -> BootstrapError {
  const auto own_pid = std::to_string(::getpid());
  if (!write_control(m_supervisor.get(), "cgroup.procs", own_pid))
    return fail(BootstrapError::missing_delegation);
  m_moved = true;
  const auto supervisor_document =
      read_control(m_supervisor.get(), "cgroup.procs");
  const auto supervisor_processes = supervisor_document
                                        ? parse_processes(*supervisor_document)
                                        : std::nullopt;
  if (!supervisor_processes || supervisor_processes->size() != 1 ||
      supervisor_processes->front() != ::getpid())
    return fail(supervisor_processes ? BootstrapError::missing_delegation
                                     : BootstrapError::internal_error);
  const auto remaining_document = read_control(m_root.get(), "cgroup.procs");
  const auto remaining =
      remaining_document ? parse_processes(*remaining_document) : std::nullopt;
  if (!remaining || !remaining->empty())
    return fail(remaining ? BootstrapError::missing_delegation
                          : BootstrapError::internal_error);
  return BootstrapError::none;
}

auto CgroupBootstrap::enable_controllers() -> BootstrapError {
  if (!write_control(m_root.get(), "cgroup.subtree_control",
                     "+cpu +memory +pids")) {
    const auto enable_error = errno;
    const auto partially_enabled =
        read_control(m_root.get(), "cgroup.subtree_control");
    const auto partial_tokens =
        partially_enabled ? parse_tokens(*partially_enabled) : std::nullopt;
    if (!partial_tokens) return fail(BootstrapError::internal_error);
    m_enabled = partial_tokens->contains("cpu") ||
                partial_tokens->contains("memory") ||
                partial_tokens->contains("pids");
    return fail(enable_error == EACCES || enable_error == EPERM ||
                        enable_error == EBUSY
                    ? BootstrapError::missing_delegation
                    : BootstrapError::internal_error);
  }
  m_enabled = true;
  const auto enabled = read_control(m_root.get(), "cgroup.subtree_control");
  const auto enabled_tokens = enabled ? parse_tokens(*enabled) : std::nullopt;
  if (!enabled_tokens || !enabled_tokens->contains("cpu") ||
      !enabled_tokens->contains("memory") || !enabled_tokens->contains("pids"))
    return fail(enabled_tokens ? BootstrapError::missing_controller
                               : BootstrapError::internal_error);
  return BootstrapError::none;
}

auto CgroupBootstrap::fail(const BootstrapError error) -> BootstrapError {
  return cleanup() ? error : BootstrapError::cleanup_failed;
}

auto TaskCgroup::create(const std::string_view prefix,
                        const std::string_view suffix,
                        const int delegated_root_descriptor)
    -> std::expected<TaskCgroup, TaskCgroupError> {
  Descriptor parent{::fcntl(delegated_root_descriptor, F_DUPFD_CLOEXEC, 5)};
  if (parent.get() < 0)
    return std::unexpected(TaskCgroupError::missing_delegation);
  const auto marker = read_control(parent.get(), "cgroup.controllers");
  if (!marker) return std::unexpected(TaskCgroupError::mechanism_absent);
  const auto name =
      std::string{prefix} + std::to_string(::getpid()) + std::string{suffix};
  if (::mkdirat(parent.get(), name.c_str(), S_IRWXU) != 0) {
    return std::unexpected(errno == EACCES || errno == EPERM || errno == EROFS
                               ? TaskCgroupError::missing_delegation
                               : TaskCgroupError::prerequisite_unavailable);
  }
  Descriptor child{::openat(parent.get(), name.c_str(),
                            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
  if (child.get() < 0) {
    static_cast<void>(::unlinkat(parent.get(), name.c_str(), AT_REMOVEDIR));
    return std::unexpected(TaskCgroupError::internal_error);
  }
  return TaskCgroup{std::move(parent), std::move(child), name};
}

TaskCgroup::TaskCgroup(Descriptor parent, Descriptor child, std::string name)
    : m_parent(std::move(parent)), m_child(std::move(child)),
      m_name(std::move(name)) {
}

TaskCgroup::TaskCgroup(TaskCgroup&& other) noexcept
    : m_parent(std::move(other.m_parent)), m_child(std::move(other.m_child)),
      m_name(std::move(other.m_name)), m_cleaned(other.m_cleaned) {
  other.m_cleaned = true;
}

auto TaskCgroup::operator=(TaskCgroup&& other) noexcept -> TaskCgroup& {
  if (this == &other) return *this;
  if (!m_cleaned) static_cast<void>(cleanup());
  m_parent = std::move(other.m_parent);
  m_child = std::move(other.m_child);
  m_name = std::move(other.m_name);
  m_cleaned = other.m_cleaned;
  other.m_cleaned = true;
  return *this;
}

TaskCgroup::~TaskCgroup() {
  if (!m_cleaned) static_cast<void>(cleanup());
}

auto TaskCgroup::parent() const noexcept -> int {
  return m_parent.get();
}

auto TaskCgroup::child() const noexcept -> int {
  return m_child.get();
}

auto TaskCgroup::name() const noexcept -> std::string_view {
  return m_name;
}

auto TaskCgroup::has_required_controllers() const -> std::optional<bool> {
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

auto TaskCgroup::processes() const -> std::optional<std::vector<pid_t>> {
  const auto value = read_control(m_child.get(), "cgroup.procs");
  return value ? parse_processes(*value) : std::nullopt;
}

auto TaskCgroup::cleanup() -> bool {
  if (m_cleaned) return true;
  bool trustworthy{true};
  if (!write_control(m_child.get(), "cgroup.kill", "1")) {
    const auto current_processes = processes();
    if (!current_processes) {
      trustworthy = false;
    } else {
      for (const auto process : *current_processes) {
        auto pidfd = pidfd_open(process);
        if (pidfd.get() < 0 || !pidfd_kill(pidfd.get())) trustworthy = false;
      }
    }
  }
  const auto deadline = std::chrono::steady_clock::now() + observation_timeout;
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

} // namespace aiforge::evaluation::process_isolation::linux_support
