#include <aiforge/adapters/transcript_view.hpp>

#include <algorithm>
#include <format>
#include <ranges>
#include <string_view>
#include <utility>
#include <variant>

#include <aiforge/presentation/text.hpp>

namespace aiforge::adapters {
namespace {

[[nodiscard]] auto error(const TranscriptViewErrorCode code,
                         std::string message)
    -> std::unexpected<TranscriptViewError> {
  return std::unexpected(TranscriptViewError{code, std::move(message)});
}

[[nodiscard]] auto role_name(const domain::Role role) -> std::string_view {
  switch (role) {
    case domain::Role::user:
      return "You";
    case domain::Role::assistant:
      return "Assistant";
    case domain::Role::tool:
      return "Tool";
    case domain::Role::system:
      return "System";
    case domain::Role::evidence:
      return "Evidence";
  }
  return "Message";
}

[[nodiscard]] auto tool_state(const domain::TranscriptToolState state)
    -> std::string_view {
  switch (state) {
    case domain::TranscriptToolState::proposed:
      return "proposed";
    case domain::TranscriptToolState::allowed:
      return "allowed";
    case domain::TranscriptToolState::awaiting_approval:
      return "awaiting approval";
    case domain::TranscriptToolState::running:
      return "running";
    case domain::TranscriptToolState::complete:
      return "complete";
    case domain::TranscriptToolState::denied:
      return "denied";
    case domain::TranscriptToolState::cancelled:
      return "cancelled";
    case domain::TranscriptToolState::failed:
      return "failed";
  }
  return "unknown";
}

[[nodiscard]] auto live(const domain::TranscriptItem& item) -> bool {
  if (const auto* message = std::get_if<domain::TranscriptMessage>(&item)) {
    return message->state == domain::TranscriptMessageState::streaming;
  }
  if (const auto* tool = std::get_if<domain::TranscriptToolSummary>(&item)) {
    return tool->state == domain::TranscriptToolState::proposed ||
           tool->state == domain::TranscriptToolState::allowed ||
           tool->state == domain::TranscriptToolState::awaiting_approval ||
           tool->state == domain::TranscriptToolState::running;
  }
  if (const auto* question =
          std::get_if<domain::TranscriptQuestionSummary>(&item)) {
    return question->state == domain::TranscriptQuestionState::awaiting_answer;
  }
  return false;
}

auto append_span(termforge::StyledText& output, std::string text,
                 termforge::TextStyle style) -> void {
  if (text.empty()) return;
  if (!output.empty() && output.back().style == style) {
    output.back().text += text;
  } else {
    output.push_back(termforge::TextSpan{std::move(text), style});
  }
}

[[nodiscard]] auto sanitized(const std::string_view value)
    -> std::expected<std::string, TranscriptViewError> {
  auto clean = presentation::sanitize_untrusted_text(value);
  if (!clean) {
    return error(TranscriptViewErrorCode::presentation_failed,
                 clean.error().message);
  }
  return std::move(*clean);
}

[[nodiscard]] auto flatten_rich(const termforge::StyledText& text)
    -> std::string {
  std::string output;
  for (const auto& span : text) output += span.text;
  return output;
}

}  // namespace

struct TranscriptView::RenderedEntry {
  termforge::StyledText rich;
  std::string plain;
  bool live{};
  auto operator==(const RenderedEntry&) const -> bool = default;
};

TranscriptView::TranscriptView(const TranscriptRenderMode mode,
                               TranscriptTheme theme)
    : m_mode(mode), m_theme(std::move(theme)), m_owner(std::this_thread::get_id()) {}

TranscriptView::~TranscriptView() = default;

auto TranscriptView::owner_thread() const noexcept -> bool {
  return std::this_thread::get_id() == m_owner;
}

auto TranscriptView::set_geometry(const termforge::Rect geometry) -> void {
  m_text_box.set_geometry(geometry);
}

auto TranscriptView::on_event(const termforge::Event& event) -> bool {
  return owner_thread() && m_text_box.on_event(event);
}

auto TranscriptView::draw(termforge::Screen& screen) -> void {
  if (owner_thread()) m_text_box.draw(screen);
}

auto TranscriptView::apply(const domain::RunEvent& event)
    -> std::expected<void, TranscriptViewError> {
  if (!owner_thread()) {
    return error(TranscriptViewErrorCode::wrong_thread,
                 "transcript widgets may be changed only on their owner thread");
  }
  try {
    auto candidate = m_projection;
    if (auto applied = candidate.apply(event); !applied) {
      return error(TranscriptViewErrorCode::projection_rejected,
                   applied.error().message);
    }
    auto rendered = render(candidate);
    if (!rendered) return std::unexpected(std::move(rendered.error()));
    if (auto synced = sync(std::move(*rendered)); !synced) return synced;
    m_projection = std::move(candidate);
    return {};
  } catch (...) {
    return error(TranscriptViewErrorCode::internal_failure,
                 "transcript view update failed internally");
  }
}

auto TranscriptView::rebuild(const std::span<const domain::RunEvent> events)
    -> std::expected<void, TranscriptViewError> {
  if (!owner_thread()) {
    return error(TranscriptViewErrorCode::wrong_thread,
                 "transcript widgets may be changed only on their owner thread");
  }
  try {
    auto candidate = domain::TranscriptProjection::rebuild(events);
    if (!candidate) {
      return error(TranscriptViewErrorCode::projection_rejected,
                   candidate.error().message);
    }
    auto rendered = render(*candidate);
    if (!rendered) return std::unexpected(std::move(rendered.error()));
    replace_all(*rendered);
    m_rendered = std::move(*rendered);
    m_projection = std::move(*candidate);
    return {};
  } catch (...) {
    return error(TranscriptViewErrorCode::internal_failure,
                 "transcript view rebuild failed internally");
  }
}

auto TranscriptView::render(const domain::TranscriptProjection& projection)
    const -> std::expected<std::vector<RenderedEntry>, TranscriptViewError> {
  try {
    std::vector<RenderedEntry> result;
    result.reserve(projection.items().size());

    const auto style = [&](const termforge::Rgb color,
                           const presentation::TextSemantic semantic =
                               presentation::TextSemantic::none) {
      auto attrs = termforge::Attr::None;
      if (presentation::has_semantic(semantic,
                                     presentation::TextSemantic::strong) ||
          presentation::has_semantic(semantic,
                                     presentation::TextSemantic::heading)) {
        attrs |= termforge::Attr::Bold;
      }
      if (presentation::has_semantic(semantic,
                                     presentation::TextSemantic::emphasis)) {
        attrs |= termforge::Attr::Italic;
      }
      if (presentation::has_semantic(semantic,
                                     presentation::TextSemantic::list_marker)) {
        attrs |= termforge::Attr::Dim;
      }
      if (presentation::has_semantic(semantic,
                                     presentation::TextSemantic::code)) {
        return termforge::TextStyle{m_theme.code, m_theme.code_background,
                                    attrs};
      }
      return termforge::TextStyle{color, {}, attrs};
    };

    const auto markdown = [&](termforge::StyledText& output,
                              const std::string_view text,
                              const termforge::Rgb base)
        -> std::expected<void, TranscriptViewError> {
      auto document = presentation::tokenize_markdown_lite(text);
      if (!document) {
        return error(TranscriptViewErrorCode::presentation_failed,
                     document.error().message);
      }
      for (std::size_t line = 0; line < document->size(); ++line) {
        if (line != 0) append_span(output, "\n", style(base));
        for (const auto& span : (*document)[line]) {
          append_span(output, span.text, style(base, span.semantic));
        }
      }
      return {};
    };

    const auto blocks = [&](termforge::StyledText& output,
                            const std::vector<domain::ContentBlock>& content,
                            const termforge::Rgb base)
        -> std::expected<void, TranscriptViewError> {
      bool first = true;
      for (const auto& block : content) {
        if (!first) append_span(output, "\n", style(base));
        first = false;
        if (const auto* text = std::get_if<domain::TextBlock>(&block)) {
          if (auto rendered = markdown(output, text->text, base); !rendered) {
            return rendered;
          }
        } else if (const auto* citation =
                       std::get_if<domain::CitationBlock>(&block)) {
          auto uri = sanitized(citation->uri);
          if (!uri) return std::unexpected(std::move(uri.error()));
          std::string value = "Source: ";
          if (citation->title) {
            auto title = sanitized(*citation->title);
            if (!title) return std::unexpected(std::move(title.error()));
            value += *title + " (" + *uri + ')';
          } else {
            value += *uri;
          }
          append_span(output, std::move(value), style(m_theme.muted));
        } else if (const auto* structured =
                       std::get_if<domain::StructuredDataBlock>(&block)) {
          auto media = sanitized(structured->media_type);
          if (!media) return std::unexpected(std::move(media.error()));
          append_span(output,
                      std::format("[structured: {}, {} bytes]", *media,
                                  structured->data.size()),
                      style(m_theme.muted));
        } else if (const auto* artifact =
                       std::get_if<domain::ArtifactReferenceBlock>(&block)) {
          std::string value = "[artifact: ";
          if (artifact->label) {
            auto label = sanitized(*artifact->label);
            if (!label) return std::unexpected(std::move(label.error()));
            value += *label;
          } else {
            value += artifact->artifact_id.value();
          }
          value.push_back(']');
          append_span(output, std::move(value), style(m_theme.artifact));
        } else if (const auto* unknown =
                       std::get_if<domain::UnknownContentBlock>(&block)) {
          auto type = sanitized(unknown->type_name);
          if (!type) return std::unexpected(std::move(type.error()));
          append_span(output, "[unsupported content: " + *type + "]",
                      style(m_theme.muted));
        }
      }
      return {};
    };

    for (std::size_t item_index = 0;
         item_index < projection.items().size(); ++item_index) {
      const auto& item = projection.items()[item_index];
      termforge::StyledText output;
      bool entry_live = live(item);
      if (const auto* message = std::get_if<domain::TranscriptMessage>(&item)) {
        const auto base = message->role == domain::Role::user
                              ? m_theme.user
                              : m_theme.assistant;
        append_span(output, std::string{role_name(message->role)} + ": ",
                    style(base, presentation::TextSemantic::strong));
        if (auto rendered = blocks(output, message->content, base); !rendered) {
          return std::unexpected(std::move(rendered.error()));
        }
        if (message->state == domain::TranscriptMessageState::cancelled) {
          append_span(output, "\n[cancelled]", style(m_theme.muted));
        } else if (message->state == domain::TranscriptMessageState::failed) {
          const std::string_view failure_text =
              message->error ? std::string_view{message->error->message}
                             : std::string_view{"request failed"};
          auto failure = sanitized(failure_text);
          if (!failure) return std::unexpected(std::move(failure.error()));
          append_span(output, "\n[error: " + *failure + "]",
                      style(m_theme.error));
        }
        if (message->usage.input_tokens != 0 ||
            message->usage.output_tokens != 0 ||
            message->usage.cached_input_tokens != 0 ||
            message->usage.reasoning_tokens != 0) {
          append_span(
              output,
              std::format("\n[usage: input={} output={} cached={} reasoning={}]",
                          message->usage.input_tokens,
                          message->usage.output_tokens,
                          message->usage.cached_input_tokens,
                          message->usage.reasoning_tokens),
              style(m_theme.muted));
        }
      } else if (const auto* tool =
                     std::get_if<domain::TranscriptToolSummary>(&item)) {
        auto name = sanitized(tool->tool_name);
        if (!name) return std::unexpected(std::move(name.error()));
        append_span(output,
                    "Tool " + *name + " — " +
                        std::string{tool_state(tool->state)},
                    style(m_theme.tool, presentation::TextSemantic::strong));
        const auto& content = tool->result.empty() ? tool->progress : tool->result;
        if (!content.empty()) {
          append_span(output, "\n", style(m_theme.tool));
          if (auto rendered = blocks(output, content, m_theme.tool); !rendered) {
            return std::unexpected(std::move(rendered.error()));
          }
        }
        if (tool->error) {
          auto failure = sanitized(tool->error->message);
          if (!failure) return std::unexpected(std::move(failure.error()));
          append_span(output, "\n[error: " + *failure + "]",
                      style(m_theme.error));
        }
      } else if (const auto* question =
                     std::get_if<domain::TranscriptQuestionSummary>(&item)) {
        std::vector<const domain::TranscriptQuestionSummary*> questions{
            question};
        if (question->invocation_id) {
          while (item_index + 1 < projection.items().size()) {
            const auto* next = std::get_if<domain::TranscriptQuestionSummary>(
                &projection.items()[item_index + 1]);
            if (next == nullptr ||
                next->invocation_id != question->invocation_id) {
              break;
            }
            questions.push_back(next);
            ++item_index;
          }
        }
        entry_live = std::ranges::any_of(questions, [](const auto* value) {
          return value->state ==
                 domain::TranscriptQuestionState::awaiting_answer;
        });
        const bool all_answered = std::ranges::all_of(
            questions, [](const auto* value) {
              return value->state == domain::TranscriptQuestionState::answered;
            });
        const bool all_cancelled = std::ranges::all_of(
            questions, [](const auto* value) {
              return value->state == domain::TranscriptQuestionState::cancelled;
            });
        append_span(output,
                    std::string{questions.size() == 1 ? "Question" : "Questions"} +
                        " — " +
                        (entry_live ? "awaiting answer"
                                    : all_answered ? "answered"
                                    : all_cancelled ? "cancelled" : "resolved"),
                    style(m_theme.question,
                          presentation::TextSemantic::strong));
        for (std::size_t question_index = 0;
             question_index < questions.size(); ++question_index) {
          const auto& current = *questions[question_index];
          auto prompt = sanitized(current.question.prompt);
          if (!prompt) return std::unexpected(std::move(prompt.error()));
          append_span(output,
                      "\n" + std::to_string(question_index + 1) + ". " +
                          *prompt,
                      style(m_theme.question));
          if (current.state == domain::TranscriptQuestionState::answered &&
              current.answer) {
            std::vector<std::string> labels;
            for (const auto& selected : current.answer->selected_option_ids) {
              const auto found = std::ranges::find(
                  current.question.options, selected,
                  &domain::QuestionOption::option_id);
              if (found != current.question.options.end()) {
                labels.push_back(found->label);
              }
            }
            if (current.answer->free_form) {
              labels.push_back(*current.answer->free_form);
            }
            std::string answer = "\n   Answer: ";
            for (std::size_t index = 0; index < labels.size(); ++index) {
              auto label = sanitized(labels[index]);
              if (!label) return std::unexpected(std::move(label.error()));
              if (index != 0) answer += ", ";
              answer += *label;
            }
            append_span(output, std::move(answer), style(m_theme.question));
          } else if (current.state ==
                     domain::TranscriptQuestionState::cancelled) {
            append_span(output, "\n   [cancelled]", style(m_theme.muted));
          }
        }
      } else if (const auto* artifact =
                     std::get_if<domain::TranscriptArtifactReference>(&item)) {
        auto media = sanitized(artifact->artifact.media_type);
        if (!media) return std::unexpected(std::move(media.error()));
        append_span(
            output,
            std::format("Artifact {} — {}, {} bytes",
                        artifact->artifact.artifact_id.value(), *media,
                        artifact->artifact.byte_size),
            style(m_theme.artifact, presentation::TextSemantic::strong));
      } else if (const auto* notice =
                     std::get_if<domain::TranscriptNotice>(&item)) {
        auto message = sanitized(notice->message);
        if (!message) return std::unexpected(std::move(message.error()));
        append_span(output,
                    notice->kind == domain::TranscriptNoticeKind::failed
                        ? "Error: " + *message
                        : "Cancelled: " + *message,
                    style(notice->kind == domain::TranscriptNoticeKind::failed
                              ? m_theme.error
                              : m_theme.muted,
                          presentation::TextSemantic::strong));
      }
      result.push_back(
          RenderedEntry{output, flatten_rich(output), entry_live});
    }
    return result;
  } catch (...) {
    return error(TranscriptViewErrorCode::internal_failure,
                 "transcript rendering failed internally");
  }
}

auto TranscriptView::append_entry(const RenderedEntry& entry, const bool live)
    -> void {
  if (m_mode == TranscriptRenderMode::rich) {
    if (live) {
      m_live = m_text_box.begin_entry(entry.rich);
    } else {
      m_text_box.append(entry.rich);
    }
  } else if (live) {
    m_live = m_text_box.begin_entry(entry.plain);
  } else {
    m_text_box.append(entry.plain);
  }
}

auto TranscriptView::replace_all(const std::vector<RenderedEntry>& entries)
    -> void {
  m_text_box.clear();
  m_live = {};
  for (std::size_t index = 0; index < entries.size(); ++index) {
    append_entry(entries[index],
                 index + 1 == entries.size() && entries[index].live);
  }
}

auto TranscriptView::sync(std::vector<RenderedEntry> next)
    -> std::expected<void, TranscriptViewError> {
  try {
    std::size_t common{};
    while (common < m_rendered.size() && common < next.size() &&
           m_rendered[common] == next[common]) {
      ++common;
    }

    if (m_rendered.size() == next.size() && !next.empty() &&
        common + 1 == next.size() && m_live) {
      const bool replaced =
          m_mode == TranscriptRenderMode::rich
              ? m_text_box.replace_entry(m_live, next.back().rich)
              : m_text_box.replace_entry(m_live, next.back().plain);
      if (!replaced) {
        replace_all(next);
      } else if (!next.back().live) {
        if (!m_text_box.finalize_entry(m_live)) {
          replace_all(next);
        }
        m_live = {};
      }
    } else if (common == m_rendered.size() && next.size() >= common) {
      if (m_live) {
        static_cast<void>(m_text_box.finalize_entry(m_live));
        m_live = {};
      }
      for (std::size_t index = common; index < next.size(); ++index) {
        append_entry(next[index],
                     index + 1 == next.size() && next[index].live);
      }
    } else {
      replace_all(next);
    }
    m_rendered = std::move(next);
    return {};
  } catch (...) {
    return error(TranscriptViewErrorCode::widget_rejected,
                 "TermForge rejected a transcript update");
  }
}

}  // namespace aiforge::adapters
