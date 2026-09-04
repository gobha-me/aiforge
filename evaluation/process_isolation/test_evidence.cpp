#include "evidence.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

namespace {

using namespace aiforge::evaluation::process_isolation;

constexpr std::array expected_probe_names{
    std::string_view{"no_new_privileges"},
    std::string_view{"rlimit_cpu"},
    std::string_view{"rlimit_address_space"},
    std::string_view{"rlimit_process_count"},
    std::string_view{"rlimit_descriptor_count"},
    std::string_view{"rlimit_file_size"},
    std::string_view{"inherited_descriptors"},
    std::string_view{"subreaper_session_cleanup"},
    std::string_view{"subreaper_double_fork_cleanup"},
    std::string_view{"landlock_read_confinement"},
    std::string_view{"user_namespace"},
    std::string_view{"mount_namespace"},
    std::string_view{"pid_namespace"},
    std::string_view{"network_namespace"},
    std::string_view{"seccomp_socket_creation_denial"},
    std::string_view{"disposable_workspace"},
    std::string_view{"openat2_resolution"},
    std::string_view{"fexecve_identity"},
    std::string_view{"execveat_identity"},
    std::string_view{"fchdir_identity"},
    std::string_view{"staged_input_identity"},
};

constexpr std::array states{ProbeState::enforced, ProbeState::unavailable,
                            ProbeState::probe_error};
constexpr std::array expected_state_names{std::string_view{"enforced"},
                                          std::string_view{"unavailable"},
                                          std::string_view{"probe_error"}};
constexpr std::array reasons{
    ReasonCode::none,
    ReasonCode::unsupported_kernel,
    ReasonCode::unsupported_architecture,
    ReasonCode::permission_denied,
    ReasonCode::mechanism_absent,
    ReasonCode::enforcement_failed,
    ReasonCode::prerequisite_unavailable,
    ReasonCode::timeout,
    ReasonCode::signaled,
    ReasonCode::nonzero_exit,
    ReasonCode::malformed_protocol,
    ReasonCode::output_limit,
    ReasonCode::cleanup_failed,
    ReasonCode::internal_error,
};
constexpr std::array expected_reason_names{
    std::string_view{"none"},
    std::string_view{"unsupported_kernel"},
    std::string_view{"unsupported_architecture"},
    std::string_view{"permission_denied"},
    std::string_view{"mechanism_absent"},
    std::string_view{"enforcement_failed"},
    std::string_view{"prerequisite_unavailable"},
    std::string_view{"timeout"},
    std::string_view{"signaled"},
    std::string_view{"nonzero_exit"},
    std::string_view{"malformed_protocol"},
    std::string_view{"output_limit"},
    std::string_view{"cleanup_failed"},
    std::string_view{"internal_error"},
};

auto report(const ProbeState state = ProbeState::unavailable,
            const ReasonCode reason = ReasonCode::mechanism_absent)
    -> EvidenceReport {
  EvidenceReport value{
      std::string(40, 'a'), "linux", "6.8.0-test", "x86_64", {}};
  for (const auto probe : required_probe_ids()) {
    value.probes.push_back({probe, state, reason});
  }
  return value;
}

auto replace_once(std::string value, const std::string_view old_text,
                  const std::string_view new_text) -> std::string {
  const auto position = value.find(old_text);
  REQUIRE(position != std::string::npos);
  value.replace(position, old_text.size(), new_text);
  return value;
}

} // namespace

