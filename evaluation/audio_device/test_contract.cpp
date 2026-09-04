#include "device_contract.hpp"
#include "scripted_device.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <future>
#include <limits>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace audio = aiforge::evaluation::audio_device;

namespace {

constexpr audio::Signed16Format mono{48000, 1};
constexpr audio::Signed16Format stereo{48000, 2};

[[nodiscard]] auto playback(const audio::Signed16Format format = mono)
    -> audio::PlaybackBuffer {
  return {format, format.channels == 1
                      ? std::vector<std::int16_t>{1, 2, 3, 4}
                      : std::vector<std::int16_t>{1, 2, 3, 4, 5, 6, 7, 8}};
}

[[nodiscard]] auto playback_plan() -> audio::ScriptedDevicePlan {
  return {.callbacks = {{1, {}, audio::CallbackStatus::normal},
                        {3, {}, audio::CallbackStatus::normal}}};
}

[[nodiscard]] auto capture_plan() -> audio::ScriptedDevicePlan {
  return {.callbacks = {
              {1, {1, 2}, audio::CallbackStatus::normal},
              {2, {3, 4, 5, 6}, audio::CallbackStatus::normal},
          }};
}

template <typename Result>
auto require_error(const Result& result,
                   const audio::DeviceContractErrorCode code) -> void {
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().code == code);
}

} // namespace

TEST_CASE("signed-16 formats buffers and limits fail before device access") {
  for (const auto format :
       {audio::Signed16Format{7999, 1}, audio::Signed16Format{192001, 1},
        audio::Signed16Format{48000, 0}, audio::Signed16Format{48000, 3}}) {
    audio::DeviceContractController controller;
    audio::ScriptedDevice device;
    require_error(controller.play(device, playback(format)),
                  audio::DeviceContractErrorCode::invalid_format);
    require_error(controller.capture(device, {format, 1}),
                  audio::DeviceContractErrorCode::invalid_format);
    CHECK(device.calls().empty());
  }

  {
    audio::DeviceContractController controller;
    audio::ScriptedDevice device;
    require_error(controller.play(device, {mono, {}}),
                  audio::DeviceContractErrorCode::invalid_buffer);
    require_error(controller.play(device, {stereo, {1, 2, 3}}),
                  audio::DeviceContractErrorCode::invalid_buffer);
    require_error(controller.capture(device, {mono, 0}),
                  audio::DeviceContractErrorCode::invalid_buffer);
    CHECK(device.calls().empty());
  }

  for (const auto limit :
       {std::size_t{}, audio::maximum_signed16_buffer_bytes + 1}) {
    audio::DeviceContractController controller{
        audio::DeviceContractLimits{limit}};
    audio::ScriptedDevice device;
    require_error(controller.play(device, playback()),
                  audio::DeviceContractErrorCode::invalid_limits);
    CHECK(device.calls().empty());
  }

  for (const auto limit :
       {std::size_t{}, audio::maximum_signed16_buffer_frames + 1}) {
    audio::DeviceContractController controller{
        audio::DeviceContractLimits{.maximum_buffer_frames = limit}};
    audio::ScriptedDevice device;
    require_error(controller.capture(device, {mono, 1}),
                  audio::DeviceContractErrorCode::invalid_limits);
    CHECK(device.calls().empty());
  }
}

