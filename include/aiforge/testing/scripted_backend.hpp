#pragma once

#include <cstddef>
#include <expected>
#include <memory>
#include <stop_token>
#include <variant>
#include <vector>

#include <aiforge/backend/backend.hpp>

namespace aiforge::testing {

struct EndOfStream {
  auto operator==(const EndOfStream&) const -> bool = default;
};

using ScriptedStep =
    std::variant<backend::BackendEvent, backend::BackendError, EndOfStream>;

struct StreamScript {
  std::vector<ScriptedStep> steps;
  auto operator==(const StreamScript&) const -> bool = default;
};

using ScriptedOutcome = std::variant<StreamScript, backend::BackendError>;

struct ScriptedExchange {
  backend::BackendRequest expected_request;
  ScriptedOutcome outcome;
  auto operator==(const ScriptedExchange&) const -> bool = default;
};

class ScriptedBackend final : public backend::Backend {
 public:
  explicit ScriptedBackend(std::vector<ScriptedExchange> exchanges);

  [[nodiscard]] auto start(backend::BackendRequest request,
                           std::stop_token stop_token)
      -> std::expected<std::unique_ptr<backend::BackendStream>,
                       backend::BackendError> override;

  [[nodiscard]] auto recorded_requests() const noexcept
      -> const std::vector<backend::BackendRequest>&;
  [[nodiscard]] auto remaining_exchanges() const noexcept -> std::size_t;

 private:
  std::vector<ScriptedExchange> m_exchanges;
  std::vector<backend::BackendRequest> m_recorded_requests;
  std::size_t m_next_exchange{};
};

} // namespace aiforge::testing
