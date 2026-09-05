#pragma once

#include "evidence_v3.hpp"

#include <array>
#include <filesystem>
#include <string_view>

namespace aiforge::evaluation::process_isolation::v3 {

[[nodiscard]] auto run_probe(ProbeId probe_id,
                             const std::filesystem::path& state_directory,
                             bool has_delegated_cgroup_root) -> ProbeRecord;

// Internal entry point used only by the fixed evaluator helper after the
// direct-tree restrictions have survived descriptor-relative re-exec.
[[nodiscard]] auto run_direct_tree_payload(std::string_view sibling_name)
    -> int;

#if defined(AIFORGE_PROCESS_ISOLATION_TEST_SUPPORT)
namespace test_support {

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

[[nodiscard]] auto direct_tree_outcome(const DirectTreeChecks& checks,
                                       bool cleanup_complete) -> ProbeRecord;
[[nodiscard]] auto prerequisite_outcome(bool supported_architecture,
                                        bool delegated,
                                        bool controllers_available)
    -> ProbeRecord;
[[nodiscard]] auto escape_attempt_outcome(
    const std::array<int, 4>& path_errors,
    const std::array<int, 8>& descriptor_errors,
    const std::array<int, 4>& clone_errors) -> ProbeRecord;

} // namespace test_support
#endif

} // namespace aiforge::evaluation::process_isolation::v3