TEST_CASE("process isolation evidence has a closed ordered probe catalog",
          "[process-isolation][evidence][failure]") {
  const auto probes = required_probe_ids();
  REQUIRE(probes.size() == expected_probe_names.size());
  for (std::size_t index = 0; index < probes.size(); ++index) {
    REQUIRE(probe_id_name(probes[index]) == expected_probe_names[index]);
  }
  for (std::size_t index = 0; index < states.size(); ++index) {
    REQUIRE(probe_state_name(states[index]) == expected_state_names[index]);
  }
  for (std::size_t index = 0; index < reasons.size(); ++index) {
    REQUIRE(reason_code_name(reasons[index]) == expected_reason_names[index]);
  }
  REQUIRE(probe_id_name(static_cast<ProbeId>(999)).empty());
  REQUIRE(probe_state_name(static_cast<ProbeState>(999)).empty());
  REQUIRE(reason_code_name(static_cast<ReasonCode>(999)).empty());
}

TEST_CASE("child evidence rejects invalid state and reason combinations",
          "[process-isolation][evidence][child][failure]") {
  const auto probe = required_probe_ids().front();

  REQUIRE(
      validate_child_record({probe, ProbeState::enforced, ReasonCode::none}));
  REQUIRE(validate_child_record(
      {probe, ProbeState::unavailable, ReasonCode::unsupported_kernel}));
  REQUIRE(validate_child_record(
      {probe, ProbeState::probe_error, ReasonCode::internal_error}));

  REQUIRE_FALSE(validate_child_record(
      {probe, ProbeState::enforced, ReasonCode::enforcement_failed}));
  REQUIRE_FALSE(validate_child_record(
      {probe, ProbeState::unavailable, ReasonCode::none}));
  REQUIRE_FALSE(validate_child_record(
      {probe, ProbeState::probe_error, ReasonCode::none}));
  for (const auto reason :
       {ReasonCode::timeout, ReasonCode::signaled, ReasonCode::nonzero_exit,
        ReasonCode::malformed_protocol, ReasonCode::output_limit,
        ReasonCode::cleanup_failed, ReasonCode::internal_error}) {
    REQUIRE_FALSE(
        validate_child_record({probe, ProbeState::unavailable, reason}));
  }
  for (const auto reason : {
           ReasonCode::unsupported_kernel,
           ReasonCode::unsupported_architecture,
           ReasonCode::permission_denied,
           ReasonCode::mechanism_absent,
           ReasonCode::enforcement_failed,
           ReasonCode::prerequisite_unavailable,
       }) {
    REQUIRE_FALSE(
        validate_child_record({probe, ProbeState::probe_error, reason}));
  }
  REQUIRE_FALSE(
      validate_child_record({static_cast<ProbeId>(999), ProbeState::probe_error,
                             ReasonCode::internal_error}));
  REQUIRE_FALSE(validate_child_record(
      {probe, static_cast<ProbeState>(999), ReasonCode::internal_error}));
  REQUIRE_FALSE(validate_child_record(
      {probe, ProbeState::probe_error, static_cast<ReasonCode>(999)}));
}

TEST_CASE("child evidence parser rejects loose or ambiguous JSON",
          "[process-isolation][evidence][child][failure]") {
  const std::string valid =
      R"({"probe_id":"rlimit_cpu","reason":"none","schema_version":1,"state":"enforced"})";

  SECTION("empty and malformed documents") {
    REQUIRE(parse_child_record("").error().code ==
            EvidenceErrorCode::malformed_json);
    REQUIRE(parse_child_record("{").error().code ==
            EvidenceErrorCode::malformed_json);
    REQUIRE(parse_child_record("{} {}").error().code ==
            EvidenceErrorCode::malformed_json);
    REQUIRE(parse_child_record("[]").error().code ==
            EvidenceErrorCode::invalid_value);
  }

  SECTION("duplicate fields") {
    const auto duplicate =
        replace_once(valid, R"("state":"enforced")",
                     R"("state":"enforced","state":"enforced")");
    REQUIRE(parse_child_record(duplicate).error().code ==
            EvidenceErrorCode::duplicate_field);
  }

  SECTION("missing and unknown fields") {
    const auto missing = replace_once(valid, R"(,"reason":"none")", "");
    REQUIRE(parse_child_record(missing).error().code ==
            EvidenceErrorCode::missing_field);
    const auto unknown = replace_once(valid, "{", R"({"detail":"secret",)");
    REQUIRE(parse_child_record(unknown).error().code ==
            EvidenceErrorCode::unknown_field);
  }

  SECTION("schema and closed values") {
    REQUIRE(parse_child_record(replace_once(valid, "\"schema_version\":1",
                                            "\"schema_version\":2"))
                .error()
                .code == EvidenceErrorCode::invalid_schema);
    REQUIRE(parse_child_record(replace_once(valid, "rlimit_cpu", "future"))
                .error()
                .code == EvidenceErrorCode::invalid_value);
    REQUIRE(parse_child_record(replace_once(valid, "enforced", "future"))
                .error()
                .code == EvidenceErrorCode::invalid_value);
    REQUIRE(parse_child_record(replace_once(valid, "none", "future"))
                .error()
                .code == EvidenceErrorCode::invalid_value);
  }

  SECTION("oversized input") {
    REQUIRE(parse_child_record(std::string(maximum_child_record_bytes + 1, ' '))
                .error()
                .code == EvidenceErrorCode::resource_exhausted);
  }
}

