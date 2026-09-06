#include <aiforge/adapters/linux_process_launcher.hpp>

#include <array>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

namespace {

using namespace std::chrono_literals;
using aiforge::adapters::LinuxProcessLauncherConfiguration;
using aiforge::adapters::LinuxProcessLauncherEstablishment;
using aiforge::adapters::LinuxProcessLauncherUnavailable;
using aiforge::domain::InvocationId;
using aiforge::runtime::BoundProcessLauncher;
using aiforge::runtime::ProcessFilesystemAccess;
using aiforge::runtime::ProcessFilesystemTargetKind;
using aiforge::runtime::ProcessLaunchEvent;
using aiforge::runtime::ProcessLaunchRequest;
using aiforge::runtime::ProcessLaunchTerminal;
using aiforge::runtime::ProcessOutputStream;
using aiforge::runtime::ProcessTerminalKind;
using aiforge::runtime::RestrictionLevel;

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    auto pattern =
        (std::filesystem::temp_directory_path() / "aiforge-linux-XXXXXX")
            .string();
    pattern.push_back('\0');
    const auto* created = ::mkdtemp(pattern.data());
    REQUIRE(created != nullptr);
    m_path = created;
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(m_path, ignored);
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  auto operator=(const TemporaryDirectory&) -> TemporaryDirectory& = delete;

  [[nodiscard]] auto path() const -> const std::filesystem::path& {
    return m_path;
  }

 private:
  std::filesystem::path m_path;
};

[[nodiscard]] auto fixture() -> std::filesystem::path {
  return std::filesystem::path{LINUX_LAUNCHER_TEST_FIXTURE};
}

[[nodiscard]] auto none_configuration() -> LinuxProcessLauncherConfiguration {
  LinuxProcessLauncherConfiguration configuration;
  configuration.restriction = RestrictionLevel::none;
  return configuration;
}

[[nodiscard]] auto establish_none() -> LinuxProcessLauncherEstablishment {
  auto established =
      aiforge::adapters::establish_linux_process_launcher(none_configuration());
  REQUIRE(established);
  REQUIRE(std::holds_alternative<BoundProcessLauncher>(*established));
  return std::move(*established);
}

[[nodiscard]] auto request(BoundProcessLauncher& launcher,
                           const std::filesystem::path& working_directory,
                           std::vector<std::string> arguments,
                           const std::chrono::milliseconds timeout = 1s,
                           const std::uint64_t output_bytes = 64U * 1024U,
                           const std::uint64_t progress_bytes = 1024U)
    -> ProcessLaunchRequest {
  auto executable_identity =
      launcher.pin_path(fixture().generic_string(),
                        ProcessFilesystemTargetKind::regular_executable);
  REQUIRE(executable_identity);
  auto working_identity =
      launcher.pin_path(working_directory.generic_string(),
                        ProcessFilesystemTargetKind::directory);
  REQUIRE(working_identity);
  auto invocation = InvocationId::from("linux-launcher-test");
  REQUIRE(invocation);
  return {std::move(*invocation),
          fixture().generic_string(),
          std::move(*executable_identity),
          std::move(arguments),
          working_directory.generic_string(),
          *working_identity,
          {{working_directory.generic_string(), *working_identity,
            ProcessFilesystemAccess::read_write}},
          {{working_directory.generic_string(), std::move(*working_identity),
            ProcessFilesystemAccess::read_write}},
          {},
          {timeout, output_bytes, progress_bytes, 50ms}};
}

struct CollectedExecution {
  std::vector<std::byte> standard_output;
  std::vector<std::byte> standard_error;
  ProcessLaunchTerminal terminal;
};

[[nodiscard]] auto collect(BoundProcessLauncher& launcher,
                           ProcessLaunchRequest request)
    -> std::expected<CollectedExecution, aiforge::runtime::ProcessLaunchError> {
  auto stream = launcher.launch(request);
  if (!stream) return std::unexpected(std::move(stream.error()));
  CollectedExecution result;
  bool terminal_seen{};
  for (;;) {
    auto next = (*stream)->next();
    if (!next) return std::unexpected(std::move(next.error()));
    if (!*next) break;
    if (const auto* progress =
            std::get_if<aiforge::runtime::ProcessLaunchProgress>(&**next)) {
      auto& output = progress->stream == ProcessOutputStream::standard_output
                         ? result.standard_output
                         : result.standard_error;
      output.insert(output.end(), progress->content.begin(),
                    progress->content.end());
    } else {
      result.terminal = std::move(std::get<ProcessLaunchTerminal>(**next));
      terminal_seen = true;
    }
  }
  if (!terminal_seen) {
    return std::unexpected(aiforge::runtime::ProcessLaunchError{
        aiforge::runtime::ProcessLaunchErrorCode::protocol_failure,
        aiforge::runtime::ProcessLaunchStage::execution, std::nullopt,
        "test stream ended without a terminal", false});
  }
  return result;
}

