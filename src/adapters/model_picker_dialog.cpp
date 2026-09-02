#include <aiforge/adapters/model_picker_dialog.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <ranges>
#include <string_view>
#include <utility>
#include <variant>

#include <termforge/widgets/detail/width.hpp>

namespace aiforge::adapters {
namespace {

constexpr std::array kCapabilities{
    model::Capability::tool_calling,
    model::Capability::vision,
    model::Capability::multiple_images,
    model::Capability::video_input,
    model::Capability::audio_input,
    model::Capability::reasoning,
    model::Capability::reasoning_effort,
    model::Capability::response_schema,
    model::Capability::log_probabilities,
    model::Capability::web_search,
    model::Capability::x_search,
    model::Capability::tee_attestation,
    model::Capability::end_to_end_encryption,
    model::Capability::optimized_for_code,
};

struct CapabilityFilter {
  model::Capability capability{model::Capability::tool_calling};
  std::optional<bool> required;
};

struct ParsedFilter {
  std::vector<std::string> terms;
  std::vector<CapabilityFilter> capabilities;
  std::string error;
};

[[nodiscard]] auto lower_ascii(const std::string_view value) -> std::string {
  std::string result;
  result.reserve(value.size());
  for (const auto character : value) {
    result.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
  }
  return result;
}

[[nodiscard]] auto capability_from_name(const std::string_view value)
    -> std::optional<model::Capability> {
  const auto found =
      std::ranges::find_if(kCapabilities, [value](const auto item) {
        return model::capability_name(item) == value;
      });
  return found == kCapabilities.end() ? std::nullopt : std::optional{*found};
}

[[nodiscard]] auto append_capability_filter(const std::string_view token,
                                            ParsedFilter& result) -> bool {
  const auto equals = token.find('=');
  if (equals == std::string_view::npos || equals == 4 ||
      equals + 1 == token.size()) {
    result.error = "capability filters use cap:<name>=true|false|unknown";
    return false;
  }
  const auto capability = capability_from_name(token.substr(4, equals - 4));
  if (!capability) {
    result.error = "unknown capability filter";
    return false;
  }
  if (std::ranges::any_of(result.capabilities, [&](const auto& filter) {
        return filter.capability == *capability;
      })) {
    result.error = "duplicate capability filter";
    return false;
  }
  const auto required = token.substr(equals + 1);
  if (required == "true") {
    result.capabilities.push_back({*capability, true});
  } else if (required == "false") {
    result.capabilities.push_back({*capability, false});
  } else if (required == "unknown") {
    result.capabilities.push_back({*capability, std::nullopt});
  } else {
    result.error = "capability filters use cap:<name>=true|false|unknown";
    return false;
  }
  return true;
}

[[nodiscard]] auto parse_filter(const std::string_view value) -> ParsedFilter {
  ParsedFilter result;
  std::size_t begin{};
  while (begin < value.size()) {
    while (begin < value.size() &&
           std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
      ++begin;
    }
    if (begin == value.size()) break;
    auto end = begin;
    while (end < value.size() &&
           std::isspace(static_cast<unsigned char>(value[end])) == 0) {
      ++end;
    }
    const auto token = lower_ascii(value.substr(begin, end - begin));
    begin = end;
    if (!token.starts_with("cap:")) {
      result.terms.push_back(token);
      continue;
    }
    if (!append_capability_filter(token, result)) return result;
  }
  return result;
}

[[nodiscard]] auto capability_support(const model::CatalogEntry& entry,
                                      const model::Capability capability)
    -> std::optional<bool> {
  const auto found = std::ranges::find(entry.capabilities, capability,
                                       &model::CapabilitySupport::capability);
  return found == entry.capabilities.end() ? std::nullopt : found->supported;
}

[[nodiscard]] auto support_text(const model::CatalogEntry& entry,
                                const model::Capability capability)
    -> std::string_view {
  const auto supported = capability_support(entry, capability);
  if (!supported) return "unknown";
  return *supported ? "true" : "false";
}

[[nodiscard]] auto coverage(const std::optional<model::Price>& price)
    -> std::string {
  if (!price) return "none";
  if (price->usd && price->diem) return "USD+diem";
  if (price->usd) return "USD";
  if (price->diem) return "diem";
  return "unknown";
}

[[nodiscard]] auto origin_text(const model::CatalogOrigin origin)
    -> std::string_view {
  switch (origin) {
    case model::CatalogOrigin::live: return "live";
    case model::CatalogOrigin::fresh_cache: return "fresh cache";
    case model::CatalogOrigin::stale_cache: return "stale cache";
  }
  return "unknown";
}

} // namespace

ModelPickerDialog::ModelPickerDialog() : Dialog("Select model") {
  set_text("Filter by ID/name or cap:<name>=true|false|unknown. Names: tools, "
           "vision, multi-image, video, audio, reasoning, reasoning-effort, "
           "schema, logprobs, web-search, x-search, tee, e2ee, code. Up/Down "
           "navigate; Enter selects; Tab changes focus; Escape cancels.");
  set_max_width(92);
  m_filter.set_placeholder("ID/name or cap:tools=true");
  m_filter.on_change([this](const std::string&) { apply_filter(); });
  m_list.on_select(
      [this](const int index, const std::string&) { choose(index); });
  add_child(&m_filter);
  add_child(&m_list);
}

auto ModelPickerDialog::set_models(const model::CatalogSnapshot& snapshot,
                                   std::optional<domain::ModelId> current)
    -> void {
  m_current = std::move(current);
  m_origin = snapshot.origin;
  m_all.clear();
  for (const auto& entry : snapshot.entries) {
    if (entry.type != "text") continue;
    std::string label{entry.id.value()};
    if (entry.name && *entry.name != entry.id.value())
      label += " — " + *entry.name;
    if (entry.offline) label += " (offline)";
    const bool missing_context = !entry.context_window_tokens;
    if (missing_context) label += " (invalid metadata)";
    if (m_current && entry.id == *m_current) label += " [current]";
    auto searchable = lower_ascii(entry.id.value());
    if (entry.name) searchable += " " + lower_ascii(*entry.name);
    m_all.push_back(Choice{entry, std::move(label), std::move(searchable),
                           entry.offline || missing_context});
  }
  std::ranges::sort(m_all, {}, [](const Choice& choice) {
    return std::string{choice.entry.id.value()};
  });
  m_filter.set_text({});
  apply_filter();
}

auto ModelPickerDialog::on_result(
    std::function<void(std::optional<domain::ModelId>)> callback) -> void {
  m_on_result = std::move(callback);
}

auto ModelPickerDialog::on_event(const termforge::Event& event) -> bool {
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

auto ModelPickerDialog::layout_content(const termforge::Rect area) -> void {
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

auto ModelPickerDialog::draw_content(termforge::Screen& screen) -> void {
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

auto ModelPickerDialog::on_escape() -> void {
  if (begin_result() && m_on_result) m_on_result(std::nullopt);
  close();
}

auto ModelPickerDialog::apply_filter() -> void {
  const auto parsed = parse_filter(m_filter.text());
  m_filter_error = parsed.error;
  std::optional<std::string> selected;
  if (m_list.selected() >= 0 &&
      static_cast<std::size_t>(m_list.selected()) < m_visible.size()) {
    selected = std::string{
        m_all[m_visible[static_cast<std::size_t>(m_list.selected())]]
            .entry.id.value()};
  }
  m_visible.clear();
  std::vector<std::string> labels;
  if (m_filter_error.empty()) {
    for (std::size_t index{}; index < m_all.size(); ++index) {
      const auto& choice = m_all[index];
      if (!std::ranges::all_of(parsed.terms, [&](const auto& term) {
            return choice.searchable.contains(term);
          })) {
        continue;
      }
      if (!std::ranges::all_of(parsed.capabilities, [&](const auto& filter) {
            return capability_support(choice.entry, filter.capability) ==
                   filter.required;
          })) {
        continue;
      }
      m_visible.push_back(index);
      labels.push_back(choice.label);
    }
  }
  m_list.set_items(std::move(labels));
  int selection = m_visible.empty() ? -1 : 0;
  for (std::size_t index{}; index < m_visible.size(); ++index) {
    const auto& id = m_all[m_visible[index]].entry.id;
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
  if (choice.unavailable) return;
  if (!begin_result()) return;
  if (m_on_result) m_on_result(choice.entry.id);
  close();
}

auto ModelPickerDialog::selected_entry() const noexcept
    -> const model::CatalogEntry* {
  const auto selected = m_list.selected();
  if (selected < 0 || static_cast<std::size_t>(selected) >= m_visible.size()) {
    return nullptr;
  }
  return &m_all[m_visible[static_cast<std::size_t>(selected)]].entry;
}

auto ModelPickerDialog::metadata_lines() const -> std::vector<std::string> {
  if (!m_filter_error.empty()) {
    return {"Filter error: " + m_filter_error,
            "Use cap:<name>=true|false|unknown.",
            "Names: tools vision multi-image video audio reasoning "
            "reasoning-effort",
            "schema logprobs web-search x-search tee e2ee code"};
  }
  const auto* entry = selected_entry();
  if (entry == nullptr) {
    return {"No models match the current filter.",
            "Capabilities: cap:<name>=true|false|unknown.",
            "Names: tools vision multi-image video audio reasoning "
            "reasoning-effort",
            "schema logprobs web-search x-search tee e2ee code"};
  }

  std::string selected{"Selected: "};
  selected += entry->id.value();
  if (entry->name && *entry->name != entry->id.value()) {
    selected += " — " + *entry->name;
  }
  if (m_current && entry->id == *m_current) selected += " [current]";

  const auto limit = [](const std::optional<std::uint64_t> value) {
    return value ? std::to_string(*value) : std::string{"unknown"};
  };
  auto limits = "Limits: context " + limit(entry->context_window_tokens) +
                " | output " + limit(entry->maximum_output_tokens) +
                " | status ";
  limits += entry->offline
                ? "offline"
                : (entry->context_window_tokens ? "online" : "invalid");

  auto capabilities =
      "Capabilities: tools " +
      std::string{support_text(*entry, model::Capability::tool_calling)} +
      " | reasoning " +
      std::string{support_text(*entry, model::Capability::reasoning)} +
      " | web " +
      std::string{support_text(*entry, model::Capability::web_search)};
  auto media =
      "Media: vision " +
      std::string{support_text(*entry, model::Capability::vision)} +
      " | multi-image " +
      std::string{support_text(*entry, model::Capability::multiple_images)} +
      " | video " +
      std::string{support_text(*entry, model::Capability::video_input)} +
      " | audio " +
      std::string{support_text(*entry, model::Capability::audio_input)};

  std::string base_pricing{"Pricing base: unknown"};
  std::string extended_pricing{"Pricing extended: none | generation none"};
  if (entry->pricing) {
    base_pricing =
        "Pricing base: input " + coverage(entry->pricing->base.input) +
        " | output " + coverage(entry->pricing->base.output) + " | cache-in " +
        coverage(entry->pricing->base.cache_input) + " | cache-write " +
        coverage(entry->pricing->base.cache_write);
    if (entry->pricing->extended) {
      extended_pricing =
          "Pricing extended@" +
          limit(entry->pricing->extended_threshold_tokens) + ": input " +
          coverage(entry->pricing->extended->input) + " | output " +
          coverage(entry->pricing->extended->output) + " | cache-in " +
          coverage(entry->pricing->extended->cache_input) + " | cache-write " +
          coverage(entry->pricing->extended->cache_write) + " | generation " +
          coverage(entry->pricing->generation);
    } else {
      extended_pricing = "Pricing extended: none | generation " +
                         coverage(entry->pricing->generation);
    }
  }
  return {std::move(selected),
          std::move(limits),
          std::move(capabilities),
          std::move(media),
          std::move(base_pricing),
          std::move(extended_pricing),
          "Catalog: " + std::string{origin_text(m_origin)}};
}

} // namespace aiforge::adapters
