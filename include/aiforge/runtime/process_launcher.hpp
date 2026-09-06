#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <aiforge/domain/ids.hpp>
#include <aiforge/runtime/application_launch_context.hpp>

namespace aiforge::runtime {

enum class ProcessFilesystemAccess {
  read_only,
  read_write,
};

enum class ProcessFilesystemTargetKind {
  regular_executable,
  directory,
};

struct ProcessFilesystemRoot {
  std::string path;
  std::string identity;
  ProcessFilesystemAccess access{ProcessFilesystemAccess::read_only};
  auto operator==(const ProcessFilesystemRoot&) const -> bool = default;
};

struct ProcessEnvironmentVariable {
  std::string name;
  std::string value;
  auto operator==(const ProcessEnvironmentVariable&) const -> bool = default;
};

struct ProcessLaunchLimits {
  std::chrono::milliseconds wall_time{};
  std::uint64_t maximum_output_bytes{};
  std::uint64_t maximum_progress_chunk_bytes{};
  std::chrono::milliseconds termination_grace{};
  auto operator==(const ProcessLaunchLimits&) const -> bool = default;
};

struct ProcessLaunchRequest {
  domain::InvocationId invocation_id;
  std::string executable;
  std::string executable_identity;
  std::vector<std::string> arguments;
  std::string working_directory;
  std::string working_directory_identity;
  // Registration-time ceilings and the invocation-selected subset remain
  // separate so adapters can prove their descriptor lineage at launch.
  std::vector<ProcessFilesystemRoot> configured_roots;
  std::vector<ProcessFilesystemRoot> requested_roots;
  std::vector<ProcessEnvironmentVariable> environment;
  ProcessLaunchLimits limits;
  auto operator==(const ProcessLaunchRequest&) const -> bool = default;
};

struct ProcessLaunchBounds {
  std::size_t maximum_arguments{256};
  std::size_t maximum_argument_bytes{std::size_t{256} * 1024U};
  std::size_t maximum_roots{128};
  std::size_t maximum_environment_variables{64};
  std::size_t maximum_path_bytes{4096};
  std::size_t maximum_identity_bytes{128};
  std::chrono::milliseconds maximum_wall_time{std::chrono::seconds{120}};
  std::uint64_t maximum_output_bytes{std::uint64_t{8} * 1024U * 1024U};
  std::uint64_t maximum_progress_chunk_bytes{4096};
  std::chrono::milliseconds maximum_termination_grace{
      std::chrono::milliseconds{100}};
  auto operator==(const ProcessLaunchBounds&) const -> bool = default;
};

enum class ProcessOutputStream {
  standard_output,
  standard_error,
};

struct ProcessLaunchProgress {
  ProcessOutputStream stream{ProcessOutputStream::standard_output};
  std::vector<std::byte> content;
  auto operator==(const ProcessLaunchProgress&) const -> bool = default;
};

enum class ProcessSpawnError {
  not_found,
  permission_denied,
  invalid_format,
  operating_system_error,
};

enum class ProcessTerminalKind {
  exited,
  signaled,
  spawn_failed,
  timed_out,
  output_limit,
};

struct ProcessLaunchTerminal {
  ProcessTerminalKind kind{ProcessTerminalKind::spawn_failed};
  std::optional<std::int32_t> exit_code;
  std::optional<std::int32_t> signal;
  std::optional<ProcessSpawnError> spawn_error;
  std::chrono::milliseconds duration{};
  std::vector<std::byte> standard_output;
  std::vector<std::byte> standard_error;
  auto operator==(const ProcessLaunchTerminal&) const -> bool = default;
};

using ProcessLaunchEvent =
    std::variant<ProcessLaunchProgress, ProcessLaunchTerminal>;

enum class ProcessLaunchStage {
  validation,
  spawn,
  execution,
  termination,
  cleanup,
};

enum class ProcessLaunchErrorCode {
  invalid_request,
  unavailable,
  contract_drift,
  cancelled,
  timed_out,
  output_limit,
  spawn_failed,
  cleanup_failed,
  protocol_failure,
  internal_failure,
};

struct ProcessLaunchError {
  ProcessLaunchErrorCode code{ProcessLaunchErrorCode::internal_failure};
  ProcessLaunchStage stage{ProcessLaunchStage::validation};
  std::optional<RestrictionUnavailableReason> unavailable_reason;
  std::string message;
  bool retryable{};
  auto operator==(const ProcessLaunchError&) const -> bool = default;
};

class ProcessLaunchStream {
 public:
  virtual ~ProcessLaunchStream() = default;

  [[nodiscard]] virtual auto next(std::stop_token stop_token = {}) noexcept
      -> std::expected<std::optional<ProcessLaunchEvent>,
                       ProcessLaunchError> = 0;
};

struct ProcessLauncherContract {
  RestrictionLevel restriction{RestrictionLevel::none};
  LaunchMechanismContract mechanism;
  std::string restriction_policy_identity;
  auto operator==(const ProcessLauncherContract&) const -> bool = default;
};

class ProcessLauncher {
 public:
  virtual ~ProcessLauncher() = default;