[[nodiscard]] auto text(const std::span<const std::byte> bytes) -> std::string {
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

} // namespace

TEST_CASE("restricted Linux establishment is immutable unavailability",
          "[linux-launcher][establishment][failure]") {
  for (const auto restriction :
       {RestrictionLevel::low, RestrictionLevel::medium,
        RestrictionLevel::high}) {
    LinuxProcessLauncherConfiguration configuration;
    configuration.restriction = restriction;
    auto established =
        aiforge::adapters::establish_linux_process_launcher(configuration);
    REQUIRE(established);
    const auto* unavailable =
        std::get_if<LinuxProcessLauncherUnavailable>(&*established);
    REQUIRE(unavailable != nullptr);
    CHECK(unavailable->context.selected_restriction() == restriction);
    CHECK_FALSE(unavailable->context.achieved_restriction());
    CHECK(unavailable->context.unavailable_reason() ==
          aiforge::runtime::RestrictionUnavailableReason::mechanism_absent);
    CHECK_FALSE(unavailable->context.restriction_policy_identity());
    CHECK(aiforge::adapters::linux_process_unavailable_conjunct_name(
              unavailable->conjunct) ==
          "same_uid_broker_execution_confinement");
  }

  LinuxProcessLauncherConfiguration automatic;
  automatic.restriction = RestrictionLevel::low;
  automatic.approval_mode = aiforge::runtime::ApprovalMode::automatic;
  automatic.matcher_policy_identity = "test.matcher.v1";
  auto established =
      aiforge::adapters::establish_linux_process_launcher(automatic);
  REQUIRE(established);
  const auto& unavailable =
      std::get<LinuxProcessLauncherUnavailable>(*established);
  CHECK(unavailable.context.matcher_policy_identity() == "test.matcher.v1");

  automatic.matcher_policy_identity.reset();
  auto invalid = aiforge::adapters::establish_linux_process_launcher(automatic);
  REQUIRE_FALSE(invalid);
  CHECK(invalid.error().code ==
        aiforge::adapters::LinuxProcessLauncherEstablishmentErrorCode::
            invalid_configuration);

  LinuxProcessLauncherConfiguration invalid_bounds;
  invalid_bounds.restriction = RestrictionLevel::high;
  invalid_bounds.bounds.maximum_arguments = 0;
  auto stable =
      aiforge::adapters::establish_linux_process_launcher(invalid_bounds);
  REQUIRE(stable);
  CHECK(std::holds_alternative<LinuxProcessLauncherUnavailable>(*stable));
}

TEST_CASE("Linux none path pinning honors tighter adapter bounds without state",
          "[linux-launcher][validation][bounds][failure]") {
  auto path_configuration = none_configuration();
  path_configuration.bounds.maximum_path_bytes = 1;
  auto path_established =
      aiforge::adapters::establish_linux_process_launcher(path_configuration);
  REQUIRE(path_established);
  auto& path_launcher = std::get<BoundProcessLauncher>(*path_established);
  CHECK(path_launcher.contract_snapshot().restriction_policy_identity ==
        "aiforge.posix-process.none.v1");
  CHECK(path_launcher.context().restriction_policy_identity() ==
        "aiforge.posix-process.none.v1");
  auto overbound = path_launcher.pin_path(
      "/overbound", ProcessFilesystemTargetKind::directory);
  REQUIRE_FALSE(overbound);
  CHECK(overbound.error().code ==
        aiforge::runtime::ProcessLaunchErrorCode::invalid_request);
  auto subsequent =
      path_launcher.pin_path("/", ProcessFilesystemTargetKind::directory);
  REQUIRE(subsequent);

  auto identity_configuration = none_configuration();
  identity_configuration.bounds.maximum_identity_bytes = 1;
  auto identity_established =
      aiforge::adapters::establish_linux_process_launcher(
          identity_configuration);
  REQUIRE(identity_established);
  auto& identity_launcher =
      std::get<BoundProcessLauncher>(*identity_established);
  auto identity =
      identity_launcher.pin_path("/", ProcessFilesystemTargetKind::directory);
  REQUIRE_FALSE(identity);
  CHECK(identity.error().code ==
        aiforge::runtime::ProcessLaunchErrorCode::invalid_request);
}

