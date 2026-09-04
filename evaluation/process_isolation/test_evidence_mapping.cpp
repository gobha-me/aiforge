#include "evidence_mapping.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace isolation = aiforge::evaluation::process_isolation;
namespace mapping = aiforge::evaluation::process_isolation::mapping;
namespace v2 = aiforge::evaluation::process_isolation::v2;

namespace {

constexpr auto source_sha = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

[[nodiscard]] auto complete_v1() -> isolation::EvidenceReport {
  isolation::EvidenceReport report{source_sha, "linux", "6.8.0", "x86_64", {}};
  for (const auto probe : isolation::required_probe_ids()) {
    report.probes.push_back(
        {probe, isolation::ProbeState::enforced, isolation::ReasonCode::none});
  }
  return report;
}

[[nodiscard]] auto complete_v2() -> v2::EvidenceReport {
  v2::EvidenceReport report{source_sha, "linux", "6.8.0", "x86_64", {}};
  for (const auto probe : v2::required_probe_ids()) {
    report.probes.push_back(
        {probe, isolation::ProbeState::enforced, v2::ReasonCode::none});
  }
  return report;
}

struct Documents {
  std::string v1;
  std::string v2;
};

struct V1Conjunct {
  isolation::ProbeId probe;
  std::size_t first_incomplete_level;
};

constexpr std::array v1_conjuncts{
    V1Conjunct{isolation::ProbeId::no_new_privileges, 0},
    V1Conjunct{isolation::ProbeId::rlimit_descriptor_count, 0},
    V1Conjunct{isolation::ProbeId::rlimit_file_size, 0},
    V1Conjunct{isolation::ProbeId::inherited_descriptors, 0},
    V1Conjunct{isolation::ProbeId::openat2_resolution, 0},
    V1Conjunct{isolation::ProbeId::fexecve_identity, 0},
    V1Conjunct{isolation::ProbeId::fchdir_identity, 0},
    V1Conjunct{isolation::ProbeId::disposable_workspace, 2},
};

struct V2Conjunct {
  v2::ProbeId probe;
  std::size_t first_incomplete_level;
};

constexpr std::array v2_conjuncts{
    V2Conjunct{v2::ProbeId::cgroup_v2_delegation, 0},
    V2Conjunct{v2::ProbeId::cgroup_required_controllers, 0},
    V2Conjunct{v2::ProbeId::cgroup_atomic_child_placement, 0},
    V2Conjunct{v2::ProbeId::cgroup_self_migration_denial, 0},
    V2Conjunct{v2::ProbeId::cgroup_whole_tree_enumeration, 0},
    V2Conjunct{v2::ProbeId::cgroup_kill, 0},
    V2Conjunct{v2::ProbeId::cgroup_populated_zero, 0},
    V2Conjunct{v2::ProbeId::cgroup_setsid_containment, 0},
    V2Conjunct{v2::ProbeId::cgroup_double_fork_containment, 0},
    V2Conjunct{v2::ProbeId::cgroup_daemon_containment, 0},
    V2Conjunct{v2::ProbeId::cgroup_clone_fork_fanout, 0},
    V2Conjunct{v2::ProbeId::cgroup_leader_exit_containment, 0},
    V2Conjunct{v2::ProbeId::cgroup_cancellation_cleanup, 0},
    V2Conjunct{v2::ProbeId::cgroup_cpu_limit_enforcement, 0},
    V2Conjunct{v2::ProbeId::cgroup_memory_limit_termination, 0},
    V2Conjunct{v2::ProbeId::cgroup_pids_limit_enforcement, 0},
    V2Conjunct{v2::ProbeId::descriptor_relative_launch, 0},
    V2Conjunct{v2::ProbeId::partial_setup_cleanup, 0},
    V2Conjunct{v2::ProbeId::landlock_read_confinement, 1},
    V2Conjunct{v2::ProbeId::landlock_write_confinement, 1},
    V2Conjunct{v2::ProbeId::landlock_execute_confinement, 1},
    V2Conjunct{v2::ProbeId::seccomp_internet_socket_family_denial, 1},
    V2Conjunct{v2::ProbeId::seccomp_unix_socket_denial, 1},
    V2Conjunct{v2::ProbeId::combined_setup_order, 1},
    V2Conjunct{v2::ProbeId::private_root_construction, 2},
    V2Conjunct{v2::ProbeId::private_mount_propagation, 2},
    V2Conjunct{v2::ProbeId::staged_input_identity, 2},
    V2Conjunct{v2::ProbeId::staged_output_identity, 2},
    V2Conjunct{v2::ProbeId::private_root_combined_setup_order, 2},
};

[[nodiscard]] auto documents(const isolation::EvidenceReport& v1_report,
                             const v2::EvidenceReport& v2_report) -> Documents {
  auto encoded_v1 = isolation::serialize_report(v1_report);
  auto encoded_v2 = v2::serialize_report(v2_report);
  REQUIRE(encoded_v1);
  REQUIRE(encoded_v2);
  return {std::move(*encoded_v1), std::move(*encoded_v2)};
}

auto record(isolation::EvidenceReport& report, const isolation::ProbeId probe)
    -> isolation::ProbeRecord& {
  const auto found = std::ranges::find(report.probes, probe,
                                       &isolation::ProbeRecord::probe_id);
  REQUIRE(found != report.probes.end());
  return *found;
}

auto record(v2::EvidenceReport& report, const v2::ProbeId probe)
    -> v2::ProbeRecord& {
  const auto found =
      std::ranges::find(report.probes, probe, &v2::ProbeRecord::probe_id);
  REQUIRE(found != report.probes.end());
  return *found;
}

void check_v3_gap(const mapping::LevelAssessment& assessment) {
  CHECK_FALSE(assessment.complete);
  CHECK(assessment.reason == mapping::AssessmentReason::unproven_conjunct);
  CHECK(assessment.conjunct == "payload_execution_nonescape");
  CHECK(assessment.evidence_reason.empty());
}

} // namespace

