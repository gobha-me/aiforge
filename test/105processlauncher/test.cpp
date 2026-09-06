#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <limits>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <aiforge/runtime/process_launcher.hpp>
#include <aiforge/testing/scripted_process_launcher.hpp>

namespace {

using namespace aiforge;
using namespace std::chrono_literals;

template <typename Id> auto id(const std::string& value) -> Id {
  return Id::from(value).value();
}

auto request(std::string secret = "fake-secret")
    -> runtime::ProcessLaunchRequest {
  return {id<domain::InvocationId>("process-call"),
          "/usr/bin/tool",
          "executable-identity",
          {"literal;argument"},
          "/workspace",
          "root-identity",
          {{"/workspace", "root-identity",
            runtime::ProcessFilesystemAccess::read_write}},
          {{"/workspace", "root-identity",
            runtime::ProcessFilesystemAccess::read_write}},
          {{"SAFE_NAME", std::move(secret)}},
          {1s, 1024, 64, 20ms}};
}

auto secret_safe_request() -> runtime::ProcessLaunchRequest {
  return request("");
}

auto contract(std::string policy = "aiforge.linux-process.none.v1")
    -> runtime::ProcessLauncherContract {
  return {runtime::RestrictionLevel::none, {}, std::move(policy)};
}

auto context() -> runtime::ApplicationLaunchContext {
  runtime::ApplicationLaunchContextConfiguration configuration;
  configuration.selected_restriction = runtime::RestrictionLevel::none;
  configuration.achieved_restriction = runtime::RestrictionLevel::none;
  configuration.unavailable_reason.reset();
  configuration.restriction_policy_identity = "aiforge.linux-process.none.v1";
  configuration.approval_mode = runtime::ApprovalMode::automatic;
  configuration.matcher_policy_identity = "matcher.v1";
  return runtime::make_application_launch_context(std::move(configuration))
      .value();
}

auto terminal(std::vector<std::byte> output = {})
    -> runtime::ProcessLaunchTerminal {
  return {runtime::ProcessTerminalKind::exited,
          0,
          std::nullopt,
          std::nullopt,
          5ms,
          std::move(output),
          {}};
}

auto script(std::vector<testing::ScriptedProcessLaunchStep> steps)
    -> testing::ScriptedProcessLaunchStream {
  return {std::move(steps)};
}

auto failure(const runtime::ProcessLaunchErrorCode code,
             const runtime::ProcessLaunchStage stage)
    -> runtime::ProcessLaunchError {
  return {code, stage, std::nullopt, "bounded scripted failure", false};
}

} // namespace

static_assert(!std::is_copy_constructible_v<runtime::BoundProcessLauncher>);
static_assert(!std::is_copy_assignable_v<runtime::BoundProcessLauncher>);
static_assert(std::is_move_constructible_v<runtime::BoundProcessLauncher>);

