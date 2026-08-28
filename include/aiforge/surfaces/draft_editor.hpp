#pragma once

#include <expected>
#include <stop_token>
#include <string>
#include <string_view>

namespace aiforge::surfaces {

enum class DraftEditorErrorCode {
  not_configured,
  invalid_configuration,
  unavailable,
  permission_denied,
  resource_exhausted,
  process_failed,
  invalid_result,
  cleanup_failed,
  cancelled,
  internal_failure,
};

struct DraftEditorError {
  DraftEditorErrorCode code{DraftEditorErrorCode::internal_failure};
  std::string message;
  auto operator==(const DraftEditorError&) const -> bool = default;
};

class DraftEditor {
 public:
  virtual ~DraftEditor() = default;

  [[nodiscard]] virtual auto edit(std::string_view draft,
                                  std::stop_token stop_token = {})
      -> std::expected<std::string, DraftEditorError> = 0;
};

} // namespace aiforge::surfaces