TEST_CASE("signed-16 buffer byte bounds are exact and overflow safe") {
  audio::DeviceContractController controller{audio::DeviceContractLimits{8}};
  {
    audio::ScriptedDevice device{
        {.callbacks = {{4, {}, audio::CallbackStatus::normal}}}};
    const auto result = controller.play(device, {mono, {1, 2, 3, 4}});
    REQUIRE(result);
    CHECK(result->frames == 4);
  }
  {
    audio::ScriptedDevice device;
    require_error(controller.play(device, {mono, {1, 2, 3, 4, 5}}),
                  audio::DeviceContractErrorCode::too_large);
    CHECK(device.calls().empty());
  }
  {
    audio::ScriptedDevice device{
        {.callbacks = {{2, {1, 2, 3, 4}, audio::CallbackStatus::normal}}}};
    const auto result = controller.capture(device, {stereo, 2});
    REQUIRE(result);
    CHECK(result->interleaved_samples == std::vector<std::int16_t>{1, 2, 3, 4});
  }
  {
    audio::ScriptedDevice device;
    require_error(controller.capture(device, {stereo, 3}),
                  audio::DeviceContractErrorCode::too_large);
    require_error(
        controller.capture(device,
                           {stereo, std::numeric_limits<std::size_t>::max()}),
        audio::DeviceContractErrorCode::too_large);
    CHECK(device.calls().empty());
  }
}

TEST_CASE("signed-16 buffer frame bounds are exact") {
  audio::DeviceContractController controller{
      audio::DeviceContractLimits{.maximum_buffer_frames = 3}};
  {
    audio::ScriptedDevice device{
        {.callbacks = {{3, {}, audio::CallbackStatus::normal}}}};
    const auto result = controller.play(device, {mono, {1, 2, 3}});
    REQUIRE(result);
    CHECK(result->frames == 3);
  }
  {
    audio::ScriptedDevice device;
    require_error(controller.play(device, playback()),
                  audio::DeviceContractErrorCode::too_large);
    require_error(controller.capture(device, {mono, 4}),
                  audio::DeviceContractErrorCode::too_large);
    CHECK(device.calls().empty());
  }
}

TEST_CASE(
    "playback and capture traverse the exact lifecycle with owned buffers") {
  audio::DeviceContractController controller;
  audio::ScriptedDevice player{playback_plan()};
  const auto played = controller.play(player, playback());
  REQUIRE(played);
  CHECK(*played == audio::OperationStats{2, 4, 0, 0});
  CHECK(player.calls() ==
        std::vector{audio::DeviceStage::open, audio::DeviceStage::start,
                    audio::DeviceStage::stream, audio::DeviceStage::stop,
                    audio::DeviceStage::close});
  REQUIRE(player.open_request());
  CHECK(player.open_request()->direction == audio::AudioDirection::playback);
  CHECK(player.open_request()->format == mono);
  CHECK(player.played_samples() == std::vector<std::int16_t>{1, 2, 3, 4});

  audio::ScriptedDevice recorder{capture_plan()};
  const auto captured = controller.capture(recorder, {stereo, 3});
  REQUIRE(captured);
  CHECK(captured->format == stereo);
  CHECK(captured->interleaved_samples ==
        std::vector<std::int16_t>{1, 2, 3, 4, 5, 6});
  CHECK(captured->stats == audio::OperationStats{2, 3, 0, 0});
  REQUIRE(recorder.open_request());
  CHECK(recorder.open_request()->direction == audio::AudioDirection::capture);
}

TEST_CASE("a final playback callback is zero-filled beyond the owned buffer") {
  audio::DeviceContractController controller;
  audio::ScriptedDevice device{{.callbacks = {
                                    {3, {}, audio::CallbackStatus::normal},
                                    {3, {}, audio::CallbackStatus::normal},
                                }}};
  const auto result = controller.play(device, playback());
  REQUIRE(result);
  CHECK(result->frames == 4);
  CHECK(device.played_samples() == std::vector<std::int16_t>{1, 2, 3, 4, 0, 0});
  CHECK(device.callback_decisions() ==
        std::vector{audio::CallbackDecision::continue_operation,
                    audio::CallbackDecision::complete});
}

