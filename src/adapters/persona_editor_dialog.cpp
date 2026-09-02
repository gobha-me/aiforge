#include <aiforge/adapters/persona_editor_dialog.hpp>

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <format>
#include <initializer_list>
#include <string_view>
#include <type_traits>
#include <utility>

#include <aiforge/detail/utf8_text.hpp>

namespace aiforge::adapters {
namespace {

[[nodiscard]] auto request_text(const PersonaEditorSubmission& submission)
    -> const std::string& {
  return std::visit(
      [](const auto& request) -> const std::string& {
        using Request = std::decay_t<decltype(request)>;
        if constexpr (std::same_as<Request, persona::PersonaCreate>) {
          return request.draft.text;
        } else {
          return request.text;
        }
      },
      submission);
}

[[nodiscard]] auto request_limits(const PersonaEditorSubmission& submission)
    -> const persona::PersonaLimits& {
  return std::visit(
      [](const auto& request) -> const persona::PersonaLimits& {
        return request.limits;
      },
      submission);
}

[[nodiscard]] auto normalized_newlines(std::string_view value) -> std::string {
  std::string normalized;
  normalized.reserve(value.size());
  for (std::size_t index{}; index < value.size(); ++index) {
    if (value[index] != '\r') {
      normalized.push_back(value[index]);
      continue;
    }
    if (index + 1 < value.size() && value[index + 1] == '\n') ++index;
    normalized.push_back('\n');
  }
  return normalized;
}

[[nodiscard]] auto encoded_size(const char32_t value) -> std::size_t {
  if (value < 0x80U) return 1;
  if (value < 0x800U) return 2;
  if (value < 0x10000U) return 3;
  return 4;
}

auto set_button_geometry(termforge::Rect area,
                         std::initializer_list<termforge::Button*> buttons)
    -> void {
  if (buttons.size() == 0) return;
  const int gaps = std::min(area.w, static_cast<int>(buttons.size()) - 1);
  const int available = std::max(0, area.w - gaps);
  const int button_count = static_cast<int>(buttons.size());
  const int width = available / button_count;
  const int remaining = available - (width * button_count);
  int x = area.x;
  int index{};
  for (auto* button : buttons) {
    const int button_width = width + (index < remaining ? 1 : 0);
    button->set_geometry({x, area.y, button_width, area.h > 0 ? 1 : 0});
    x += button_width + (index < gaps ? 1 : 0);
    ++index;
  }
}

} // namespace

PersonaEditorDialog::PersonaEditorDialog() : Dialog("Persona editor") {
  set_max_width(96);
  m_composer.set_max_height(12);
  m_composer.set_enter_mode(termforge::ComposerEnterMode::Newline);
  m_composer.on_change([this](const std::string&) {
    m_error.clear();
    m_preview.reset();
    update_body();
  });
  m_review.on_activate([this] { review(); });
  m_save.on_activate([this] { save(); });
  m_back.on_activate([this] { show_editing(); });
  m_cancel.on_activate([this] { cancel(); });
  show_editing();
}

auto PersonaEditorDialog::set_submission(PersonaEditorSubmission submission,
                                         const bool selected) -> void {
  m_submission = std::move(submission);
  m_reviewed_submission = m_submission;
  m_selected = selected;
  m_preview.reset();
  m_error.clear();
  m_save_blocked = false;
  m_composer.set_text(std::string{request_text(m_submission)});
  set_title(std::visit(
      [](const auto& request) {
        using Request = std::decay_t<decltype(request)>;
        if constexpr (std::same_as<Request, persona::PersonaCreate>) {
          return "Create persona " + request.draft.name;
        } else {
          return "Edit persona " + request.expected.name;
        }
      },
      m_submission));
  show_editing();
}

auto PersonaEditorDialog::on_save(SavePersona callback) -> void {
  m_on_save = std::move(callback);
}

auto PersonaEditorDialog::on_result(
    std::function<void(PersonaEditorDialogResult)> callback) -> void {
  m_on_result = std::move(callback);
}

auto PersonaEditorDialog::stage() const noexcept -> PersonaEditorDialogStage {
  return m_stage;
}

auto PersonaEditorDialog::draft_text() const noexcept -> const std::string& {
  return m_composer.text();
}

auto PersonaEditorDialog::preview() const noexcept
    -> const std::optional<domain::PersonaDocument>& {
  return m_preview;
}

auto PersonaEditorDialog::error_message() const noexcept -> const std::string& {
  return m_error;
}

auto PersonaEditorDialog::content_rows() const -> int {
  return m_stage == PersonaEditorDialogStage::editing ? 14 : 1;
}

auto PersonaEditorDialog::layout_content(const termforge::Rect area) -> void {
  if (m_stage == PersonaEditorDialogStage::editing) {
    const int composer_rows = std::max(0, area.h - 2);
    m_composer.set_geometry({area.x, area.y, area.w, composer_rows});
    set_button_geometry(
        {area.x, area.y + std::max(0, area.h - 1), area.w, area.h > 0 ? 1 : 0},
        {&m_review, &m_cancel});
    m_save.set_geometry({});
    m_back.set_geometry({});
    return;
  }

  m_composer.set_geometry({});
  m_review.set_geometry({});
  set_button_geometry({area.x, area.y, area.w, area.h > 0 ? 1 : 0},
                      {&m_save, &m_back, &m_cancel});
}

auto PersonaEditorDialog::draw_content(termforge::Screen& screen) -> void {
  if (m_stage == PersonaEditorDialogStage::editing) {
    m_composer.draw(screen);
    m_review.draw(screen);
    m_cancel.draw(screen);
    return;
  }
  m_save.draw(screen);
  m_back.draw(screen);
  m_cancel.draw(screen);
}

auto PersonaEditorDialog::on_event(const termforge::Event& event) -> bool {
  if (m_stage == PersonaEditorDialogStage::editing && m_composer.focused()) {
    if (const auto* paste = std::get_if<termforge::PasteEvent>(&event)) {
      if (!accepts_insertion(normalized_newlines(paste->text))) return true;
    }
    if (const auto* key = std::get_if<termforge::KeyEvent>(&event);
        key != nullptr && key->action != termforge::KeyAction::Release) {
      if (!accepts_key(*key)) return true;
    }
  }
  return Dialog::on_event(event);
}

auto PersonaEditorDialog::on_escape() -> void {
  cancel();
}

auto PersonaEditorDialog::show_editing() -> void {
  m_stage = PersonaEditorDialogStage::editing;
  m_preview.reset();
  clear_children();
  add_child(&m_composer);
  add_child(&m_review);
  add_child(&m_cancel);
  update_body();
}

auto PersonaEditorDialog::show_preview() -> void {
  m_stage = PersonaEditorDialogStage::preview;
  clear_children();
  add_child(&m_save);
  add_child(&m_back);
  add_child(&m_cancel);
  update_body();
}

auto PersonaEditorDialog::review() -> void {
  auto submission = submission_with_text(m_composer.text());
  auto prepared = prepare(submission);
  if (!prepared) {
    m_error = prepared.error().message;
    update_body();
    return;
  }
  m_error.clear();
  m_reviewed_submission = std::move(submission);
  m_preview = std::move(*prepared);
  show_preview();
}

auto PersonaEditorDialog::save() -> void {
  if (!m_preview) {
    m_error = "Review the persona before saving";
    update_body();
    return;
  }
  if (!m_on_save) {
    m_error = "Persona saving is unavailable";
    update_body();
    return;
  }
  if (m_save_blocked) {
    m_error = "Reload the persona before another save attempt";
    update_body();
    return;
  }
  const auto callback = m_on_save;
  auto saved = callback(m_reviewed_submission);
  if (!saved) {
    m_error = saved.error().message;
    m_save_blocked = saved.error().may_have_applied;
    update_body();
    return;
  }
  const auto valid = std::visit(
      [&](const auto& request) {
        return persona::validate_persona_write_receipt(request, *saved);
      },
      m_reviewed_submission);
  if (!valid) {
    m_error = valid.error().message;
    m_save_blocked = valid.error().may_have_applied;
    update_body();
    return;
  }
  if (!begin_result()) return;
  const auto result = m_on_result;
  close();
  if (result) result(PersonaEditorDialogResult{std::move(*saved), false});
}

auto PersonaEditorDialog::cancel() -> void {
  if (!begin_result()) return;
  const auto result = m_on_result;
  close();
  if (result) result(PersonaEditorDialogResult{std::nullopt, m_save_blocked});
}

auto PersonaEditorDialog::update_body() -> void {
  std::string body;
  if (m_stage == PersonaEditorDialogStage::editing) {
    body = "Enter edits the bounded multiline content. Tab reaches Review and "
           "Cancel; Escape cancels without saving.";
    if (!m_error.empty()) body += "\nError: " + m_error;
    Dialog::set_text(std::move(body));
    return;
  }

  if (!m_preview) {
    body = "Preview is unavailable.";
  } else {
    const auto& reference = m_preview->reference;
    body = std::format(
        "Name: {}\nSource: {}\nBytes: {} / {}\nSHA-256: {}\nSelected: "
        "{}\nSave performs one digest-checked write; Back returns to editing.",
        reference.name, reference.source_location,
        reference.content_digest.byte_size,
        request_limits(m_reviewed_submission).maximum_file_bytes,
        reference.content_digest.value, m_selected ? "yes" : "no");
  }
  if (!m_error.empty()) body += "\nError: " + m_error;
  if (m_save_blocked) {
    body += "\nThe write may have applied; cancel and reopen to reload before "
            "saving again.";
  }
  Dialog::set_text(std::move(body));
}

auto PersonaEditorDialog::submission_with_text(std::string text) const
    -> PersonaEditorSubmission {
  auto submission = m_submission;
  std::visit(
      [&](auto& request) {
        using Request = std::decay_t<decltype(request)>;
        if constexpr (std::same_as<Request, persona::PersonaCreate>) {
          request.draft.text = std::move(text);
        } else {
          request.text = std::move(text);
        }
      },
      submission);
  return submission;
}

auto PersonaEditorDialog::prepare(
    const PersonaEditorSubmission& submission) const
    -> std::expected<domain::PersonaDocument, persona::PersonaEditorError> {
  return std::visit(
      [](const auto& request) -> std::expected<domain::PersonaDocument,
                                               persona::PersonaEditorError> {
        using Request = std::decay_t<decltype(request)>;
        if constexpr (std::same_as<Request, persona::PersonaCreate>) {
          return persona::prepare_persona_create(request);
        } else {
          return persona::prepare_persona_replace(request);
        }
      },
      submission);
}

auto PersonaEditorDialog::accepts_insertion(std::string insertion) -> bool {
  const auto maximum = request_limits(m_submission).maximum_file_bytes;
  if (insertion.size() > maximum ||
      m_composer.text().size() > maximum - insertion.size()) {
    m_error = "Persona content exceeds its byte limit";
    update_body();
    return false;
  }
  auto prospective = m_composer.text();
  prospective.insert(m_composer.cursor_pos(), insertion);
  if (!detail::is_safe_utf8_text(prospective)) {
    m_error = "Persona content must be UTF-8 without unsafe controls";
    update_body();
    return false;
  }
  return true;
}

auto PersonaEditorDialog::accepts_key(const termforge::KeyEvent& key) -> bool {
  if (key.key == termforge::Key::Enter) return accepts_insertion("\n");
  if (key.key != termforge::Key::Char || key.ctrl || key.alt ||
      key.ch < 0x20U || key.ch == 0x7fU || key.ch > 0x10ffffU ||
      (key.ch >= 0xd800U && key.ch <= 0xdfffU)) {
    return true;
  }
  if (detail::is_unsafe_text_control(key.ch)) {
    m_error = "Persona content must be UTF-8 without unsafe controls";
    update_body();
    return false;
  }
  const auto maximum = request_limits(m_submission).maximum_file_bytes;
  if (encoded_size(key.ch) > maximum ||
      m_composer.text().size() > maximum - encoded_size(key.ch)) {
    m_error = "Persona content exceeds its byte limit";
    update_body();
    return false;
  }
  return true;
}

} // namespace aiforge::adapters
