#include <aiforge/detail/utf8_text.hpp>
#include <aiforge/surfaces/local_browser_commands.hpp>
#include <charconv>
#include <type_traits>

namespace aiforge::surfaces {
namespace {
using Code = domain::LocalSourceErrorCode;
using Error = domain::LocalSourceError;
using Action = std::expected<std::optional<LocalBrowserAction>, Error>;
auto failure(const char* message) -> std::unexpected<Error> {
  return std::unexpected(Error{Code::invalid_request, message});
}
auto quoted_word(std::string_view& text) -> std::expected<std::string, Error> {
  const char quote = text.front();
  text.remove_prefix(1);
  std::string value;
  while (!text.empty()) {
    const char next = text.front();
    text.remove_prefix(1);
    if (next == quote) {
      if (!text.empty() && text.front() != ' ' && text.front() != '\t')
        return failure("File command has an invalid quoted argument");
      return value;
    }
    if (next == '\\' && !text.empty() &&
        (text.front() == quote || text.front() == '\\')) {
      value.push_back(text.front());
      text.remove_prefix(1);
    } else
      value.push_back(next);
  }
  return failure("File command has an unclosed quoted argument");
}
auto word(std::string_view& text) -> std::expected<std::string, Error> {
  if (text.front() == '\'' || text.front() == '"') return quoted_word(text);
  const auto end = text.find_first_of(" \t");
  const auto value = text.substr(0, end);
  text.remove_prefix(value.size());
  if (value.find_first_of("\"'") != std::string_view::npos)
    return failure("Quote the complete file command argument");
  return std::string{value};
}
auto words(std::string_view text)
    -> std::expected<std::vector<std::string>, Error> {
  if (text.size() > 8192 ||
      (!text.empty() && !detail::is_safe_utf8_text(text)) ||
      text.find_first_of("\r\n") != std::string_view::npos)
    return failure("File command must be bounded single-line UTF-8 text");
  std::vector<std::string> result;
  while (!text.empty()) {
    const auto start = text.find_first_not_of(" \t");
    if (start == std::string_view::npos) break;
    text.remove_prefix(start);
    if (result.size() == 5)
      return failure("File command has too many arguments");
    auto value = word(text);
    if (!value) return std::unexpected(value.error());
    if (value->size() > 4096) return failure("File command path is too long");
    result.push_back(std::move(*value));
  }
  return result;
}
auto folder(const std::string& number, const LocalBrowserState& state)
    -> std::expected<domain::LocalRootIdentity, Error> {
  std::size_t index{};
  const auto parsed =
      std::from_chars(number.data(), std::string_view{number}.end(), index);
  if (parsed.ec != std::errc{} ||
      parsed.ptr != std::string_view{number}.end() || index == 0 ||
      index > state.folders.size() ||
      state.folders.size() > runtime::LocalSourceGrants::maximum_capacity)
    return failure("Choose a current one-based folder number");
  const auto& root = state.folders[index - 1].root;
  if (!domain::validate_local_root_identity(root))
    return failure("Current folder identity is invalid");
  return root;
}
auto rooted(const std::vector<std::string>& args,
            const LocalBrowserState& state) -> Action {
  if (args.size() < 2) return failure("File action requires a folder number");
  auto root = folder(args[1], state);
  if (!root) return std::unexpected(root.error());
  if (args[0] == "remove-folder" && args.size() == 2)
    return LocalBrowseRemoveFolder{std::move(*root)};
  if (args[0] == "open" && args.size() <= 4) {
    const auto path = args.size() >= 3 ? args[2] : std::string{};
    const auto filter = args.size() == 4 ? args[3] : std::string{};
    if (!domain::validate_local_relative_path(path, true) ||
        filter.size() > 255 || filter.find('/') != std::string::npos)
      return failure("File navigation path or filter is invalid");
    return LocalBrowseNavigate{std::move(*root), path, filter};
  }
  if (args.size() != 3 || !domain::validate_local_relative_path(args[2]))
    return failure("Use a folder number and one bounded relative file path");
  if (args[0] == "preview")
    return LocalBrowsePreview{std::move(*root), args[2]};
  if (args[0] == "add")
    return LocalBrowseAddEvidence{std::move(*root), args[2]};
  if (args[0] == "remove")
    return LocalBrowseRemoveEvidence{std::move(*root), args[2]};
  return failure("Use files "
                 "folders/add-folder/remove-folder/open/preview/add/remove/"
                 "tray/clear/cancel");
}
} // namespace

auto dispatch_local_browser_action(LocalSourceBrowser& browser,
                                   const LocalBrowserAction& action)
    -> std::expected<void, Error> {
  try {
    return std::visit(
        [&](const auto& value) -> std::expected<void, Error> {
          using Type = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<Type, LocalBrowseInspect>)
            return {};
          else if constexpr (std::is_same_v<Type, LocalBrowseAddFolder>)
            return browser.add_folder(value.path);
          else if constexpr (std::is_same_v<Type, LocalBrowseRemoveFolder>)
            return browser.remove_folder(value.root);
          else if constexpr (std::is_same_v<Type, LocalBrowseNavigate>)
            return browser.navigate(value.root, value.directory, value.filter);
          else if constexpr (std::is_same_v<Type, LocalBrowsePreview>)
            return browser.preview(value.root, value.path);
          else if constexpr (std::is_same_v<Type, LocalBrowseAddEvidence>)
            return browser.add_evidence(value.root, value.path);
          else if constexpr (std::is_same_v<Type, LocalBrowseRemoveEvidence>)
            return browser.remove_evidence(value.root, value.path);
          else if constexpr (std::is_same_v<Type, LocalBrowseClearEvidence>)
            return browser.clear_evidence();
          else {
            browser.cancel_browsing();
            return {};
          }
        },
        action);
  } catch (...) {
    return std::unexpected(Error{Code::internal_failure, "File action failed"});
  }
}
auto parse_local_browser_command(std::string_view text,
                                 const LocalBrowserState& state) -> Action {
  try {
    if (!text.starts_with("/files") ||
        (text.size() > 6 && text[6] != ' ' && text[6] != '\t'))
      return std::nullopt;
    auto args = words(text.substr(6));
    if (!args) return std::unexpected(args.error());
    if (args->empty() || (args->size() == 1 && (args->front() == "folders" ||
                                                args->front() == "tray")))
      return LocalBrowseInspect{};
    if (args->front() == "clear" && args->size() == 1)
      return LocalBrowseClearEvidence{};
    if (args->front() == "cancel" && args->size() == 1)
      return LocalBrowseCancel{};
    if (args->front() == "add-folder" && args->size() == 2) {
      if ((*args)[1].empty() || (*args)[1].front() != '/')
        return failure("Add Folder requires an absolute path");
      return LocalBrowseAddFolder{std::move((*args)[1])};
    }
    return rooted(*args, state);
  } catch (...) {
    return std::unexpected(
        Error{Code::internal_failure, "File command parsing failed"});
  }
}
} // namespace aiforge::surfaces
