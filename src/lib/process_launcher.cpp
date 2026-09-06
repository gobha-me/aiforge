#include <aiforge/runtime/process_launcher.hpp>

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <ranges>
#include <set>
#include <string_view>
#include <utility>

namespace aiforge::runtime {
namespace {

[[nodiscard]] auto launch_failure(const ProcessLaunchErrorCode code,
                                  const ProcessLaunchStage stage,
                                  std::string message)
    -> std::unexpected<ProcessLaunchError> {
  return std::unexpected(
      ProcessLaunchError{code, stage, std::nullopt, std::move(message), false});
}

[[nodiscard]] auto binding_failure(const ProcessLauncherBindingErrorCode code,
                                   std::string message)
    -> std::unexpected<ProcessLauncherBindingError> {
  return std::unexpected(ProcessLauncherBindingError{code, std::move(message)});
}

[[nodiscard]] auto has_control(const std::string_view value) -> bool {
  return std::ranges::any_of(value, [](const unsigned char character) {
    return character < 0x20U || character == 0x7FU;
  });
}

[[nodiscard]] auto bounded_text(const std::string_view value,
                                const std::size_t maximum) -> bool {
  return !value.empty() && value.size() <= maximum && !has_control(value) &&
         value.find('\0') == std::string_view::npos;
}

[[nodiscard]] auto valid_identity(const std::string_view value,
                                  const std::size_t maximum) -> bool {
  return bounded_text(value, maximum) &&
         std::ranges::all_of(value, [](const unsigned char character) {
           return (character >= 'a' && character <= 'z') ||
                  (character >= 'A' && character <= 'Z') ||
                  (character >= '0' && character <= '9') || character == '.' ||
                  character == '_' || character == '-' || character == ':';
         });
}

[[nodiscard]] auto normalized_absolute_path(const std::string_view value,
                                            const std::size_t maximum) -> bool {
  if (!bounded_text(value, maximum)) return false;
  const std::filesystem::path path{value};
  return path.is_absolute() && path.generic_string() == value &&
         path.lexically_normal().generic_string() == value;
}

[[nodiscard]] auto path_is_within(const std::string_view parent_text,
                                  const std::string_view child_text) -> bool {
  const std::filesystem::path parent{parent_text};
  const std::filesystem::path child{child_text};
  auto parent_part = parent.begin();
  auto child_part = child.begin();
  for (; parent_part != parent.end() && child_part != child.end();
       ++parent_part, ++child_part) {
    if (*parent_part != *child_part) return false;
  }
  return parent_part == parent.end();
}

[[nodiscard]] auto valid_environment_name(const std::string_view value)
    -> bool {
  if (value.empty() || value.size() > 255U || value.front() == '=' ||
      (value.front() >= '0' && value.front() <= '9')) {
    return false;
  }
  return std::ranges::all_of(value, [](const unsigned char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '_';
  });
}

[[nodiscard]] auto valid_access(const ProcessFilesystemAccess access) noexcept
    -> bool {
  switch (access) {
    case ProcessFilesystemAccess::read_only:
    case ProcessFilesystemAccess::read_write: return true;
  }
  return false;
}

[[nodiscard]] auto valid_target_kind(
    const ProcessFilesystemTargetKind kind) noexcept -> bool {
  switch (kind) {
    case ProcessFilesystemTargetKind::regular_executable:
    case ProcessFilesystemTargetKind::directory: return true;
  }
  return false;
}

[[nodiscard]] auto valid_terminal_kind(const ProcessTerminalKind kind) noexcept
    -> bool {
  switch (kind) {
    case ProcessTerminalKind::exited:
    case ProcessTerminalKind::signaled:
    case ProcessTerminalKind::spawn_failed:
    case ProcessTerminalKind::timed_out:
    case ProcessTerminalKind::output_limit: return true;
  }
  return false;
}

[[nodiscard]] auto valid_spawn_error(const ProcessSpawnError error) noexcept
    -> bool {
  switch (error) {
    case ProcessSpawnError::not_found:
    case ProcessSpawnError::permission_denied:
    case ProcessSpawnError::invalid_format:
    case ProcessSpawnError::operating_system_error: return true;
  }
  return false;
}

[[nodiscard]] auto add_bounded(std::uint64_t& total, const std::uint64_t amount,
                               const std::uint64_t maximum) -> bool {
  if (amount > maximum || total > maximum - amount) return false;
  total += amount;
  return true;
}

[[nodiscard]] auto valid_contract(const ProcessLauncherContract& contract)
    -> bool {
  return contract.restriction == RestrictionLevel::none &&
         valid_identity(contract.mechanism.identity, 128U) &&
         valid_identity(contract.mechanism.version, 128U) &&
         valid_identity(contract.restriction_policy_identity, 128U);
}

[[nodiscard]] auto validate_terminal_for_limits(
    const ProcessLaunchLimits& limits, const ProcessLaunchTerminal& terminal)
    -> std::expected<void, ProcessLaunchError> {
  const bool valid_exit = !terminal.exit_code || (*terminal.exit_code >= 0 &&
                                                  *terminal.exit_code <= 255);
  const bool valid_signal = !terminal.signal || *terminal.signal > 0;
  const bool valid_correlation =
      valid_terminal_kind(terminal.kind) && valid_exit && valid_signal &&
      (terminal.kind == ProcessTerminalKind::exited) ==
          terminal.exit_code.has_value() &&
      (terminal.kind == ProcessTerminalKind::signaled) ==
          terminal.signal.has_value() &&
      (terminal.kind == ProcessTerminalKind::spawn_failed) ==
          terminal.spawn_error.has_value() &&
      (!terminal.spawn_error || valid_spawn_error(*terminal.spawn_error));
  std::uint64_t output_bytes{};
  if (!valid_correlation || terminal.duration < std::chrono::milliseconds{} ||
      !add_bounded(output_bytes, terminal.standard_output.size(),
                   limits.maximum_output_bytes) ||
      !add_bounded(output_bytes, terminal.standard_error.size(),
                   limits.maximum_output_bytes)) {
    return launch_failure(ProcessLaunchErrorCode::protocol_failure,
                          ProcessLaunchStage::execution,
                          "process terminal is malformed or overbound");
  }
  return {};
}

[[nodiscard]] auto validate_progress_for_limits(
    const ProcessLaunchLimits& limits, const ProcessLaunchProgress& progress)
    -> std::expected<void, ProcessLaunchError> {
  const bool valid_stream =
      progress.stream == ProcessOutputStream::standard_output ||
      progress.stream == ProcessOutputStream::standard_error;
  if (!valid_stream || progress.content.empty() ||
      progress.content.size() > limits.maximum_progress_chunk_bytes) {
    return launch_failure(ProcessLaunchErrorCode::protocol_failure,
                          ProcessLaunchStage::execution,
                          "process progress is malformed or overbound");
  }
  return {};
}

class ValidatingProcessLaunchStream final : public ProcessLaunchStream {
 public:
  ValidatingProcessLaunchStream(std::unique_ptr<ProcessLaunchStream> stream,
                                ProcessLaunchLimits limits)
      : m_stream(std::move(stream)), m_limits(limits) {}

