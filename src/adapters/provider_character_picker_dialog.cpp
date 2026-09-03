#include <aiforge/adapters/provider_character_picker_dialog.hpp>
#include <aiforge/detail/utf8_text.hpp>

#include <algorithm>
#include <cctype>
#include <ranges>
#include <string_view>
#include <utility>
#include <variant>

#include <termforge/widgets/detail/width.hpp>

namespace aiforge::adapters {
namespace {

constexpr std::size_t maximum_filter_bytes = 4096U;

[[nodiscard]] constexpr auto encoded_size(const char32_t character)
    -> std::size_t {
  if (character < 0x80U) return 1U;
  if (character < 0x800U) return 2U;
  if (character < 0x10000U) return 3U;
  return 4U;
}

[[nodiscard]] auto lower_ascii(const std::string_view value) -> std::string {
  std::string result;
  result.reserve(value.size());
  for (const auto character : value) {
    result.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
  }
  return result;
}

[[nodiscard]] auto flattened(const std::string_view value) -> std::string {
  std::string result{value};
  std::ranges::replace_if(
      result,
      [](const char character) {
        return character == '\n' || character == '\r' || character == '\t';
      },
      ' ');
  return result;
}

[[nodiscard]] auto compatibility_reason(
    const backend::ProviderCharacterSummary& character,
    const model::CatalogSnapshot& models, const domain::ModelId& current_model)
    -> std::string {
  if (!character.model_id) return "character has no model metadata";
  const auto* model = model::find_model(models, *character.model_id, "text");
  if (model == nullptr) return "required text model is unavailable";
  if (model->offline) return "required text model is offline";
  if (!model->context_window_tokens)
    return "required text model has invalid metadata";
  if (*character.model_id != current_model) {
    return "requires model " + std::string{character.model_id->value()} +
           "; current model is " + std::string{current_model.value()};
  }
  return {};
}

} // namespace

ProviderCharacterPickerDialog::ProviderCharacterPickerDialog()
    : Dialog("Select provider character") {
  set_text("Filter by character ID, name, description, tag, or model. Provider "
           "characters affect the provider request only; they do not change "
           "AIForge personas, workspace instructions, or tool authority. "
           "Up/Down navigate; Enter selects; Tab changes focus; Escape "
           "cancels.");
  set_max_width(92);
  m_filter.set_placeholder("Character ID, name, tag, or model");
  m_filter.on_change([this](const std::string&) {
    m_filter_error.clear();
    apply_filter();
  });
  m_list.on_select(
      [this](const int index, const std::string&) { choose(index); });
  add_child(&m_filter);
  add_child(&m_list);
}

auto ProviderCharacterPickerDialog::set_characters(
    const backend::ProviderCharacterCatalog& characters,
    const model::CatalogSnapshot& models, const domain::ModelId& current_model,
    std::optional<domain::ProviderCharacterId> current_character) -> void {
  m_current_character = std::move(current_character);
  m_current_model = current_model;
  m_source_id = characters.source_id;
  m_filter_error.clear();
  m_all.clear();
  m_all.push_back({std::nullopt,
                   "Provider default (no provider character)" +
                       std::string{m_current_character ? "" : " [current]"},
                   "provider default none off",
                   {}});
  for (const auto& entry : characters.entries) {
    std::string label{entry.id.value()};
    if (entry.name && *entry.name != entry.id.value())
      label += " — " + flattened(*entry.name);
    const auto unavailable = compatibility_reason(entry, models, current_model);
    if (!unavailable.empty()) label += " (" + unavailable + ")";
    if (m_current_character && entry.id == *m_current_character)
      label += " [current]";

    auto searchable = lower_ascii(entry.id.value());
    if (entry.name) searchable += " " + lower_ascii(*entry.name);
    if (entry.description) searchable += " " + lower_ascii(*entry.description);
    if (entry.model_id)
      searchable += " " + lower_ascii(entry.model_id->value());
    for (const auto& tag : entry.tags)
      searchable += " " + lower_ascii(tag);
    m_all.push_back(
        {entry, std::move(label), std::move(searchable), unavailable});
  }
  m_filter.set_text({});
  apply_filter();
}

auto ProviderCharacterPickerDialog::on_result(
    std::function<void(std::optional<ProviderCharacterPickerResult>)> callback)
    -> void {
  m_on_result = std::move(callback);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Input routes.
auto ProviderCharacterPickerDialog::on_event(const termforge::Event& event)
    -> bool {
  if (std::holds_alternative<termforge::PasteEvent>(event)) {
    const auto& paste = std::get<termforge::PasteEvent>(event).text;
    auto prospective = m_filter.text();
    if (paste.size() > maximum_filter_bytes ||
        prospective.size() > maximum_filter_bytes - paste.size() ||
        paste.find_first_of("\n\r\t") != std::string::npos ||
        !detail::is_safe_utf8_text(paste)) {
      m_filter_error =
          "Filter paste rejected: use bounded UTF-8 without controls.";
      mark_dirty();
      return true;
    }
    prospective.insert(static_cast<std::size_t>(m_filter.cursor_pos()), paste);
    if (!m_filter.focused()) static_cast<void>(ring().focus(&m_filter));
    m_filter.set_text(std::move(prospective));
    m_filter_error.clear();
    apply_filter();
    return true;
  }
  if (const auto* key = std::get_if<termforge::KeyEvent>(&event);
      key != nullptr && key->action != termforge::KeyAction::Release &&
      !key->ctrl && !key->alt) {
    switch (key->key) {
      case termforge::Key::Up:
      case termforge::Key::Down:
      case termforge::Key::PageUp:
      case termforge::Key::PageDown:
      case termforge::Key::Enter:
        if (m_list.on_event(event)) return true;
        break;
      case termforge::Key::Char:
        if (key->ch > 0x10ffffU || detail::is_unsafe_text_control(key->ch)) {
          m_filter_error =
              "Filter input rejected: use UTF-8 without unsafe controls.";
          mark_dirty();
          return true;
        }
        if (key->ch >= 0x20U && key->ch != 0x7fU) {
          const auto bytes = encoded_size(key->ch);
          if (m_filter.text().size() > maximum_filter_bytes - bytes) {
            m_filter_error =
                "Filter input rejected: the 4096-byte limit was reached.";
            mark_dirty();
            return true;
          }
        }
        if (!m_filter.focused() && key->ch >= 0x20 && key->ch != 0x7f) {
          static_cast<void>(ring().focus(&m_filter));
          if (m_filter.on_event(event)) return true;
        }
        break;
      default: break;
    }
  }
  return Dialog::on_event(event);
}

auto ProviderCharacterPickerDialog::layout_content(const termforge::Rect area)
    -> void {
  m_metadata_area = {};
  if (area.h <= 0 || area.w <= 0) {
    m_filter.set_geometry({area.x, area.y, 0, 0});
    m_list.set_geometry({area.x, area.y, 0, 0});
    return;
  }
  m_filter.set_geometry({area.x, area.y, area.w, 1});
  const int remaining = std::max(0, area.h - 2);
  const int metadata_rows = remaining >= 7 ? std::min(7, remaining / 2) : 0;
  const int metadata_gap = metadata_rows > 0 ? 1 : 0;
  const int list_rows = std::max(0, remaining - metadata_rows - metadata_gap);
  m_list.set_geometry({area.x, area.y + 2, area.w, list_rows});
  if (metadata_rows > 0) {
    m_metadata_area = {area.x, area.y + 2 + list_rows + metadata_gap, area.w,
                       metadata_rows};
  }
}

auto ProviderCharacterPickerDialog::draw_content(termforge::Screen& screen)
    -> void {
  m_filter.draw(screen);
  m_list.draw(screen);
  const auto lines = metadata_lines();
  for (int row{}; row < m_metadata_area.h && std::cmp_less(row, lines.size());
       ++row) {
    screen.write_text(
        m_metadata_area.x, m_metadata_area.y + row,
        termforge::detail::truncate_to_width(
            lines[static_cast<std::size_t>(row)], m_metadata_area.w),
        fg(), bg());
  }
}

auto ProviderCharacterPickerDialog::on_escape() -> void {
  if (begin_result() && m_on_result) m_on_result(std::nullopt);
  close();
}

auto ProviderCharacterPickerDialog::apply_filter() -> void {
  const auto filter = lower_ascii(m_filter.text());
  std::optional<std::string> selected;
  if (m_list.selected() >= 0 &&
      static_cast<std::size_t>(m_list.selected()) < m_visible.size()) {
    const auto& choice =
        m_all[m_visible[static_cast<std::size_t>(m_list.selected())]];
    selected =
        choice.entry ? std::string{choice.entry->id.value()} : std::string{};
  }
  m_visible.clear();
  std::vector<std::string> labels;
  for (std::size_t index{}; index < m_all.size(); ++index) {
    const auto& choice = m_all[index];
    if (!choice.searchable.contains(filter)) continue;
    m_visible.push_back(index);
    labels.push_back(choice.label);
  }
  m_list.set_items(std::move(labels));
  int selection = m_visible.empty() ? -1 : 0;
  for (std::size_t index{}; index < m_visible.size(); ++index) {
    const auto& choice = m_all[m_visible[index]];
    const auto key =
        choice.entry ? std::string{choice.entry->id.value()} : std::string{};
    const bool current =
        choice.entry
            ? m_current_character && choice.entry->id == *m_current_character
            : !m_current_character;
    if ((selected && key == *selected) || (!selected && current)) {
      selection = static_cast<int>(index);
      break;
    }
  }
  m_list.set_selected(selection);
}

auto ProviderCharacterPickerDialog::choose(const int index) -> void {
  if (index < 0 || static_cast<std::size_t>(index) >= m_visible.size()) return;
  const auto& choice = m_all[m_visible[static_cast<std::size_t>(index)]];
  if (!choice.unavailable_reason.empty() || !begin_result()) return;
  ProviderCharacterPickerResult result;
  if (choice.entry) result.selection = *choice.entry;
  if (m_on_result) m_on_result(std::move(result));
  close();
}

auto ProviderCharacterPickerDialog::selected_choice() const noexcept
    -> const Choice* {
  const auto selected = m_list.selected();
  if (selected < 0 || static_cast<std::size_t>(selected) >= m_visible.size())
    return nullptr;
  return &m_all[m_visible[static_cast<std::size_t>(selected)]];
}

auto ProviderCharacterPickerDialog::metadata_lines() const
    -> std::vector<std::string> {
  if (!m_filter_error.empty()) {
    return {m_filter_error,
            "AIForge persona, workspace instructions, and tool authority are "
            "unchanged."};
  }
  const auto* choice = selected_choice();
  if (choice == nullptr) {
    return {"No provider characters match the current filter.",
            "The current AIForge persona and tool authority are unchanged."};
  }
  if (!choice->entry) {
    return {"Selected: provider default (no provider character)",
            "Compatibility: current model " +
                std::string{m_current_model.value()},
            "AIForge persona, workspace instructions, and tool authority are "
            "unchanged.",
            "Catalog: " + m_source_id};
  }

  const auto& entry = *choice->entry;
  std::vector<std::string> lines;
  std::string selected{"Selected: "};
  selected += entry.id.value();
  if (entry.name && *entry.name != entry.id.value())
    selected += " — " + flattened(*entry.name);
  if (m_current_character && entry.id == *m_current_character)
    selected += " [current]";
  lines.push_back(std::move(selected));
  lines.push_back(choice->unavailable_reason.empty()
                      ? "Compatibility: current model " +
                            std::string{m_current_model.value()}
                      : "Unavailable: " + choice->unavailable_reason);
  if (entry.description)
    lines.push_back("Description: " + flattened(*entry.description));
  if (!entry.tags.empty()) {
    std::string tags{"Tags:"};
    for (const auto& tag : entry.tags)
      tags += " " + flattened(tag);
    lines.push_back(std::move(tags));
  }
  lines.push_back("AIForge persona, workspace instructions, and tool authority "
                  "are unchanged.");
  lines.push_back("Catalog: " + m_source_id);
  return lines;
}

} // namespace aiforge::adapters
