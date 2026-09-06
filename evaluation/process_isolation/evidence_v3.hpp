#pragma once

#include "evidence.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace aiforge::evaluation::process_isolation::v3 {

inline constexpr std::uint32_t evidence_schema_version = 3;
inline constexpr std::size_t maximum_child_record_bytes = 4096;
inline constexpr std::size_t maximum_report_bytes = 8192;
inline constexpr std::size_t maximum_platform_metadata_bytes = 128;

enum class ProbeId {
  direct_process_tree_cgroup_nonescape,
  low_capability_nonescalation,
  private_root_capability_discard,
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
  ProbeId probe_id{ProbeId::direct_process_tree_cgroup_nonescape};
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

} // namespace aiforge::evaluation::process_isolation::v3
