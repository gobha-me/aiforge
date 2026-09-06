#pragma once

#include <cstddef>
#include <deque>
#include <mutex>
#include <variant>
#include <vector>

#include <aiforge/runtime/process_launcher.hpp>

namespace aiforge::testing {

struct ProcessLaunchEndOfStream {
  auto operator==(const ProcessLaunchEndOfStream&) const -> bool = default;
};

using ScriptedProcessLaunchStep =
    std::variant<runtime::ProcessLaunchEvent, runtime::ProcessLaunchError,
                 ProcessLaunchEndOfStream>;

struct ScriptedProcessLaunchStream {
  std::vector<ScriptedProcessLaunchStep> steps;
  auto operator==(const ScriptedProcessLaunchStream&) const -> bool = default;
};

using ScriptedProcessLaunchOutcome =
    std::variant<ScriptedProcessLaunchStream, runtime::ProcessLaunchError>;

struct ScriptedProcessLaunchExchange {
  // Values are erased on construction and never matched or recorded. Only
  // environment names are inspectable in this deterministic fake.
  runtime::ProcessLaunchRequest expected_request;
  ScriptedProcessLaunchOutcome outcome;
  auto operator==(const ScriptedProcessLaunchExchange&) const -> bool = default;
};

using ScriptedProcessPathPinOutcome =
    std::variant<std::string, runtime::ProcessLaunchError>;

struct ScriptedProcessPathPinExchange {
  std::string expected_path;
  runtime::ProcessFilesystemTargetKind expected_kind{
      runtime::ProcessFilesystemTargetKind::directory};
  ScriptedProcessPathPinOutcome outcome;
  auto operator==(const ScriptedProcessPathPinExchange&) const
      -> bool = default;
};

struct RecordedProcessPathPin {
  std::string path;
  runtime::ProcessFilesystemTargetKind kind{
      runtime::ProcessFilesystemTargetKind::directory};
  auto operator==(const RecordedProcessPathPin&) const -> bool = default;
};

class ScriptedProcessLauncher final : public runtime::ProcessLauncher {
 public:
  ScriptedProcessLauncher(
      runtime::ProcessLauncherContract contract,
      std::vector<ScriptedProcessLaunchExchange> exchanges,
      runtime::ProcessLaunchBounds bounds = {},
      std::vector<ScriptedProcessPathPinExchange> path_pins = {});

  [[nodiscard]] auto recorded_requests() const
      -> std::vector<runtime::ProcessLaunchRequest>;
  [[nodiscard]] auto recorded_path_pins() const
      -> std::vector<RecordedProcessPathPin>;
  [[nodiscard]] auto remaining_exchanges() const -> std::size_t;
  [[nodiscard]] auto remaining_path_pins() const -> std::size_t;

 private:
  [[nodiscard]] auto do_pin_path(
      std::string path, runtime::ProcessFilesystemTargetKind kind) noexcept
      -> std::expected<std::string, runtime::ProcessLaunchError> override;
  [[nodiscard]] auto do_launch(runtime::ProcessLaunchRequest request,
                               std::stop_token stop_token) noexcept
      -> std::expected<std::unique_ptr<runtime::ProcessLaunchStream>,
                       runtime::ProcessLaunchError> override;

  mutable std::mutex m_mutex;
  std::deque<ScriptedProcessLaunchExchange> m_exchanges;
  runtime::ProcessLaunchBounds m_bounds;
  std::vector<runtime::ProcessLaunchRequest> m_recorded_requests;
  std::deque<ScriptedProcessPathPinExchange> m_path_pins;
  std::vector<RecordedProcessPathPin> m_recorded_path_pins;
};

} // namespace aiforge::testing
