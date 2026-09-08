#include <aiforge/adapters/local_source_browser_dialog.hpp>
#include <aiforge/detail/utf8_text.hpp>
#include <algorithm>
#include <termforge/widgets/detail/width.hpp>

namespace aiforge::adapters {
namespace {
using namespace surfaces;
constexpr std::size_t input_limit = 4096;
auto escaped(std::string_view text, bool multiline = false) -> std::string {
  constexpr std::string_view hex = "0123456789abcdef";
  std::string result;
  for (std::size_t offset = 0; offset < text.size();) {
    const auto point = detail::decode_utf8_codepoint(text, offset);
    const auto count = point ? point->bytes : 1;
    const bool safe =
        point && !detail::is_unsafe_text_control(point->value) &&
        (point->value >= 32 || (multiline && point->value == '\n')) &&
        point->value != '\\';
    if (safe) {
      result.append(text.substr(offset, count));
    } else {
      for (std::size_t i = 0; i < count; ++i) {
        const auto byte = static_cast<unsigned char>(text[offset + i]);
        result += "\\x";
        result += hex[byte >> 4U];
        result += hex[byte & 15U];
      }
    }
    offset += count;
  }
  return result;
}
template <typename T>
auto selected(const std::vector<T>& values, int index) -> const T* {
  return index >= 0 && static_cast<std::size_t>(index) < values.size()
             ? &values[static_cast<std::size_t>(index)]
             : nullptr;
}
auto entry_label(const runtime::LocalDirectoryEntry& entry) -> std::string {
  const auto prefix =
      entry.kind == runtime::LocalEntryKind::directory      ? "[dir] "
      : entry.kind == runtime::LocalEntryKind::regular_file ? "[file] "
                                                            : "[unsupported] ";
  return prefix + escaped(entry.relative_path);
}
auto list_summary(const std::optional<runtime::LocalListResult>& listing)
    -> std::string {
  if (!listing) return "Choose a granted folder to list files.";
  return "Directory: " +
         std::string{termforge::detail::truncate_to_width(
             escaped(listing->directory.empty() ? "." : listing->directory),
             50)} +
         " | " +
         (listing->state == runtime::LocalListingState::complete ? "Complete"
                                                                 : "Partial") +
         " listing; scanned " + std::to_string(listing->scanned_entries) +
         ", filtered " + std::to_string(listing->skipped_filtered_entries) +
         ", unsupported " +
         std::to_string(listing->skipped_unsupported_entries) +
         ", over limit " + std::to_string(listing->skipped_capacity_entries);
}
} // namespace

LocalSourceBrowserDialog::LocalSourceBrowserDialog(LocalSourceBrowser& browser)
    : Dialog("Context - Local files"), m_browser(browser) {
  set_max_width(110);
  m_path.set_placeholder("Absolute folder path (session read access)");
  m_filter.set_placeholder("Filename substring; Enter applies filter");
  m_menu.set_menus(
      {{"Browse",
        {{"Files", [this] { show(View::files); }},
         {"Preview selected",
          [this] { open_entry(m_files.selected(), false); }},
         {"Parent directory", [this] { navigate(true); }},
         {"Refresh / filter", [this] { navigate(false); }},
         {"Cancel pending browsing",
          [this] { perform(LocalBrowseCancel{}); }}}},
       {"Evidence",
        {{"Add selected file",
          [this] { open_entry(m_files.selected(), true); }},
         {"Selection tray", [this] { show(View::tray); }},
         {"Remove selected evidence", [this] { remove_selected(); }},
         {"Clear evidence", [this] { perform(LocalBrowseClearEvidence{}); }}}},
       {"Folders",
        {{"Enter folder path",
          [this] {
            show(View::files);
            static_cast<void>(ring().focus(&m_path));
          }},
         {"Grant folder read access",
          [this] { perform(LocalBrowseAddFolder{m_path.text()}); }},
         {"Remove selected folder", [this] { remove_folder(); }}}},
       {"View",
        {{"Files", [this] { show(View::files); }},
         {"Preview", [this] { show(View::preview); }},
         {"Selection tray", [this] { show(View::tray); }}}}});
  m_folders.on_select(
      [this](int index, const std::string&) { open_folder(index); });
  m_files.on_select(
      [this](int index, const std::string&) { open_entry(index, false); });
  m_tray.on_select([this](int, const std::string&) {
    report("Use Remove to remove selected evidence");
  });
  m_grant.on_activate([this] { perform(LocalBrowseAddFolder{m_path.text()}); });
  m_apply_filter.on_activate([this] { navigate(false); });
  m_add.on_activate([this] { open_entry(m_files.selected(), true); });
  m_remove.on_activate([this] { remove_selected(); });
  m_close.on_activate([this] { on_escape(); });
  children();
  refresh();
}
auto LocalSourceBrowserDialog::children() -> void {
  auto* previous = ring().current();
  clear_children();
  add_child(&m_menu);
  if (m_wide || m_view == View::files) {
    add_child(&m_path);
    add_child(&m_grant);
    add_child(&m_filter);
    add_child(&m_apply_filter);
    add_child(&m_folders);
    add_child(&m_files);
    add_child(&m_add);
  }
  if (m_wide || m_view == View::preview) add_child(&m_preview);
  if (m_wide || m_view == View::tray) {
    add_child(&m_tray);
    add_child(&m_remove);
  }
  add_child(&m_close);
  static_cast<void>(ring().focus(previous));
}
auto LocalSourceBrowserDialog::show(View view) -> void {
  if (m_view == view) return;
  m_view = view;
  children();
  mark_dirty();
}
auto LocalSourceBrowserDialog::report(std::string message) -> void {
  m_status = std::move(message);
  set_text(escaped(m_status) + "\n" + m_body);
}
auto LocalSourceBrowserDialog::execute(const LocalBrowserAction& action)
    -> std::expected<void, domain::LocalSourceError> {
  try {
    const auto result = dispatch_local_browser_action(m_browser, action);
    refresh();
    if (result && std::holds_alternative<LocalBrowsePreview>(action))
      show(View::preview);
    if (result && (std::holds_alternative<LocalBrowseNavigate>(action) ||
                   std::holds_alternative<LocalBrowseAddFolder>(action)))
      show(View::files);
    if (!result)
      report(result.error().message);
    else
      report("Local file action accepted; browsing does not submit evidence");
    return result;
  } catch (...) {
    return std::unexpected(
        domain::LocalSourceError{domain::LocalSourceErrorCode::internal_failure,
                                 "Local file presentation failed"});
  }
}
auto LocalSourceBrowserDialog::perform(LocalBrowserAction action) -> void {
  if (const auto result = execute(action); !result)
    report(result.error().message);
}
auto LocalSourceBrowserDialog::open_folder(int index) -> void {
  const auto* root = selected(m_folder_roots, index);
  if (root == nullptr) {
    report("Choose a granted folder first");
    return;
  }
  perform(LocalBrowseNavigate{*root, {}, m_filter.text()});
}
auto LocalSourceBrowserDialog::open_entry(int index, bool add) -> void {
  if (!m_listing) {
    report("Choose a listed file or directory first");
    return;
  }
  const auto* entry = selected(m_listing->entries, index);
  if (entry == nullptr) {
    report("Choose a listed file or directory first");
    return;
  }
  const auto root = m_listing->token.root;
  if (entry->kind == runtime::LocalEntryKind::directory && !add) {
    perform(LocalBrowseNavigate{root, entry->relative_path, m_filter.text()});
  } else if (entry->kind != runtime::LocalEntryKind::regular_file) {
    report("Only regular files can be previewed or added as evidence");
  } else if (add) {
    perform(LocalBrowseAddEvidence{root, entry->relative_path});
  } else {
    perform(LocalBrowsePreview{root, entry->relative_path});
    show(View::preview);
  }
}
auto LocalSourceBrowserDialog::remove_selected() -> void {
  const auto* source = selected(m_selection, m_tray.selected());
  if (source == nullptr) {
    report("Choose evidence in the selection tray first");
    return;
  }
  perform(LocalBrowseRemoveEvidence{source->root, source->relative_path});
}
auto LocalSourceBrowserDialog::remove_folder() -> void {
  const auto* root = selected(m_folder_roots, m_folders.selected());
  if (root == nullptr) {
    report("Choose a granted folder first");
    return;
  }
  perform(LocalBrowseRemoveFolder{*root});
}
auto LocalSourceBrowserDialog::navigate(bool parent) -> void {
  if (!m_listing) {
    open_folder(m_folders.selected());
    return;
  }
  auto directory = m_listing->directory;
  if (parent) {
    const auto slash = directory.rfind('/');
    directory =
        slash == std::string::npos ? std::string{} : directory.substr(0, slash);
  }
  perform(LocalBrowseNavigate{m_listing->token.root, std::move(directory),
                              m_filter.text()});
}
auto LocalSourceBrowserDialog::refresh_folders() -> void {
  std::vector<domain::LocalRootIdentity> roots;
  std::vector<std::string> labels;
  for (const auto& folder : m_browser.state().folders) {
    roots.push_back(folder.root);
    labels.push_back(std::to_string(roots.size()) + ": " +
                     escaped(folder.path));
  }
  if (roots == m_folder_roots && labels == m_folder_labels) return;
  const auto* previous = selected(m_folder_roots, m_folders.selected());
  const auto position =
      previous != nullptr ? std::ranges::find(roots, *previous) : roots.end();
  const auto index =
      position == roots.end() ? 0 : static_cast<int>(position - roots.begin());
  m_folder_roots = std::move(roots);
  m_folder_labels = std::move(labels);
  m_folders.set_items(m_folder_labels);
  m_folders.set_selected(index);
}
auto LocalSourceBrowserDialog::refresh_listing() -> void {
  const auto& current = m_browser.state().listing;
  if (current == m_listing) return;
  std::string previous;
  if (m_listing && current && m_listing->token.root == current->token.root &&
      m_listing->directory == current->directory) {
    if (const auto* entry = selected(m_listing->entries, m_files.selected()))
      previous = entry->relative_path;
  }
  m_listing = current;
  std::vector<std::string> labels;
  int index{};
  if (m_listing)
    for (const auto& entry : m_listing->entries) {
      if (entry.relative_path == previous)
        index = static_cast<int>(labels.size());
      labels.push_back(entry_label(entry));
    }
  m_files.set_items(std::move(labels));
  m_files.set_selected(index);
}
auto LocalSourceBrowserDialog::refresh_tray() -> void {
  const auto& current = m_browser.state().selection;
  const auto* previous = selected(m_selection, m_tray.selected());
  const auto position = previous != nullptr
                            ? std::ranges::find(current, *previous)
                            : current.end();
  const auto index = position == current.end()
                         ? 0
                         : static_cast<int>(position - current.begin());
  std::vector<std::string> labels;
  for (const auto& source : current) {
    const auto root = std::ranges::find(m_folder_roots, source.root);
    const auto number = root == m_folder_roots.end()
                            ? "?"
                            : std::to_string(root - m_folder_roots.begin() + 1);
    labels.push_back(number + ": " + escaped(source.relative_path) + " (" +
                     std::to_string(source.content_digest.byte_size) +
                     " bytes)");
  }
  if (current == m_selection && labels == m_tray_labels) return;
  m_selection = current;
  m_tray_labels = labels;
  m_tray.set_items(std::move(labels));
  m_tray.set_selected(index);
}
auto LocalSourceBrowserDialog::refresh_preview() -> void {
  const auto& current = m_browser.state().preview;
  if (current == m_preview_value) return;
  m_preview_value = current;
  m_preview.clear();
  if (!current) return;
  m_preview.append(escaped(current->relative_path) + " | " +
                   (current->state == runtime::LocalPreviewState::prefix
                        ? "Prefix preview"
                        : "Complete preview") +
                   " of " + std::to_string(current->observed_file_bytes) +
                   " bytes; not selected automatically");
  m_preview.append(escaped(current->text, true));
  m_preview.scroll(-1000000);
}
auto LocalSourceBrowserDialog::refresh() -> void {
  const auto& state = m_browser.state();
  if (m_session_epoch != state.session_epoch) {
    m_session_epoch = state.session_epoch;
    m_path.set_text({});
    m_filter.set_text({});
    m_browser_message.clear();
    report(
        "Folder access is session scoped; Add folder grants read access only");
  }
  refresh_folders();
  refresh_listing();
  refresh_tray();
  refresh_preview();
  std::uint64_t bytes{};
  for (const auto& source : state.selection)
    bytes += source.content_digest.byte_size;
  m_body = list_summary(state.listing) +
           "\nSelection tray: " + std::to_string(state.selection.size()) +
           " files, " + std::to_string(bytes) +
           " bytes; inclusion is decided at submission.";
  if (state.granting) m_body += "\nGranting folder read access...";
  if (state.reading) m_body += "\nReading...";
  if (state.preparing) m_body += "\nPreparing exact context...";
  if (state.message != m_browser_message) {
    m_browser_message = state.message;
    if (!state.message.empty()) report(state.message);
  }
  set_text(escaped(m_status) + "\n" + m_body);
  mark_dirty();
}
auto LocalSourceBrowserDialog::status() const noexcept -> const std::string& {
  return m_status;
}
auto LocalSourceBrowserDialog::display_text() const noexcept
    -> const std::string& {
  return m_body;
}
auto LocalSourceBrowserDialog::content_rows() const -> int {
  return 24;
}
auto LocalSourceBrowserDialog::content_cols() const -> int {
  return 104;
}
auto LocalSourceBrowserDialog::layout_wide(termforge::Rect area) -> void {
  const auto left = area.w / 2;
  const auto tray_rows = std::min(4, area.h / 3);
  const auto upper = std::max(0, area.h - tray_rows);
  const auto folder_rows = std::min(3, upper / 3);
  m_folders.set_geometry({area.x, area.y, left, folder_rows});
  m_files.set_geometry(
      {area.x, area.y + folder_rows, left, upper - folder_rows});
  m_preview.set_geometry({area.x + left, area.y, area.w - left, upper});
  m_tray.set_geometry({area.x, area.y + upper, area.w, tray_rows});
}
auto LocalSourceBrowserDialog::layout_narrow(termforge::Rect area) -> void {
  m_folders.set_geometry({});
  m_files.set_geometry({});
  m_preview.set_geometry({});
  m_tray.set_geometry({});
  if (m_view == View::files) {
    const auto folders = std::min(3, area.h / 3);
    m_folders.set_geometry({area.x, area.y, area.w, folders});
    m_files.set_geometry({area.x, area.y + folders, area.w, area.h - folders});
  } else if (m_view == View::preview)
    m_preview.set_geometry(area);
  else
    m_tray.set_geometry(area);
}
auto LocalSourceBrowserDialog::layout_content(termforge::Rect area) -> void {
  const bool wide = area.w >= 72;
  if (wide != m_wide) {
    m_wide = wide;
    children();
  }
  const auto row = area.h > 0 ? 1 : 0;
  m_menu.set_geometry({area.x, area.y, area.w, row});
  auto y = area.y + row;
  auto remaining = std::max(0, area.h - row);
  m_path.set_geometry({});
  m_grant.set_geometry({});
  m_filter.set_geometry({});
  m_apply_filter.set_geometry({});
  if (m_wide || m_view == View::files) {
    const auto width = std::min(18, area.w / 2);
    const auto line = remaining > 1 ? 1 : 0;
    m_path.set_geometry({area.x, y, area.w - width, line});
    m_grant.set_geometry({area.x + area.w - width, y, width, line});
    y += line;
    remaining -= line;
    const auto filter_line = remaining > 1 ? 1 : 0;
    m_filter.set_geometry({area.x, y, area.w - width, filter_line});
    m_apply_filter.set_geometry(
        {area.x + area.w - width, y, width, filter_line});
    y += filter_line;
    remaining -= filter_line;
  }
  const auto buttons = remaining > 0 ? 1 : 0;
  termforge::Rect content{area.x, y, area.w, remaining - buttons};
  if (m_wide)
    layout_wide(content);
  else
    layout_narrow(content);
  y += content.h;
  const auto width = area.w / 3;
  m_add.set_geometry({area.x, y, width, buttons});
  m_remove.set_geometry({area.x + width, y, width, buttons});
  m_close.set_geometry(
      {area.x + (2 * width), y, area.w - (2 * width), buttons});
}
auto LocalSourceBrowserDialog::draw_content(termforge::Screen& screen) -> void {
  m_folders.draw(screen);
  m_files.draw(screen);
  m_preview.draw(screen);
  m_tray.draw(screen);
  if (m_wide || m_view == View::files) {
    m_path.draw(screen);
    m_grant.draw(screen);
    m_filter.draw(screen);
    m_apply_filter.draw(screen);
    m_add.draw(screen);
  }
  if (m_wide || m_view == View::tray) m_remove.draw(screen);
  m_close.draw(screen);
  // The bounded summary is the dialog body; menus paint last over children.
  m_menu.draw(screen);
}
auto LocalSourceBrowserDialog::input_event(const termforge::Event& event)
    -> bool {
  auto* input = m_path.focused()     ? &m_path
                : m_filter.focused() ? &m_filter
                                     : nullptr;
  if (input == nullptr) return false;
  if (const auto* paste = std::get_if<termforge::PasteEvent>(&event)) {
    if (paste->text.size() >
            input_limit - std::min(input_limit, input->text().size()) ||
        (!paste->text.empty() && !detail::is_safe_utf8_text(paste->text)) ||
        paste->text.find_first_of("\r\n\t") != std::string::npos) {
      report("Folder path/filter must be bounded printable text");
      return true;
    }
    auto value = input->text();
    value.insert(static_cast<std::size_t>(input->cursor_pos()), paste->text);
    input->set_text(std::move(value));
    return true;
  }
  const auto* key = std::get_if<termforge::KeyEvent>(&event);
  if (key == nullptr || key->action == termforge::KeyAction::Release)
    return false;
  if (key->key == termforge::Key::Enter) {
    if (input == &m_path)
      perform(LocalBrowseAddFolder{m_path.text()});
    else
      navigate(false);
    return true;
  }
  if (key->key == termforge::Key::Char &&
      input->text().size() > input_limit - 4) {
    report("Folder path/filter exceeds its text limit");
    return true;
  }
  return false;
}
auto LocalSourceBrowserDialog::on_event(const termforge::Event& event) -> bool {
  if (input_event(event)) return true;
  return Dialog::on_event(event);
}
auto LocalSourceBrowserDialog::on_escape() -> void {
  m_browser.cancel_browsing();
  report("Local files closed; evidence tray retained");
  if (begin_result()) close();
}
} // namespace aiforge::adapters
