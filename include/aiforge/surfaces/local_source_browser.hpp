#pragma once

#include <aiforge/runtime/local_source_grants.hpp>
#include <aiforge/runtime/local_source_worker.hpp>

namespace aiforge::surfaces {

struct LocalBrowserFolder {
  domain::LocalRootIdentity root;
  std::uint64_t lease_generation{};
  // Ephemeral user-supplied path. Presentation must escape it; it is never a
  // durable source identity or permission to reopen the directory.
  std::string path;
};
struct LocalBrowserState {
  std::optional<domain::SessionId> session_id;
  std::uint64_t session_epoch{};
  std::uint64_t selection_revision{1};
  std::vector<LocalBrowserFolder> folders;
  std::vector<domain::LocalSourceIdentity> selection;
  std::optional<runtime::LocalListResult> listing;
  std::optional<runtime::LocalPreviewResult> preview;
  bool granting{};
  bool reading{};
  bool preparing{};
  bool preparing_repository{};
  std::string message;
};

// One application-owned, owner-thread controller across session switches.
// Navigation, preview, explicit evidence selection and context preparation are
// separate operations. This boundary never submits inference or edits a draft.
// All filesystem work, including Add Folder and exact selection reads, uses
// the same bounded worker pool. Returned state contains raw identities; a TUI
// adapter must escape labels and preview text before terminal presentation.
class LocalSourceBrowser final {
 public:
  [[nodiscard]] static auto create(
      std::shared_ptr<runtime::LocalSourceGrantFactory> factory,
      domain::LocalSourceLimits limits = {}, std::size_t worker_capacity = 2)
      -> std::expected<std::unique_ptr<LocalSourceBrowser>,
                       domain::LocalSourceError>;
  // Share application-owned capacity and request allocation with other source
  // controllers. Browser lifecycle operations cancel only browser-owned work;
  // the application owns global session invalidation across controllers.
  [[nodiscard]] static auto create_with_worker(
      std::shared_ptr<runtime::LocalSourceGrantFactory> factory,
      std::shared_ptr<runtime::LocalSourceWorker> worker,
      domain::LocalSourceLimits limits = {})
      -> std::expected<std::unique_ptr<LocalSourceBrowser>,
                       domain::LocalSourceError>;
  ~LocalSourceBrowser();
  LocalSourceBrowser(const LocalSourceBrowser&) = delete;
  auto operator=(const LocalSourceBrowser&) -> LocalSourceBrowser& = delete;

  [[nodiscard]] auto activate_session(const domain::SessionId& session)
      -> std::expected<void, domain::LocalSourceError>;
  // End logical access before other application-owned services are torn down.
  [[nodiscard]] auto deactivate_session()
      -> std::expected<void, domain::LocalSourceError>;
  [[nodiscard]] auto add_folder(std::string absolute_path)
      -> std::expected<void, domain::LocalSourceError>;
  [[nodiscard]] auto remove_folder(const domain::LocalRootIdentity& root)
      -> std::expected<void, domain::LocalSourceError>;
  [[nodiscard]] auto navigate(const domain::LocalRootIdentity& root,
                              std::string directory = {},
                              std::string filename_filter = {})
      -> std::expected<void, domain::LocalSourceError>;
  [[nodiscard]] auto preview(const domain::LocalRootIdentity& root,
                             std::string relative_path)
      -> std::expected<void, domain::LocalSourceError>;
  // Always performs an exact bounded read. A prefix preview cannot add a file.
  // Replacing a selected path is explicit and changes its captured digest.
  [[nodiscard]] auto add_evidence(const domain::LocalRootIdentity& root,
                                  std::string relative_path)
      -> std::expected<void, domain::LocalSourceError>;
  [[nodiscard]] auto remove_evidence(const domain::LocalRootIdentity& root,
                                     std::string_view relative_path)
      -> std::expected<void, domain::LocalSourceError>;
  [[nodiscard]] auto clear_evidence()
      -> std::expected<void, domain::LocalSourceError>;
  // Applies only exact current completions. Failures preserve usable state.
  [[nodiscard]] auto poll() -> std::expected<void, domain::LocalSourceError>;
  auto cancel_browsing() -> void;

  [[nodiscard]] auto pending_evidence_selection() const noexcept -> bool;
  [[nodiscard]] auto prepare_selection()
      -> std::expected<runtime::LocalContextWorkToken,
                       domain::LocalSourceError>;
  // Recovery always uses this original sealed admission, never today's tray.
  [[nodiscard]] auto revalidate(const domain::LocalContextAdmission& original)
      -> std::expected<runtime::LocalContextWorkToken,
                       domain::LocalSourceError>;
  [[nodiscard]] auto poll_context(const runtime::LocalContextWorkToken& token)
      -> std::expected<std::optional<runtime::LocalContextWorkCompletion>,
                       domain::LocalSourceError>;
  auto cancel_context() -> void;
  [[nodiscard]] auto prepare_repository(
      const std::shared_ptr<runtime::RepositoryContextController>& controller,
      const std::variant<runtime::RepositoryContextRequest,
                         domain::RepositoryContextAdmission>& operation)
      -> std::expected<runtime::RepositoryContextWorkToken,
                       domain::LocalSourceError>;
  [[nodiscard]] auto poll_repository(
      const runtime::RepositoryContextWorkToken& token)
      -> std::expected<std::optional<runtime::RepositoryContextWorkCompletion>,
                       domain::LocalSourceError>;
  auto cancel_repository() -> void;
  [[nodiscard]] auto repository_preparation_ready() const noexcept -> bool;
  [[nodiscard]] auto state() const noexcept -> const LocalBrowserState&;
  [[nodiscard]] auto occupied_workers() const -> std::size_t;
  [[nodiscard]] auto occupied_grants() const -> std::size_t;

 private:
  struct Impl;
  explicit LocalSourceBrowser(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> m_impl;
};

} // namespace aiforge::surfaces