TEST_CASE("device failures are closed and cleanup still runs") {
  struct Case {
    audio::DeviceStage stage;
    audio::DeviceFailureCode failure;
    audio::DeviceContractErrorCode expected;
  };
  for (const auto test_case :
       {Case{audio::DeviceStage::open,
             audio::DeviceFailureCode::permission_denied,
             audio::DeviceContractErrorCode::permission_denied},
        Case{audio::DeviceStage::open, audio::DeviceFailureCode::unavailable,
             audio::DeviceContractErrorCode::unavailable},
        Case{audio::DeviceStage::start,
             audio::DeviceFailureCode::unsupported_format,
             audio::DeviceContractErrorCode::unsupported_format},
        Case{audio::DeviceStage::stream, audio::DeviceFailureCode::device_lost,
             audio::DeviceContractErrorCode::device_lost}}) {
    auto plan = playback_plan();
    plan.failures.push_back({test_case.stage, test_case.failure});
    audio::ScriptedDevice device{std::move(plan)};
    audio::DeviceContractController controller;
    const auto result = controller.play(device, playback());
    require_error(result, test_case.expected);
    CHECK(result.error().stage == test_case.stage);
    CHECK(device.calls().back() == audio::DeviceStage::close);
  }
}

TEST_CASE("callback xruns device loss and malformed spans fail closed") {
  struct Case {
    audio::AudioDirection direction;
    audio::ScriptedCallbackStep callback;
    audio::DeviceContractErrorCode expected;
  };
  const std::vector<Case> cases{
      {audio::AudioDirection::playback,
       {1, {}, audio::CallbackStatus::playback_underrun},
       audio::DeviceContractErrorCode::playback_underrun},
      {audio::AudioDirection::capture,
       {1, {1}, audio::CallbackStatus::capture_overrun},
       audio::DeviceContractErrorCode::capture_overrun},
      {audio::AudioDirection::capture,
       {1, {1}, audio::CallbackStatus::device_lost},
       audio::DeviceContractErrorCode::device_lost},
      {audio::AudioDirection::capture,
       {1, {}, audio::CallbackStatus::normal},
       audio::DeviceContractErrorCode::callback_contract_violation},
  };
  for (const auto& test_case : cases) {
    audio::ScriptedDevice device{{.callbacks = {test_case.callback}}};
    audio::DeviceContractController controller;
    if (test_case.direction == audio::AudioDirection::playback) {
      const auto result = controller.play(device, playback());
      require_error(result, test_case.expected);
      if (test_case.expected ==
          audio::DeviceContractErrorCode::playback_underrun)
        CHECK(result.error().stats.xruns == 1);
    } else {
      const auto result = controller.capture(device, {mono, 1});
      require_error(result, test_case.expected);
    }
  }
}

TEST_CASE("capture overrun and incomplete capture publish no partial buffer") {
  audio::DeviceContractController controller;
  {
    audio::ScriptedDevice device{
        {.callbacks = {
             {1, {1}, audio::CallbackStatus::normal},
             {2, {2, 3}, audio::CallbackStatus::normal},
         }}};
    const auto result = controller.capture(device, {mono, 2});
    require_error(result, audio::DeviceContractErrorCode::capture_overrun);
    CHECK(result.error().stats.frames == 1);
  }
  {
    audio::ScriptedDevice device{
        {.callbacks = {{1, {1}, audio::CallbackStatus::normal}}}};
    const auto result = controller.capture(device, {mono, 2});
    require_error(result, audio::DeviceContractErrorCode::incomplete_stream);
    CHECK(result.error().stats.frames == 1);
  }
}

TEST_CASE("the controller rejects a second operation while one is active") {
  auto plan = playback_plan();
  plan.barrier = audio::DeviceStage::open;
  audio::ScriptedDevice device{std::move(plan)};
  audio::DeviceContractController controller;
  auto first = std::async(std::launch::async,
                          [&] { return controller.play(device, playback()); });
  device.wait_until_blocked(audio::DeviceStage::open);
  CHECK(controller.active());

  audio::ScriptedDevice other{playback_plan()};
  const auto second = controller.play(other, playback());
  require_error(second, audio::DeviceContractErrorCode::operation_in_progress);
  CHECK(other.calls().empty());

  device.release_barrier();
  REQUIRE(first.get());
  CHECK_FALSE(controller.active());
}

