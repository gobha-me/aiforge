#include "evidence.hpp"
#include "probe_process.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <stop_token>
#include <string_view>
#include <thread>

#include <miniaudio.h>
#include <unistd.h>

#ifndef AIFORGE_MINIAUDIO_DEPENDENCY_SOURCE
#error "miniaudio dependency source identity was not supplied"
#endif

namespace audio = aiforge::evaluation::audio_device;

namespace {

struct CallbackState {
  std::atomic<std::uint64_t> callbacks{};
  std::atomic<std::uint64_t> frames{};
};

struct DirectionOutcome {
  bool available{};
  bool callback_observed{};
  bool controller_stopped{};
  bool cancellation_requested{};
  bool quiescent{};
  std::uint64_t callbacks{};
  std::uint64_t frames{};
};

auto data_callback(ma_device* device, void* output, const void* /*input*/,
                   const ma_uint32 frame_count) noexcept -> void {
  auto* state = static_cast<CallbackState*>(device->pUserData);
  if (output != nullptr)
    ma_silence_pcm_frames(output, frame_count, device->playback.format,
                          device->playback.channels);
  state->frames.fetch_add(frame_count, std::memory_order_relaxed);
  state->callbacks.fetch_add(1, std::memory_order_release);
}

[[nodiscard]] auto dependency_source() -> audio::DependencySource {
  constexpr auto source = std::string_view{AIFORGE_MINIAUDIO_DEPENDENCY_SOURCE};
  static_assert(source == "installed_package" ||
                source == "controlled_source_fallback");
  if (source == "installed_package")
    return audio::DependencySource::installed_package;
  return audio::DependencySource::controlled_source_fallback;
}

[[nodiscard]] auto exercise_direction(ma_context& context,
                                      const ma_device_type type)
    -> DirectionOutcome {
  auto state = CallbackState{};
  auto config = ma_device_config_init(type);
  config.sampleRate = 8'000;
  config.periodSizeInFrames = 64;
  config.periods = 2;
  config.dataCallback = data_callback;
  config.pUserData = &state;
  if (type == ma_device_type_playback) {
    config.playback.format = ma_format_s16;
    config.playback.channels = 1;
  } else {
    config.capture.format = ma_format_s16;
    config.capture.channels = 1;
  }

  auto device = ma_device{};
  if (ma_device_init(&context, &config, &device) != MA_SUCCESS) return {};

  auto outcome = DirectionOutcome{.available = true};
  if (ma_device_start(&device) == MA_SUCCESS) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
    while (state.callbacks.load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < deadline)
      std::this_thread::yield();
    outcome.callback_observed =
        state.callbacks.load(std::memory_order_acquire) != 0;
    auto cancellation = std::stop_source{};
    auto stop_callback =
        std::stop_callback{cancellation.get_token(), [&device, &outcome] {
                             outcome.controller_stopped =
                                 ma_device_stop(&device) == MA_SUCCESS;
                           }};
    outcome.cancellation_requested =
        cancellation.request_stop() && cancellation.stop_requested();
  }

  ma_device_uninit(&device);
  outcome.callbacks = state.callbacks.load(std::memory_order_acquire);
  outcome.frames = state.frames.load(std::memory_order_relaxed);
  outcome.quiescent = outcome.controller_stopped;
  for (std::size_t iteration{}; iteration < 1'024; ++iteration) {
    std::this_thread::yield();
    if (state.callbacks.load(std::memory_order_acquire) != outcome.callbacks)
      outcome.quiescent = false;
  }
  return outcome;
}

[[nodiscard]] auto observed_record(const audio::ProbeKey key,
                                   const std::uint64_t callbacks = 0,
                                   const std::uint64_t frames = 0)
    -> audio::ProbeRecord {
  return {.probe_id = key.probe_id,
          .direction = key.direction,
          .state = audio::ProbeState::observed,
          .reason = audio::ReasonCode::none,
          .callbacks = callbacks,
          .frames = frames,
          .cancellation_observed =
              key.probe_id == audio::ProbeId::controller_thread_cancellation,
          .cleanup_complete = true};
}

[[nodiscard]] auto unavailable_record(const audio::ProbeKey key,
                                      const audio::ReasonCode reason)
    -> audio::ProbeRecord {
  return {.probe_id = key.probe_id,
          .direction = key.direction,
          .state = audio::ProbeState::unavailable,
          .reason = reason,
          .cleanup_complete = true};
}

[[nodiscard]] auto record_for(const audio::ProbeKey key,
                              const bool null_selected,
                              const DirectionOutcome& outcome)
    -> audio::ProbeRecord {
  using enum audio::ProbeId;
  if (key.probe_id == runtime_backend_forced)
    return null_selected
               ? observed_record(key)
               : audio::ProbeRecord{.probe_id = key.probe_id,
                                    .direction = key.direction,
                                    .state = audio::ProbeState::probe_error,
                                    .reason = audio::ReasonCode::internal_error,
                                    .cleanup_complete = true};
  if (key.probe_id == physical_device_access_excluded)
    return observed_record(key);
  if (!outcome.available)
    return unavailable_record(
        key, key.probe_id == device_availability_behavior
                 ? audio::ReasonCode::candidate_limitation
                 : audio::ReasonCode::prerequisite_unavailable);
  if (key.probe_id == device_availability_behavior) return observed_record(key);
  if (key.probe_id == playback_callback_lifecycle ||
      key.probe_id == capture_callback_lifecycle)
    return outcome.callback_observed
               ? observed_record(key, outcome.callbacks, outcome.frames)
               : unavailable_record(key, audio::ReasonCode::contract_failed);
  if (key.probe_id == controller_thread_cancellation)
    return outcome.cancellation_requested && outcome.controller_stopped
               ? observed_record(key, outcome.callbacks, outcome.frames)
               : unavailable_record(key, audio::ReasonCode::contract_failed);
  if (key.probe_id == callback_quiescent_after_close)
    return outcome.quiescent
               ? observed_record(key, outcome.callbacks, outcome.frames)
               : audio::ProbeRecord{.probe_id = key.probe_id,
                                    .direction = key.direction,
                                    .state = audio::ProbeState::probe_error,
                                    .reason = audio::ReasonCode::cleanup_failed,
                                    .callbacks = outcome.callbacks,
                                    .frames = outcome.frames,
                                    .cleanup_complete = false};
  return unavailable_record(key, audio::ReasonCode::candidate_limitation);
}

[[nodiscard]] auto make_report() -> audio::CandidateReport {
  constexpr auto backends = std::array{ma_backend_null};
  auto context = ma_context{};
  const auto context_ready = ma_context_init(backends.data(), backends.size(),
                                             nullptr, &context) == MA_SUCCESS;
  const auto null_selected =
      context_ready && context.backend == ma_backend_null;

  auto playback = DirectionOutcome{};
  auto capture = DirectionOutcome{};
  if (null_selected) {
    playback = exercise_direction(context, ma_device_type_playback);
    capture = exercise_direction(context, ma_device_type_capture);
  }

  auto report = audio::CandidateReport{
      .candidate_id = audio::CandidateId::miniaudio,
      .candidate_version = "0.11.25",
      .dependency_source = dependency_source(),
      .linkage = audio::Linkage::static_library,
      .runtime_backend = audio::RuntimeBackend::null_backend,
      .device_access = false,
      .codec_features = false,
      .probes = {},
  };
  for (const auto key : audio::required_candidate_probe_keys()) {
    const auto& outcome =
        key.direction == audio::Direction::capture ? capture : playback;
    report.probes.push_back(record_for(key, null_selected, outcome));
  }
  if (context_ready) ma_context_uninit(&context);
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
