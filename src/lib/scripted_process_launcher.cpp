#include <aiforge/testing/scripted_process_launcher.hpp>

#include <algorithm>
#include <iterator>
#include <optional>
#include <utility>

namespace aiforge::testing {
namespace {

[[nodiscard]] auto error(const runtime::ProcessLaunchErrorCode code,
                         const runtime::ProcessLaunchStage stage,
                         std::string message)
    -> std::unexpected<runtime::ProcessLaunchError> {
  return std::unexpected(runtime::ProcessLaunchError{
      code, stage, std::nullopt, std::move(message), false});
}

auto erase_secret(std::string& value) noexcept -> void {
  auto* bytes = reinterpret_cast<volatile char*>(value.data());
  for (std::size_t index = 0; index < value.size(); ++index)
    bytes[index] = 0;
  value.clear();
}

auto erase_environment_values(runtime::ProcessLaunchRequest& request) noexcept
    -> void {
  for (auto& variable : request.environment)
    erase_secret(variable.value);
}

class ScriptedProcessStream final : public runtime::ProcessLaunchStream {
 public:
  explicit ScriptedProcessStream(ScriptedProcessLaunchStream script)
      : m_steps(std::make_move_iterator(script.steps.begin()),
                std::make_move_iterator(script.steps.end())) {}

  auto next(const std::stop_token stop_token) noexcept
      -> std::expected<std::optional<runtime::ProcessLaunchEvent>,
                       runtime::ProcessLaunchError> override {
    try {
      std::scoped_lock lock{m_mutex};
      if (m_ended) return std::optional<runtime::ProcessLaunchEvent>{};
      if (stop_token.stop_requested()) {
        m_ended = true;
        return error(runtime::ProcessLaunchErrorCode::cancelled,
                     runtime::ProcessLaunchStage::termination,
                     "scripted process launch cancelled");
      }
      if (m_steps.empty()) {
        m_ended = true;
        return error(runtime::ProcessLaunchErrorCode::protocol_failure,
                     runtime::ProcessLaunchStage::execution,
                     "scripted process stream exhausted without an end step");
      }
      auto step = std::move(m_steps.front());
      m_steps.pop_front();
      if (auto* event = std::get_if<runtime::ProcessLaunchEvent>(&step)) {
        return std::optional<runtime::ProcessLaunchEvent>{std::move(*event)};
      }
      m_ended = true;
      if (auto* failure = std::get_if<runtime::ProcessLaunchError>(&step)) {
        return std::unexpected(std::move(*failure));
      }
      return std::optional<runtime::ProcessLaunchEvent>{};
    } catch (...) {
      m_ended = true;
      return error(runtime::ProcessLaunchErrorCode::internal_failure,
                   runtime::ProcessLaunchStage::execution,
                   "scripted process stream failed internally");
    }
  }

 private:
  std::deque<ScriptedProcessLaunchStep> m_steps;
  std::mutex m_mutex;
  bool m_ended{};
};

} // namespace

ScriptedProcessLauncher::ScriptedProcessLauncher(
    runtime::ProcessLauncherContract contract,
    std::vector<ScriptedProcessLaunchExchange> exchanges,
    const runtime::ProcessLaunchBounds bounds,
    std::vector<ScriptedProcessPathPinExchange> path_pins)
    : ProcessLauncher(std::move(contract)),
      m_exchanges(std::make_move_iterator(exchanges.begin()),
                  std::make_move_iterator(exchanges.end())),
      m_bounds(bounds), m_path_pins(std::make_move_iterator(path_pins.begin()),
                                    std::make_move_iterator(path_pins.end())) {
  for (auto& exchange : m_exchanges)
    erase_environment_values(exchange.expected_request);
}

auto ScriptedProcessLauncher::do_pin_path(
    std::string path, const runtime::ProcessFilesystemTargetKind kind) noexcept
    -> std::expected<std::string, runtime::ProcessLaunchError> {
  try {
    std::scoped_lock lock{m_mutex};
    m_recorded_path_pins.push_back({path, kind});
    if (m_path_pins.empty()) {
      return error(runtime::ProcessLaunchErrorCode::unavailable,
                   runtime::ProcessLaunchStage::validation,
                   "scripted process launcher has no path pin remaining");
    }
    auto& exchange = m_path_pins.front();
    if (exchange.expected_path != path || exchange.expected_kind != kind) {
      return error(runtime::ProcessLaunchErrorCode::protocol_failure,
                   runtime::ProcessLaunchStage::validation,
                   "process path pin did not match the script");
    }
    auto outcome = std::move(exchange.outcome);
    m_path_pins.pop_front();
    if (auto* failure = std::get_if<runtime::ProcessLaunchError>(&outcome)) {
      return std::unexpected(std::move(*failure));
    }
    return std::move(std::get<std::string>(outcome));
  } catch (...) {
    return error(runtime::ProcessLaunchErrorCode::internal_failure,
                 runtime::ProcessLaunchStage::validation,
                 "scripted process path pinning failed internally");
  }
}

auto ScriptedProcessLauncher::do_launch(
    runtime::ProcessLaunchRequest request,
    const std::stop_token stop_token) noexcept
    -> std::expected<std::unique_ptr<runtime::ProcessLaunchStream>,
                     runtime::ProcessLaunchError> {
  try {
    if (stop_token.stop_requested()) {
      erase_environment_values(request);
      return error(runtime::ProcessLaunchErrorCode::cancelled,
                   runtime::ProcessLaunchStage::validation,
                   "scripted process launch cancelled");
    }
    if (auto valid =
            runtime::validate_process_launch_request(request, m_bounds);
        !valid) {
      erase_environment_values(request);
      return std::unexpected(std::move(valid.error()));
    }
    erase_environment_values(request);
    std::scoped_lock lock{m_mutex};
    m_recorded_requests.push_back(request);
    if (m_exchanges.empty()) {
      return error(runtime::ProcessLaunchErrorCode::unavailable,
                   runtime::ProcessLaunchStage::validation,
                   "scripted process launcher has no exchange remaining");
    }
    auto& exchange = m_exchanges.front();
    if (request != exchange.expected_request) {
      return error(runtime::ProcessLaunchErrorCode::protocol_failure,
                   runtime::ProcessLaunchStage::validation,
                   "process launch request did not match the script");
    }
    auto outcome = std::move(exchange.outcome);
    m_exchanges.pop_front();
    if (auto* failure = std::get_if<runtime::ProcessLaunchError>(&outcome)) {
      return std::unexpected(std::move(*failure));
    }
    return std::make_unique<ScriptedProcessStream>(
        std::move(std::get<ScriptedProcessLaunchStream>(outcome)));
  } catch (...) {
    erase_environment_values(request);
    return error(runtime::ProcessLaunchErrorCode::internal_failure,
                 runtime::ProcessLaunchStage::validation,
                 "scripted process launcher failed internally");
  }
}

auto ScriptedProcessLauncher::recorded_requests() const
    -> std::vector<runtime::ProcessLaunchRequest> {
  std::scoped_lock lock{m_mutex};
  return m_recorded_requests;
}

auto ScriptedProcessLauncher::recorded_path_pins() const
    -> std::vector<RecordedProcessPathPin> {
  std::scoped_lock lock{m_mutex};
  return m_recorded_path_pins;
}

auto ScriptedProcessLauncher::remaining_exchanges() const -> std::size_t {
  std::scoped_lock lock{m_mutex};
  return m_exchanges.size();
}

auto ScriptedProcessLauncher::remaining_path_pins() const -> std::size_t {
  std::scoped_lock lock{m_mutex};
  return m_path_pins.size();
}

} // namespace aiforge::testing
