#include <aiforge/testing/scripted_user_global_instruction_source.hpp>

#include <utility>

namespace aiforge::testing {
namespace {

[[nodiscard]] auto failure(
    const instructions::UserGlobalInstructionErrorCode code,
    std::string message)
    -> std::unexpected<instructions::UserGlobalInstructionError> {
  return std::unexpected(instructions::UserGlobalInstructionError{
      code, std::move(message), false});
}

} // namespace

ScriptedUserGlobalInstructionSource::ScriptedUserGlobalInstructionSource(
    std::vector<UserGlobalInstructionLoadOutcome> outcomes)
    : m_outcomes(std::move(outcomes)) {
}

auto ScriptedUserGlobalInstructionSource::load(
    const instructions::UserGlobalInstructionLimits limits,
    const std::stop_token stop_token)
    -> std::expected<std::optional<domain::UserGlobalInstructionDocument>,
                     instructions::UserGlobalInstructionError> {
  try {
    if (stop_token.stop_requested()) {
      return failure(instructions::UserGlobalInstructionErrorCode::cancelled,
                     "user-global instruction loading cancelled");
    }
    constexpr instructions::UserGlobalInstructionLimits maximums;
    if (limits.maximum_file_bytes == 0 ||
        limits.maximum_file_bytes > maximums.maximum_file_bytes) {
      return failure(
          instructions::UserGlobalInstructionErrorCode::invalid_request,
          "user-global instruction limits are invalid");
    }
    m_recorded_limits.push_back(limits);
    if (m_next >= m_outcomes.size()) {
      return failure(
          instructions::UserGlobalInstructionErrorCode::internal_failure,
          "scripted user-global instruction source is exhausted");
    }
    auto& outcome = m_outcomes[m_next++];
    if (auto* error =
            std::get_if<instructions::UserGlobalInstructionError>(&outcome)) {
      return std::unexpected(*error);
    }
    return std::get<std::optional<domain::UserGlobalInstructionDocument>>(
        outcome);
  } catch (...) {
    return failure(
        instructions::UserGlobalInstructionErrorCode::internal_failure,
        "scripted user-global instruction loading failed internally");
  }
}

auto ScriptedUserGlobalInstructionSource::recorded_limits() const noexcept
    -> const std::vector<instructions::UserGlobalInstructionLimits>& {
  return m_recorded_limits;
}

auto ScriptedUserGlobalInstructionSource::remaining_loads() const noexcept
    -> std::size_t {
  return m_outcomes.size() - m_next;
}

} // namespace aiforge::testing
