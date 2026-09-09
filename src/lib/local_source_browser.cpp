#include <aiforge/surfaces/local_source_browser.hpp>

#include <aiforge/detail/utf8_text.hpp>
#include <algorithm>
#include <limits>
#include <utility>

namespace aiforge::surfaces {
namespace {
using Code = domain::LocalSourceErrorCode;
using Error = domain::LocalSourceError;
using Status = std::expected<void, Error>;
auto fail(Code code, std::string message) -> std::unexpected<Error> {
  return std::unexpected(Error{code, std::move(message)});
}
auto worker_error(const runtime::LocalSourceWorkerError& error)
    -> std::unexpected<Error> {
  using WorkerCode = runtime::LocalSourceWorkerErrorCode;
  switch (error.code) {
    case WorkerCode::busy:
      return fail(Code::resource_exhausted, "Local file workers are busy");
    case WorkerCode::resource_exhausted:
      return fail(Code::resource_exhausted,
                  "Source worker request identities are exhausted");
    case WorkerCode::stale_request:
      return fail(Code::stale_lease, "Local file work is no longer current");
    case WorkerCode::invalid_request:
      return fail(Code::invalid_request, "Local file request is invalid");
    case WorkerCode::internal_failure:
      return fail(Code::internal_failure, "Local file worker could not start");
  }
  return fail(Code::internal_failure, "Local file work failed");
}
auto increment(std::uint64_t& value) -> Status {
  if (value == std::numeric_limits<std::uint64_t>::max())
    return fail(Code::resource_exhausted, "Local file revision limit reached");
  ++value;
  return {};
}
} // namespace

struct LocalSourceBrowser::Impl {
  enum class ReadPurpose { listing, preview, selection };
  struct GrantWork {
    runtime::LocalFolderGrantRequest request;
    std::unique_ptr<runtime::LocalSourceGrantReservation> reservation;
  };
  struct ReadWork {
    runtime::LocalSourceRequestToken token;
    ReadPurpose purpose;
  };
  struct ContextWork {
    runtime::LocalContextWorkToken token;
    bool recovery{};
  };
  domain::LocalSourceLimits limits;
  std::shared_ptr<runtime::LocalSourceGrantFactory> factory;
  std::shared_ptr<runtime::LocalSourceGrants> grants;
  std::shared_ptr<runtime::LocalContextController> context;
  std::shared_ptr<runtime::LocalSourceWorker> worker;
  LocalBrowserState state;
  std::uint64_t lease_generation{};
  std::optional<GrantWork> granting;
  std::optional<ReadWork> reading;
  std::optional<ContextWork> preparing;
  std::optional<runtime::RepositoryContextWorkToken> repository_work;