  ProcessLauncher(const ProcessLauncher&) = delete;
  auto operator=(const ProcessLauncher&) -> ProcessLauncher& = delete;
  ProcessLauncher(ProcessLauncher&&) = delete;
  auto operator=(ProcessLauncher&&) -> ProcessLauncher& = delete;

  [[nodiscard]] auto contract() const noexcept
      -> const ProcessLauncherContract& {
    return m_contract;
  }

  [[nodiscard]] auto pin_path(const ProcessLauncherContract& expected_contract,
                              std::string path,
                              ProcessFilesystemTargetKind kind) noexcept
      -> std::expected<std::string, ProcessLaunchError>;

  [[nodiscard]] auto launch(const ProcessLauncherContract& expected_contract,
                            ProcessLaunchRequest request,
                            std::stop_token stop_token = {}) noexcept
      -> std::expected<std::unique_ptr<ProcessLaunchStream>,
                       ProcessLaunchError>;

 protected:
  explicit ProcessLauncher(ProcessLauncherContract contract)
      : m_contract(std::move(contract)) {}

 private:
  [[nodiscard]] virtual auto do_pin_path(
      std::string path, ProcessFilesystemTargetKind kind) noexcept
      -> std::expected<std::string, ProcessLaunchError> = 0;
  [[nodiscard]] virtual auto do_launch(ProcessLaunchRequest request,
                                       std::stop_token stop_token) noexcept
      -> std::expected<std::unique_ptr<ProcessLaunchStream>,
                       ProcessLaunchError> = 0;

  const ProcessLauncherContract m_contract;
};

enum class ProcessLauncherBindingErrorCode {
  missing_launcher,
  unavailable_context,
  unsupported_restriction,
  contract_mismatch,
  invalid_contract,
};

struct ProcessLauncherBindingError {
  ProcessLauncherBindingErrorCode code{
      ProcessLauncherBindingErrorCode::invalid_contract};
  std::string message;
  auto operator==(const ProcessLauncherBindingError&) const -> bool = default;
};

class BoundProcessLauncher final {
 public:
  BoundProcessLauncher(const BoundProcessLauncher&) = delete;
  auto operator=(const BoundProcessLauncher&) -> BoundProcessLauncher& = delete;
  BoundProcessLauncher(BoundProcessLauncher&&) noexcept = default;
  auto operator=(BoundProcessLauncher&&) noexcept
      -> BoundProcessLauncher& = default;

  [[nodiscard]] auto context() const noexcept
      -> const ApplicationLaunchContext& {
    return m_context;
  }
  [[nodiscard]] auto contract_snapshot() const noexcept
      -> const ProcessLauncherContract& {
    return m_contract;
  }
  [[nodiscard]] auto pin_path(std::string path,
                              ProcessFilesystemTargetKind kind) const noexcept
      -> std::expected<std::string, ProcessLaunchError>;
  [[nodiscard]] auto launch(ProcessLaunchRequest request,
                            std::stop_token stop_token = {}) const noexcept
      -> std::expected<std::unique_ptr<ProcessLaunchStream>,
                       ProcessLaunchError>;

 private:
  BoundProcessLauncher(ApplicationLaunchContext context,
                       ProcessLauncherContract contract,
                       std::shared_ptr<ProcessLauncher> launcher)
      : m_context(std::move(context)), m_contract(std::move(contract)),
        m_launcher(std::move(launcher)) {}

  ApplicationLaunchContext m_context;
  ProcessLauncherContract m_contract;
  std::shared_ptr<ProcessLauncher> m_launcher;

  friend auto bind_process_launcher(ApplicationLaunchContext context,
                                    std::shared_ptr<ProcessLauncher> launcher)
      -> std::expected<BoundProcessLauncher, ProcessLauncherBindingError>;
};

[[nodiscard]] auto validate_process_launch_bounds(ProcessLaunchBounds bounds)
    -> std::expected<void, ProcessLaunchError>;

[[nodiscard]] auto validate_process_launch_request(
    const ProcessLaunchRequest& request, ProcessLaunchBounds bounds = {})
    -> std::expected<void, ProcessLaunchError>;

[[nodiscard]] auto validate_process_launch_terminal(
    const ProcessLaunchRequest& request, const ProcessLaunchTerminal& terminal)
    -> std::expected<void, ProcessLaunchError>;

[[nodiscard]] auto validate_process_launch_progress(
    const ProcessLaunchRequest& request, const ProcessLaunchProgress& progress)
    -> std::expected<void, ProcessLaunchError>;

[[nodiscard]] auto bind_process_launcher(
    ApplicationLaunchContext context, std::shared_ptr<ProcessLauncher> launcher)
    -> std::expected<BoundProcessLauncher, ProcessLauncherBindingError>;

} // namespace aiforge::runtime