TEST_CASE("cancellation is deterministic at every lifecycle stage") {
  for (const auto stage : {audio::DeviceStage::open, audio::DeviceStage::start,
                           audio::DeviceStage::stream, audio::DeviceStage::stop,
                           audio::DeviceStage::close}) {
    auto plan = playback_plan();
    plan.barrier = stage;
    audio::ScriptedDevice device{std::move(plan)};
    audio::DeviceContractController controller;
    audio::CancellationFlag cancellation;
    auto operation = std::async(std::launch::async, [&] {
      return controller.play(device, playback(), &cancellation);
    });
    device.wait_until_blocked(stage);
    cancellation.request();
    device.release_barrier();

    const auto result = operation.get();
    require_error(result, audio::DeviceContractErrorCode::cancelled);
    CHECK(result.error().stage == stage);
    CHECK(device.calls().back() == audio::DeviceStage::close);
    CHECK_FALSE(controller.active());
  }

  audio::CancellationFlag already_cancelled;
  already_cancelled.request();
  audio::ScriptedDevice untouched;
  audio::DeviceContractController controller;
  require_error(controller.play(untouched, playback(), &already_cancelled),
                audio::DeviceContractErrorCode::cancelled);
  CHECK(untouched.calls().empty());
}

TEST_CASE(
    "cleanup failure has precedence over cancellation and stream failure") {
  {
    auto plan = playback_plan();
    plan.failures = {
        {audio::DeviceStage::stream, audio::DeviceFailureCode::device_lost},
        {audio::DeviceStage::stop, audio::DeviceFailureCode::internal_failure},
        {audio::DeviceStage::close, audio::DeviceFailureCode::internal_failure},
    };
    audio::ScriptedDevice device{std::move(plan)};
    audio::DeviceContractController controller;
    const auto result = controller.play(device, playback());
    require_error(result, audio::DeviceContractErrorCode::cleanup_failed);
    CHECK(result.error().stage == audio::DeviceStage::close);
  }
  {
    auto plan = playback_plan();
    plan.barrier = audio::DeviceStage::close;
    plan.failures = {{audio::DeviceStage::close,
                      audio::DeviceFailureCode::internal_failure}};
    audio::ScriptedDevice device{std::move(plan)};
    audio::DeviceContractController controller;
    audio::CancellationFlag cancellation;
    auto operation = std::async(std::launch::async, [&] {
      return controller.play(device, playback(), &cancellation);
    });
    device.wait_until_blocked(audio::DeviceStage::close);
    cancellation.request();
    device.release_barrier();
    const auto result = operation.get();
    require_error(result, audio::DeviceContractErrorCode::cleanup_failed);
    CHECK(result.error().stage == audio::DeviceStage::close);
  }
}

TEST_CASE(
    "callbacks use a device thread and teardown callbacks touch no buffer") {
  const auto owner = std::this_thread::get_id();
  auto plan = playback_plan();
  plan.callback_during_close = true;
  audio::ScriptedDevice device{std::move(plan)};
  audio::DeviceContractController controller;
  const auto result = controller.play(device, playback());
  require_error(result, audio::DeviceContractErrorCode::late_callback);
  CHECK(result.error().stats.late_callbacks == 1);
  CHECK(device.callback_thread() != owner);
  CHECK(device.close_callback_output_unchanged());
  REQUIRE_FALSE(device.callback_decisions().empty());
  CHECK(device.callback_decisions().back() == audio::CallbackDecision::abort);
}

TEST_CASE("a callback after a terminal decision fails the operation") {
  auto plan = playback_plan();
  plan.callbacks.push_back({1, {}, audio::CallbackStatus::normal});
  plan.continue_after_terminal = true;
  audio::ScriptedDevice device{std::move(plan)};
  audio::DeviceContractController controller;
  const auto result = controller.play(device, playback());
  require_error(result,
                audio::DeviceContractErrorCode::callback_contract_violation);
  CHECK(result.error().stats.callbacks == 3);
  CHECK(result.error().stats.frames == 4);
}
