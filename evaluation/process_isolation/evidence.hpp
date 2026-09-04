#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace aiforge::evaluation::process_isolation {

inline constexpr std::uint32_t evidence_schema_version = 1;
inline constexpr std::size_t maximum_child_record_bytes = 4096;
inline constexpr std::size_t maximum_report_bytes = 65536;
inline constexpr std::size_t maximum_platform_metadata_bytes = 128;

enum class ProbeId {
  no_new_privileges,
  rlimit_cpu,
  rlimit_address_space,
  rlimit_process_count,
  rlimit_descriptor_count,
  rlimit_file_size,
  inherited_descriptors,
  subreaper_session_cleanup,
  subreaper_double_fork_cleanup,
  landlock_read_confinement,
  user_namespace,
  mount_namespace,
  pid_namespace,
  network_namespace,
  seccomp_socket_creation_denial,
  disposable_workspace,
  openat2_resolution,
  fexecve_identity,
  execveat_identity,
  fchdir_identity,
  staged_input_identity,
};

enum class ProbeState { enforced, unavailable, probe_error };

enum class ReasonCode {
  none,
  unsupported_kernel,
  unsupported_architecture,
  permission_denied,
  mechanism_absent,
  enforcement_failed,
  prerequisite_unavailable,
  timeout,
  signaled,
  nonzero_exit,
  malformed_protocol,
  output_limit,
  cleanup_failed,
  internal_error,
};

struct ProbeRecord {
  ProbeId probe_id{ProbeId::no_new_privileges};
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

enum class EvidenceErrorCode {
  malformed_json,
  duplicate_field,
  unknown_field,
  missing_field,
  invalid_schema,
  invalid_value,
  resource_exhausted,
};

struct EvidenceError {
  EvidenceErrorCode code{EvidenceErrorCode::invalid_value};
  std::string message;
  auto operator==(const EvidenceError&) const -> bool = default;
};

[[nodiscard]] auto required_probe_ids() -> std::span<const ProbeId>;
[[nodiscard]] auto probe_id_name(ProbeId value) -> std::string_view;
[[nodiscard]] auto probe_state_name(ProbeState value) -> std::string_view;
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

} // namespace aiforge::evaluation::process_isolation
