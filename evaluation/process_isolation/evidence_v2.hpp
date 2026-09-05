#pragma once

#include "evidence.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace aiforge::evaluation::process_isolation::v2 {

inline constexpr std::uint32_t evidence_schema_version = 2;
inline constexpr std::size_t maximum_child_record_bytes = 4096;
inline constexpr std::size_t maximum_report_bytes = 65536;
inline constexpr std::size_t maximum_platform_metadata_bytes = 128;

enum class ProbeId {
  cgroup_v2_delegation,
  cgroup_required_controllers,
  cgroup_atomic_child_placement,
  cgroup_self_migration_denial,
  cgroup_whole_tree_enumeration,
  cgroup_kill,
  cgroup_populated_zero,
  cgroup_setsid_containment,
  cgroup_double_fork_containment,
  cgroup_daemon_containment,
  cgroup_clone_fork_fanout,
  cgroup_leader_exit_containment,
  cgroup_cancellation_cleanup,
  cgroup_cpu_limit_enforcement,
  cgroup_memory_limit_termination,
  cgroup_pids_limit_enforcement,
  landlock_read_confinement,
  landlock_write_confinement,
  landlock_execute_confinement,
  seccomp_internet_socket_family_denial,
  seccomp_unix_socket_denial,
  private_root_construction,
  private_mount_propagation,
  descriptor_relative_launch,
  staged_input_identity,
  staged_output_identity,
  combined_setup_order,
  private_root_combined_setup_order,
  partial_setup_cleanup,
};

enum class ReasonCode {
  none,
  unsupported_kernel,
  unsupported_architecture,
  permission_denied,
  mechanism_absent,
  missing_delegation,
  missing_controller,
  enforcement_failed,
  prerequisite_unavailable,
  unsupported_combination,
  timeout,
  cancelled,
  limit_not_triggered,
  pid_reuse,
  setup_race,
  signaled,
  nonzero_exit,
  malformed_protocol,
  output_limit,
  cleanup_failed,
  internal_error,
};

struct ProbeRecord {
  ProbeId probe_id{ProbeId::cgroup_v2_delegation};
  ProbeState state{ProbeState::probe_error};
  ReasonCode reason{ReasonCode::internal_error};
  auto operator==(const ProbeRecord&) const -> bool = default;
};

struct EvidenceReport {
  std::string source_sha;
  std::string platform;
  std::string kernel;
  std::string architecture;
  std::vector<ProbeRecord> probes;
  auto operator==(const EvidenceReport&) const -> bool = default;
};

[[nodiscard]] auto required_probe_ids() -> std::span<const ProbeId>;
[[nodiscard]] auto probe_id_name(ProbeId value) -> std::string_view;
[[nodiscard]] auto reason_code_name(ReasonCode value) -> std::string_view;

[[nodiscard]] auto validate_child_record(const ProbeRecord& value)
    -> std::expected<void, EvidenceError>;
[[nodiscard]] auto serialize_child_record(const ProbeRecord& value)
    -> std::expected<std::string, EvidenceError>;
[[nodiscard]] auto parse_child_record(std::string_view document)
    -> std::expected<ProbeRecord, EvidenceError>;

[[nodiscard]] auto validate_report(const EvidenceReport& value)
    -> std::expected<void, EvidenceError>;
[[nodiscard]] auto serialize_report(const EvidenceReport& value)
    -> std::expected<std::string, EvidenceError>;
[[nodiscard]] auto parse_report(std::string_view document)
    -> std::expected<EvidenceReport, EvidenceError>;
[[nodiscard]] auto evidence_run_succeeded(const EvidenceReport& value)
    -> std::expected<bool, EvidenceError>;

} // namespace aiforge::evaluation::process_isolation::v2
