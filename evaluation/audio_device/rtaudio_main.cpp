#include "evidence.hpp"
#include "probe_process.hpp"

#include <algorithm>
#include <cerrno>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <RtAudio.h>
#include <unistd.h>

#ifndef AIFORGE_RTAUDIO_DEPENDENCY_SOURCE
#error "RtAudio dependency source identity was not supplied"
#endif

namespace audio = aiforge::evaluation::audio_device;

namespace {

[[nodiscard]] auto dependency_source() -> audio::DependencySource {
  constexpr auto source = std::string_view{AIFORGE_RTAUDIO_DEPENDENCY_SOURCE};
  static_assert(source == "installed_package" ||
                source == "controlled_source_fallback");
  if (source == "installed_package")
    return audio::DependencySource::installed_package;
  return audio::DependencySource::controlled_source_fallback;
}

[[nodiscard]] auto record_for(const audio::ProbeKey key,
                              const bool dummy_selected) -> audio::ProbeRecord {
  using enum audio::ProbeId;
  auto record = audio::ProbeRecord{key.probe_id, key.direction};
  record.cleanup_complete = true;

  if (key.probe_id == runtime_backend_forced) {
    record.state = dummy_selected ? audio::ProbeState::observed
                                  : audio::ProbeState::probe_error;
    record.reason = dummy_selected ? audio::ReasonCode::none
                                   : audio::ReasonCode::internal_error;
    return record;
  }
  if (key.probe_id == physical_device_access_excluded) {
    record.state = audio::ProbeState::observed;
    record.reason = audio::ReasonCode::none;
    return record;
  }

  record.state = audio::ProbeState::unavailable;
  record.reason = key.probe_id == device_availability_behavior
                      ? audio::ReasonCode::no_device
                      : audio::ReasonCode::prerequisite_unavailable;
  return record;
}

[[nodiscard]] auto make_report() -> audio::CandidateReport {
  auto compiled_apis = std::vector<RtAudio::Api>{};
  RtAudio::getCompiledApi(compiled_apis);
  auto dummy_selected = false;
  if (std::ranges::find(compiled_apis, RtAudio::RTAUDIO_DUMMY) !=
      compiled_apis.end()) {
    auto ignored_error = RtAudioErrorCallback{
        [](const RtAudioErrorType /*type*/, const std::string& /*message*/) {}};
    auto device = RtAudio{RtAudio::RTAUDIO_DUMMY, std::move(ignored_error)};
    dummy_selected = device.getCurrentApi() == RtAudio::RTAUDIO_DUMMY;
  }

  auto report = audio::CandidateReport{
      .candidate_id = audio::CandidateId::rtaudio,
      .candidate_version = "6.0.1",
      .dependency_source = dependency_source(),
      .linkage = audio::Linkage::static_library,
      .runtime_backend = audio::RuntimeBackend::dummy,
      .device_access = false,
      .codec_features = false,
      .probes = {},
  };
  for (const auto key : audio::required_candidate_probe_keys())
    report.probes.push_back(record_for(key, dummy_selected));
  return report;
}

[[nodiscard]] auto write_all(const std::string_view document) -> bool {
  std::size_t offset{};
  while (offset < document.size()) {
    const auto count = ::write(STDOUT_FILENO, document.data() + offset,
                               document.size() - offset);
    if (count < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (count == 0) return false;
    offset += static_cast<std::size_t>(count);
  }
  return true;
}

} // namespace

auto main(const int argc, char* argv[]) -> int {
  if (!audio::harden_probe_process()) return 70;
  try {
    if (argc != 1 || argv == nullptr) return 64;
    const auto document = audio::serialize_candidate_report(make_report());
    if (!document || !write_all(*document)) return 70;
    return 0;
  } catch (...) {
    return 70;
  }
}
