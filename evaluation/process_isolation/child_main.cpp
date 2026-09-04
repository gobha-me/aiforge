#include "probes.hpp"

#include <cerrno>
#include <string>
#include <string_view>

#include <unistd.h>

namespace isolation = aiforge::evaluation::process_isolation;

namespace {

[[nodiscard]] auto parse_probe_id(const std::string_view name)
    -> const isolation::ProbeId* {
  for (const auto& probe_id : isolation::required_probe_ids()) {
    if (isolation::probe_id_name(probe_id) == name) return &probe_id;
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

} // namespace

auto main(const int argc, char* argv[]) -> int {
  try {
    if (argc != 3 || argv == nullptr || argv[1] == nullptr ||
        argv[2] == nullptr)
      return 64;
    const auto* probe_id = parse_probe_id(argv[1]);
    if (probe_id == nullptr) return 64;

    auto record =
        isolation::ProbeRecord{*probe_id, isolation::ProbeState::probe_error,
                               isolation::ReasonCode::internal_error};
    if (isolation::child_process_is_sanitized())
      record = isolation::run_probe(*probe_id, argv[2]);
    const auto document = isolation::serialize_child_record(record);
    if (!document || !write_all(*document)) return 70;
    return 0;
  } catch (...) {
    return 70;
  }
}
