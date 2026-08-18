#pragma once

#include <cstddef>
#include <expected>
#include <iosfwd>
#include <optional>
#include <stop_token>
#include <string>

#include <aiforge/backend/backend.hpp>

namespace aiforge::surfaces {

enum class OneShotErrorCode {
  invalid_input,
  input_too_large,
  model_lookup_failed,
  context_failed,
  run_failed,
  output_failed,
  cancelled,
  internal_failure,
};

struct OneShotError {
  OneShotErrorCode code{OneShotErrorCode::internal_failure};
  std::string message;
  auto operator==(const OneShotError&) const -> bool = default;
};

struct OneShotLimits {
  std::size_t maximum_input_bytes{1024U * 1024U};
  std::uint64_t preferred_output_tokens{4096};
  auto operator==(const OneShotLimits&) const -> bool = default;
};

struct OneShotRequest {
  std::string prompt;
  std::optional<std::string> stdin_evidence;
  domain::ModelId model_id;
};

struct OneShotResult {
  domain::Usage usage;
  auto operator==(const OneShotResult&) const -> bool = default;
};

class OneShotSurface final {
 public:
  OneShotSurface(backend::Backend& backend,
                 backend::ModelContextProvider& model_context,
                 OneShotLimits limits = {});

  [[nodiscard]] auto run(OneShotRequest request, std::ostream& output,
                         std::ostream& error,
                         std::stop_token stop_token = {})
      -> std::expected<OneShotResult, OneShotError>;

 private:
  backend::Backend& m_backend;
  backend::ModelContextProvider& m_model_context;
  OneShotLimits m_limits;
};

}  // namespace aiforge::surfaces