  auto next(const std::stop_token stop_token) noexcept
      -> std::expected<std::optional<ProcessLaunchEvent>,
                       ProcessLaunchError> override {
    try {
      std::scoped_lock lock{m_mutex};
      if (m_ended) return std::optional<ProcessLaunchEvent>{};
      auto next = m_stream->next(stop_token);
      if (!next) {
        m_ended = true;
        return std::unexpected(std::move(next.error()));
      }
      if (!*next) {
        m_ended = true;
        if (!m_terminal_seen) {
          return launch_failure(ProcessLaunchErrorCode::protocol_failure,
                                ProcessLaunchStage::execution,
                                "process stream ended without a terminal");
        }
        return std::optional<ProcessLaunchEvent>{};
      }
      if (const auto* progress = std::get_if<ProcessLaunchProgress>(&**next)) {
        if (m_terminal_seen) {
          m_ended = true;
          return launch_failure(
              ProcessLaunchErrorCode::protocol_failure,
              ProcessLaunchStage::execution,
              "process stream emitted progress after terminal");
        }
        if (auto valid = validate_progress_for_limits(m_limits, *progress);
            !valid) {
          m_ended = true;
          return std::unexpected(std::move(valid.error()));
        }
        if (progress->content.size() >
            m_limits.maximum_output_bytes -
                std::min<std::uint64_t>(m_output_bytes,
                                        m_limits.maximum_output_bytes)) {
          m_ended = true;
          return launch_failure(ProcessLaunchErrorCode::protocol_failure,
                                ProcessLaunchStage::execution,
                                "process stream exceeded its output bound");
        }
        m_output_bytes += progress->content.size();
        auto& output = progress->stream == ProcessOutputStream::standard_output
                           ? m_standard_output
                           : m_standard_error;
        output.insert(output.end(), progress->content.begin(),
                      progress->content.end());
        return std::optional<ProcessLaunchEvent>{std::move(**next)};
      }

      const auto& terminal = std::get<ProcessLaunchTerminal>(**next);
      if (m_terminal_seen) {
        m_ended = true;
        return launch_failure(ProcessLaunchErrorCode::protocol_failure,
                              ProcessLaunchStage::execution,
                              "process stream emitted a second terminal");
      }
      if (auto valid = validate_terminal_for_limits(m_limits, terminal);
          !valid) {
        m_ended = true;
        return std::unexpected(std::move(valid.error()));
      }
      if (terminal.standard_output != m_standard_output ||
          terminal.standard_error != m_standard_error) {
        m_ended = true;
        return launch_failure(
            ProcessLaunchErrorCode::protocol_failure,
            ProcessLaunchStage::execution,
            "process terminal output disagreed with emitted progress");
      }
      m_terminal_seen = true;
      return std::optional<ProcessLaunchEvent>{std::move(**next)};
    } catch (...) {
      m_ended = true;
      return launch_failure(ProcessLaunchErrorCode::internal_failure,
                            ProcessLaunchStage::execution,
                            "process stream validation failed internally");
    }
  }