TEST_CASE("process launch requests reject malformed or widened authority",
          "[process-launcher][validation][failure]") {
  REQUIRE(runtime::validate_process_launch_request(request()));

  auto invalid = request();
  invalid.executable = "tool";
  REQUIRE_FALSE(runtime::validate_process_launch_request(invalid));

  invalid = request();
  invalid.executable_identity = "bad/path";
  REQUIRE_FALSE(runtime::validate_process_launch_request(invalid));

  invalid = request();
  invalid.arguments.front().push_back('\0');
  REQUIRE_FALSE(runtime::validate_process_launch_request(invalid));

  invalid = request();
  invalid.configured_roots.push_back(invalid.configured_roots.front());
  REQUIRE_FALSE(runtime::validate_process_launch_request(invalid));

  invalid = request();
  invalid.requested_roots.front().access =
      static_cast<runtime::ProcessFilesystemAccess>(255);
  REQUIRE_FALSE(runtime::validate_process_launch_request(invalid));

  invalid = request();
  invalid.requested_roots.front().path = "/outside";
  REQUIRE_FALSE(runtime::validate_process_launch_request(invalid));

  invalid = request();
  invalid.configured_roots.front().access =
      runtime::ProcessFilesystemAccess::read_only;
  REQUIRE_FALSE(runtime::validate_process_launch_request(invalid));

  invalid = request();
  invalid.requested_roots.front().identity = "different-identity";
  REQUIRE_FALSE(runtime::validate_process_launch_request(invalid));

  invalid = request();
  invalid.working_directory_identity = "different-identity";
  REQUIRE_FALSE(runtime::validate_process_launch_request(invalid));

  invalid = request();
  invalid.working_directory = "/outside";
  REQUIRE_FALSE(runtime::validate_process_launch_request(invalid));

  invalid = request();
  invalid.environment.push_back(invalid.environment.front());
  REQUIRE_FALSE(runtime::validate_process_launch_request(invalid));

  invalid = request();
  invalid.environment.front().name = "9INVALID";
  REQUIRE_FALSE(runtime::validate_process_launch_request(invalid));

  invalid = request();
  invalid.environment.front().name = "N\xC3\x89";
  REQUIRE_FALSE(runtime::validate_process_launch_request(invalid));

  invalid = request();
  invalid.limits.wall_time = 121s;
  REQUIRE_FALSE(runtime::validate_process_launch_request(invalid));

  invalid = request();
  invalid.limits.maximum_progress_chunk_bytes = 4097;
  REQUIRE_FALSE(runtime::validate_process_launch_request(invalid));

  invalid = request();
  invalid.limits.termination_grace = 101ms;
  REQUIRE_FALSE(runtime::validate_process_launch_request(invalid));

  auto bounds = runtime::ProcessLaunchBounds{};
  bounds.maximum_argument_bytes = 4;
  REQUIRE_FALSE(runtime::validate_process_launch_request(request(), bounds));

  bounds = {};
  bounds.maximum_wall_time = 121s;
  REQUIRE_FALSE(runtime::validate_process_launch_bounds(bounds));
}

TEST_CASE("process launch root bounds preserve the independent tool ceilings",
          "[process-launcher][validation][bounds][failure]") {
  auto boundary = request();
  boundary.configured_roots.clear();
  boundary.requested_roots.clear();
  boundary.working_directory = "/root/0";
  boundary.working_directory_identity = "root-identity-0";
  constexpr auto hard_maximum = runtime::ProcessLaunchBounds{}.maximum_roots;
  static_assert(hard_maximum == 128);
  boundary.configured_roots.reserve(hard_maximum + 1U);
  boundary.requested_roots.reserve(hard_maximum + 1U);
  for (std::size_t index{}; index < hard_maximum; ++index) {
    boundary.configured_roots.push_back(
        {"/root/" + std::to_string(index),
         "root-identity-" + std::to_string(index),
         runtime::ProcessFilesystemAccess::read_only});
    boundary.requested_roots.push_back(
        {"/root/" + std::to_string(index),
         "root-identity-" + std::to_string(index),
         runtime::ProcessFilesystemAccess::read_only});
  }
  REQUIRE(runtime::validate_process_launch_request(boundary));

  auto tighter = runtime::ProcessLaunchBounds{};
  tighter.maximum_roots = 64;
  REQUIRE_FALSE(runtime::validate_process_launch_request(boundary, tighter));

  boundary.configured_roots.push_back(
      {"/root/overbound", "root-identity-overbound",
       runtime::ProcessFilesystemAccess::read_only});
  REQUIRE_FALSE(runtime::validate_process_launch_request(boundary));
}

