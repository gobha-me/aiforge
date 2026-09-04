#include "device_contract.hpp"
#include "evidence.hpp"
#include "probe_process.hpp"
#include "scripted_device.hpp"

#include <aiforge/audio/wav.hpp>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <future>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <unistd.h>

namespace audio = aiforge::evaluation::audio_device;
namespace wav = aiforge::audio;

namespace {

constexpr audio::Signed16Format mono{48'000, 1};

[[nodiscard]] auto playback() -> audio::PlaybackBuffer {
  return {mono, {1, 2, 3, 4}};
}

[[nodiscard]] auto playback_plan() -> audio::ScriptedDevicePlan {
  return {.callbacks = {{1, {}, audio::CallbackStatus::normal},
                        {3, {}, audio::CallbackStatus::normal}},
          .failures = {},
          .barrier = std::nullopt,
          .callback_during_close = false,
          .continue_after_terminal = false};
}

[[nodiscard]] auto capture_plan() -> audio::ScriptedDevicePlan {
  return {.callbacks = {{1, {1}, audio::CallbackStatus::normal},
                        {1, {2}, audio::CallbackStatus::normal}},
          .failures = {},
          .barrier = std::nullopt,
          .callback_during_close = false,
          .continue_after_terminal = false};
}

[[nodiscard]] auto callback_plan(audio::ScriptedCallbackStep callback)
    -> audio::ScriptedDevicePlan {
  return {.callbacks = {std::move(callback)},
          .failures = {},
          .barrier = std::nullopt,
          .callback_during_close = false,
          .continue_after_terminal = false};
}

template <typename Result>
[[nodiscard]] auto has_error(const Result& result,
                             const audio::DeviceContractErrorCode code)
    -> bool {
  return !result && result.error().code == code;
}

[[nodiscard]] auto cancellation_probe(const audio::Direction direction,
                                      const audio::DeviceStage stage) -> bool {
  auto plan = direction == audio::Direction::playback ? playback_plan()
                                                      : capture_plan();
  plan.barrier = stage;
  auto device = audio::ScriptedDevice{std::move(plan)};
  auto controller = audio::DeviceContractController{};
  auto cancellation = audio::CancellationFlag{};
  auto operation = std::async(std::launch::async, [&] {
    if (direction == audio::Direction::playback)
      return controller.play(device, playback(), &cancellation)
          .transform([](const audio::OperationStats&) { return true; });
    return controller.capture(device, {mono, 2}, &cancellation)
        .transform([](const audio::CaptureResult&) { return true; });
  });
  device.wait_until_blocked(stage);
  cancellation.request();
  device.release_barrier();
  const auto result = operation.get();
  const auto calls = device.calls();
  return !result &&
         result.error().code == audio::DeviceContractErrorCode::cancelled &&
         result.error().stage == stage && !calls.empty() &&
         calls.back() == audio::DeviceStage::close && !controller.active();
}

[[nodiscard]] auto stage_for(const audio::ProbeId probe) -> audio::DeviceStage {
  using enum audio::ProbeId;
  switch (probe) {
    case cancel_during_open: return audio::DeviceStage::open;
    case cancel_during_start: return audio::DeviceStage::start;
    case cancel_during_stream: return audio::DeviceStage::stream;
    case cancel_during_stop: return audio::DeviceStage::stop;
    case cancel_during_close: return audio::DeviceStage::close;
    default: return audio::DeviceStage::open;
  }
}

[[nodiscard]] auto evaluate(const audio::ProbeKey key) -> bool {
  using enum audio::ProbeId;
  switch (key.probe_id) {
    case invalid_format_rejected: {
      auto device = audio::ScriptedDevice{};
      auto controller = audio::DeviceContractController{};
      return has_error(controller.play(device, {{7'999, 1}, {1}}),
                       audio::DeviceContractErrorCode::invalid_format) &&
             device.calls().empty();
    }
    case malformed_wav_rejected: {
      constexpr auto malformed = std::array<std::byte, 12>{};
      const auto result = wav::validate_pcm_wav(malformed);
      return !result && result.error().code == wav::PcmWavErrorCode::malformed;
    }
    case oversized_wav_rejected: {
      constexpr auto bytes = std::array<std::byte, 13>{};
      const auto result =
          wav::validate_pcm_wav(bytes, {.maximum_bytes = 12,
                                        .maximum_chunks = 1,
                                        .maximum_channels = 1,
                                        .minimum_sample_rate = 8'000,
                                        .maximum_sample_rate = 8'000});
      return !result && result.error().code == wav::PcmWavErrorCode::too_large;
    }
    case permission_denial_classified: {
      auto plan = capture_plan();
      plan.failures.push_back({audio::DeviceStage::open,
                               audio::DeviceFailureCode::permission_denied});
      auto device = audio::ScriptedDevice{std::move(plan)};
      auto controller = audio::DeviceContractController{};
      return has_error(controller.capture(device, {mono, 2}),
                       audio::DeviceContractErrorCode::permission_denied);
    }
    case device_loss_classified: {
      auto step =
          audio::ScriptedCallbackStep{1,
                                      key.direction == audio::Direction::capture
                                          ? std::vector<std::int16_t>{1}
                                          : std::vector<std::int16_t>{},
                                      audio::CallbackStatus::device_lost};
      auto device = audio::ScriptedDevice{callback_plan(std::move(step))};
      auto controller = audio::DeviceContractController{};
      if (key.direction == audio::Direction::playback)
        return has_error(controller.play(device, playback()),
                         audio::DeviceContractErrorCode::device_lost);
      return has_error(controller.capture(device, {mono, 1}),
                       audio::DeviceContractErrorCode::device_lost);
    }
    case playback_underrun_observed: {
      auto device = audio::ScriptedDevice{
          callback_plan({1, {}, audio::CallbackStatus::playback_underrun})};
      auto controller = audio::DeviceContractController{};
      const auto result = controller.play(device, playback());
      return has_error(result,
                       audio::DeviceContractErrorCode::playback_underrun) &&
             result.error().stats.xruns == 1;
    }
    case capture_overrun_rejected: {
      auto device = audio::ScriptedDevice{
          callback_plan({2, {1, 2}, audio::CallbackStatus::normal})};
      auto controller = audio::DeviceContractController{};
      return has_error(controller.capture(device, {mono, 1}),
                       audio::DeviceContractErrorCode::capture_overrun);
    }
    case concurrent_operation_rejected: {
      auto plan = playback_plan();
      plan.barrier = audio::DeviceStage::open;
      auto first_device = audio::ScriptedDevice{std::move(plan)};
      auto second_device = audio::ScriptedDevice{playback_plan()};
      auto controller = audio::DeviceContractController{};
      auto first = std::async(std::launch::async, [&] {
        return controller.play(first_device, playback());
      });
      first_device.wait_until_blocked(audio::DeviceStage::open);
      const auto second = controller.play(second_device, playback());
      first_device.release_barrier();
      const auto first_result = first.get();
      return first_result &&
             has_error(second,
                       audio::DeviceContractErrorCode::operation_in_progress) &&
             second_device.calls().empty();
    }
    case cancel_during_open:
    case cancel_during_start:
    case cancel_during_stream:
    case cancel_during_stop:
    case cancel_during_close:
      return cancellation_probe(key.direction, stage_for(key.probe_id));
    case playback_owner_quiescent: {
      auto device = audio::ScriptedDevice{playback_plan()};
      auto controller = audio::DeviceContractController{};
      const auto result = controller.play(device, playback());
      return result &&
             device.played_samples() == playback().interleaved_samples &&
             !controller.active();
    }
    case capture_bound_enforced: {
      auto device = audio::ScriptedDevice{};
      auto controller =
          audio::DeviceContractController{audio::DeviceContractLimits{2}};
      return has_error(controller.capture(device, {mono, 2}),
                       audio::DeviceContractErrorCode::too_large) &&
             device.calls().empty();
    }
    case partial_capture_not_published: {
      auto device = audio::ScriptedDevice{
          callback_plan({1, {1}, audio::CallbackStatus::normal})};
      auto controller = audio::DeviceContractController{};
      return has_error(controller.capture(device, {mono, 2}),
                       audio::DeviceContractErrorCode::incomplete_stream);
    }
    case late_callback_rejected: {
      auto plan = playback_plan();
      plan.callback_during_close = true;
      auto device = audio::ScriptedDevice{std::move(plan)};
      auto controller = audio::DeviceContractController{};
      const auto result = controller.play(device, playback());
      return has_error(result, audio::DeviceContractErrorCode::late_callback) &&
             device.close_callback_output_unchanged();
    }
    case teardown_quiescent: {
      auto device = audio::ScriptedDevice{playback_plan()};
      auto controller = audio::DeviceContractController{};
      const auto result = controller.play(device, playback());
      const auto calls = device.calls();
      return result && !calls.empty() &&
             calls.back() == audio::DeviceStage::close && !controller.active();
    }
    default: return false;
  }
}

[[nodiscard]] auto make_report() -> audio::ContractReport {
  auto report = audio::ContractReport{};
  for (const auto key : audio::required_contract_probe_keys()) {
    const auto passed = evaluate(key);
    report.probes.push_back(
        {.probe_id = key.probe_id,
         .direction = key.direction,
         .state = passed ? audio::ProbeState::observed
                         : audio::ProbeState::unavailable,
         .reason = passed ? audio::ReasonCode::none
                          : audio::ReasonCode::contract_failed,
         .cancellation_observed =
             passed && key.probe_id >= audio::ProbeId::cancel_during_open &&
             key.probe_id <= audio::ProbeId::cancel_during_close,
         .cleanup_complete = true});
  }
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
    const auto document = audio::serialize_contract_report(make_report());
    if (!document || !write_all(*document)) return 70;
    return 0;
  } catch (...) {
    return 70;
  }
}
