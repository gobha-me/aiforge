#include "evidence.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace aiforge::evaluation::audio_device;

[[nodiscard]] auto observed(const ProbeKey key) -> ProbeRecord {
  const bool cancellation =
      key.probe_id == ProbeId::cancel_during_open ||
      key.probe_id == ProbeId::cancel_during_start ||
      key.probe_id == ProbeId::cancel_during_stream ||
      key.probe_id == ProbeId::cancel_during_stop ||
      key.probe_id == ProbeId::cancel_during_close ||
      key.probe_id == ProbeId::controller_thread_cancellation;
  return {key.probe_id,
          key.direction,
          ProbeState::observed,
          ReasonCode::none,
          0,
          0,
          0,
          cancellation,
          true};
}

[[nodiscard]] auto probes(const std::span<const ProbeKey> required)
    -> std::vector<ProbeRecord> {
  std::vector<ProbeRecord> result;
  result.reserve(required.size());
  for (const auto key : required)
    result.push_back(observed(key));
  return result;
}

[[nodiscard]] auto candidate(const CandidateId id) -> CandidateReport {
  return {id,
          std::string{candidate_version(id)},
          DependencySource::controlled_source_fallback,
          Linkage::static_library,
          id == CandidateId::rtaudio ? RuntimeBackend::dummy
                                     : RuntimeBackend::null_backend,
          false,
          false,
          probes(required_candidate_probe_keys())};
}

[[nodiscard]] auto report() -> EvidenceReport {
  return {std::string(40, 'a'),
          "linux",
          "x86_64",
          {probes(required_contract_probe_keys())},
          {candidate(CandidateId::rtaudio), candidate(CandidateId::miniaudio)}};
}

[[nodiscard]] auto replace_once(std::string value, const std::string_view from,
                                const std::string_view to) -> std::string {
  const auto position = value.find(from);
  REQUIRE(position != std::string::npos);
  value.replace(position, from.size(), to);
  return value;
}

TEST_CASE("audio-device probe values reject dishonest combinations first",
          "[audio-device][evidence][failure]") {
  const auto ordinary = required_contract_probe_keys().front();
  const auto cancellation =
      ProbeKey{ProbeId::cancel_during_open, Direction::playback};
  REQUIRE(validate_probe_record(observed(ordinary)));
  REQUIRE(validate_probe_record(observed(cancellation)));

  auto value = observed(ordinary);
  value.reason = ReasonCode::contract_failed;
  REQUIRE_FALSE(validate_probe_record(value));
  value.state = ProbeState::unavailable;
  REQUIRE(validate_probe_record(value));
  value.reason = ReasonCode::timeout;
  REQUIRE_FALSE(validate_probe_record(value));
  value.state = ProbeState::probe_error;
  REQUIRE(validate_probe_record(value));

  value = observed(cancellation);
  value.cancellation_observed = false;
  REQUIRE_FALSE(validate_probe_record(value));
  value = observed(ordinary);
  value.cancellation_observed = true;
  REQUIRE_FALSE(validate_probe_record(value));

  value = observed(ordinary);
  value.cleanup_complete = false;
  REQUIRE_FALSE(validate_probe_record(value));
  value.state = ProbeState::probe_error;
  value.reason = ReasonCode::cleanup_failed;
  REQUIRE(validate_probe_record(value));
  value.cleanup_complete = true;
  REQUIRE_FALSE(validate_probe_record(value));

  value = observed(ordinary);
  value.frames = maximum_observed_frames + 1;
  REQUIRE(validate_probe_record(value).error().code ==
          EvidenceErrorCode::resource_exhausted);
}

TEST_CASE("audio-device child parsers reject loose and ambiguous JSON",
          "[audio-device][evidence][child][failure]") {
  const auto encoded =
      serialize_candidate_report(candidate(CandidateId::rtaudio));
  REQUIRE(encoded);

  REQUIRE(parse_candidate_report("").error().code ==
          EvidenceErrorCode::malformed_json);
  REQUIRE(parse_candidate_report("{").error().code ==
          EvidenceErrorCode::malformed_json);
  REQUIRE(parse_candidate_report("[]").error().code ==
          EvidenceErrorCode::invalid_value);

  const auto duplicate =
      replace_once(*encoded, R"("kind":"candidate")",
                   R"("kind":"candidate","kind":"candidate")");
  REQUIRE(parse_candidate_report(duplicate).error().code ==
          EvidenceErrorCode::duplicate_field);

  const auto unknown = replace_once(*encoded, "{", R"({"detail":"secret",)");
  REQUIRE(parse_candidate_report(unknown).error().code ==
          EvidenceErrorCode::unknown_field);
  const auto missing = replace_once(*encoded, R"("device_access":false,)", "");
  REQUIRE(parse_candidate_report(missing).error().code ==
          EvidenceErrorCode::missing_field);
  const auto wrong_schema =
      replace_once(*encoded, R"("schema_version":1)", R"("schema_version":2)");
  REQUIRE(parse_candidate_report(wrong_schema).error().code ==
          EvidenceErrorCode::invalid_schema);
  const auto wrong_candidate = replace_once(
      *encoded, R"("candidate_id":"rtaudio")", R"("candidate_id":"future")");
  REQUIRE(parse_candidate_report(wrong_candidate).error().code ==
          EvidenceErrorCode::invalid_value);
  REQUIRE(
      parse_candidate_report(std::string(maximum_child_report_bytes + 1, ' '))
          .error()
          .code == EvidenceErrorCode::resource_exhausted);
}

