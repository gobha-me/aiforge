#include "evidence.hpp"
#include "runner.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>

#include <unistd.h>

namespace audio = aiforge::evaluation::audio_device;

namespace {

[[nodiscard]] auto executable_directory() -> std::filesystem::path {
  std::string path(4096, '\0');
  const auto count = ::readlink("/proc/self/exe", path.data(), path.size());
  REQUIRE(count > 0);
  REQUIRE(static_cast<std::size_t>(count) < path.size());
  path.resize(static_cast<std::size_t>(count));
  return std::filesystem::path{path}.parent_path();
}

} // namespace

TEST_CASE("audio device harness emits complete bounded synthetic evidence") {
  const auto directory = executable_directory();
  audio::RunnerOptions options;
  options.contract.executable =
      directory / "aiforge_audio_device_contract_probe";
  options.rtaudio.executable = directory / "aiforge_audio_device_rtaudio_probe";
  options.miniaudio.executable =
      directory / "aiforge_audio_device_miniaudio_probe";

  const auto report = audio::run_evaluation(std::string(40, 'a'), options);
  REQUIRE(report);
  REQUIRE(audio::validate_report(*report));
  CHECK(report->source_sha == std::string(40, 'a'));
  CHECK(report->platform == "linux");
  CHECK(report->architecture.size() <= audio::maximum_platform_metadata_bytes);
  CHECK(report->contract.probes.size() ==
        audio::required_contract_probe_keys().size());
  CHECK(report->candidates.size() == audio::required_candidate_ids().size());

  for (const auto& probe : report->contract.probes) {
    CAPTURE(audio::probe_id_name(probe.probe_id));
    CAPTURE(audio::direction_name(probe.direction));
    CHECK(probe.state == audio::ProbeState::observed);
    CHECK(probe.reason == audio::ReasonCode::none);
  }
  for (const auto& candidate : report->candidates) {
    CAPTURE(audio::candidate_id_name(candidate.candidate_id));
    CHECK_FALSE(candidate.device_access);
    CHECK_FALSE(candidate.codec_features);
    CHECK(candidate.probes.size() ==
          audio::required_candidate_probe_keys().size());
    CHECK(std::ranges::none_of(candidate.probes, [](const auto& probe) {
      return probe.state == audio::ProbeState::probe_error;
    }));
  }

  const auto first = audio::serialize_report(*report);
  const auto second = audio::serialize_report(*report);
  REQUIRE(first);
  REQUIRE(second);
  CHECK(*first == *second);
  CHECK(first->size() <= audio::maximum_report_bytes);
  REQUIRE(audio::parse_report(*first));
  CHECK(*audio::parse_report(*first) == *report);
  REQUIRE(audio::evidence_run_succeeded(*report));
  CHECK(*audio::evidence_run_succeeded(*report));
}
