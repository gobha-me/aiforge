#pragma once

#include "evidence_v2.hpp"

#include <filesystem>

namespace aiforge::evaluation::process_isolation::v2 {

[[nodiscard]] auto run_probe(ProbeId probe_id,
                             const std::filesystem::path& state_directory,
                             bool has_delegated_cgroup_root) -> ProbeRecord;

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
[[nodiscard]] auto pid_identity_outcome(bool pidfd_opened, bool identity_stable)
    -> ProbeRecord;
[[nodiscard]] auto setup_order_outcome(bool placed, bool filesystem_applied,
                                       bool network_applied,
                                       bool payload_reached,
                                       bool target_waiting_for_cleanup)
    -> ProbeRecord;

} // namespace test_support
#endif

} // namespace aiforge::evaluation::process_isolation::v2
