#include "evidence.hpp"
#include "evidence_v2.hpp"
#include "evidence_v3.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <string_view>
#include <utility>

namespace isolation = aiforge::evaluation::process_isolation;
namespace v2 = aiforge::evaluation::process_isolation::v2;
namespace v3 = aiforge::evaluation::process_isolation::v3;

namespace {

constexpr std::array expected_probe_names{
    std::string_view{"direct_process_tree_cgroup_nonescape"},
    std::string_view{"low_capability_nonescalation"},
    std::string_view{"private_root_capability_discard"},
};

[[nodiscard]] auto report() -> v3::EvidenceReport {
  v3::EvidenceReport result{
      std::string(40, 'a'), "linux", "6.8.0-test", "x86_64", {}};
  for (const auto id : v3::required_probe_ids()) {
    result.probes.push_back({id, isolation::ProbeState::unavailable,
                             v3::ReasonCode::prerequisite_unavailable});
  }
  return result;
}

[[nodiscard]] auto replace_once(std::string value, const std::string_view from,
                                const std::string_view to) -> std::string {
  const auto position = value.find(from);
  REQUIRE(position != std::string::npos);
  value.replace(position, from.size(), to);
  return value;
}

} // namespace

TEST_CASE("evidence v3 has exactly the immutable supplemental row order",
          "[process-isolation][evidence-v3][failure]") {
  REQUIRE(v3::required_probe_ids().size() == expected_probe_names.size());
  for (std::size_t index{}; index < expected_probe_names.size(); ++index) {
    CHECK(v3::probe_id_name(v3::required_probe_ids()[index]) ==
          expected_probe_names[index]);
  }
  CHECK(v3::probe_id_name(static_cast<v3::ProbeId>(999)).empty());
  CHECK(v3::reason_code_name(static_cast<v3::ReasonCode>(999)).empty());
}

TEST_CASE("v3 evidence is rejected by earlier immutable schema parsers",
          "[process-isolation][evidence-v3][compatibility][failure]") {
  const auto v3_child = v3::serialize_child_record(
      {v3::ProbeId::direct_process_tree_cgroup_nonescape,
       isolation::ProbeState::enforced, v3::ReasonCode::none});
  const auto v2_child = v2::serialize_child_record(
      {v2::ProbeId::cgroup_v2_delegation, isolation::ProbeState::enforced,
       v2::ReasonCode::none});
  REQUIRE(v3_child);
  REQUIRE(v2_child);
  CHECK_FALSE(isolation::parse_child_record(*v3_child));
  CHECK_FALSE(v2::parse_child_record(*v3_child));
  CHECK_FALSE(v3::parse_child_record(*v2_child));

  const auto v3_report = v3::serialize_report(report());
  REQUIRE(v3_report);
  CHECK_FALSE(isolation::parse_report(*v3_report));
  CHECK_FALSE(v2::parse_report(*v3_report));
}

TEST_CASE("evidence v3 rejects every invalid state and reason correlation",
          "[process-isolation][evidence-v3][failure]") {
  const auto id = v3::required_probe_ids().front();
  CHECK(v3::validate_child_record(
      {id, isolation::ProbeState::enforced, v3::ReasonCode::none}));
  for (const auto reason :
       {v3::ReasonCode::unsupported_kernel,
        v3::ReasonCode::unsupported_architecture,
        v3::ReasonCode::permission_denied, v3::ReasonCode::mechanism_absent,
        v3::ReasonCode::missing_delegation, v3::ReasonCode::missing_controller,
        v3::ReasonCode::enforcement_failed,
        v3::ReasonCode::prerequisite_unavailable,
        v3::ReasonCode::unsupported_combination}) {
    CHECK(v3::validate_child_record(
        {id, isolation::ProbeState::unavailable, reason}));
    CHECK_FALSE(v3::validate_child_record(
        {id, isolation::ProbeState::probe_error, reason}));
  }
  for (const auto reason :
       {v3::ReasonCode::timeout, v3::ReasonCode::cancelled,
        v3::ReasonCode::pid_reuse, v3::ReasonCode::setup_race,
        v3::ReasonCode::signaled, v3::ReasonCode::nonzero_exit,
        v3::ReasonCode::malformed_protocol, v3::ReasonCode::output_limit,
        v3::ReasonCode::cleanup_failed, v3::ReasonCode::internal_error}) {
    CHECK(v3::validate_child_record(
        {id, isolation::ProbeState::probe_error, reason}));
    CHECK_FALSE(v3::validate_child_record(
        {id, isolation::ProbeState::unavailable, reason}));
  }
  CHECK_FALSE(v3::validate_child_record(
      {id, isolation::ProbeState::unavailable, v3::ReasonCode::none}));
  CHECK_FALSE(v3::validate_child_record({id, isolation::ProbeState::enforced,
                                         v3::ReasonCode::enforcement_failed}));
}

