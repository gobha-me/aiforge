#pragma once

#include <cstddef>
#include <variant>
#include <vector>

#include <aiforge/repository/exact_source_edit.hpp>

namespace aiforge::testing {

using ExactSourceReadOutcome =
    std::variant<repository::ExactSourceReadResult,
                 repository::ExactSourceEditError>;

struct ExactSourceReadExchange {
  repository::ExactSourceReadRequest expected_request;
  ExactSourceReadOutcome outcome;
  auto operator==(const ExactSourceReadExchange&) const -> bool = default;
};

using ExactSourceEditOutcome =
    std::variant<repository::ExactSourceEditReceipt,
                 repository::ExactSourceEditError>;

struct ExactSourceEditExchange {
  repository::ExactSourceEditRequest expected_request;
  ExactSourceEditOutcome outcome;
  auto operator==(const ExactSourceEditExchange&) const -> bool = default;
};

class ScriptedExactSourceEditor final
    : public repository::ExactSourceEditor {
 public:
  explicit ScriptedExactSourceEditor(
      std::vector<ExactSourceReadExchange> read_exchanges = {},
      std::vector<ExactSourceEditExchange> edit_exchanges = {});

  [[nodiscard]] auto read(repository::ExactSourceReadRequest request,
                          std::stop_token stop_token = {})
      -> std::expected<repository::ExactSourceReadResult,
                       repository::ExactSourceEditError> override;

  [[nodiscard]] auto apply(repository::ExactSourceEditRequest request,
                           std::stop_token stop_token = {})
      -> std::expected<repository::ExactSourceEditReceipt,
                       repository::ExactSourceEditError> override;

  [[nodiscard]] auto recorded_read_requests() const noexcept
      -> const std::vector<repository::ExactSourceReadRequest>&;
  [[nodiscard]] auto recorded_edit_requests() const noexcept
      -> const std::vector<repository::ExactSourceEditRequest>&;
  [[nodiscard]] auto remaining_read_exchanges() const noexcept -> std::size_t;
  [[nodiscard]] auto remaining_edit_exchanges() const noexcept -> std::size_t;

 private:
  std::vector<ExactSourceReadExchange> m_read_exchanges;
  std::vector<ExactSourceEditExchange> m_edit_exchanges;
  std::vector<repository::ExactSourceReadRequest> m_recorded_read_requests;
  std::vector<repository::ExactSourceEditRequest> m_recorded_edit_requests;
  std::size_t m_next_read_exchange{};
  std::size_t m_next_edit_exchange{};
};

}  // namespace aiforge::testing
