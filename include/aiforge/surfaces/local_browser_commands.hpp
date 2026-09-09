#pragma once

#include <aiforge/surfaces/local_source_browser.hpp>
#include <variant>

namespace aiforge::surfaces {
struct LocalBrowseInspect {};
struct LocalBrowseAddFolder {
  std::string path;
};
struct LocalBrowseRemoveFolder {
  domain::LocalRootIdentity root;
};
struct LocalBrowseNavigate {
  domain::LocalRootIdentity root;
  std::string directory;
  std::string filter;
};
struct LocalBrowsePreview {
  domain::LocalRootIdentity root;
  std::string path;
};
struct LocalBrowseAddEvidence {
  domain::LocalRootIdentity root;
  std::string path;
};
struct LocalBrowseRemoveEvidence {
  domain::LocalRootIdentity root;
  std::string path;
};
struct LocalBrowseClearEvidence {};
struct LocalBrowseCancel {};
using LocalBrowserAction = std::variant<
    LocalBrowseInspect, LocalBrowseAddFolder, LocalBrowseRemoveFolder,
    LocalBrowseNavigate, LocalBrowsePreview, LocalBrowseAddEvidence,
    LocalBrowseRemoveEvidence, LocalBrowseClearEvidence, LocalBrowseCancel>;
[[nodiscard]] auto dispatch_local_browser_action(
    LocalSourceBrowser& browser, const LocalBrowserAction& action)
    -> std::expected<void, domain::LocalSourceError>;
// /files commands use one-based folder numbers from the current bounded state.
// Quoted paths are data, never shell commands. Null means a different command.
[[nodiscard]] auto parse_local_browser_command(std::string_view text,
                                               const LocalBrowserState& state)
    -> std::expected<std::optional<LocalBrowserAction>,
                     domain::LocalSourceError>;
} // namespace aiforge::surfaces