TEST_CASE("Linux none launcher rejects ambiguous and drifted path authority",
          "[linux-launcher][validation][failure]") {
  TemporaryDirectory temporary;
  auto established = establish_none();
  auto& launcher = std::get<BoundProcessLauncher>(established);

  REQUIRE_FALSE(
      launcher.pin_path("relative", ProcessFilesystemTargetKind::directory));
  const auto symlink = temporary.path() / "link";
  std::filesystem::create_directory_symlink(temporary.path(), symlink);
  REQUIRE_FALSE(launcher.pin_path(symlink.generic_string(),
                                  ProcessFilesystemTargetKind::directory));

  auto drifted = request(launcher, temporary.path(), {"inspect"});
  const auto original = temporary.path().string() + "-original";
  std::filesystem::rename(temporary.path(), original);
  std::filesystem::create_directory(temporary.path());
  auto launched = launcher.launch(std::move(drifted));
  REQUIRE_FALSE(launched);
  CHECK(launched.error().code ==
        aiforge::runtime::ProcessLaunchErrorCode::contract_drift);
  std::filesystem::remove(temporary.path());
  std::filesystem::rename(original, temporary.path());

  const auto configured_root = temporary.path() / "configured";
  std::filesystem::create_directory(configured_root);
  auto configured_identity = launcher.pin_path(
      configured_root.generic_string(), ProcessFilesystemTargetKind::directory);
  REQUIRE(configured_identity);
  auto configured_drift = request(launcher, temporary.path(), {"inspect"});
  configured_drift.configured_roots.push_back(
      {configured_root.generic_string(), std::move(*configured_identity),
       ProcessFilesystemAccess::read_only});
  const auto configured_original = configured_root.string() + "-original";
  std::filesystem::rename(configured_root, configured_original);
  std::filesystem::create_directory(configured_root);
  auto configured_launch = launcher.launch(std::move(configured_drift));
  REQUIRE_FALSE(configured_launch);
  CHECK(configured_launch.error().code ==
        aiforge::runtime::ProcessLaunchErrorCode::contract_drift);

  const auto requested_root = temporary.path() / "requested";
  std::filesystem::create_directory(requested_root);
  auto requested_identity = launcher.pin_path(
      requested_root.generic_string(), ProcessFilesystemTargetKind::directory);
  REQUIRE(requested_identity);
  auto requested_drift = request(launcher, temporary.path(), {"inspect"});
  requested_drift.requested_roots.push_back(
      {requested_root.generic_string(), std::move(*requested_identity),
       ProcessFilesystemAccess::read_only});
  const auto requested_original = requested_root.string() + "-original";
  std::filesystem::rename(requested_root, requested_original);
  std::filesystem::create_directory(requested_root);
  auto requested_launch = launcher.launch(std::move(requested_drift));
  REQUIRE_FALSE(requested_launch);
  CHECK(requested_launch.error().code ==
        aiforge::runtime::ProcessLaunchErrorCode::contract_drift);
}

