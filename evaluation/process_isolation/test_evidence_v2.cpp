#include "evidence.hpp"
#include "evidence_v2.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

namespace isolation = aiforge::evaluation::process_isolation;
namespace v2 = aiforge::evaluation::process_isolation::v2;

namespace {

constexpr std::array expected_probe_names{
    std::string_view{"cgroup_v2_delegation"},
    std::string_view{"cgroup_required_controllers"},
    std::string_view{"cgroup_atomic_child_placement"},
    std::string_view{"cgroup_self_migration_denial"},
    std::string_view{"cgroup_whole_tree_enumeration"},
    std::string_view{"cgroup_kill"},
    std::string_view{"cgroup_populated_zero"},
    std::string_view{"cgroup_setsid_containment"},
    std::string_view{"cgroup_double_fork_containment"},
    std::string_view{"cgroup_daemon_containment"},
    std::string_view{"cgroup_clone_fork_fanout"},
    std::string_view{"cgroup_leader_exit_containment"},
    std::string_view{"cgroup_cancellation_cleanup"},
    std::string_view{"cgroup_cpu_limit_enforcement"},
    std::string_view{"cgroup_memory_limit_termination"},
    std::string_view{"cgroup_pids_limit_enforcement"},
    std::string_view{"landlock_read_confinement"},
    std::string_view{"landlock_write_confinement"},
    std::string_view{"landlock_execute_confinement"},
    std::string_view{"seccomp_internet_socket_family_denial"},
    std::string_view{"seccomp_unix_socket_denial"},
    std::string_view{"private_root_construction"},
    std::string_view{"private_mount_propagation"},
    std::string_view{"descriptor_relative_launch"},
    std::string_view{"staged_input_identity"},
    std::string_view{"staged_output_identity"},
    std::string_view{"combined_setup_order"},
    std::string_view{"private_root_combined_setup_order"},
    std::string_view{"partial_setup_cleanup"},
};

auto report() -> v2::EvidenceReport {
  v2::EvidenceReport result{
      std::string(40, 'a'), "linux", "6.8.0-test", "x86_64", {}};
  for (const auto id : v2::required_probe_ids()) {
    result.probes.push_back({id, isolation::ProbeState::unavailable,
                             v2::ReasonCode::prerequisite_unavailable});
  }
  return result;
}

auto replace_once(std::string value, const std::string_view from,
                  const std::string_view to) -> std::string {
  const auto position = value.find(from);
  REQUIRE(position != std::string::npos);
  value.replace(position, from.size(), to);
  return value;
}

} // namespace

TEST_CASE("evidence v2 has a closed ordered non-policy probe catalog",
          "[process-isolation][evidence-v2][failure]") {
  REQUIRE(v2::required_probe_ids().size() == expected_probe_names.size());
  for (std::size_t index{}; index < expected_probe_names.size(); ++index) {
    CHECK(v2::probe_id_name(v2::required_probe_ids()[index]) ==
          expected_probe_names[index]);
  }
  CHECK(v2::probe_id_name(static_cast<v2::ProbeId>(999)).empty());
  CHECK(v2::reason_code_name(static_cast<v2::ReasonCode>(999)).empty());
  CHECK(expected_probe_names[19] == "seccomp_internet_socket_family_denial");
  CHECK(expected_probe_names[20] == "seccomp_unix_socket_denial");
}

TEST_CASE("v1 and v2 evidence parsers reject each other's schemas",
          "[process-isolation][evidence-v2][compatibility][failure]") {
  const auto v1 = isolation::serialize_child_record(
      {isolation::ProbeId::no_new_privileges, isolation::ProbeState::enforced,
       isolation::ReasonCode::none});
  const auto v2_document = v2::serialize_child_record(
      {v2::ProbeId::cgroup_v2_delegation, isolation::ProbeState::enforced,
       v2::ReasonCode::none});
  REQUIRE(v1);
  REQUIRE(v2_document);
  CHECK_FALSE(isolation::parse_child_record(*v2_document));
  CHECK_FALSE(v2::parse_child_record(*v1));

  const auto encoded_v2 = v2::serialize_report(report());
  REQUIRE(encoded_v2);
  CHECK_FALSE(isolation::parse_report(*encoded_v2));
}