TEST_CASE("audio-device reports require exact provenance and fixed order",
          "[audio-device][evidence][report][failure]") {
  auto value = report();

  value.source_sha = std::string(39, 'a');
  REQUIRE_FALSE(validate_report(value));
  value = report();
  value.source_sha = std::string(40, 'A');
  REQUIRE_FALSE(validate_report(value));
  value = report();
  value.platform = "ubuntu";
  REQUIRE_FALSE(validate_report(value));
  value = report();
  value.architecture = "host/path";
  REQUIRE_FALSE(validate_report(value));

  value = report();
  value.contract.probes.pop_back();
  REQUIRE_FALSE(validate_report(value));
  value = report();
  std::swap(value.contract.probes[0], value.contract.probes[1]);
  REQUIRE_FALSE(validate_report(value));
  value = report();
  std::swap(value.candidates[0], value.candidates[1]);
  REQUIRE_FALSE(validate_report(value));
  value = report();
  value.candidates.front().candidate_version = "6.0.0";
  REQUIRE_FALSE(validate_report(value));
  value = report();
  value.candidates.front().runtime_backend = RuntimeBackend::null_backend;
  REQUIRE_FALSE(validate_report(value));
  value = report();
  value.candidates.front().device_access = true;
  REQUIRE_FALSE(validate_report(value));
  value = report();
  value.candidates.back().codec_features = true;
  REQUIRE_FALSE(validate_report(value));
}

TEST_CASE("audio-device report parser rejects added disclosure fields",
          "[audio-device][evidence][report][failure]") {
  const auto encoded = serialize_report(report());
  REQUIRE(encoded);
  const auto top_level =
      replace_once(*encoded, "{", R"({"hostname":"secret",)");
  REQUIRE(parse_report(top_level).error().code ==
          EvidenceErrorCode::unknown_field);
  const auto nested =
      replace_once(*encoded, R"("candidate_id":"rtaudio")",
                   R"("candidate_id":"rtaudio","device_name":"secret")");
  REQUIRE(parse_report(nested).error().code ==
          EvidenceErrorCode::unknown_field);
  const auto probe = replace_once(*encoded, R"("callbacks":0)",
                                  R"("callbacks":0,"raw_error":"secret")");
  REQUIRE(parse_report(probe).error().code == EvidenceErrorCode::unknown_field);
  REQUIRE(
      parse_report(std::string(maximum_report_bytes + 1, ' ')).error().code ==
      EvidenceErrorCode::resource_exhausted);
}

TEST_CASE("required contract and backend observations gate a complete run",
          "[audio-device][evidence][report][failure]") {
  auto value = report();
  auto succeeded = evidence_run_succeeded(value);
  REQUIRE(succeeded);
  REQUIRE(*succeeded);

  value.candidates.front().probes.back() = {
      ProbeId::callback_quiescent_after_close,
      Direction::capture,
      ProbeState::unavailable,
      ReasonCode::candidate_limitation,
      0,
      0,
      0,
      false,
      true};
  succeeded = evidence_run_succeeded(value);
  REQUIRE(succeeded);
  REQUIRE(*succeeded);

  value.contract.probes.front() = {ProbeId::invalid_format_rejected,
                                   Direction::none,
                                   ProbeState::unavailable,
                                   ReasonCode::contract_failed,
                                   0,
                                   0,
                                   0,
                                   false,
                                   true};
  succeeded = evidence_run_succeeded(value);
  REQUIRE(succeeded);
  REQUIRE_FALSE(*succeeded);

  value = report();
  value.candidates.front().probes.front() = {ProbeId::runtime_backend_forced,
                                             Direction::none,
                                             ProbeState::unavailable,
                                             ReasonCode::contract_failed,
                                             0,
                                             0,
                                             0,
                                             false,
                                             true};
  succeeded = evidence_run_succeeded(value);
  REQUIRE(succeeded);
  REQUIRE_FALSE(*succeeded);

  value = report();
  value.contract.probes.front() = {ProbeId::invalid_format_rejected,
                                   Direction::none,
                                   ProbeState::probe_error,
                                   ReasonCode::timeout,
                                   0,
                                   0,
                                   0,
                                   false,
                                   true};
  succeeded = evidence_run_succeeded(value);
  REQUIRE(succeeded);
  REQUIRE_FALSE(*succeeded);
}

TEST_CASE("audio-device evidence round trips as canonical bounded JSON last",
          "[audio-device][evidence][smoke]") {
  const auto contract = ContractReport{probes(required_contract_probe_keys())};
  const auto contract_json = serialize_contract_report(contract);
  REQUIRE(contract_json);
  REQUIRE(contract_json->size() <= maximum_child_report_bytes);
  REQUIRE(contract_json->back() == '\n');
  REQUIRE(contract_json->find('\n') == contract_json->size() - 1);
  REQUIRE(parse_contract_report(*contract_json) == contract);

  const auto candidate_value = candidate(CandidateId::miniaudio);
  const auto candidate_json = serialize_candidate_report(candidate_value);
  REQUIRE(candidate_json);
  REQUIRE(candidate_json->size() <= maximum_child_report_bytes);
  REQUIRE(parse_candidate_report(*candidate_json) == candidate_value);

  const auto value = report();
  const auto encoded = serialize_report(value);
  REQUIRE(encoded);
  REQUIRE(encoded->size() <= maximum_report_bytes);
  REQUIRE(encoded->back() == '\n');
  REQUIRE(encoded->find('\n') == encoded->size() - 1);
  const auto decoded = parse_report(*encoded);
  REQUIRE(decoded);
  REQUIRE(*decoded == value);
  REQUIRE(serialize_report(*decoded) == encoded);
}

} // namespace