TEST_CASE("only exact none launch contexts bind",
          "[process-launcher][binding][failure]") {
  auto launcher = std::make_shared<testing::ScriptedProcessLauncher>(
      contract(), std::vector<testing::ScriptedProcessLaunchExchange>{});
  auto bound = runtime::bind_process_launcher(context(), launcher);
  REQUIRE(bound);
  REQUIRE(bound->contract_snapshot() == contract());
  REQUIRE(bound->context().matcher_policy_identity() == "matcher.v1");

  REQUIRE_FALSE(runtime::bind_process_launcher(context(), {}));

  auto wrong_policy = std::make_shared<testing::ScriptedProcessLauncher>(
      contract("different.v1"),
      std::vector<testing::ScriptedProcessLaunchExchange>{});
  auto mismatch = runtime::bind_process_launcher(context(), wrong_policy);
  REQUIRE_FALSE(mismatch);
  REQUIRE(mismatch.error().code ==
          runtime::ProcessLauncherBindingErrorCode::contract_mismatch);

  runtime::ApplicationLaunchContextConfiguration unavailable_configuration;
  unavailable_configuration.selected_restriction =
      runtime::RestrictionLevel::high;
  const auto unavailable = runtime::make_application_launch_context(
      std::move(unavailable_configuration));
  REQUIRE(unavailable);
  auto restricted = runtime::bind_process_launcher(*unavailable, launcher);
  REQUIRE_FALSE(restricted);
  REQUIRE(restricted.error().code ==
          runtime::ProcessLauncherBindingErrorCode::unsupported_restriction);

  auto invalid_contract = contract();
  invalid_contract.restriction_policy_identity = "/host/path";
  auto invalid = std::make_shared<testing::ScriptedProcessLauncher>(
      std::move(invalid_contract),
      std::vector<testing::ScriptedProcessLaunchExchange>{});
  auto rejected = runtime::bind_process_launcher(context(), invalid);
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().code ==
          runtime::ProcessLauncherBindingErrorCode::invalid_contract);
}

TEST_CASE("immutable launcher contract rejects stale snapshots before work",
          "[process-launcher][contract][failure]") {
  auto launcher = std::make_shared<testing::ScriptedProcessLauncher>(
      contract(), std::vector<testing::ScriptedProcessLaunchExchange>{
                      {secret_safe_request(),
                       script({runtime::ProcessLaunchEvent{terminal()},
                               testing::ProcessLaunchEndOfStream{}})}});
  auto stale = contract("stale.v1");
  auto rejected = launcher->launch(stale, request());
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().code ==
          runtime::ProcessLaunchErrorCode::contract_drift);
  REQUIRE(launcher->remaining_exchanges() == 1);
  REQUIRE(launcher->recorded_requests().empty());

  auto bad_pin = launcher->pin_path(
      stale, "/workspace", runtime::ProcessFilesystemTargetKind::directory);
  REQUIRE_FALSE(bad_pin);
  REQUIRE(bad_pin.error().code ==
          runtime::ProcessLaunchErrorCode::contract_drift);
  REQUIRE(launcher->recorded_path_pins().empty());
}

TEST_CASE("base launcher validates cancellation requests and pinned identities",
          "[process-launcher][seam][failure]") {
  auto launcher = std::make_shared<testing::ScriptedProcessLauncher>(
      contract(), std::vector<testing::ScriptedProcessLaunchExchange>{},
      runtime::ProcessLaunchBounds{},
      std::vector<testing::ScriptedProcessPathPinExchange>{
          {"/workspace", runtime::ProcessFilesystemTargetKind::directory,
           std::string{"bad/path"}},
          {"/workspace", runtime::ProcessFilesystemTargetKind::directory,
           std::string{"workspace-identity"}}});

  auto invalid_identity =
      launcher->pin_path(contract(), "/workspace",
                         runtime::ProcessFilesystemTargetKind::directory);
  REQUIRE_FALSE(invalid_identity);
  REQUIRE(invalid_identity.error().code ==
          runtime::ProcessLaunchErrorCode::protocol_failure);
  REQUIRE(launcher->pin_path(contract(), "/workspace",
                             runtime::ProcessFilesystemTargetKind::directory) ==
          "workspace-identity");

  auto relative = launcher->pin_path(
      contract(), "workspace", runtime::ProcessFilesystemTargetKind::directory);
  REQUIRE_FALSE(relative);
  REQUIRE(launcher->recorded_path_pins().size() == 2);

  std::stop_source cancelled;
  cancelled.request_stop();
  auto cancelled_launch =
      launcher->launch(contract(), request(), cancelled.get_token());
  REQUIRE_FALSE(cancelled_launch);
  REQUIRE(cancelled_launch.error().code ==
          runtime::ProcessLaunchErrorCode::cancelled);
  REQUIRE(launcher->recorded_requests().empty());
}

