#include "probes_v3.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace isolation = aiforge::evaluation::process_isolation;
namespace v3 = aiforge::evaluation::process_isolation::v3;

namespace {

[[nodiscard]] auto parse_probe_id(const std::string_view name)
    -> const v3::ProbeId* {
  for (const auto& probe_id : v3::required_probe_ids()) {
    if (v3::probe_id_name(probe_id) == name) return &probe_id;
  }
  return nullptr;
}

[[nodiscard]] auto write_all(const std::string_view document) -> bool {
  std::size_t offset{};
  while (offset < document.size()) {
    const auto count = ::write(STDOUT_FILENO, document.data() + offset,
                               document.size() - offset);
    if (count < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (count == 0) return false;
    offset += static_cast<std::size_t>(count);
  }
  return true;
}

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

[[nodiscard]] auto allowed_descriptor(const std::string_view name,
                                      const int directory,
                                      const std::initializer_list<int> allowed)
    -> bool {
  if (name.empty() || name.front() == '.') return true;
  char* end{};
  errno = 0;
  const std::string owned_name{name};
  const auto parsed = std::strtol(owned_name.c_str(), &end, 10);
  const auto descriptor = static_cast<int>(parsed);
  return errno == 0 && end != owned_name.c_str() && *end == '\0' &&
         (descriptor == directory ||
          std::ranges::find(allowed, descriptor) != allowed.end());
}

[[nodiscard]] auto descriptors_are_sanitized(
    const std::initializer_list<int> allowed) -> bool {
  const auto directory =
      ::open("/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (directory < 0) return false;
  const bool safe = visit_directory_entries(directory, [&](const auto name) {
    return allowed_descriptor(name, directory, allowed);
  });
  static_cast<void>(::close(directory));
  return safe;
}

[[nodiscard]] auto common_process_state() -> bool {
  if (::environ != nullptr && ::environ[0] != nullptr) return false;
  errno = 0;
  if (::fcntl(STDIN_FILENO, F_GETFD) != -1 || errno != EBADF) return false;
  struct stat output{};
  struct stat error{};
  return ::fstat(STDOUT_FILENO, &output) == 0 &&
         ::fstat(STDERR_FILENO, &error) == 0 && S_ISFIFO(output.st_mode) &&
         S_ISCHR(error.st_mode);
}

[[nodiscard]] auto probe_is_sanitized(const bool has_delegated_root) -> bool {
  struct stat root{};
  if (!common_process_state() ||
      (has_delegated_root &&
       (::fstat(4, &root) != 0 || !S_ISDIR(root.st_mode))))
    return false;
  return has_delegated_root
             ? descriptors_are_sanitized({STDOUT_FILENO, STDERR_FILENO, 4})
             : descriptors_are_sanitized({STDOUT_FILENO, STDERR_FILENO});
}

[[nodiscard]] auto payload_is_sanitized() -> bool {
  struct stat outcome{};
  if (!common_process_state() || ::fstat(3, &outcome) != 0 ||
      !S_ISFIFO(outcome.st_mode))
    return false;
  errno = 0;
  if (::fcntl(4, F_GETFD) != -1 || errno != EBADF) return false;
  for (const auto descriptor : {5, 6, 7, 8}) {
    struct stat cgroup{};
    if (::fstat(descriptor, &cgroup) != 0 || !S_ISDIR(cgroup.st_mode))
      return false;
  }
  return descriptors_are_sanitized(
      {STDOUT_FILENO, STDERR_FILENO, 3, 5, 6, 7, 8});
}

} // namespace

auto main(const int argc, char* argv[]) -> int {
  try {
    if (argc == 3 && argv != nullptr && argv[1] != nullptr &&
        argv[2] != nullptr &&
        std::string_view{argv[1]} == "--direct-tree-payload") {
      return payload_is_sanitized() ? v3::run_direct_tree_payload(argv[2]) : 70;
    }
    if (argc != 4 || argv == nullptr || argv[1] == nullptr ||
        argv[2] == nullptr || argv[3] == nullptr)
      return 64;
    const auto* probe_id = parse_probe_id(argv[1]);
    if (probe_id == nullptr) return 64;
    auto record = v3::ProbeRecord{*probe_id, isolation::ProbeState::probe_error,
                                  v3::ReasonCode::internal_error};
    const std::string_view cgroup_mode{argv[3]};
    const bool has_delegated_root = cgroup_mode == "delegated-root-fd-4";
    if (!has_delegated_root && cgroup_mode != "no-delegated-root") return 64;
    if (probe_is_sanitized(has_delegated_root))
      record = v3::run_probe(*probe_id, argv[2], has_delegated_root);
    const auto document = v3::serialize_child_record(record);
    if (!document || !write_all(*document)) return 70;
    return 0;
  } catch (...) {
    return 70;
  }
}
