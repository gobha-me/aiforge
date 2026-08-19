#pragma once

#include <expected>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <aiforge/domain/transcript_projection.hpp>
#include <termforge/core/screen.hpp>
#include <termforge/widgets/text_box.hpp>

namespace aiforge::adapters {

enum class TranscriptRenderMode {
  rich,
  plain_text,
};

struct TranscriptTheme {
  termforge::Rgb user{0x7D, 0xD3, 0xFC};
  termforge::Rgb assistant{0xE5, 0xE7, 0xEB};
  termforge::Rgb tool{0xC4, 0xB5, 0xFD};
  termforge::Rgb question{0xFD, 0xBA, 0x74};
  termforge::Rgb artifact{0x86, 0xEF, 0xAC};
  termforge::Rgb error{0xFC, 0x81, 0x81};
  termforge::Rgb muted{0x94, 0xA3, 0xB8};
  termforge::Rgb code{0xF8, 0xFA, 0xFC};
  termforge::Rgb code_background{0x1E, 0x29, 0x3B};
  auto operator==(const TranscriptTheme&) const -> bool = default;
};

enum class TranscriptViewErrorCode {
  wrong_thread,
  projection_rejected,
  presentation_failed,
  widget_rejected,
  internal_failure,
};

struct TranscriptViewError {
  TranscriptViewErrorCode code;
  std::string message;
  auto operator==(const TranscriptViewError&) const -> bool = default;
};

class TranscriptView final {
 public:
  explicit TranscriptView(
      TranscriptRenderMode mode = TranscriptRenderMode::rich,
      TranscriptTheme theme = {});
  ~TranscriptView();

  [[nodiscard]] auto apply(const domain::RunEvent& event)
      -> std::expected<void, TranscriptViewError>;
  [[nodiscard]] auto rebuild(std::span<const domain::RunEvent> events)
      -> std::expected<void, TranscriptViewError>;

  auto set_geometry(termforge::Rect geometry) -> void;
  [[nodiscard]] auto on_event(const termforge::Event& event) -> bool;
  auto draw(termforge::Screen& screen) -> void;

  [[nodiscard]] auto projection() const noexcept
      -> const domain::TranscriptProjection& {
    const auto runs = m_projection.runs();
    return runs.empty() ? m_empty_projection : runs.back();
  }
  [[nodiscard]] auto session_projection() const noexcept
      -> const domain::SessionTranscriptProjection& {
    return m_projection;
  }
  [[nodiscard]] auto widget() noexcept -> termforge::TextBox& {
    return m_text_box;
  }
  [[nodiscard]] auto widget() const noexcept -> const termforge::TextBox& {
    return m_text_box;
  }

 private:
  struct RenderedEntry;

  [[nodiscard]] auto render(
      const domain::SessionTranscriptProjection& projection)
      const -> std::expected<std::vector<RenderedEntry>, TranscriptViewError>;
  [[nodiscard]] auto render_run(
      const domain::TranscriptProjection& projection)
      const -> std::expected<std::vector<RenderedEntry>, TranscriptViewError>;
  [[nodiscard]] auto sync(std::vector<RenderedEntry> next)
      -> std::expected<void, TranscriptViewError>;
  auto replace_all(const std::vector<RenderedEntry>& entries) -> void;
  auto append_entry(const RenderedEntry& entry, bool live) -> void;
  [[nodiscard]] auto owner_thread() const noexcept -> bool;

  TranscriptRenderMode m_mode;
  TranscriptTheme m_theme;
  std::thread::id m_owner;
  domain::SessionTranscriptProjection m_projection;
  domain::TranscriptProjection m_empty_projection;
  termforge::TextBox m_text_box;
  std::vector<RenderedEntry> m_rendered;
  termforge::TextEntryHandle m_live;
};

}  // namespace aiforge::adapters