TEST_CASE("every selected evidence row retains cleanup-failure dominance",
          "[process-isolation][evidence-mapping][failure]") {
  for (const auto& conjunct : v1_conjuncts) {
    auto v1_report = complete_v1();
    auto v2_report = complete_v2();
    record(v1_report, conjunct.probe) = {conjunct.probe,
                                         isolation::ProbeState::probe_error,
                                         isolation::ReasonCode::cleanup_failed};
    const auto encoded = documents(v1_report, v2_report);
    const auto assessment =
        mapping::assess_linux_evidence(source_sha, encoded.v1, encoded.v2);
    CAPTURE(isolation::probe_id_name(conjunct.probe));
    for (std::size_t index{}; index < assessment.levels.size(); ++index) {
      if (index < conjunct.first_incomplete_level) {
        check_v3_gap(assessment.levels[index]);
        continue;
      }
      CHECK_FALSE(assessment.levels[index].complete);
      CHECK(assessment.levels[index].reason ==
            mapping::AssessmentReason::indeterminate_evidence);
      CHECK(assessment.levels[index].conjunct ==
            isolation::probe_id_name(conjunct.probe));
      CHECK(assessment.levels[index].evidence_reason == "cleanup_failed");
    }
  }

  for (const auto& conjunct : v2_conjuncts) {
    auto v1_report = complete_v1();
    auto v2_report = complete_v2();
    record(v2_report, conjunct.probe) = {conjunct.probe,
                                         isolation::ProbeState::probe_error,
                                         v2::ReasonCode::cleanup_failed};
    const auto encoded = documents(v1_report, v2_report);
    const auto assessment =
        mapping::assess_linux_evidence(source_sha, encoded.v1, encoded.v2);
    CAPTURE(v2::probe_id_name(conjunct.probe));
    for (std::size_t index{}; index < assessment.levels.size(); ++index) {
      if (index < conjunct.first_incomplete_level) {
        check_v3_gap(assessment.levels[index]);
        continue;
      }
      CHECK_FALSE(assessment.levels[index].complete);
      CHECK(assessment.levels[index].reason ==
            mapping::AssessmentReason::indeterminate_evidence);
      CHECK(assessment.levels[index].conjunct ==
            v2::probe_id_name(conjunct.probe));
      CHECK(assessment.levels[index].evidence_reason == "cleanup_failed");
    }
  }
}

