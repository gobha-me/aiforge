#pragma once

#include "evidence_v2.hpp"

#include <filesystem>

namespace aiforge::evaluation::process_isolation::v2 {

[[nodiscard]] auto run_probe(ProbeId probe_id,
                             const std::filesystem::path& state_directory,
                             bool has_delegated_cgroup_root) -> ProbeRecord;

// Internal entry point used only by the fixed evaluator helper after a
// descriptor-relative re-exec. It never becomes a production launch surface.
[[nodiscard]] auto run_combined_setup_payload(
    const std::filesystem::path& state_directory) -> int;

#if defined(AIFORGE_PROCESS_ISOLATION_TEST_SUPPORT)
namespace test_support {

[[nodiscard]] auto cgroup_prerequisite_outcome(bool unified, bool delegated,
                                               bool cpu, bool memory, bool pids)
    -> ProbeRecord;
[[nodiscard]] auto migration_attempt_outcome(bool confinement_applied,
                                             int parent_error,
                                             int sibling_error) -> ProbeRecord;
[[nodiscard]] auto memory_limit_outcome(bool killed, bool oom_kill_advanced)
    -> ProbeRecord;
[[nodiscard]] auto pids_limit_outcome(bool exhausted, bool tree_complete,
                                      bool cleanup_complete) -> ProbeRecord;
[[nodiscard]] auto execute_confinement_outcome(bool local_executed,
                                               bool outside_denied)
    -> ProbeRecord;
[[nodiscard]] auto mount_propagation_outcome(bool child_mount_established,
                                             bool visible_in_parent,
                                             bool cleanup_complete)
    -> ProbeRecord;
[[nodiscard]] auto cancellation_cleanup_outcome(bool tree_ready,
                                                bool cancellation_requested,
                                                bool tree_terminated,
                                                bool cleanup_complete)
    -> ProbeRecord;
[[nodiscard]] auto write_confinement_outcome(bool allowed_write_succeeded,
                                             bool existing_write_denied,
                                             bool truncation_denied,
                                             bool removal_denied,
                                             bool creation_denied,
                                             bool rename_denied) -> ProbeRecord;
[[nodiscard]] auto pid_identity_outcome(bool pidfd_opened, bool identity_stable)
    -> ProbeRecord;
[[nodiscard]] auto setup_order_outcome(
    bool limits_applied, bool placed, bool staged_descriptors,
    bool descriptor_launched, bool private_root_applied,
    bool filesystem_applied, bool internet_denied, bool unix_denied,
    bool payload_reached, bool target_waiting_for_cleanup) -> ProbeRecord;

} // namespace test_support
#endif

} // namespace aiforge::evaluation::process_isolation::v2
