#include "runner.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace aiforge::evaluation::audio_device;
using namespace std::chrono_literals;

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

[[nodiscard]] auto probes(const std::span<const ProbeKey> keys)
    -> std::vector<ProbeRecord> {
  std::vector<ProbeRecord> result;
  result.reserve(keys.size());
  for (const auto key : keys)
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

[[nodiscard]] auto command(std::string document) -> ChildCommand {
  return {std::filesystem::canonical("/usr/bin/printf"), {std::move(document)}};
}

[[nodiscard]] auto options() -> RunnerOptions {
  const auto contract =
      serialize_contract_report({probes(required_contract_probe_keys())});
  const auto rtaudio =
      serialize_candidate_report(candidate(CandidateId::rtaudio));
  const auto miniaudio =
      serialize_candidate_report(candidate(CandidateId::miniaudio));
  REQUIRE(contract);
  REQUIRE(rtaudio);
  REQUIRE(miniaudio);
  return {command(*contract), command(*rtaudio), command(*miniaudio), 5s,
          maximum_child_report_bytes};
}

[[nodiscard]] auto all_reason(const std::span<const ProbeRecord> probes,
                              const ReasonCode reason,
                              const bool cleanup_complete = true) -> bool {
  return std::ranges::all_of(probes, [&](const auto& record) {
    return record.state == ProbeState::probe_error && record.reason == reason &&
           record.cleanup_complete == cleanup_complete;
  });
}

[[nodiscard]] auto core_file_count() -> std::size_t {
  return static_cast<std::size_t>(std::ranges::count_if(
      std::filesystem::directory_iterator{std::filesystem::current_path()},
      [](const auto& entry) {
        return entry.path().filename().string().starts_with("core");
      }));
}

TEST_CASE("audio-device runner rejects invalid authority and resource options",
          "[audio-device][runner][failure]") {
  auto value = options();
  const auto initial = run_evaluation(std::string(40, 'a'), value);
  INFO((initial.has_value() ? std::string{} : initial.error().message));
  REQUIRE(initial);

  REQUIRE(run_evaluation(std::string(39, 'a'), value).error().code ==
          RunnerErrorCode::invalid_options);
  value = options();
  value.contract.executable = "relative";
  REQUIRE(run_evaluation(std::string(40, 'a'), value).error().code ==
          RunnerErrorCode::invalid_options);
  value = options();
  value.child_timeout = 0ms;
  REQUIRE(run_evaluation(std::string(40, 'a'), value).error().code ==
          RunnerErrorCode::invalid_options);
  value = options();
  value.child_timeout = 5001ms;
  REQUIRE(run_evaluation(std::string(40, 'a'), value).error().code ==
          RunnerErrorCode::invalid_options);
  value = options();
  value.maximum_child_output_bytes = maximum_child_report_bytes + 1;
  REQUIRE(run_evaluation(std::string(40, 'a'), value).error().code ==
          RunnerErrorCode::invalid_options);
  value = options();
  value.contract.argument_prefix.assign(17, "safe");
  REQUIRE(run_evaluation(std::string(40, 'a'), value).error().code ==
          RunnerErrorCode::invalid_options);
}

TEST_CASE("audio-device runner converts child failures into closed evidence",
          "[audio-device][runner][failure]") {
  SECTION("missing executable") {
    auto value = options();
    value.contract.executable = "/definitely/missing/audio-device-probe";
    const auto report = run_evaluation(std::string(40, 'a'), value);
    INFO((report.has_value() ? std::string{} : report.error().message));
    REQUIRE(report);
    REQUIRE(all_reason(report->contract.probes, ReasonCode::internal_error));
    REQUIRE(evidence_run_succeeded(*report) == false);
  }

  SECTION("malformed protocol") {
    auto value = options();
    value.contract = command("{malformed");
    const auto report = run_evaluation(std::string(40, 'a'), value);
    INFO((report.has_value() ? std::string{} : report.error().message));
    REQUIRE(report);
    REQUIRE(
        all_reason(report->contract.probes, ReasonCode::malformed_protocol));
  }

  SECTION("output ceiling") {
    auto value = options();
    value.maximum_child_output_bytes = 32;
    const auto report = run_evaluation(std::string(40, 'a'), value);
    INFO((report.has_value() ? std::string{} : report.error().message));
    REQUIRE(report);
    REQUIRE(all_reason(report->contract.probes, ReasonCode::output_limit));
    REQUIRE(all_reason(report->candidates[0].probes, ReasonCode::output_limit));
    REQUIRE(all_reason(report->candidates[1].probes, ReasonCode::output_limit));
  }

  SECTION("timeout") {
    auto value = options();
    value.contract = {std::filesystem::canonical("/usr/bin/sleep"), {"1"}};
    value.child_timeout = 10ms;
    const auto report = run_evaluation(std::string(40, 'a'), value);
    INFO((report.has_value() ? std::string{} : report.error().message));
    REQUIRE(report);
    REQUIRE(all_reason(report->contract.probes, ReasonCode::timeout));
  }

  SECTION("nonzero exit") {
    auto value = options();
    value.rtaudio = {std::filesystem::canonical("/usr/bin/false"), {}};
    const auto report = run_evaluation(std::string(40, 'a'), value);
    INFO((report.has_value() ? std::string{} : report.error().message));
    REQUIRE(report);
    REQUIRE(all_reason(report->candidates[0].probes, ReasonCode::nonzero_exit));
    REQUIRE_FALSE(
        all_reason(report->candidates[1].probes, ReasonCode::nonzero_exit));
  }

  SECTION("signal cannot create a core artifact") {
    auto value = options();
    const auto before = core_file_count();
    value.miniaudio = {std::filesystem::canonical("/bin/sh"),
                       {"-c", "kill -ABRT $$"}};
    const auto report = run_evaluation(std::string(40, 'a'), value);
    INFO((report.has_value() ? std::string{} : report.error().message));
    REQUIRE(report);
    REQUIRE(all_reason(report->candidates[1].probes, ReasonCode::signaled));
    REQUIRE(core_file_count() == before);
  }
}

TEST_CASE("audio-device runner aggregates fixed sanitized reports last",
          "[audio-device][runner][smoke]") {
  const auto report = run_evaluation(std::string(40, 'a'), options());
  INFO((report.has_value() ? std::string{} : report.error().message));
  REQUIRE(report);
  REQUIRE(report->source_sha == std::string(40, 'a'));
  REQUIRE(report->platform == "linux");
  REQUIRE_FALSE(report->architecture.empty());
  REQUIRE(report->contract.probes.size() ==
          required_contract_probe_keys().size());
  REQUIRE(report->candidates.size() == required_candidate_ids().size());
  REQUIRE(report->candidates[0].candidate_id == CandidateId::rtaudio);
  REQUIRE(report->candidates[1].candidate_id == CandidateId::miniaudio);
  REQUIRE(evidence_run_succeeded(*report) == true);
  const auto encoded = serialize_report(*report);
  REQUIRE(encoded);
  REQUIRE(encoded->find("/usr/bin") == std::string::npos);
  REQUIRE(encoded->find("printf") == std::string::npos);
}

} // namespace