TEST_CASE("v1 and v2 cannot complete a restricted level without v3",
          "[process-isolation][evidence-mapping][failure]") {
  const auto encoded = documents(complete_v1(), complete_v2());
  const auto assessment =
      mapping::assess_linux_evidence(source_sha, encoded.v1, encoded.v2);
  for (const auto& level : assessment.levels)
    check_v3_gap(level);
  CHECK(mapping::assessment_reason_name(
            mapping::AssessmentReason::unproven_conjunct) ==
        "unproven_conjunct");
}

TEST_CASE("restriction evidence rejects missing malformed stale and "
          "conflicting reports",
          "[process-isolation][evidence-mapping][failure]") {
  auto v1_report = complete_v1();
  auto v2_report = complete_v2();
  auto encoded = documents(v1_report, v2_report);

  for (const auto& assessment : {
           mapping::assess_linux_evidence(source_sha, std::nullopt, encoded.v2),
           mapping::assess_linux_evidence(source_sha, encoded.v1, std::nullopt),
       }) {
    for (const auto& level : assessment.levels) {
      CHECK_FALSE(level.complete);
      CHECK(level.reason == mapping::AssessmentReason::missing_evidence);
    }
  }

  for (const auto& assessment : {
           mapping::assess_linux_evidence(source_sha, "{}", encoded.v2),
           mapping::assess_linux_evidence(source_sha, encoded.v1, "{}"),
           mapping::assess_linux_evidence("not-a-source", encoded.v1,
                                          encoded.v2),
       }) {
    for (const auto& level : assessment.levels) {
      CHECK_FALSE(level.complete);
      CHECK(level.reason == mapping::AssessmentReason::malformed_evidence);
    }
  }

  auto unsupported_platform = encoded.v2;
  const auto linux = unsupported_platform.find("\"platform\":\"linux\"");
  REQUIRE(linux != std::string::npos);
  unsupported_platform.replace(
      linux, std::string_view{"\"platform\":\"linux\""}.size(),
      "\"platform\":\"other\"");
  for (const auto& level : mapping::assess_linux_evidence(
                               source_sha, encoded.v1, unsupported_platform)
                               .levels) {
    CHECK_FALSE(level.complete);
    CHECK(level.reason == mapping::AssessmentReason::malformed_evidence);
  }

  v1_report.source_sha = std::string(40, 'b');
  v2_report.source_sha = v1_report.source_sha;
  encoded = documents(v1_report, v2_report);
  for (const auto& level :
       mapping::assess_linux_evidence(source_sha, encoded.v1, encoded.v2)
           .levels) {
    CHECK_FALSE(level.complete);
    CHECK(level.reason == mapping::AssessmentReason::stale_evidence);
  }

  v2_report.source_sha = std::string(40, 'c');
  encoded = documents(v1_report, v2_report);
  for (const auto& level :
       mapping::assess_linux_evidence(source_sha, encoded.v1, encoded.v2)
           .levels) {
    CHECK_FALSE(level.complete);
    CHECK(level.reason == mapping::AssessmentReason::conflicting_evidence);
  }

  v1_report = complete_v1();
  v2_report = complete_v2();
  v2_report.kernel = "6.9.0";
  encoded = documents(v1_report, v2_report);
  for (const auto& level :
       mapping::assess_linux_evidence(source_sha, encoded.v1, encoded.v2)
           .levels) {
    CHECK_FALSE(level.complete);
    CHECK(level.reason == mapping::AssessmentReason::conflicting_evidence);
  }
}

