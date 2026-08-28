#include <aiforge/testing/scripted_backend.hpp>

#include <optional>
#include <utility>

namespace aiforge::testing {
namespace {

class ScriptedStream final : public backend::BackendStream {
 public:
  explicit ScriptedStream(StreamScript script)
      : m_steps(std::move(script.steps)) {}

  auto next(const std::stop_token stop_token)
      -> std::expected<std::optional<backend::BackendEvent>,
                       backend::BackendError> override {
    if (m_ended) return std::optional<backend::BackendEvent>{};

    if (stop_token.stop_requested()) {
      m_ended = true;
      return std::optional<backend::BackendEvent>{
          backend::ResponseCancelled{std::string{"stop requested"}}};
    }

    if (m_next_step >= m_steps.size()) {
      m_ended = true;
      return std::optional<backend::BackendEvent>{};
    }

    const auto& step = m_steps[m_next_step++];
    if (const auto* event = std::get_if<backend::BackendEvent>(&step)) {
      return std::optional<backend::BackendEvent>{*event};
    }
    if (const auto* error = std::get_if<backend::BackendError>(&step)) {
      m_ended = true;
      return std::unexpected(*error);
    }

    m_ended = true;
    return std::optional<backend::BackendEvent>{};
  }

 private:
  std::vector<ScriptedStep> m_steps;
  std::size_t m_next_step{};
  bool m_ended{};
};

[[nodiscard]] auto cancelled_error() -> backend::BackendError {
  return backend::BackendError{backend::BackendErrorKind::cancelled,
                               "backend start cancelled", false, std::nullopt};
}

} // namespace

ScriptedBackend::ScriptedBackend(std::vector<ScriptedExchange> exchanges)
    : m_exchanges(std::move(exchanges)) {
}

auto ScriptedBackend::start(backend::BackendRequest request,
                            const std::stop_token stop_token)
    -> std::expected<std::unique_ptr<backend::BackendStream>,
                     backend::BackendError> {
  if (stop_token.stop_requested()) return std::unexpected(cancelled_error());

  m_recorded_requests.push_back(request);

  if (m_next_exchange >= m_exchanges.size()) {
    return std::unexpected(backend::BackendError{
        backend::BackendErrorKind::script_exhausted,
        "scripted backend has no exchange remaining", false, std::nullopt});
  }

  const auto& exchange = m_exchanges[m_next_exchange];
  if (request != exchange.expected_request) {
    return std::unexpected(backend::BackendError{
        backend::BackendErrorKind::script_mismatch,
        "backend request did not match the script", false, std::nullopt});
  }

  ++m_next_exchange;
  if (const auto* error =
          std::get_if<backend::BackendError>(&exchange.outcome)) {
    return std::unexpected(*error);
  }

  auto stream = std::make_unique<ScriptedStream>(
      std::get<StreamScript>(exchange.outcome));
  return std::unique_ptr<backend::BackendStream>{std::move(stream)};
}

auto ScriptedBackend::recorded_requests() const noexcept
    -> const std::vector<backend::BackendRequest>& {
  return m_recorded_requests;
}

auto ScriptedBackend::remaining_exchanges() const noexcept -> std::size_t {
  return m_exchanges.size() - m_next_exchange;
}

} // namespace aiforge::testing
