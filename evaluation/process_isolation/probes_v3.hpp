#pragma once

#include "evidence_v3.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
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

struct PrivateCapabilityChecks {
  bool private_root_before_discard{};
  bool namespace_setpcap_available{};
  bool bounding_emptied{};
  bool securebits_locked{};
  bool capability_sets_empty{};
  bool ambient_empty{};
  bool no_new_privileges{};
  bool namespace_creation_denied{};
  bool capability_regain_denied{};
  bool descriptor_exec{};
  bool setup_descriptors_closed{};
  bool fork_descendant_rechecked{};
  bool clone_descendant_rechecked{};
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
[[nodiscard]] auto low_capability_outcome(const LowCapabilityChecks& checks,
                                          bool cleanup_complete) -> ProbeRecord;
[[nodiscard]] auto capability_prerequisite_outcome(bool supported_architecture,
                                                   bool cap_last_readable,
                                                   bool cap_last_bounded)
    -> ProbeRecord;
[[nodiscard]] auto cap_last_outcome(std::string_view document) -> ProbeRecord;
[[nodiscard]] auto bounding_subset_outcome(std::uint64_t launch,
                                           std::uint64_t current)
    -> ProbeRecord;
[[nodiscard]] auto encoded_bounding_fingerprint(std::uint64_t fingerprint)
    -> std::optional<std::string>;
[[nodiscard]] auto bounding_read_outcome(int error_number) -> ProbeRecord;
[[nodiscard]] auto x32_namespace_outcome(long result, int error_number)
    -> ProbeRecord;
[[nodiscard]] auto private_capability_outcome(
    const PrivateCapabilityChecks& checks, bool cleanup_complete)
    -> ProbeRecord;
[[nodiscard]] auto securebits_outcome(unsigned long observed) -> ProbeRecord;

} // namespace test_support
#endif

} // namespace aiforge::evaluation::process_isolation::v3