TEST_CASE("reports reject invalid provenance and incomplete probe sets",
          "[process-isolation][evidence][report][failure]") {
  auto value = report();

  SECTION("source SHA is exact lowercase hexadecimal") {
    value.source_sha = std::string(39, 'a');
    REQUIRE_FALSE(validate_report(value));
    value.source_sha = std::string(40, 'A');
    REQUIRE_FALSE(validate_report(value));
    value.source_sha = std::string(40, 'g');
    REQUIRE_FALSE(validate_report(value));
  }

  SECTION("schema v1 is Linux-only") {
    for (const auto invalid : {"", "Linux", "ubuntu", "linux-test"}) {
      value.platform = invalid;
      REQUIRE_FALSE(validate_report(value));
    }
  }

  SECTION("kernel and architecture are nonempty bounded safe ASCII") {
    for (auto* field : {&value.kernel, &value.architecture}) {
      const auto original = *field;
      field->clear();
      REQUIRE_FALSE(validate_report(value));
      *field = std::string(maximum_platform_metadata_bytes + 1, 'a');
      REQUIRE_FALSE(validate_report(value));
      *field = "host/path";
      REQUIRE_FALSE(validate_report(value));
      *field = "unsafe value";
      REQUIRE_FALSE(validate_report(value));
      *field = original;
    }
  }

  SECTION("probe rows must be complete ordered and unique") {
    value.probes.pop_back();
    REQUIRE_FALSE(validate_report(value));

    value = report();
    value.probes.push_back(value.probes.back());
    REQUIRE_FALSE(validate_report(value));

    value = report();
    std::swap(value.probes[0], value.probes[1]);
    REQUIRE_FALSE(validate_report(value));

    value = report();
    value.probes[1].probe_id = value.probes[0].probe_id;
    REQUIRE_FALSE(validate_report(value));
  }
}