TEST_CASE("evidence v2 rejects invalid state and reason combinations",
          "[process-isolation][evidence-v2][failure]") {
  const auto id = v2::required_probe_ids().front();
  CHECK(v2::validate_child_record(
      {id, isolation::ProbeState::enforced, v2::ReasonCode::none}));
  for (const auto reason :
       {v2::ReasonCode::missing_delegation, v2::ReasonCode::missing_controller,
        v2::ReasonCode::unsupported_combination,
        v2::ReasonCode::limit_not_triggered}) {
    CHECK(v2::validate_child_record(
        {id, isolation::ProbeState::unavailable, reason}));
    CHECK_FALSE(v2::validate_child_record(
        {id, isolation::ProbeState::probe_error, reason}));
  }
  for (const auto reason :
       {v2::ReasonCode::timeout, v2::ReasonCode::cancelled,
        v2::ReasonCode::pid_reuse, v2::ReasonCode::setup_race,
        v2::ReasonCode::cleanup_failed}) {
    CHECK(v2::validate_child_record(
        {id, isolation::ProbeState::probe_error, reason}));
    CHECK_FALSE(v2::validate_child_record(
        {id, isolation::ProbeState::unavailable, reason}));
  }
  CHECK_FALSE(v2::validate_child_record({id, isolation::ProbeState::enforced,
                                         v2::ReasonCode::enforcement_failed}));
}

TEST_CASE("evidence v2 child parser is strict and bounded",
          "[process-isolation][evidence-v2][child][failure]") {
  const auto encoded = v2::serialize_child_record(
      {v2::ProbeId::cgroup_required_controllers,
       isolation::ProbeState::unavailable, v2::ReasonCode::missing_controller});
  REQUIRE(encoded);
  CHECK(v2::parse_child_record(*encoded));
  CHECK_FALSE(v2::parse_child_record(""));
  CHECK_FALSE(v2::parse_child_record("{} {}"));
  CHECK_FALSE(v2::parse_child_record(
      replace_once(*encoded, R"("reason":"missing_controller")",
                   R"("reason":"missing_controller","reason":"none")")));
  CHECK_FALSE(v2::parse_child_record(
      replace_once(*encoded, "{", R"({"detail":"must-not-cross",)")));
  CHECK_FALSE(v2::parse_child_record(replace_once(
      *encoded, R"("schema_version":2)", R"("schema_version":3)")));
  CHECK_FALSE(v2::parse_child_record(
      std::string(v2::maximum_child_record_bytes + 1, 'x')));
}

TEST_CASE("evidence v2 report rejects incomplete reordered and loose input",
          "[process-isolation][evidence-v2][report][failure]") {
  auto value = report();
  const auto encoded = v2::serialize_report(value);
  REQUIRE(encoded);

  value.probes.pop_back();
  CHECK_FALSE(v2::validate_report(value));
  value = report();
  std::swap(value.probes[0], value.probes[1]);
  CHECK_FALSE(v2::validate_report(value));
  CHECK_FALSE(v2::parse_report(
      replace_once(*encoded, "{", R"({"hostname":"must-not-cross",)")));
  CHECK_FALSE(v2::parse_report(replace_once(*encoded, R"("schema_version":2)",
                                            R"("schema_version":1)")));
  CHECK_FALSE(v2::parse_report(std::string(v2::maximum_report_bytes + 1, 'x')));
}

TEST_CASE("evidence v2 canonical report round trips without a level claim",
          "[process-isolation][evidence-v2][smoke]") {
  const auto value = report();
  const auto encoded = v2::serialize_report(value);
  REQUIRE(encoded);
  CHECK(encoded->size() <= v2::maximum_report_bytes);
  CHECK(encoded->find("isolation_level") == std::string::npos);
  const auto decoded = v2::parse_report(*encoded);
  REQUIRE(decoded);
  CHECK(*decoded == value);
  CHECK(v2::serialize_report(*decoded) == encoded);
  const auto succeeded = v2::evidence_run_succeeded(*decoded);
  REQUIRE(succeeded);
  CHECK(*succeeded);
}