TEST_CASE("restriction evidence rejects representative missing level conjuncts",
          "[process-isolation][evidence-mapping][failure]") {
  struct V2Failure {
    v2::ProbeId probe;
    isolation::ProbeState state;
    v2::ReasonCode reason;
    std::size_t first_incomplete_level;
  };
  constexpr std::array failures{
      V2Failure{v2::ProbeId::cgroup_v2_delegation,
                isolation::ProbeState::unavailable,
                v2::ReasonCode::missing_delegation, 0},
      V2Failure{v2::ProbeId::cgroup_required_controllers,
                isolation::ProbeState::unavailable,
                v2::ReasonCode::missing_controller, 0},
      V2Failure{v2::ProbeId::cgroup_self_migration_denial,
                isolation::ProbeState::unavailable,
                v2::ReasonCode::enforcement_failed, 0},
      V2Failure{v2::ProbeId::partial_setup_cleanup,
                isolation::ProbeState::probe_error,
                v2::ReasonCode::cleanup_failed, 0},
      V2Failure{v2::ProbeId::landlock_write_confinement,
                isolation::ProbeState::unavailable,
                v2::ReasonCode::mechanism_absent, 1},
      V2Failure{v2::ProbeId::landlock_execute_confinement,
                isolation::ProbeState::unavailable,
                v2::ReasonCode::unsupported_kernel, 1},
      V2Failure{v2::ProbeId::seccomp_unix_socket_denial,
                isolation::ProbeState::unavailable,
                v2::ReasonCode::unsupported_combination, 1},
      V2Failure{v2::ProbeId::combined_setup_order,
                isolation::ProbeState::probe_error, v2::ReasonCode::setup_race,
                1},
      V2Failure{v2::ProbeId::private_root_construction,
                isolation::ProbeState::unavailable,
                v2::ReasonCode::permission_denied, 2},
      V2Failure{v2::ProbeId::staged_output_identity,
                isolation::ProbeState::unavailable,
                v2::ReasonCode::enforcement_failed, 2},
      V2Failure{v2::ProbeId::private_root_combined_setup_order,
                isolation::ProbeState::probe_error, v2::ReasonCode::setup_race,
                2},
  };

  for (const auto& failure : failures) {
    auto v1_report = complete_v1();
    auto v2_report = complete_v2();
    record(v2_report, failure.probe) = {failure.probe, failure.state,
                                        failure.reason};
    const auto encoded = documents(v1_report, v2_report);
    const auto assessment =
        mapping::assess_linux_evidence(source_sha, encoded.v1, encoded.v2);
    const bool measured_failure_precedes_gap =
        failure.first_incomplete_level == 0 ||
        (failure.state == isolation::ProbeState::probe_error &&
         failure.reason == v2::ReasonCode::cleanup_failed);
    for (std::size_t index{}; index < assessment.levels.size(); ++index) {
      if (index < failure.first_incomplete_level ||
          !measured_failure_precedes_gap) {
        check_v3_gap(assessment.levels[index]);
        continue;
      }
      CHECK_FALSE(assessment.levels[index].complete);
      const auto expected_reason =
          failure.state == isolation::ProbeState::probe_error
              ? mapping::AssessmentReason::indeterminate_evidence
              : mapping::AssessmentReason::unavailable_conjunct;
      CHECK(assessment.levels[index].reason == expected_reason);
      CHECK(assessment.levels[index].conjunct ==
            v2::probe_id_name(failure.probe));
      CHECK(assessment.levels[index].evidence_reason ==
            v2::reason_code_name(failure.reason));
    }
  }
}

TEST_CASE("restriction evidence is conjunctive without downgrade",
          "[process-isolation][evidence-mapping][failure]") {
  auto v1_report = complete_v1();
  auto v2_report = complete_v2();

  record(v1_report, isolation::ProbeId::no_new_privileges) = {
      isolation::ProbeId::no_new_privileges, isolation::ProbeState::unavailable,
      isolation::ReasonCode::permission_denied};
  auto encoded = documents(v1_report, v2_report);
  auto assessment =
      mapping::assess_linux_evidence(source_sha, encoded.v1, encoded.v2);
  for (const auto& level : assessment.levels) {
    CHECK_FALSE(level.complete);
    CHECK(level.reason == mapping::AssessmentReason::unavailable_conjunct);
    CHECK(level.conjunct == "no_new_privileges");
    CHECK(level.evidence_reason == "permission_denied");
  }

  v1_report = complete_v1();
  record(v2_report, v2::ProbeId::landlock_read_confinement) = {
      v2::ProbeId::landlock_read_confinement,
      isolation::ProbeState::unavailable, v2::ReasonCode::mechanism_absent};
  encoded = documents(v1_report, v2_report);
  assessment =
      mapping::assess_linux_evidence(source_sha, encoded.v1, encoded.v2);
  for (const auto& level : assessment.levels)
    check_v3_gap(level);

  v2_report = complete_v2();
  record(v2_report, v2::ProbeId::private_mount_propagation) = {
      v2::ProbeId::private_mount_propagation,
      isolation::ProbeState::unavailable, v2::ReasonCode::permission_denied};
  encoded = documents(v1_report, v2_report);
  assessment =
      mapping::assess_linux_evidence(source_sha, encoded.v1, encoded.v2);
  for (const auto& level : assessment.levels)
    check_v3_gap(level);
}