TEST_CASE("launcher stream rejects malformed ordering and output",
          "[process-launcher][stream][failure]") {
  const runtime::ProcessLaunchProgress output{
      runtime::ProcessOutputStream::standard_output,
      {std::byte{'o'}, std::byte{'k'}}};
  const runtime::ProcessLaunchProgress late{
      runtime::ProcessOutputStream::standard_error, {std::byte{'x'}}};
  auto cumulative_request = secret_safe_request();
  cumulative_request.limits.maximum_output_bytes = 100;
  cumulative_request.limits.maximum_progress_chunk_bytes = 64;
  std::vector exchanges{
      testing::ScriptedProcessLaunchExchange{
          secret_safe_request(), script({testing::ProcessLaunchEndOfStream{}})},
      testing::ScriptedProcessLaunchExchange{
          secret_safe_request(),
          script({runtime::ProcessLaunchEvent{terminal()},
                  runtime::ProcessLaunchEvent{late}})},
      testing::ScriptedProcessLaunchExchange{
          secret_safe_request(),
          script({runtime::ProcessLaunchEvent{output},
                  runtime::ProcessLaunchEvent{
                      terminal({std::byte{'n'}, std::byte{'o'}})}})},
      testing::ScriptedProcessLaunchExchange{
          secret_safe_request(),
          script({runtime::ProcessLaunchEvent{runtime::ProcessLaunchProgress{
              runtime::ProcessOutputStream::standard_output,
              std::vector<std::byte>(65, std::byte{0x01})}}})},
      testing::ScriptedProcessLaunchExchange{
          cumulative_request,
          script({runtime::ProcessLaunchEvent{runtime::ProcessLaunchProgress{
                      runtime::ProcessOutputStream::standard_output,
                      std::vector<std::byte>(60, std::byte{0x01})}},
                  runtime::ProcessLaunchEvent{runtime::ProcessLaunchProgress{
                      runtime::ProcessOutputStream::standard_error,
                      std::vector<std::byte>(60, std::byte{0x02})}}})},
  };
  auto launcher =
      testing::ScriptedProcessLauncher{contract(), std::move(exchanges)};

  auto premature = launcher.launch(contract(), request());
  REQUIRE(premature);
  auto ended = (*premature)->next();
  REQUIRE_FALSE(ended);
  REQUIRE(ended.error().code ==
          runtime::ProcessLaunchErrorCode::protocol_failure);

  auto post_terminal = launcher.launch(contract(), request());
  REQUIRE(post_terminal);
  REQUIRE((*post_terminal)->next()->has_value());
  auto post = (*post_terminal)->next();
  REQUIRE_FALSE(post);
  REQUIRE(post.error().code ==
          runtime::ProcessLaunchErrorCode::protocol_failure);

  auto mismatch = launcher.launch(contract(), request());
  REQUIRE(mismatch);
  REQUIRE((*mismatch)->next()->has_value());
  auto mismatched_terminal = (*mismatch)->next();
  REQUIRE_FALSE(mismatched_terminal);
  REQUIRE(mismatched_terminal.error().code ==
          runtime::ProcessLaunchErrorCode::protocol_failure);

  auto oversized = launcher.launch(contract(), request());
  REQUIRE(oversized);
  auto oversized_chunk = (*oversized)->next();
  REQUIRE_FALSE(oversized_chunk);
  REQUIRE(oversized_chunk.error().code ==
          runtime::ProcessLaunchErrorCode::protocol_failure);

  auto cumulative_actual = request();
  cumulative_actual.limits = cumulative_request.limits;
  auto cumulative = launcher.launch(contract(), std::move(cumulative_actual));
  REQUIRE(cumulative);
  REQUIRE((*cumulative)->next()->has_value());
  auto over_total = (*cumulative)->next();
  REQUIRE_FALSE(over_total);
  REQUIRE(over_total.error().code ==
          runtime::ProcessLaunchErrorCode::protocol_failure);
}

