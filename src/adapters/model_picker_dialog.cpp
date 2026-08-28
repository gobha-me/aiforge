#include <aiforge/adapters/model_picker_dialog.hpp>

#include <algorithm>
#include <cctype>
#include <ranges>
#include <utility>

namespace aiforge::adapters {
namespace {

[[nodiscard]] auto lower_ascii(const std::string_view value) -> std::string {
  std::string result;
  result.reserve(value.size());
  for (const auto character : value) {
    result.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
  }
  return result;
}

} // namespace

ModelPickerDialog::ModelPickerDialog() : Dialog("Select model") {
  set_text("Filter available text models. Tab moves to the list; Enter "
           "selects; Escape cancels.");
  set_max_width(76);
  m_filter.set_placeholder("Filter by model ID or name");
  m_filter.on_change([this](const std::string&) { apply_filter(); });
  m_list.on_select(
      [this](const int index, const std::string&) { choose(index); });
  add_child(&m_filter);
  add_child(&m_list);
}

auto ModelPickerDialog::set_models(const model::CatalogSnapshot& snapshot,
                                   const domain::ModelId& current) -> void {
  m_current = current;
  m_all.clear();
  for (const auto& entry : snapshot.entries) {
    if (entry.type != "text") continue;
    std::string label{entry.id.value()};
    if (entry.name && *entry.name != entry.id.value())
      label += " — " + *entry.name;
    if (entry.offline) label += " (offline)";
    if (entry.id == current) label += " [current]";
    auto searchable = lower_ascii(entry.id.value());
    if (entry.name) searchable += " " + lower_ascii(*entry.name);
    m_all.push_back(Choice{entry.id, std::move(label), std::move(searchable),
                           entry.offline});
  }
  std::ranges::sort(m_all, {}, [](const Choice& choice) {
    return std::string{choice.id.value()};
  });
  m_filter.set_text({});
  apply_filter();
}

auto ModelPickerDialog::on_result(
    std::function<void(std::optional<domain::ModelId>)> callback) -> void {
  m_on_result = std::move(callback);
}

auto ModelPickerDialog::layout_content(const termforge::Rect area) -> void {
  if (area.h <= 0 || area.w <= 0) {
    m_filter.set_geometry({area.x, area.y, 0, 0});
    m_list.set_geometry({area.x, area.y, 0, 0});
    return;
  }
  m_filter.set_geometry({area.x, area.y, area.w, 1});
  m_list.set_geometry({area.x, area.y + 2, area.w, std::max(0, area.h - 2)});
}

auto ModelPickerDialog::draw_content(termforge::Screen& screen) -> void {
  m_filter.draw(screen);
  m_list.draw(screen);
}

auto ModelPickerDialog::on_escape() -> void {
  if (begin_result() && m_on_result) m_on_result(std::nullopt);
  close();
}

auto ModelPickerDialog::apply_filter() -> void {
  const auto needle = lower_ascii(m_filter.text());
  std::optional<std::string> selected;
  if (m_list.selected() >= 0 &&
      static_cast<std::size_t>(m_list.selected()) < m_visible.size()) {
    selected = std::string{
        m_all[m_visible[static_cast<std::size_t>(m_list.selected())]]
            .id.value()};
  }
  m_visible.clear();
  std::vector<std::string> labels;
  for (std::size_t index{}; index < m_all.size(); ++index) {
    if (!needle.empty() && !m_all[index].searchable.contains(needle)) continue;
    m_visible.push_back(index);
    labels.push_back(m_all[index].label);
  }
  m_list.set_items(std::move(labels));
  int selection = m_visible.empty() ? -1 : 0;
  for (std::size_t index{}; index < m_visible.size(); ++index) {
    const auto& id = m_all[m_visible[index]].id;
    if ((selected && id.value() == *selected) ||
        (!selected && m_current && id == *m_current)) {
      selection = static_cast<int>(index);
      break;
    }
  }
  m_list.set_selected(selection);
}

auto ModelPickerDialog::choose(const int index) -> void {
  if (index < 0 || static_cast<std::size_t>(index) >= m_visible.size()) return;
  const auto& choice = m_all[m_visible[static_cast<std::size_t>(index)]];
  if (choice.offline) return;
  if (!begin_result()) return;
  if (m_on_result) m_on_result(choice.id);
  close();
}

} // namespace aiforge::adapters