TEST_CASE("Linux none launcher resolves descendants under retained roots",
          "[linux-launcher][validation][descendant]") {
  TemporaryDirectory temporary;
  const auto base = temporary.path() / "base";
  const auto requested = base / "requested";
  const auto working = requested / "work";
  std::filesystem::create_directories(working);

  auto established = establish_none();
  auto& launcher = std::get<BoundProcessLauncher>(established);
  auto base_identity = launcher.pin_path(
      base.generic_string(), ProcessFilesystemTargetKind::directory);
  auto requested_identity = launcher.pin_path(
      requested.generic_string(), ProcessFilesystemTargetKind::directory);
  REQUIRE(base_identity);
  REQUIRE(requested_identity);

  auto nested = request(launcher, working, {"inspect"});
  nested.configured_roots = {{base.generic_string(), *base_identity,
                              ProcessFilesystemAccess::read_write}};
  nested.requested_roots = {{requested.generic_string(), *requested_identity,
                             ProcessFilesystemAccess::read_write}};
  auto nested_execution = collect(launcher, std::move(nested));
  REQUIRE(nested_execution);
  CHECK(nested_execution->terminal.kind == ProcessTerminalKind::exited);
  CHECK(text(nested_execution->standard_output)
            .contains("cwd=" + working.generic_string() + "\n"));

  const auto target = base / "target";
  const auto link = base / "link";
  std::filesystem::create_directory(target);
  std::filesystem::create_directory_symlink(target, link);
  auto target_identity = launcher.pin_path(
      target.generic_string(), ProcessFilesystemTargetKind::directory);
  REQUIRE(target_identity);
  auto symlinked = request(launcher, working, {"inspect"});
  symlinked.working_directory = link.generic_string();
  symlinked.working_directory_identity = *target_identity;
  symlinked.configured_roots = {{base.generic_string(), *base_identity,
                                 ProcessFilesystemAccess::read_write}};
  symlinked.requested_roots = {{link.generic_string(), *target_identity,
                                ProcessFilesystemAccess::read_write}};
  auto symlinked_launch = launcher.launch(std::move(symlinked));
  REQUIRE_FALSE(symlinked_launch);
  CHECK(symlinked_launch.error().code ==
        aiforge::runtime::ProcessLaunchErrorCode::contract_drift);
}

TEST_CASE("Linux none launcher preserves literal argv and sanitized state",
          "[linux-launcher][execution]") {
  TemporaryDirectory temporary;
  auto established = establish_none();
  auto& launcher = std::get<BoundProcessLauncher>(established);
  REQUIRE(::setenv("UNLISTED_VALUE", "ambient-secret", 1) == 0);
  auto launch_request = request(
      launcher, temporary.path(),
      {"inspect", "literal;still-one", "$(not-a-shell)", "line\nbreak"});
  launch_request.environment.push_back({"SAFE_VALUE", "safe-value"});
  auto execution = collect(launcher, std::move(launch_request));
  REQUIRE(::unsetenv("UNLISTED_VALUE") == 0);
  REQUIRE(execution);
  CHECK(execution->terminal.kind == ProcessTerminalKind::exited);
  CHECK(execution->terminal.exit_code == 7);
  const auto output = text(execution->standard_output);
  CHECK(output.find("literal;still-one") != std::string::npos);
  CHECK(output.find("$(not-a-shell)") != std::string::npos);
  CHECK(output.find("line\nbreak") != std::string::npos);
  CHECK(output.find("safe=safe-value") != std::string::npos);
  CHECK(output.find("unlisted=<unset>") != std::string::npos);
  CHECK(output.find("stdin=eof") != std::string::npos);
  CHECK(text(execution->standard_error) == "stderr=separate\n");
  CHECK(execution->terminal.standard_output == execution->standard_output);
  CHECK(execution->terminal.standard_error == execution->standard_error);

  std::array<int, 2> descriptors{-1, -1};
  REQUIRE(::pipe(descriptors.data()) == 0);
  const auto ambient = ::fcntl(descriptors[0], F_DUPFD, 32);
  REQUIRE(ambient >= 32);
  static_cast<void>(::close(descriptors[0]));
  static_cast<void>(::close(descriptors[1]));
  auto descriptor_execution =
      collect(launcher, request(launcher, temporary.path(),
                                {"descriptor", std::to_string(ambient)}));
  static_cast<void>(::close(ambient));
  REQUIRE(descriptor_execution);
  CHECK(text(descriptor_execution->standard_output) == "descriptor=closed\n");
}

TEST_CASE("Linux none launcher keeps concurrent executions independent",
          "[linux-launcher][execution][concurrency]") {
  TemporaryDirectory temporary;
  auto established = establish_none();
  auto& launcher = std::get<BoundProcessLauncher>(established);

  constexpr std::size_t launch_count{4};
  std::vector<std::future<
      std::expected<CollectedExecution, aiforge::runtime::ProcessLaunchError>>>
      executions;
  executions.reserve(launch_count);
  for (std::size_t index{}; index < launch_count; ++index) {
    const auto marker = "concurrent-" + std::to_string(index);
    auto launch_request =
        request(launcher, temporary.path(), {"inspect", marker});
    launch_request.environment.push_back({"SAFE_VALUE", marker});
    executions.push_back(std::async(
        std::launch::async,
        [&launcher, launch_request = std::move(launch_request)]() mutable {
          return collect(launcher, std::move(launch_request));
        }));
  }

  for (std::size_t index{}; index < launch_count; ++index) {
    auto execution = executions[index].get();
    REQUIRE(execution);
    CHECK(execution->terminal.kind == ProcessTerminalKind::exited);
    const auto output = text(execution->standard_output);
    const auto marker = "concurrent-" + std::to_string(index);
    CHECK(output.contains("arg1=" + marker + "\n"));
    CHECK(output.contains("safe=" + marker + "\n"));
    for (std::size_t other{}; other < launch_count; ++other) {
      if (other == index) continue;
      CHECK_FALSE(output.contains("concurrent-" + std::to_string(other)));
    }
  }
}

