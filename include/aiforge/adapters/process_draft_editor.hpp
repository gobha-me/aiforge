#pragma once

#include <aiforge/surfaces/draft_editor.hpp>
#include <chrono>
#include <cstddef>

namespace aiforge::adapters {

struct ProcessDraftEditorLimits {
  std::size_t maximum_draft_bytes{1024U * 1024U};
  std::chrono::milliseconds timeout{std::chrono::hours{24}};
  std::chrono::milliseconds termination_grace{std::chrono::milliseconds{250}};
  auto operator==(const ProcessDraftEditorLimits&) const -> bool = default;
};

class ProcessDraftEditor final : public surfaces::DraftEditor {
 public:
  explicit ProcessDraftEditor(ProcessDraftEditorLimits limits = {})
      : m_limits(limits) {}

  [[nodiscard]] auto edit(std::string_view draft,
                          std::stop_token stop_token = {})
      -> std::expected<std::string, surfaces::DraftEditorError> override;

 private:
  ProcessDraftEditorLimits m_limits;
};

} // namespace aiforge::adapters