TEST_CASE("evidence v3 child parsing rejects loose or oversized input",
          "[process-isolation][evidence-v3][child][failure]") {
  const auto encoded = v3::serialize_child_record(
      {v3::ProbeId::low_capability_nonescalation,
       isolation::ProbeState::unavailable, v3::ReasonCode::unsupported_kernel});
  REQUIRE(encoded);
  CHECK(v3::parse_child_record(*encoded));
  CHECK_FALSE(v3::parse_child_record(""));
  CHECK_FALSE(v3::parse_child_record("{} {}"));
  CHECK_FALSE(v3::parse_child_record(
      replace_once(*encoded, R"("reason":"unsupported_kernel")",
                   R"("reason":"unsupported_kernel","reason":"none")")));
  CHECK_FALSE(v3::parse_child_record(
      replace_once(*encoded, "{", R"({"detail":"must-not-cross",)")));
  CHECK_FALSE(v3::parse_child_record(replace_once(
      *encoded, R"("schema_version":3)", R"("schema_version":2)")));
  CHECK_FALSE(v3::parse_child_record(replace_once(
      *encoded, "low_capability_nonescalation", "unknown_future_probe")));
  CHECK_FALSE(v3::parse_child_record(
      std::string(v3::maximum_child_record_bytes + 1, 'x')));
}

TEST_CASE("evidence v3 reports reject provenance and catalog mutations",
          "[process-isolation][evidence-v3][report][failure]") {
  const auto canonical = report();
  const auto encoded = v3::serialize_report(canonical);
  REQUIRE(encoded);

  auto invalid = canonical;
  invalid.probes.pop_back();
  CHECK_FALSE(v3::validate_report(invalid));
  invalid = canonical;
  std::swap(invalid.probes[0], invalid.probes[1]);
  CHECK_FALSE(v3::validate_report(invalid));
  invalid = canonical;
  invalid.probes.push_back(invalid.probes.back());
  CHECK_FALSE(v3::validate_report(invalid));
  for (const auto& source :
       {std::string(39, 'a'), std::string(40, 'A'), std::string(41, 'a')}) {
    invalid = canonical;
    invalid.source_sha = source;
    CHECK_FALSE(v3::validate_report(invalid));
  }
  for (const auto& metadata :
       {std::string{}, std::string(129, 'a'), std::string{"host/path"},
        std::string{"line\nbreak"}}) {
    invalid = canonical;
    invalid.kernel = metadata;
    CHECK_FALSE(v3::validate_report(invalid));
  }
  invalid = canonical;
  invalid.platform = "other";
  CHECK_FALSE(v3::validate_report(invalid));

  CHECK_FALSE(v3::parse_report(
      replace_once(*encoded, "{", R"({"hostname":"must-not-cross",)")));
  CHECK_FALSE(v3::parse_report(replace_once(*encoded, R"("schema_version":3)",
                                            R"("schema_version":2)")));
  CHECK_FALSE(v3::parse_report(replace_once(
      *encoded, R"("source_sha":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")",
      R"("source_sha":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","source_sha":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")")));
  CHECK_FALSE(v3::parse_report(std::string(v3::maximum_report_bytes + 1, 'x')));
}

TEST_CASE("evidence v3 canonical report round trips without authority",
          "[process-isolation][evidence-v3][smoke]") {
  const auto value = report();
  const auto encoded = v3::serialize_report(value);
  REQUIRE(encoded);
  CHECK(encoded->size() <= v3::maximum_report_bytes);
  CHECK(encoded->find("isolation_level") == std::string::npos);
  CHECK(encoded->find("launch_available") == std::string::npos);
  const auto decoded = v3::parse_report(*encoded);
  REQUIRE(decoded);
  CHECK(*decoded == value);
  CHECK(v3::serialize_report(*decoded) == encoded);
  const auto succeeded = v3::evidence_run_succeeded(*decoded);
  REQUIRE(succeeded);
  CHECK(*succeeded);

  auto indeterminate = value;
  indeterminate.probes.front() = {
      v3::ProbeId::direct_process_tree_cgroup_nonescape,
      isolation::ProbeState::probe_error, v3::ReasonCode::cleanup_failed};
  const auto failed = v3::evidence_run_succeeded(indeterminate);
  REQUIRE(failed);
  CHECK_FALSE(*failed);
}
