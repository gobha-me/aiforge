#include <aiforge/testing/scripted_exact_source_editor.hpp>

#include <utility>

namespace aiforge::testing {
namespace {

[[nodiscard]] auto failure(std::string message)
    -> std::unexpected<repository::ExactSourceEditError> {
  return std::unexpected(repository::ExactSourceEditError{
      repository::ExactSourceEditErrorCode::internal_failure,
      std::move(message),
      {},
      {},
      false,
      false});
}

[[nodiscard]] auto cancelled()
    -> std::unexpected<repository::ExactSourceEditError> {
  return std::unexpected(repository::ExactSourceEditError{
      repository::ExactSourceEditErrorCode::cancelled,
      "exact-source operation cancelled",
      {},
      {},
      false,
      false});
}

} // namespace

ScriptedExactSourceEditor::ScriptedExactSourceEditor(
    std::vector<ExactSourceReadExchange> read_exchanges,
    std::vector<ExactSourceEditExchange> edit_exchanges,
    const bool guarantees_tracked_regular_files)
    : m_read_exchanges(std::move(read_exchanges)),
      m_edit_exchanges(std::move(edit_exchanges)),
      m_guarantees_tracked_regular_files(guarantees_tracked_regular_files) {
}

auto ScriptedExactSourceEditor::guarantees_tracked_regular_files()
    const noexcept -> bool {
  return m_guarantees_tracked_regular_files;
}

auto ScriptedExactSourceEditor::guarantees_read_only_execution() const noexcept
    -> bool {
  return true;
}

auto ScriptedExactSourceEditor::is_coupled_to(
    const repository::RepositorySnapshotSource& source) const noexcept -> bool {
  return m_snapshot_source == &source;
}

void ScriptedExactSourceEditor::couple_to(
    const repository::RepositorySnapshotSource& source,
    const bool guarantees_tracked_regular_files) noexcept {
  m_snapshot_source = &source;
  m_guarantees_tracked_regular_files = guarantees_tracked_regular_files;
}

auto ScriptedExactSourceEditor::read(repository::ExactSourceReadRequest request,
                                     const std::stop_token stop_token)
    -> std::expected<repository::ExactSourceReadResult,
                     repository::ExactSourceEditError> {
  try {
    if (stop_token.stop_requested()) return cancelled();
    m_recorded_read_requests.push_back(request);
    if (m_next_read_exchange >= m_read_exchanges.size()) {
      return failure("scripted exact-source reads are exhausted");
    }
    const auto& exchange = m_read_exchanges[m_next_read_exchange];
    if (exchange.expected_request != request) {
      return failure("exact-source read did not match the script");
    }
    ++m_next_read_exchange;
    if (const auto* error =
            std::get_if<repository::ExactSourceEditError>(&exchange.outcome)) {
      return std::unexpected(*error);
    }
    return std::get<repository::ExactSourceReadResult>(exchange.outcome);
  } catch (...) {
    return failure("scripted exact-source read failed internally");
  }
}

auto ScriptedExactSourceEditor::apply(
    repository::ExactSourceEditRequest request,
    const std::stop_token stop_token)
    -> std::expected<repository::ExactSourceEditReceipt,
                     repository::ExactSourceEditError> {
  try {
    if (stop_token.stop_requested()) return cancelled();
    m_recorded_edit_requests.push_back(request);
    if (m_next_edit_exchange >= m_edit_exchanges.size()) {
      return failure("scripted exact-source edits are exhausted");
    }
    const auto& exchange = m_edit_exchanges[m_next_edit_exchange];
    if (exchange.expected_request != request) {
      return failure("exact-source edit did not match the script");
    }
    ++m_next_edit_exchange;
    if (const auto* error =
            std::get_if<repository::ExactSourceEditError>(&exchange.outcome)) {
      return std::unexpected(*error);
    }
    return std::get<repository::ExactSourceEditReceipt>(exchange.outcome);
  } catch (...) {
    return failure("scripted exact-source edit failed internally");
  }
}

auto ScriptedExactSourceEditor::recorded_read_requests() const noexcept
    -> const std::vector<repository::ExactSourceReadRequest>& {
  return m_recorded_read_requests;
}

auto ScriptedExactSourceEditor::recorded_edit_requests() const noexcept
    -> const std::vector<repository::ExactSourceEditRequest>& {
  return m_recorded_edit_requests;
}

auto ScriptedExactSourceEditor::remaining_read_exchanges() const noexcept
    -> std::size_t {
  return m_read_exchanges.size() - m_next_read_exchange;
}

auto ScriptedExactSourceEditor::remaining_edit_exchanges() const noexcept
    -> std::size_t {
  return m_edit_exchanges.size() - m_next_edit_exchange;
}

} // namespace aiforge::testing
