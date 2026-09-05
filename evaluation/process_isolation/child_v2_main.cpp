#include "probes.hpp"
#include "probes_v2.hpp"

#include <cerrno>
#include <cstdlib>
#include <string_view>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace isolation = aiforge::evaluation::process_isolation;
namespace v2 = aiforge::evaluation::process_isolation::v2;

namespace {

[[nodiscard]] auto parse_probe_id(const std::string_view name)
    -> const v2::ProbeId* {
  for (const auto& probe_id : v2::required_probe_ids()) {
    if (v2::probe_id_name(probe_id) == name) return &probe_id;
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

[[nodiscard]] auto child_is_sanitized(const bool has_delegated_root) -> bool {
  if (::environ != nullptr && ::environ[0] != nullptr) return false;
  errno = 0;
  if (::fcntl(STDIN_FILENO, F_GETFD) != -1 || errno != EBADF) return false;
  struct stat output{};
  struct stat error{};
  struct stat root{};
  if (::fstat(STDOUT_FILENO, &output) != 0 ||
      ::fstat(STDERR_FILENO, &error) != 0 || !S_ISFIFO(output.st_mode) ||
      !S_ISCHR(error.st_mode) ||
      (has_delegated_root &&
       (::fstat(4, &root) != 0 || !S_ISDIR(root.st_mode))))
    return false;
  DIR* directory = ::opendir("/proc/self/fd");
  if (directory == nullptr) return false;
  const auto inspection = ::dirfd(directory);
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
        descriptor != inspection && !(has_delegated_root && descriptor == 4)) {
      safe = false;
      break;
    }
  }
  static_cast<void>(::closedir(directory));
  return safe;
}

[[nodiscard]] auto combined_payload_is_sanitized() -> bool {
  if (::environ != nullptr && ::environ[0] != nullptr) return false;
  errno = 0;
  if (::fcntl(STDIN_FILENO, F_GETFD) != -1 || errno != EBADF) return false;
  struct stat output{};
  struct stat error{};
  struct stat ready{};
  struct stat staged_input{};
  struct stat staged_output{};
  if (::fstat(STDOUT_FILENO, &output) != 0 ||
      ::fstat(STDERR_FILENO, &error) != 0 || ::fstat(3, &ready) != 0 ||
      ::fstat(5, &staged_input) != 0 || ::fstat(6, &staged_output) != 0 ||
      !S_ISFIFO(output.st_mode) || !S_ISCHR(error.st_mode) ||
      !S_ISFIFO(ready.st_mode) || !S_ISREG(staged_input.st_mode) ||
      !S_ISREG(staged_output.st_mode))
    return false;
  DIR* directory = ::opendir("/proc/self/fd");
  if (directory == nullptr) return false;
  const auto inspection = ::dirfd(directory);
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
        descriptor != 3 && descriptor != 5 && descriptor != 6 &&
        descriptor != inspection) {
      safe = false;
      break;
    }
  }
  static_cast<void>(::closedir(directory));
  return safe;
}

} // namespace

auto main(const int argc, char* argv[]) -> int {
  try {
    if (argc == 3 && argv != nullptr && argv[1] != nullptr &&
        argv[2] != nullptr &&
        std::string_view{argv[1]} == "--combined-setup-payload") {
      return combined_payload_is_sanitized()
                 ? v2::run_combined_setup_payload(argv[2])
                 : 70;
    }
    if (argc != 4 || argv == nullptr || argv[1] == nullptr ||
        argv[2] == nullptr || argv[3] == nullptr) {
      return 64;
    }
    const auto* probe_id = parse_probe_id(argv[1]);
    if (probe_id == nullptr) return 64;
    auto record = v2::ProbeRecord{*probe_id, isolation::ProbeState::probe_error,
                                  v2::ReasonCode::internal_error};
    const std::string_view cgroup_mode{argv[3]};
    const bool has_delegated_root = cgroup_mode == "delegated-root-fd-4";
    if (!has_delegated_root && cgroup_mode != "no-delegated-root") return 64;
    if (child_is_sanitized(has_delegated_root))
      record = v2::run_probe(*probe_id, argv[2], has_delegated_root);
    const auto document = v2::serialize_child_record(record);
    if (!document || !write_all(*document)) return 70;
    return 0;
  } catch (...) {
    return 70;
  }
}