 private:
  std::unique_ptr<ProcessLaunchStream> m_stream;
  ProcessLaunchLimits m_limits;
  std::mutex m_mutex;
  std::uint64_t m_output_bytes{};
  std::vector<std::byte> m_standard_output;
  std::vector<std::byte> m_standard_error;
  bool m_terminal_seen{};
  bool m_ended{};
};

} // namespace

auto validate_process_launch_bounds(const ProcessLaunchBounds bounds)
    -> std::expected<void, ProcessLaunchError> {
  constexpr ProcessLaunchBounds maximums;
  if (bounds.maximum_arguments == 0 ||
      bounds.maximum_arguments > maximums.maximum_arguments ||
      bounds.maximum_argument_bytes == 0 ||
      bounds.maximum_argument_bytes > maximums.maximum_argument_bytes ||
      bounds.maximum_roots == 0 ||
      bounds.maximum_roots > maximums.maximum_roots ||
      bounds.maximum_environment_variables == 0 ||
      bounds.maximum_environment_variables >
          maximums.maximum_environment_variables ||
      bounds.maximum_path_bytes == 0 ||
      bounds.maximum_path_bytes > maximums.maximum_path_bytes ||
      bounds.maximum_identity_bytes == 0 ||
      bounds.maximum_identity_bytes > maximums.maximum_identity_bytes ||
      bounds.maximum_wall_time <= std::chrono::milliseconds::zero() ||
      bounds.maximum_wall_time > maximums.maximum_wall_time ||
      bounds.maximum_output_bytes == 0 ||
      bounds.maximum_output_bytes > maximums.maximum_output_bytes ||
      bounds.maximum_progress_chunk_bytes == 0 ||
      bounds.maximum_progress_chunk_bytes >
          maximums.maximum_progress_chunk_bytes ||
      bounds.maximum_progress_chunk_bytes > bounds.maximum_output_bytes ||
      bounds.maximum_termination_grace <= std::chrono::milliseconds::zero() ||
      bounds.maximum_termination_grace > maximums.maximum_termination_grace) {
    return launch_failure(ProcessLaunchErrorCode::invalid_request,
                          ProcessLaunchStage::validation,
                          "process launch bounds are invalid");
  }
  return {};
}

auto validate_process_launch_request(const ProcessLaunchRequest& request,
                                     const ProcessLaunchBounds bounds)
    -> std::expected<void, ProcessLaunchError> {
  try {
    if (auto valid = validate_process_launch_bounds(bounds); !valid)
      return valid;
    if (!normalized_absolute_path(request.executable,
                                  bounds.maximum_path_bytes) ||
        !valid_identity(request.executable_identity,
                        bounds.maximum_identity_bytes) ||
        !normalized_absolute_path(request.working_directory,
                                  bounds.maximum_path_bytes) ||
        !valid_identity(request.working_directory_identity,
                        bounds.maximum_identity_bytes) ||
        request.arguments.size() > bounds.maximum_arguments ||
        request.configured_roots.empty() ||
        request.configured_roots.size() > bounds.maximum_roots ||
        request.requested_roots.empty() ||
        request.requested_roots.size() > bounds.maximum_roots ||
        request.environment.size() > bounds.maximum_environment_variables) {
      return launch_failure(ProcessLaunchErrorCode::invalid_request,
                            ProcessLaunchStage::validation,
                            "process launch request is malformed or overbound");
    }

    const auto& limits = request.limits;
    if (limits.wall_time <= std::chrono::milliseconds::zero() ||
        limits.wall_time > bounds.maximum_wall_time ||
        limits.maximum_output_bytes == 0 ||
        limits.maximum_output_bytes > bounds.maximum_output_bytes ||
        limits.maximum_progress_chunk_bytes == 0 ||
        limits.maximum_progress_chunk_bytes >
            bounds.maximum_progress_chunk_bytes ||
        limits.maximum_progress_chunk_bytes > limits.maximum_output_bytes ||
        limits.termination_grace <= std::chrono::milliseconds::zero() ||
        limits.termination_grace > bounds.maximum_termination_grace) {
      return launch_failure(ProcessLaunchErrorCode::invalid_request,
                            ProcessLaunchStage::validation,
                            "process launch limits are invalid");
    }

    std::uint64_t argument_bytes{};
    if (!add_bounded(argument_bytes, request.executable.size() + 1U,
                     bounds.maximum_argument_bytes)) {
      return launch_failure(ProcessLaunchErrorCode::invalid_request,
                            ProcessLaunchStage::validation,
                            "process arguments are malformed or overbound");
    }
    for (const auto& argument : request.arguments) {
      if (argument.find('\0') != std::string::npos ||
          !add_bounded(argument_bytes, argument.size() + 1U,
                       bounds.maximum_argument_bytes)) {
        return launch_failure(ProcessLaunchErrorCode::invalid_request,
                              ProcessLaunchStage::validation,
                              "process arguments are malformed or overbound");
      }
    }

    const auto valid_roots = [&](const auto& roots) {
      std::set<std::string_view> paths;
      return std::ranges::all_of(roots, [&](const auto& root) {
        return normalized_absolute_path(root.path, bounds.maximum_path_bytes) &&
               valid_identity(root.identity, bounds.maximum_identity_bytes) &&
               valid_access(root.access) && paths.insert(root.path).second;
      });
    };
    if (!valid_roots(request.configured_roots) ||
        !valid_roots(request.requested_roots)) {
      return launch_failure(ProcessLaunchErrorCode::invalid_request,
                            ProcessLaunchStage::validation,
                            "process filesystem roots are invalid");
    }
    const auto configured_covers = [&](const auto& requested) {
      return std::ranges::any_of(
          request.configured_roots, [&](const auto& configured) {
            const bool access_covers =
                configured.access == ProcessFilesystemAccess::read_write ||
                requested.access == ProcessFilesystemAccess::read_only;
            const bool identity_agrees =
                configured.path != requested.path ||
                configured.identity == requested.identity;
            return access_covers && identity_agrees &&
                   path_is_within(configured.path, requested.path);
          });
    };
    if (!std::ranges::all_of(request.requested_roots, configured_covers) ||
        std::ranges::none_of(request.requested_roots, [&](const auto& root) {
          return path_is_within(root.path, request.working_directory);
        })) {
      return launch_failure(ProcessLaunchErrorCode::invalid_request,
                            ProcessLaunchStage::validation,
                            "process filesystem authority is widened");
    }

    std::set<std::string_view> environment_names;
    for (const auto& variable : request.environment) {
      if (!valid_environment_name(variable.name) ||
          variable.value.find('\0') != std::string::npos ||
          !environment_names.insert(variable.name).second ||
          !add_bounded(argument_bytes,
                       variable.name.size() + variable.value.size() + 2U,
                       bounds.maximum_argument_bytes)) {
        return launch_failure(ProcessLaunchErrorCode::invalid_request,
                              ProcessLaunchStage::validation,
                              "process environment is invalid");
      }
    }
    return {};
  } catch (...) {
    return launch_failure(ProcessLaunchErrorCode::internal_failure,
                          ProcessLaunchStage::validation,
                          "process launch validation failed internally");
  }
}

auto validate_process_launch_terminal(const ProcessLaunchRequest& request,
                                      const ProcessLaunchTerminal& terminal)
    -> std::expected<void, ProcessLaunchError> {
  return validate_terminal_for_limits(request.limits, terminal);
}

auto validate_process_launch_progress(const ProcessLaunchRequest& request,
                                      const ProcessLaunchProgress& progress)
    -> std::expected<void, ProcessLaunchError> {
  return validate_progress_for_limits(request.limits, progress);
}

auto ProcessLauncher::pin_path(const ProcessLauncherContract& expected_contract,
                               std::string path,
                               const ProcessFilesystemTargetKind kind) noexcept
    -> std::expected<std::string, ProcessLaunchError> {
  try {
    if (expected_contract != m_contract) {
      return launch_failure(ProcessLaunchErrorCode::contract_drift,
                            ProcessLaunchStage::validation,
                            "process launcher contract changed after binding");
    }
    if (!normalized_absolute_path(path,
                                  ProcessLaunchBounds{}.maximum_path_bytes) ||
        !valid_target_kind(kind)) {
      return launch_failure(ProcessLaunchErrorCode::invalid_request,
                            ProcessLaunchStage::validation,
                            "process path is not a normalized absolute target");
    }
    auto pinned = do_pin_path(std::move(path), kind);
    if (!pinned) return pinned;
    if (!valid_identity(*pinned,
                        ProcessLaunchBounds{}.maximum_identity_bytes)) {
      return launch_failure(ProcessLaunchErrorCode::protocol_failure,
                            ProcessLaunchStage::validation,
                            "process launcher returned an invalid identity");
    }
    return pinned;
  } catch (...) {
    return launch_failure(ProcessLaunchErrorCode::internal_failure,
                          ProcessLaunchStage::validation,
                          "process path pinning failed internally");
  }
}

auto ProcessLauncher::launch(const ProcessLauncherContract& expected_contract,
                             ProcessLaunchRequest request,
                             const std::stop_token stop_token) noexcept
    -> std::expected<std::unique_ptr<ProcessLaunchStream>, ProcessLaunchError> {
  try {
    if (stop_token.stop_requested()) {
      return launch_failure(ProcessLaunchErrorCode::cancelled,
                            ProcessLaunchStage::validation,
                            "process launch cancelled");
    }
    if (expected_contract != m_contract) {
      return launch_failure(ProcessLaunchErrorCode::contract_drift,
                            ProcessLaunchStage::validation,
                            "process launcher contract changed after binding");
    }
    if (auto valid = validate_process_launch_request(request); !valid)
      return std::unexpected(std::move(valid.error()));
    const auto limits = request.limits;
    auto stream = do_launch(std::move(request), stop_token);
    if (!stream) return stream;
    if (!*stream) {
      return launch_failure(ProcessLaunchErrorCode::protocol_failure,
                            ProcessLaunchStage::execution,
                            "process launcher returned a null stream");
    }
    return std::make_unique<ValidatingProcessLaunchStream>(std::move(*stream),
                                                           limits);
  } catch (...) {
    return launch_failure(ProcessLaunchErrorCode::internal_failure,
                          ProcessLaunchStage::validation,
                          "process launch failed internally");
  }
}

auto BoundProcessLauncher::pin_path(
    std::string path, const ProcessFilesystemTargetKind kind) const noexcept
    -> std::expected<std::string, ProcessLaunchError> {
  return m_launcher->pin_path(m_contract, std::move(path), kind);
}

auto BoundProcessLauncher::launch(
    ProcessLaunchRequest request,
    const std::stop_token stop_token) const noexcept
    -> std::expected<std::unique_ptr<ProcessLaunchStream>, ProcessLaunchError> {
  return m_launcher->launch(m_contract, std::move(request), stop_token);
}

auto bind_process_launcher(ApplicationLaunchContext context,
                           std::shared_ptr<ProcessLauncher> launcher)
    -> std::expected<BoundProcessLauncher, ProcessLauncherBindingError> {
  try {
    if (!launcher) {
      return binding_failure(ProcessLauncherBindingErrorCode::missing_launcher,
                             "process launcher is missing");
    }
    if (context.selected_restriction() != RestrictionLevel::none) {
      return binding_failure(
          ProcessLauncherBindingErrorCode::unsupported_restriction,
          "restricted process launch mechanisms are unavailable");
    }
    if (!context.achieved_restriction() ||
        !context.restriction_policy_identity()) {
      return binding_failure(
          ProcessLauncherBindingErrorCode::unavailable_context,
          "process launcher cannot bind to an unavailable launch context");
    }
    const auto contract = launcher->contract();
    if (!valid_contract(contract)) {
      return binding_failure(ProcessLauncherBindingErrorCode::invalid_contract,
                             "process launcher contract is invalid");
    }
    if (*context.achieved_restriction() != RestrictionLevel::none ||
        contract.mechanism != context.mechanism() ||
        contract.restriction_policy_identity !=
            *context.restriction_policy_identity()) {
      return binding_failure(ProcessLauncherBindingErrorCode::contract_mismatch,
                             "process launcher and launch context disagree");
    }
    return BoundProcessLauncher{std::move(context), contract,
                                std::move(launcher)};
  } catch (...) {
    return binding_failure(ProcessLauncherBindingErrorCode::invalid_contract,
                           "process launcher binding failed internally");
  }
}

} // namespace aiforge::runtime