TEST_CASE("report parser rejects malformed schemas and nested records",
          "[process-isolation][evidence][report][failure]") {
  const auto encoded = serialize_report(report());
  REQUIRE(encoded);

  SECTION("duplicate top-level and nested fields") {
    auto duplicate = replace_once(*encoded, "{", R"({"kernel":"secret",)");
    REQUIRE(parse_report(duplicate).error().code ==
            EvidenceErrorCode::duplicate_field);

    duplicate =
        replace_once(*encoded, R"("reason":"mechanism_absent")",
                     R"("reason":"mechanism_absent","reason":"timeout")");
    REQUIRE(parse_report(duplicate).error().code ==
            EvidenceErrorCode::duplicate_field);
  }

  SECTION("unknown top-level and nested fields") {
    auto unknown = replace_once(*encoded, "{", R"({"hostname":"secret",)");
    REQUIRE(parse_report(unknown).error().code ==
            EvidenceErrorCode::unknown_field);

    unknown =
        replace_once(*encoded, R"("probe_id":"no_new_privileges")",
                     R"("detail":"secret","probe_id":"no_new_privileges")");
    REQUIRE(parse_report(unknown).error().code ==
            EvidenceErrorCode::unknown_field);
  }

  SECTION("missing fields and incorrect types") {
    const auto missing = replace_once(*encoded, R"("platform":"linux",)", "");
    REQUIRE(parse_report(missing).error().code ==
            EvidenceErrorCode::missing_field);
    const auto wrong_type = replace_once(*encoded, R"("architecture":"x86_64")",
                                         R"("architecture":["x86_64"])");
    REQUIRE(parse_report(wrong_type).error().code ==
            EvidenceErrorCode::invalid_value);
  }

  SECTION("wrong schema and excessive input") {
    const auto wrong_schema =
        replace_once(*encoded, "\"schema_version\":1", "\"schema_version\":0");
    REQUIRE(parse_report(wrong_schema).error().code ==
            EvidenceErrorCode::invalid_schema);
    REQUIRE(
        parse_report(std::string(maximum_report_bytes + 1, ' ')).error().code ==
        EvidenceErrorCode::resource_exhausted);
  }

  SECTION("missing rows and out-of-order rows") {
    auto raw = *encoded;
    const auto final_row = raw.rfind(R"(,{"probe_id":)");
    REQUIRE(final_row != std::string::npos);
    const auto array_end = raw.find("]", final_row);
    REQUIRE(array_end != std::string::npos);
    raw.erase(final_row, array_end - final_row);
    REQUIRE(parse_report(raw).error().code == EvidenceErrorCode::invalid_value);

    auto out_of_order = report();
    std::swap(out_of_order.probes[0], out_of_order.probes[1]);
    const auto out_of_order_json = serialize_report(out_of_order);
    REQUIRE_FALSE(out_of_order_json);
  }
}

TEST_CASE("probe errors fail a run while unavailable evidence is successful",
          "[process-isolation][evidence][report][failure]") {
  auto unavailable = report();
  const auto unavailable_result = evidence_run_succeeded(unavailable);
  REQUIRE(unavailable_result);
  REQUIRE(*unavailable_result);

  unavailable.probes.front().state = ProbeState::probe_error;
  unavailable.probes.front().reason = ReasonCode::timeout;
  const auto probe_error = evidence_run_succeeded(unavailable);
  REQUIRE(probe_error);
  REQUIRE_FALSE(*probe_error);

  unavailable.probes.pop_back();
  REQUIRE_FALSE(evidence_run_succeeded(unavailable));
}

TEST_CASE("process isolation evidence round trips as canonical bounded JSON",
          "[process-isolation][evidence][smoke]") {
  const ProbeRecord child{ProbeId::openat2_resolution, ProbeState::enforced,
                          ReasonCode::none};
  const auto child_json = serialize_child_record(child);
  REQUIRE(child_json);
  REQUIRE(
      *child_json ==
      R"({"probe_id":"openat2_resolution","reason":"none","schema_version":1,"state":"enforced"})"
      "\n");
  REQUIRE(child_json->size() <= maximum_child_record_bytes);
  const auto child_round_trip = parse_child_record(*child_json);
  REQUIRE(child_round_trip);
  REQUIRE(*child_round_trip == child);

  const auto value = report();
  const auto encoded = serialize_report(value);
  REQUIRE(encoded);
  REQUIRE(encoded->back() == '\n');
  REQUIRE(encoded->find('\n') == encoded->size() - 1);
  REQUIRE(encoded->size() <= maximum_report_bytes);
  const auto decoded = parse_report(*encoded);
  REQUIRE(decoded);
  REQUIRE(*decoded == value);
  REQUIRE(serialize_report(*decoded) == encoded);
}
