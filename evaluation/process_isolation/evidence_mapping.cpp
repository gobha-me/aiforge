#include "evidence_mapping.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <ranges>
#include <utility>

namespace aiforge::evaluation::process_isolation::mapping {
namespace {

using V1Probe = ::aiforge::evaluation::process_isolation::ProbeId;
using V2Probe = ::aiforge::evaluation::process_isolation::v2::ProbeId;

constexpr std::array low_v1{
    V1Probe::no_new_privileges,  V1Probe::rlimit_descriptor_count,
    V1Probe::rlimit_file_size,   V1Probe::inherited_descriptors,
    V1Probe::openat2_resolution, V1Probe::fexecve_identity,
    V1Probe::fchdir_identity,
};

constexpr std::array low_v2{
    V2Probe::cgroup_v2_delegation,
    V2Probe::cgroup_required_controllers,
    V2Probe::cgroup_atomic_child_placement,
    V2Probe::cgroup_self_migration_denial,
    V2Probe::cgroup_whole_tree_enumeration,
    V2Probe::cgroup_kill,
    V2Probe::cgroup_populated_zero,
    V2Probe::cgroup_setsid_containment,
    V2Probe::cgroup_double_fork_containment,
    V2Probe::cgroup_daemon_containment,
    V2Probe::cgroup_clone_fork_fanout,
    V2Probe::cgroup_leader_exit_containment,
    V2Probe::cgroup_cancellation_cleanup,
    V2Probe::cgroup_cpu_limit_enforcement,
    V2Probe::cgroup_memory_limit_termination,
    V2Probe::cgroup_pids_limit_enforcement,
    V2Probe::descriptor_relative_launch,
    V2Probe::partial_setup_cleanup,
};

constexpr std::array medium_v2{
    V2Probe::landlock_read_confinement,
    V2Probe::landlock_write_confinement,
    V2Probe::landlock_execute_confinement,
    V2Probe::seccomp_internet_socket_family_denial,
    V2Probe::seccomp_unix_socket_denial,
    V2Probe::combined_setup_order,
};

constexpr std::array high_v1{V1Probe::disposable_workspace};

constexpr std::array high_v2{
    V2Probe::private_root_construction,
    V2Probe::private_mount_propagation,
    V2Probe::staged_input_identity,
    V2Probe::staged_output_identity,
    V2Probe::private_root_combined_setup_order,
};

[[nodiscard]] auto valid_source_sha(const std::string_view value) -> bool {
  return value.size() == 40 &&
         std::ranges::all_of(value, [](const unsigned char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

[[nodiscard]] auto unavailable(const EvidenceLevel level,
                               const AssessmentReason reason,
                               const std::string_view conjunct,
                               const std::string_view evidence_reason = {})
    -> LevelAssessment {
  return {level, false, reason, conjunct, evidence_reason};
}

[[nodiscard]] auto all_unavailable(const AssessmentReason reason,
                                   const std::string_view detail)
    -> EvidenceAssessment {
  return {{{unavailable(EvidenceLevel::low, reason, detail),
            unavailable(EvidenceLevel::medium, reason, detail),
            unavailable(EvidenceLevel::high, reason, detail)}}};
}

[[nodiscard]] auto assess_record(const ProbeRecord& record)
    -> std::optional<LevelAssessment> {
  if (record.state == ProbeState::enforced && record.reason == ReasonCode::none)
    return std::nullopt;
  const auto reason = record.state == ProbeState::probe_error
                          ? AssessmentReason::indeterminate_evidence
                          : AssessmentReason::unavailable_conjunct;
  return unavailable(EvidenceLevel::low, reason, probe_id_name(record.probe_id),
                     reason_code_name(record.reason));
}

[[nodiscard]] auto assess_record(const v2::ProbeRecord& record)
    -> std::optional<LevelAssessment> {
  if (record.state == ProbeState::enforced &&
      record.reason == v2::ReasonCode::none)
    return std::nullopt;
  const auto reason = record.state == ProbeState::probe_error
                          ? AssessmentReason::indeterminate_evidence
                          : AssessmentReason::unavailable_conjunct;
  return unavailable(EvidenceLevel::low, reason,
                     v2::probe_id_name(record.probe_id),
                     v2::reason_code_name(record.reason));
}

[[nodiscard]] auto find_record(const EvidenceReport& report,
                               const V1Probe probe) -> const ProbeRecord* {
  const auto found =
      std::ranges::find(report.probes, probe, &ProbeRecord::probe_id);
  return found == report.probes.end() ? nullptr : &*found;
}

[[nodiscard]] auto find_record(const v2::EvidenceReport& report,
                               const V2Probe probe) -> const v2::ProbeRecord* {
  const auto found =
      std::ranges::find(report.probes, probe, &v2::ProbeRecord::probe_id);
  return found == report.probes.end() ? nullptr : &*found;
}

[[nodiscard]] auto cleanup_failed(const ProbeRecord& record) -> bool {
  return record.state == ProbeState::probe_error &&
         record.reason == ReasonCode::cleanup_failed;
}

[[nodiscard]] auto cleanup_failed(const v2::ProbeRecord& record) -> bool {
  return record.state == ProbeState::probe_error &&
         record.reason == v2::ReasonCode::cleanup_failed;
}

template <typename Report, typename Required>
[[nodiscard]] auto first_unmet(const Report& report, const Required& required)
    -> std::optional<LevelAssessment> {
  for (const auto probe : required) {
    const auto* record = find_record(report, probe);
    if (record == nullptr)
      return unavailable(EvidenceLevel::low,
                         AssessmentReason::malformed_evidence, "required probe",
                         "missing");
    if (auto result = assess_record(*record)) return result;
  }
  return std::nullopt;
}

template <typename Report, typename Required>
[[nodiscard]] auto first_cleanup_failure(const Report& report,
                                         const Required& required)
    -> std::optional<LevelAssessment> {
  for (const auto probe : required) {
    const auto* record = find_record(report, probe);
    if (record != nullptr && cleanup_failed(*record))
      return assess_record(*record);
  }
  return std::nullopt;
}

[[nodiscard]] auto for_level(LevelAssessment assessment,
                             const EvidenceLevel level) -> LevelAssessment {
  assessment.level = level;
  return assessment;
}

struct ValidatedReports {
  EvidenceReport v1;
  v2::EvidenceReport v2;
};

[[nodiscard]] auto validate_reports(
    const std::string_view expected_source_sha,
    const std::optional<std::string_view> schema_v1_document,
    const std::optional<std::string_view> schema_v2_document)
    -> std::expected<ValidatedReports, EvidenceAssessment> {
  if (!valid_source_sha(expected_source_sha))
    return std::unexpected(
        all_unavailable(AssessmentReason::malformed_evidence,
                        "expected source revision is invalid"));
  if (!schema_v1_document)
    return std::unexpected(all_unavailable(AssessmentReason::missing_evidence,
                                           "schema-v1 evidence is missing"));
  if (!schema_v2_document)
    return std::unexpected(all_unavailable(AssessmentReason::missing_evidence,
                                           "schema-v2 evidence is missing"));
  auto v1_report = parse_report(*schema_v1_document);
  if (!v1_report)
    return std::unexpected(all_unavailable(AssessmentReason::malformed_evidence,
                                           "schema-v1 evidence is malformed"));
  auto v2_report = v2::parse_report(*schema_v2_document);
  if (!v2_report)
    return std::unexpected(all_unavailable(AssessmentReason::malformed_evidence,
                                           "schema-v2 evidence is malformed"));
  if (v1_report->source_sha != v2_report->source_sha)
    return std::unexpected(
        all_unavailable(AssessmentReason::conflicting_evidence,
                        "evidence source revisions conflict"));
  if (v1_report->kernel != v2_report->kernel ||
      v1_report->architecture != v2_report->architecture ||
      v1_report->platform != v2_report->platform)
    return std::unexpected(
        all_unavailable(AssessmentReason::conflicting_evidence,
                        "evidence host identities conflict"));
  if (v1_report->source_sha != expected_source_sha)
    return std::unexpected(all_unavailable(
        AssessmentReason::stale_evidence, "evidence source revision is stale"));
  return ValidatedReports{std::move(*v1_report), std::move(*v2_report)};
}

} // namespace

auto assess_linux_evidence(
    const std::string_view expected_source_sha,
    const std::optional<std::string_view> schema_v1_document,
    const std::optional<std::string_view> schema_v2_document)
    -> EvidenceAssessment {
  const auto reports = validate_reports(expected_source_sha, schema_v1_document,
                                        schema_v2_document);
  if (!reports) return reports.error();
  const auto& v1_report = reports->v1;
  const auto& v2_report = reports->v2;

  std::optional<LevelAssessment> low_cleanup =
      first_cleanup_failure(v1_report, low_v1);
  if (!low_cleanup) low_cleanup = first_cleanup_failure(v2_report, low_v2);
  auto low_failure = low_cleanup;
  if (!low_failure) low_failure = first_unmet(v1_report, low_v1);
  if (!low_failure) low_failure = first_unmet(v2_report, low_v2);
  if (!low_failure)
    low_failure =
        unavailable(EvidenceLevel::low, AssessmentReason::unproven_conjunct,
                    "payload_execution_nonescape");
  const auto low = for_level(*low_failure, EvidenceLevel::low);

  auto medium_cleanup = low_cleanup;
  if (!medium_cleanup)
    medium_cleanup = first_cleanup_failure(v2_report, medium_v2);
  const auto medium =
      for_level(medium_cleanup.value_or(low), EvidenceLevel::medium);

  auto high_cleanup = medium_cleanup;
  if (!high_cleanup) high_cleanup = first_cleanup_failure(v1_report, high_v1);
  if (!high_cleanup) high_cleanup = first_cleanup_failure(v2_report, high_v2);
  const auto high =
      for_level(high_cleanup.value_or(medium), EvidenceLevel::high);
  return {{{low, medium, high}}};
}

auto evidence_level_name(const EvidenceLevel value) -> std::string_view {
  switch (value) {
    case EvidenceLevel::low: return "low";
    case EvidenceLevel::medium: return "medium";
    case EvidenceLevel::high: return "high";
  }
  return "unknown";
}

auto assessment_reason_name(const AssessmentReason value) -> std::string_view {
  switch (value) {
    case AssessmentReason::none: return "none";
    case AssessmentReason::missing_evidence: return "missing_evidence";
    case AssessmentReason::malformed_evidence: return "malformed_evidence";
    case AssessmentReason::stale_evidence: return "stale_evidence";
    case AssessmentReason::conflicting_evidence: return "conflicting_evidence";
    case AssessmentReason::unproven_conjunct: return "unproven_conjunct";
    case AssessmentReason::unavailable_conjunct: return "unavailable_conjunct";
    case AssessmentReason::indeterminate_evidence:
      return "indeterminate_evidence";
  }
  return "unknown";
}

} // namespace aiforge::evaluation::process_isolation::mapping