TEST_CASE("scripted launcher is strict and preserves closed failures",
          "[process-launcher][fake][failure]") {
  auto launcher = testing::ScriptedProcessLauncher{
      contract(),
      {{secret_safe_request(),
        failure(runtime::ProcessLaunchErrorCode::cleanup_failed,
                runtime::ProcessLaunchStage::cleanup)}}};

  auto mismatch = request();
  mismatch.arguments = {"different"};
  auto rejected = launcher.launch(contract(), std::move(mismatch));
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().code ==
          runtime::ProcessLaunchErrorCode::protocol_failure);
  REQUIRE(launcher.remaining_exchanges() == 1);

  auto failed = launcher.launch(contract(), request());
  REQUIRE_FALSE(failed);
  REQUIRE(failed.error().code ==
          runtime::ProcessLaunchErrorCode::cleanup_failed);
  REQUIRE(failed.error().stage == runtime::ProcessLaunchStage::cleanup);

  auto exhausted = launcher.launch(contract(), request());
  REQUIRE_FALSE(exhausted);
  REQUIRE(exhausted.error().code ==
          runtime::ProcessLaunchErrorCode::unavailable);
}

TEST_CASE("scripted launcher serializes concurrent secret-safe captures",
          "[process-launcher][fake][concurrency][failure]") {
  constexpr std::size_t launch_count{16};
  std::vector<testing::ScriptedProcessLaunchExchange> exchanges;
  exchanges.reserve(launch_count);
  for (std::size_t index = 0; index < launch_count; ++index) {
    exchanges.push_back(
        {secret_safe_request(), script({runtime::ProcessLaunchEvent{terminal()},
                                        testing::ProcessLaunchEndOfStream{}})});
  }
  auto launcher = std::make_shared<testing::ScriptedProcessLauncher>(
      contract(), std::move(exchanges));
  std::atomic<std::size_t> succeeded{};
  std::vector<std::thread> threads;
  threads.reserve(launch_count);
  for (std::size_t index = 0; index < launch_count; ++index) {
    threads.emplace_back([launcher, index, &succeeded] {
      auto launched = launcher->launch(
          contract(), request("fake-secret-" + std::to_string(index)));
      if (launched) ++succeeded;
    });
  }
  for (auto& thread : threads)
    thread.join();

  REQUIRE(succeeded == launch_count);
  REQUIRE(launcher->remaining_exchanges() == 0);
  const auto recorded = launcher->recorded_requests();
  REQUIRE(recorded.size() == launch_count);
  for (const auto& captured : recorded) {
    REQUIRE(captured.environment.size() == 1);
    REQUIRE(captured.environment.front().name == "SAFE_NAME");
    REQUIRE(captured.environment.front().value.empty());
  }
}

TEST_CASE("valid process streams end only after one matching terminal",
          "[process-launcher][smoke]") {
  const runtime::ProcessLaunchProgress progress{
      runtime::ProcessOutputStream::standard_output,
      {std::byte{'o'}, std::byte{'k'}}};
  auto launcher = testing::ScriptedProcessLauncher{
      contract(),
      {{secret_safe_request(), script({runtime::ProcessLaunchEvent{progress},
                                       runtime::ProcessLaunchEvent{terminal(
                                           {std::byte{'o'}, std::byte{'k'}})},
                                       testing::ProcessLaunchEndOfStream{}})}}};

  auto stream = launcher.launch(contract(), request());
  REQUIRE(stream);
  const auto first = (*stream)->next();
  REQUIRE(first);
  REQUIRE(*first == std::optional<runtime::ProcessLaunchEvent>{progress});
  REQUIRE((*stream)->next()->has_value());
  REQUIRE_FALSE((*stream)->next()->has_value());
  REQUIRE_FALSE((*stream)->next()->has_value());
}