TEST_CASE("cleanup uncertainty makes dependent evidence indeterminate",
          "[process-isolation][evidence-mapping][failure]") {
  auto v1_report = complete_v1();
  auto v2_report = complete_v2();
  record(v2_report, v2::ProbeId::cgroup_v2_delegation) = {
      v2::ProbeId::cgroup_v2_delegation, isolation::ProbeState::unavailable,
      v2::ReasonCode::missing_delegation};
  record(v2_report, v2::ProbeId::cgroup_cancellation_cleanup) = {
      v2::ProbeId::cgroup_cancellation_cleanup,
      isolation::ProbeState::probe_error, v2::ReasonCode::cleanup_failed};
  const auto encoded = documents(v1_report, v2_report);
  const auto assessment =
      mapping::assess_linux_evidence(source_sha, encoded.v1, encoded.v2);
  for (const auto& level : assessment.levels) {
    CHECK_FALSE(level.complete);
    CHECK(level.reason == mapping::AssessmentReason::indeterminate_evidence);
    CHECK(level.conjunct == "cgroup_cancellation_cleanup");
    CHECK(level.evidence_reason == "cleanup_failed");
  }

  v2_report = complete_v2();
  record(v2_report, v2::ProbeId::landlock_read_confinement) = {
      v2::ProbeId::landlock_read_confinement,
      isolation::ProbeState::unavailable, v2::ReasonCode::mechanism_absent};
  record(v2_report, v2::ProbeId::combined_setup_order) = {
      v2::ProbeId::combined_setup_order, isolation::ProbeState::probe_error,
      v2::ReasonCode::cleanup_failed};
  const auto medium_encoded = documents(v1_report, v2_report);
  const auto medium_assessment = mapping::assess_linux_evidence(
      source_sha, medium_encoded.v1, medium_encoded.v2);
  check_v3_gap(medium_assessment.levels[0]);
  for (const auto index : {1U, 2U}) {
    CHECK_FALSE(medium_assessment.levels[index].complete);
    CHECK(medium_assessment.levels[index].reason ==
          mapping::AssessmentReason::indeterminate_evidence);
    CHECK(medium_assessment.levels[index].conjunct == "combined_setup_order");
    CHECK(medium_assessment.levels[index].evidence_reason == "cleanup_failed");
  }

  v2_report = complete_v2();
  record(v2_report, v2::ProbeId::private_root_construction) = {
      v2::ProbeId::private_root_construction,
      isolation::ProbeState::unavailable, v2::ReasonCode::permission_denied};
  record(v2_report, v2::ProbeId::private_root_combined_setup_order) = {
      v2::ProbeId::private_root_combined_setup_order,
      isolation::ProbeState::probe_error, v2::ReasonCode::cleanup_failed};
  const auto high_encoded = documents(v1_report, v2_report);
  const auto high_assessment = mapping::assess_linux_evidence(
      source_sha, high_encoded.v1, high_encoded.v2);
  check_v3_gap(high_assessment.levels[0]);
  check_v3_gap(high_assessment.levels[1]);
  CHECK_FALSE(high_assessment.levels[2].complete);
  CHECK(high_assessment.levels[2].reason ==
        mapping::AssessmentReason::indeterminate_evidence);
  CHECK(high_assessment.levels[2].conjunct ==
        "private_root_combined_setup_order");
  CHECK(high_assessment.levels[2].evidence_reason == "cleanup_failed");
}

TEST_CASE("unselected rows do not hide the known evidence gap",
          "[process-isolation][evidence-mapping][smoke]") {
  auto v1_report = complete_v1();
  auto v2_report = complete_v2();
  record(v1_report, isolation::ProbeId::user_namespace) = {
      isolation::ProbeId::user_namespace, isolation::ProbeState::unavailable,
      isolation::ReasonCode::permission_denied};
  const auto encoded = documents(v1_report, v2_report);
  const auto assessment =
      mapping::assess_linux_evidence(source_sha, encoded.v1, encoded.v2);
  REQUIRE(assessment.levels.size() == 3);
  for (const auto& level : assessment.levels)
    check_v3_gap(level);
}