TEST_CASE("Linux none launcher bounds output timeout and cancellation",
          "[linux-launcher][limits][failure]") {
  TemporaryDirectory temporary;
  auto established = establish_none();
  auto& launcher = std::get<BoundProcessLauncher>(established);

  auto overbound = collect(launcher, request(launcher, temporary.path(),
                                             {"emit", "100", "x"}, 1s, 16, 8));
  REQUIRE(overbound);
  CHECK(overbound->terminal.kind == ProcessTerminalKind::output_limit);
  CHECK(overbound->standard_output.size() == 16);

  auto timed_out =
      collect(launcher, request(launcher, temporary.path(), {"hang"}, 25ms));
  REQUIRE(timed_out);
  CHECK(timed_out->terminal.kind == ProcessTerminalKind::timed_out);
}

TEST_CASE("Linux none launcher bounds cleanup of an infinite writer",
          "[linux-launcher][lifecycle][failure]") {
  TemporaryDirectory temporary;
  auto established = establish_none();
  auto& launcher = std::get<BoundProcessLauncher>(established);

  const auto launch_writer = [&]() {
    auto stream =
        launcher.launch(request(launcher, temporary.path(),
                                {"ignore-term-flood"}, 5s, 8U * 1024U * 1024U));
    REQUIRE(stream);
    auto ready = (*stream)->next();
    REQUIRE(ready);
    REQUIRE(*ready);
    const auto* progress =
        std::get_if<aiforge::runtime::ProcessLaunchProgress>(&**ready);
    REQUIRE(progress != nullptr);
    REQUIRE(text(progress->content) == "ready\n");
    std::this_thread::sleep_for(50ms);
    return stream;
  };

  auto cancelled_stream = launch_writer();
  std::stop_source cancellation;
  cancellation.request_stop();
  const auto cancel_started = std::chrono::steady_clock::now();
  auto cancelled = (*cancelled_stream)->next(cancellation.get_token());
  const auto cancel_duration =
      std::chrono::steady_clock::now() - cancel_started;
  REQUIRE_FALSE(cancelled);
  CHECK(cancelled.error().code ==
        aiforge::runtime::ProcessLaunchErrorCode::cancelled);
  CHECK(cancel_duration < 1s);

  auto abandoned_stream = launch_writer();
  const auto destruction_started = std::chrono::steady_clock::now();
  abandoned_stream->reset();
  const auto destruction_duration =
      std::chrono::steady_clock::now() - destruction_started;
  CHECK(destruction_duration < 1s);
}

TEST_CASE("Linux none launcher reports signals and spawn format failures",
          "[linux-launcher][result][failure]") {
  TemporaryDirectory temporary;
  auto established = establish_none();
  auto& launcher = std::get<BoundProcessLauncher>(established);

  auto signaled =
      collect(launcher, request(launcher, temporary.path(), {"signal"}));
  REQUIRE(signaled);
  CHECK(signaled->terminal.kind == ProcessTerminalKind::signaled);
  CHECK(signaled->terminal.signal == SIGTERM);

  const auto malformed = temporary.path() / "malformed";
  {
    std::ofstream output{malformed};
    output << "not an executable format\n";
  }
  std::filesystem::permissions(malformed,
                               std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_exec);
  auto executable_identity =
      launcher.pin_path(malformed.generic_string(),
                        ProcessFilesystemTargetKind::regular_executable);
  REQUIRE(executable_identity);
  auto failed_request = request(launcher, temporary.path(), {});
  failed_request.executable = malformed.generic_string();
  failed_request.executable_identity = std::move(*executable_identity);
  auto failed = collect(launcher, std::move(failed_request));
  REQUIRE(failed);
  CHECK(failed->terminal.kind == ProcessTerminalKind::spawn_failed);
  CHECK(failed->terminal.spawn_error ==
        aiforge::runtime::ProcessSpawnError::invalid_format);
}
