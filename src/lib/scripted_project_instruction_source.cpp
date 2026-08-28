#include <aiforge/testing/scripted_project_instruction_source.hpp>

#include <utility>

namespace aiforge::testing {

ScriptedProjectInstructionSource::ScriptedProjectInstructionSource(
    std::vector<ProjectInstructionExchange> exchanges)
    : m_exchanges(std::move(exchanges)) {
}

auto ScriptedProjectInstructionSource::discover(
    repository::ProjectInstructionRequest request,
    const std::stop_token stop_token)
    -> std::expected<domain::ProjectInstructionDiscovery,
                     repository::ProjectInstructionError> {
  try {
    if (stop_token.stop_requested()) {
      return std::unexpected(repository::ProjectInstructionError{
          repository::ProjectInstructionErrorCode::cancelled,
          "project instruction discovery cancelled", std::nullopt, false});
    }
    m_recorded_requests.push_back(request);
    if (m_next_exchange >= m_exchanges.size()) {
      return std::unexpected(repository::ProjectInstructionError{
          repository::ProjectInstructionErrorCode::internal_failure,
          "scripted project instruction source exhausted", std::nullopt,
          false});
    }
    auto& exchange = m_exchanges[m_next_exchange++];
    if (exchange.expected_request != request) {
      return std::unexpected(repository::ProjectInstructionError{
          repository::ProjectInstructionErrorCode::invalid_request,
          "project instruction request did not match script", std::nullopt,
          false});
    }
    if (auto* error = std::get_if<repository::ProjectInstructionError>(
            &exchange.outcome)) {
      return std::unexpected(*error);
    }
    return std::get<domain::ProjectInstructionDiscovery>(exchange.outcome);
  } catch (...) {
    return std::unexpected(repository::ProjectInstructionError{
        repository::ProjectInstructionErrorCode::internal_failure,
        "scripted project instruction source failed internally", std::nullopt,
        false});
  }
}

auto ScriptedProjectInstructionSource::recorded_requests() const noexcept
    -> const std::vector<repository::ProjectInstructionRequest>& {
  return m_recorded_requests;
}

auto ScriptedProjectInstructionSource::remaining_exchanges() const noexcept
    -> std::size_t {
  return m_exchanges.size() - m_next_exchange;
}

} // namespace aiforge::testing