  auto next_request() -> std::expected<std::uint64_t, Error> {
    auto result = worker->allocate_request_id();
    if (!result) return worker_error(result.error());
    return *result;
  }
  auto resolve(const domain::LocalRootIdentity& root)
      -> std::expected<runtime::LocalContextGrant, Error> {
    if (!state.session_id)
      return fail(Code::unavailable, "Select a session before browsing files");
    auto result = grants->resolve(*state.session_id, root);
    if (!result) return std::unexpected(result.error());
    if (!*result)
      return fail(Code::stale_lease, "Add the folder to grant read access");
    return std::move(**result);
  }
  auto cancel_read() -> void {
    if (reading) {
      [[maybe_unused]] const auto cancelled = worker->cancel(reading->token);
    }
    reading.reset();
    state.reading = false;
  }
  auto cancel_context() -> void {
    if (preparing) {
      [[maybe_unused]] const auto cancelled = worker->cancel(preparing->token);
    }
    preparing.reset();
    state.preparing = false;
  }
  auto cancel_repository() -> void {
    if (repository_work) {
      [[maybe_unused]] const auto cancelled = worker->cancel(*repository_work);
    }
    repository_work.reset();
    state.preparing_repository = false;
  }
  auto cancel_browsing() -> void {
    if (granting) {
      [[maybe_unused]] const auto cancelled =
          worker->cancel(granting->request.token);
    }
    granting.reset();
    state.granting = false;
    cancel_read();
  }
  auto selection_changed(std::vector<domain::LocalSourceIdentity> selection)
      -> Status {
    if (auto valid = domain::validate_local_source_selection(selection, limits);
        !valid)
      return valid;
    if (selection == state.selection) return {};
    if (auto changed = increment(state.selection_revision); !changed)
      return changed;
    cancel_read();
    // A new tray cannot replace or cancel the fixed membership of recovery.
    if (preparing && !preparing->recovery) cancel_context();
    state.selection = std::move(selection);
    return {};
  }
  auto begin_read(const domain::LocalRootIdentity& root, std::string path,
                  ReadPurpose purpose, std::string filter = {}) -> Status {
    if (!state.session_id)
      return fail(Code::unavailable, "Select a session before browsing files");
    auto grant = resolve(root);
    if (!grant) return std::unexpected(grant.error());
    auto id = next_request();
    if (!id) return std::unexpected(id.error());
    const runtime::LocalSourceRequestToken token{*state.session_id, root,
                                                 grant->lease_generation, *id,
                                                 state.selection_revision};
    auto request_limits = limits;
    if (purpose == ReadPurpose::selection)
      request_limits.maximum_preview_bytes = limits.maximum_file_bytes;
    std::optional<runtime::LocalSourceWorkRequest> request;
    if (purpose == ReadPurpose::listing) {
      runtime::LocalListRequest list{token, std::move(path), request_limits,
                                     std::move(filter)};
      if (auto valid = runtime::validate_local_list_request(list); !valid)
        return valid;
      request = std::move(list);
    } else {
      runtime::LocalPreviewRequest preview{
          token, std::move(path),
          purpose == ReadPurpose::selection
              ? runtime::LocalPreviewMode::exact
              : runtime::LocalPreviewMode::bounded_prefix,
          request_limits};
      if (auto valid = runtime::validate_local_preview_request(preview); !valid)
        return valid;
      request = std::move(preview);
    }
    cancel_read();
    if (purpose == ReadPurpose::selection && preparing && !preparing->recovery)
      cancel_context();
    if (auto started = worker->submit(grant->reader, std::move(*request));
        !started)
      return worker_error(started.error());
    reading = ReadWork{token, purpose};
    state.reading = true;
    return {};
  }
  auto apply_selection(const runtime::LocalPreviewResult& preview) -> Status {
    if (preview.state != runtime::LocalPreviewState::complete ||
        !preview.source)
      return fail(Code::invalid_result, "Exact file evidence is unavailable");
    auto selection = state.selection;
    const auto found = std::ranges::find_if(selection, [&](const auto& source) {
      return source.root == preview.source->root &&
             source.relative_path == preview.source->relative_path;
    });
    if (found == selection.end())
      selection.push_back(*preview.source);
    else
      *found = *preview.source;
    return selection_changed(std::move(selection));
  }
  auto poll_grant() -> Status {
    if (!granting) return {};
    auto completion = worker->poll(granting->request.token);
    if (!completion) return worker_error(completion.error());
    if (!*completion) return {};
    auto work = std::move(*granting);
    granting.reset();
    state.granting = false;
    if (!(**completion).result)
      return std::unexpected((**completion).result.error());
    auto grant = work.reservation->accept(*(**completion).result);
    if (!grant) return std::unexpected(grant.error());
    const auto existing =
        std::ranges::find_if(state.folders, [&](const auto& folder) {
          return folder.root == grant->root;
        });
    LocalBrowserFolder folder{grant->root, grant->lease_generation,
                              std::move(work.request.absolute_path)};
    if (existing == state.folders.end())
      state.folders.push_back(std::move(folder));
    else
      *existing = std::move(folder);
    // Installing/replacing authority never resumes a blocked inference itself.
    cancel_context();
    state.message = "Folder added; select files to use as evidence";
    return {};
  }
  auto poll_read() -> Status {
    if (!reading) return {};
    auto completion = worker->poll(reading->token);
    if (!completion) return worker_error(completion.error());
    if (!*completion) return {};
    const auto work = *reading;
    reading.reset();
    state.reading = false;
    if (!(**completion).result)
      return std::unexpected((**completion).result.error());
    auto grant = resolve(work.token.root);
    if (!grant || grant->lease_generation != work.token.lease_generation ||
        work.token.selection_revision != state.selection_revision)
      return fail(Code::stale_lease, "Local file result is no longer current");
    auto& result = *(**completion).result;
    if (work.purpose == ReadPurpose::listing) {
      auto* list = std::get_if<runtime::LocalListResult>(&result);
      if (list == nullptr)
        return fail(Code::invalid_result, "Invalid local listing result");
      state.listing = std::move(*list);
      state.message =
          state.listing->state == runtime::LocalListingState::partial
              ? "Partial listing; narrow the folder or filter"
              : "Folder listing ready";
      return {};
    }
    auto* preview = std::get_if<runtime::LocalPreviewResult>(&result);
    if (preview == nullptr)
      return fail(Code::invalid_result, "Invalid local preview result");
    if (work.purpose == ReadPurpose::selection) {
      if (auto applied = apply_selection(*preview); !applied) return applied;
      state.message = "File added to evidence; submit your message when ready";
    } else {
      state.message =
          preview->state == runtime::LocalPreviewState::prefix
              ? "Preview prefix; Add reads the complete bounded file"
              : "Preview ready; Add selects this file as evidence";
    }
    state.preview = std::move(*preview);
    return {};
  }
  auto begin_context(
      std::variant<runtime::LocalContextRequest, domain::LocalContextAdmission>
          input,
      std::uint64_t revision, bool recovery)
      -> std::expected<runtime::LocalContextWorkToken, Error> {
    if (!state.session_id)
      return fail(Code::unavailable,
                  "Select a session before preparing context");
    if (preparing)
      return fail(Code::resource_exhausted,
                  "File context is already preparing");
    auto id = next_request();
    if (!id) return std::unexpected(id.error());
    runtime::LocalContextWorkToken token{*state.session_id, state.session_epoch,
                                         *id, revision};
    if (auto started = worker->submit(context, {token, std::move(input)});
        !started)
      return worker_error(started.error());
    preparing = ContextWork{token, recovery};
    state.preparing = true;
    return token;
  }
};

LocalSourceBrowser::LocalSourceBrowser(std::unique_ptr<Impl> impl)
    : m_impl(std::move(impl)) {
}
LocalSourceBrowser::~LocalSourceBrowser() {
  [[maybe_unused]] const auto ended = deactivate_session();
}
auto LocalSourceBrowser::deactivate_session() -> Status {
  try {
    if (m_impl->state.session_id) {
      auto invalidated =
          m_impl->grants->invalidate_session(*m_impl->state.session_id);
      if (!invalidated) return invalidated;
    }
    m_impl->cancel_browsing();
    m_impl->cancel_context();
    m_impl->cancel_repository();
    // Preserve the application-wide epoch and request/generation high-water
    // marks so even reopening the same session cannot revive a late result.
    m_impl->state.session_id.reset();
    m_impl->state.folders.clear();
    m_impl->state.selection.clear();
    m_impl->state.listing.reset();
    m_impl->state.preview.reset();
    m_impl->state.message.clear();
    return {};
  } catch (...) {
    return fail(Code::internal_failure, "Local browser deactivation failed");
  }
}

auto LocalSourceBrowser::create(
    std::shared_ptr<runtime::LocalSourceGrantFactory> factory,
    domain::LocalSourceLimits limits, std::size_t worker_capacity)
    -> std::expected<std::unique_ptr<LocalSourceBrowser>, Error> {
  try {
    auto worker = runtime::LocalSourceWorker::create(worker_capacity);
    if (!worker) return worker_error(worker.error());
    return create_with_worker(
        std::move(factory),
        std::shared_ptr<runtime::LocalSourceWorker>{std::move(*worker)},
        limits);
  } catch (...) {
    return fail(Code::internal_failure, "Local file browser could not start");
  }
}
auto LocalSourceBrowser::create_with_worker(
    std::shared_ptr<runtime::LocalSourceGrantFactory> factory,
    std::shared_ptr<runtime::LocalSourceWorker> worker,
    domain::LocalSourceLimits limits)
    -> std::expected<std::unique_ptr<LocalSourceBrowser>, Error> {
  try {
    if (!worker || !factory || !factory->guarantees_pinned_read_only_sources())
      return fail(Code::invalid_request,
                  "Pinned local folder access or worker is unavailable");
    if (auto valid = domain::validate_local_source_limits(limits); !valid)
      return std::unexpected(valid.error());
    auto grants = runtime::LocalSourceGrants::create(limits.maximum_roots);
    if (!grants) return std::unexpected(grants.error());
    auto impl = std::make_unique<Impl>();
    impl->limits = limits;
    impl->factory = std::move(factory);
    impl->grants = *grants;
    impl->context =
        std::make_shared<runtime::LocalContextController>(*grants, limits);
    impl->worker = std::move(worker);
    return std::unique_ptr<LocalSourceBrowser>(
        new LocalSourceBrowser(std::move(impl)));
  } catch (...) {
    return fail(Code::internal_failure, "Local file browser could not start");
  }
}
auto LocalSourceBrowser::activate_session(const domain::SessionId& session)
    -> Status {
  try {
    if (session.value().empty() || !detail::is_safe_utf8_text(session.value()))
      return fail(Code::invalid_request,
                  "Local browser session identity is invalid");
    auto epoch = m_impl->state.session_epoch;
    if (auto changed = increment(epoch); !changed) return changed;
    if (auto activated = m_impl->grants->activate_session(session, epoch);
        !activated)
      return activated;
    m_impl->cancel_browsing();
    m_impl->cancel_context();
    m_impl->cancel_repository();
    m_impl->state = {};
    m_impl->state.session_id = session;
    m_impl->state.session_epoch = epoch;
    return {};
  } catch (...) {
    return fail(Code::internal_failure, "Local browser session failed");
  }
}
auto LocalSourceBrowser::add_folder(std::string absolute_path) -> Status {
  try {
    if (!m_impl->state.session_id)
      return fail(Code::unavailable, "Select a session before browsing files");
    if (m_impl->granting)
      return fail(Code::resource_exhausted,
                  "A folder grant is already pending");
    auto id = m_impl->next_request();
    if (!id) return std::unexpected(id.error());
    if (auto next = increment(m_impl->lease_generation); !next) return next;
    runtime::LocalFolderGrantRequest request{
        {*m_impl->state.session_id, m_impl->state.session_epoch, *id},
        std::move(absolute_path),
        m_impl->lease_generation,
        m_impl->limits};
    auto reservation = m_impl->grants->reserve(request, m_impl->factory);
    if (!reservation) return std::unexpected(reservation.error());
    if (auto started =
            m_impl->worker->submit((*reservation)->factory(), request);
        !started)
      return worker_error(started.error());
    m_impl->granting =
        Impl::GrantWork{std::move(request), std::move(*reservation)};
    m_impl->state.granting = true;
    return {};
  } catch (...) {
    return fail(Code::internal_failure, "Folder grant failed");
  }
}
auto LocalSourceBrowser::remove_folder(const domain::LocalRootIdentity& root)
    -> Status {
  try {
    auto& state = m_impl->state;
    if (!m_impl->state.session_id)
      return fail(Code::unavailable, "Select a session before browsing files");
    const auto found = std::ranges::find_if(
        state.folders, [&](const auto& folder) { return folder.root == root; });
    if (found == state.folders.end())
      return fail(Code::unavailable, "Folder is not granted");
    auto selection = state.selection;
    std::erase_if(selection,
                  [&](const auto& source) { return source.root == root; });
    if (selection != state.selection &&
        state.selection_revision == std::numeric_limits<std::uint64_t>::max())
      return fail(Code::resource_exhausted,
                  "Local file revision limit reached");
    if (auto revoked = m_impl->grants->revoke(*state.session_id, root,
                                              found->lease_generation);
        !revoked)
      return revoked;
    m_impl->cancel_browsing();
    m_impl->cancel_context();
    m_impl->cancel_repository();
    state.folders.erase(found);
    if (state.listing && state.listing->token.root == root)
      state.listing.reset();
    if (state.preview && state.preview->token.root == root)
      state.preview.reset();
    return m_impl->selection_changed(std::move(selection));
  } catch (...) {
    return fail(Code::internal_failure, "Folder removal failed");
  }
}
auto LocalSourceBrowser::navigate(const domain::LocalRootIdentity& root,
                                  std::string directory,
                                  std::string filename_filter) -> Status {
  try {
    return m_impl->begin_read(root, std::move(directory),
                              Impl::ReadPurpose::listing,
                              std::move(filename_filter));
  } catch (...) {
    return fail(Code::internal_failure, "Folder navigation failed");
  }
}
auto LocalSourceBrowser::preview(const domain::LocalRootIdentity& root,
                                 std::string relative_path) -> Status {
  try {
    return m_impl->begin_read(root, std::move(relative_path),
                              Impl::ReadPurpose::preview);
  } catch (...) {
    return fail(Code::internal_failure, "File preview failed");
  }
}
auto LocalSourceBrowser::add_evidence(const domain::LocalRootIdentity& root,
                                      std::string relative_path) -> Status {
  try {
    return m_impl->begin_read(root, std::move(relative_path),
                              Impl::ReadPurpose::selection);
  } catch (...) {
    return fail(Code::internal_failure, "File selection failed");
  }
}
auto LocalSourceBrowser::remove_evidence(const domain::LocalRootIdentity& root,
                                         std::string_view relative_path)
    -> Status {
  try {
    auto selection = m_impl->state.selection;
    if (std::erase_if(selection, [&](const auto& source) {
          return source.root == root && source.relative_path == relative_path;
        }) == 0)
      return fail(Code::unavailable, "File is not selected");
    return m_impl->selection_changed(std::move(selection));
  } catch (...) {
    return fail(Code::internal_failure, "Evidence removal failed");
  }
}
auto LocalSourceBrowser::clear_evidence() -> Status {
  try {
    m_impl->cancel_read();
    return m_impl->selection_changed({});
  } catch (...) {
    return fail(Code::internal_failure, "Evidence removal failed");
  }
}
auto LocalSourceBrowser::poll() -> Status {
  try {
    auto grant = m_impl->poll_grant();
    auto read = m_impl->poll_read();
    if (!grant) {
      m_impl->state.message = grant.error().message;
      return grant;
    }
    if (!read) {
      m_impl->state.message = read.error().message;
      return read;
    }
    return {};
  } catch (...) {
    return fail(Code::internal_failure, "Local file completion failed");
  }
}
auto LocalSourceBrowser::cancel_browsing() -> void {
  m_impl->cancel_browsing();
}
auto LocalSourceBrowser::pending_evidence_selection() const noexcept -> bool {
  return m_impl->reading &&
         m_impl->reading->purpose == Impl::ReadPurpose::selection;
}
auto LocalSourceBrowser::prepare_selection()
    -> std::expected<runtime::LocalContextWorkToken, Error> {
  try {
    if (!m_impl->state.session_id)
      return fail(Code::unavailable, "Select a session before browsing files");
    if (m_impl->reading &&
        m_impl->reading->purpose == Impl::ReadPurpose::selection)
      return fail(Code::resource_exhausted,
                  "Wait for the selected file read to finish");
    return m_impl->begin_context(
        runtime::LocalContextRequest{*m_impl->state.session_id,
                                     m_impl->state.selection_revision,
                                     m_impl->state.selection},
        m_impl->state.selection_revision, false);
  } catch (...) {
    return fail(Code::internal_failure, "File context preparation failed");
  }
}
auto LocalSourceBrowser::revalidate(
    const domain::LocalContextAdmission& original)
    -> std::expected<runtime::LocalContextWorkToken, Error> {
  try {
    if (!m_impl->state.session_id)
      return fail(Code::unavailable, "Select a session before browsing files");
    if (original.session_id != *m_impl->state.session_id)
      return fail(Code::invalid_request,
                  "File admission belongs to another session");
    // Validate the bounded envelope before copying it into a worker request.
    if (auto valid = domain::validate_local_context_admission(original); !valid)
      return fail(Code::invalid_request, "Original file admission is invalid");
    return m_impl->begin_context(original, original.selection_revision, true);
  } catch (...) {
    return fail(Code::internal_failure, "File context recovery failed");
  }
}
auto LocalSourceBrowser::poll_context(
    const runtime::LocalContextWorkToken& token)
    -> std::expected<std::optional<runtime::LocalContextWorkCompletion>,
                     Error> {
  try {
    if (!m_impl->preparing || m_impl->preparing->token != token)
      return fail(Code::stale_lease,
                  "File context result is no longer current");
    auto completion = m_impl->worker->poll(token);
    if (!completion) return worker_error(completion.error());
    if (*completion) {
      m_impl->preparing.reset();
      m_impl->state.preparing = false;
    }
    return std::move(*completion);
  } catch (...) {
    return fail(Code::internal_failure, "File context completion failed");
  }
}
auto LocalSourceBrowser::cancel_context() -> void {
  m_impl->cancel_context();
}
auto LocalSourceBrowser::prepare_repository(
    const std::shared_ptr<runtime::RepositoryContextController>& controller,
    const std::variant<runtime::RepositoryContextRequest,
                       domain::RepositoryContextAdmission>& operation)
    -> std::expected<runtime::RepositoryContextWorkToken, Error> {
  try {
    if (!m_impl->state.session_id || !controller || !controller->owns_source())
      return fail(Code::unavailable, "Owned repository context is unavailable");
    std::uint64_t revision{};
    if (const auto* request =
            std::get_if<runtime::RepositoryContextRequest>(&operation)) {
      if (auto valid = runtime::validate_repository_context_request(*request);
          !valid)
        return fail(Code::invalid_request,
                    "Repository context request is invalid");
      revision = request->selection_revision;
    } else {
      const auto& admission =
          std::get<domain::RepositoryContextAdmission>(operation);
      if (auto valid = domain::validate_repository_context_admission(admission);
          !valid)
        return fail(Code::invalid_request,
                    "Repository context admission is invalid");
      revision = admission.selection_revision;
    }
    if (m_impl->repository_work)
      return fail(Code::resource_exhausted,
                  "Repository context is already preparing");
    auto id = m_impl->next_request();
    if (!id) return std::unexpected(id.error());
    runtime::RepositoryContextWorkToken token{
        *m_impl->state.session_id, m_impl->state.session_epoch, *id, revision};
    auto submitted = m_impl->worker->submit(
        controller, runtime::RepositoryContextWorkRequest{token, operation});
    if (!submitted) return worker_error(submitted.error());
    m_impl->repository_work = token;
    m_impl->state.preparing_repository = true;
    return token;
  } catch (...) {
    return fail(Code::internal_failure,
                "Repository context preparation failed");
  }
}
auto LocalSourceBrowser::poll_repository(
    const runtime::RepositoryContextWorkToken& token)
    -> std::expected<std::optional<runtime::RepositoryContextWorkCompletion>,
                     Error> {
  try {
    if (!m_impl->repository_work || *m_impl->repository_work != token)
      return fail(Code::stale_lease,
                  "Repository context result is no longer current");
    auto completion = m_impl->worker->poll(token);
    if (!completion) return worker_error(completion.error());
    if (*completion) {
      m_impl->repository_work.reset();
      m_impl->state.preparing_repository = false;
    }
    return std::move(*completion);
  } catch (...) {
    return fail(Code::internal_failure, "Repository context completion failed");
  }
}
auto LocalSourceBrowser::cancel_repository() -> void {
  m_impl->cancel_repository();
}
auto LocalSourceBrowser::repository_preparation_ready() const noexcept -> bool {
  return m_impl->repository_work &&
         m_impl->worker->ready_result(*m_impl->repository_work);
}
auto LocalSourceBrowser::state() const noexcept -> const LocalBrowserState& {
  return m_impl->state;
}
auto LocalSourceBrowser::occupied_workers() const -> std::size_t {
  return m_impl->worker->occupied_slots();
}
auto LocalSourceBrowser::occupied_grants() const -> std::size_t {
  return m_impl->grants->occupied_slots();
}

} // namespace aiforge::surfaces
