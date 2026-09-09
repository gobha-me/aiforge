#include "../159foldergrant/fixture.hpp"
#include <aiforge/surfaces/local_source_browser.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {
using namespace folder_grant_test;
using Browser = surfaces::LocalSourceBrowser;
using Code = domain::LocalSourceErrorCode;
auto root() -> domain::LocalRootIdentity {
  return {1, std::string(64, 'a')};
}
auto source(std::string path) -> domain::LocalSourceIdentity {
  return {root(),
          std::move(path),
          {"sha256",
           "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824",
           5}};
}
struct Observation {
  std::atomic<unsigned> grants{}, lists{}, previews{}, exact{}, reads{},
      revokes{};
  std::atomic<bool> fail{}, changed{};
  std::shared_ptr<Gate> grant_gate, read_gate;
};
class BrowserLease final : public runtime::LocalSourceLease {
 public:
  BrowserLease(runtime::LocalFolderGrantRequest request,
               std::shared_ptr<Observation> observation)
      : m_request(std::move(request)), m_observation(std::move(observation)) {}
  auto root_identity() const noexcept
      -> const domain::LocalRootIdentity& override {
    return m_root;
  }
  auto session_id() const noexcept -> const domain::SessionId& override {
    return m_request.token.session_id;
  }
  auto lease_generation() const noexcept -> std::uint64_t override {
    return m_request.lease_generation;
  }
  auto guarantees_pinned_read_only_sources() const noexcept -> bool override {
    return true;
  }
  auto revoke() noexcept -> void override {
    m_revoked = true;
    ++m_observation->revokes;
  }
  auto list(runtime::LocalListRequest value, std::stop_token)
      -> std::expected<runtime::LocalListResult,
                       domain::LocalSourceError> override {
    ++m_observation->lists;
    if (auto ready = check(); !ready) return std::unexpected(ready.error());
    return runtime::LocalListResult{
        value.token,
        value.directory,
        {{"notes.txt", runtime::LocalEntryKind::regular_file}},
        1,
        runtime::LocalListingState::partial};
  }
  auto preview(runtime::LocalPreviewRequest value, std::stop_token)
      -> std::expected<runtime::LocalPreviewResult,
                       domain::LocalSourceError> override {
    ++m_observation->previews;
    if (auto ready = check(); !ready) return std::unexpected(ready.error());
    if (value.mode == runtime::LocalPreviewMode::exact) {
      ++m_observation->exact;
      return runtime::LocalPreviewResult{value.token,
                                         value.relative_path,
                                         5,
                                         "hello",
                                         runtime::LocalPreviewState::complete,
                                         source(value.relative_path)};
    }
    return runtime::LocalPreviewResult{value.token,
                                       value.relative_path,
                                       5,
                                       "he",
                                       runtime::LocalPreviewState::prefix,
                                       std::nullopt};
  }
  auto revalidate(runtime::LocalRevalidateRequest value, std::stop_token)
      -> std::expected<runtime::LocalReadResult,
                       domain::LocalSourceError> override {
    ++m_observation->reads;
    if (auto ready = check(); !ready) return std::unexpected(ready.error());
    return runtime::LocalReadResult{value.token, value.expected_source,
                                    m_observation->changed ? "jello" : "hello"};
  }

 private:
  auto check() -> std::expected<void, domain::LocalSourceError> {
    if (m_observation->read_gate) m_observation->read_gate->wait();
    if (m_revoked || m_observation->fail)
      return std::unexpected(
          domain::LocalSourceError{Code::io_failure, "secret /private/path"});
    return {};
  }
  runtime::LocalFolderGrantRequest m_request;
  std::shared_ptr<Observation> m_observation;
  domain::LocalRootIdentity m_root{root()};
  std::atomic<bool> m_revoked{};
};
class Factory final : public runtime::LocalSourceGrantFactory {
 public:
  std::shared_ptr<Observation> observation{std::make_shared<Observation>()};
  bool trusted{true};
  auto guarantees_pinned_read_only_sources() const noexcept -> bool override {
    return trusted;
  }
  auto grant(const runtime::LocalFolderGrantRequest& value, std::stop_token)
      -> std::expected<runtime::LocalFolderGrantResult,
                       domain::LocalSourceError> override {
    ++observation->grants;
    if (observation->grant_gate) observation->grant_gate->wait();
    return runtime::LocalFolderGrantResult{
        value.token, root(),
        std::make_shared<BrowserLease>(value, observation)};
  }
};
struct Fixture {
  std::shared_ptr<Factory> factory{std::make_shared<Factory>()};
  std::unique_ptr<Browser> browser;
  explicit Fixture(domain::LocalSourceLimits limits = {},
                   std::size_t capacity = 2)
      : browser(Browser::create(factory, limits, capacity).value()) {
    REQUIRE(browser->activate_session(request().token.session_id));
  }
  auto finish(bool success = true) -> void {
    bool failed{};
    REQUIRE(until([&] {
      const auto result = browser->poll();
      if (!result) failed = true;
      return !browser->state().granting && !browser->state().reading;
    }));
    REQUIRE(failed == !success);
    REQUIRE(until([&] { return browser->occupied_workers() == 0; }));
  }
  auto grant() -> void {
    REQUIRE(browser->add_folder("/private/folder"));
    finish();
  }
  auto select(std::string path = "notes.txt") -> void {
    REQUIRE(browser->add_evidence(root(), std::move(path)));
    finish();
  }
  auto context(runtime::LocalContextWorkToken token)
      -> runtime::LocalContextWorkCompletion {
    std::optional<runtime::LocalContextWorkCompletion> completion;
    REQUIRE(until([&] {
      auto result = browser->poll_context(token);
      REQUIRE(result);
      if (*result) completion = std::move(**result);
      return completion.has_value();
    }));
    REQUIRE(until([&] { return browser->occupied_workers() == 0; }));
    return std::move(*completion);
  }
  auto original() -> domain::LocalContextAdmission {
    auto token = browser->prepare_selection();
    REQUIRE(token);
    auto completed = context(*token);
    REQUIRE(completed.result);
    auto admission = completed.result->admission;
    admission.capacity = {100, 10, 0};
    REQUIRE(domain::seal_local_context_admission(admission));
    return admission;
  }
};
} // namespace

TEST_CASE("browser rejects invalid construction and missing explicit scope "
          "before work") {
  auto factory = std::make_shared<Factory>();
  REQUIRE_FALSE(Browser::create({}));
  factory->trusted = false;
  REQUIRE_FALSE(Browser::create(factory));
  factory->trusted = true;
  REQUIRE_FALSE(Browser::create(factory, {}, 0));
  auto browser = Browser::create(factory).value();
  REQUIRE_FALSE(browser->add_folder("/private/folder"));
  REQUIRE_FALSE(browser->navigate(root()));
  REQUIRE_FALSE(browser->prepare_selection());
  REQUIRE(browser->activate_session(request().token.session_id));
  REQUIRE_FALSE(browser->add_folder("relative"));
  REQUIRE_FALSE(browser->preview(root(), "notes.txt"));
  REQUIRE(factory->observation->grants == 0);
  REQUIRE(browser->occupied_workers() == 0);
}
TEST_CASE(
    "browser preserves usable preview and tray when reads or bounds fail") {
  domain::LocalSourceLimits limits;
  limits.maximum_selected_files = 1;
  Fixture fixture{limits};
  fixture.grant();
  fixture.select();
  const auto selected = fixture.browser->state().selection;
  const auto preview = fixture.browser->state().preview;
  const auto revision = fixture.browser->state().selection_revision;
  SECTION("failed read") {
    fixture.factory->observation->fail = true;
    REQUIRE(fixture.browser->preview(root(), "other.txt"));
    fixture.finish(false);
    REQUIRE(fixture.browser->state().message.find("secret") ==
            std::string::npos);
  }
  SECTION("selected file bound") {
    REQUIRE(fixture.browser->add_evidence(root(), "other.txt"));
    fixture.finish(false);
  }
  SECTION("invalid navigation") {
    REQUIRE_FALSE(fixture.browser->navigate(root(), "../escape"));
    REQUIRE_FALSE(fixture.browser->navigate(root(), "", "bad/filter"));
  }
  REQUIRE(fixture.browser->state().selection == selected);
  REQUIRE(fixture.browser->state().preview == preview);
  REQUIRE(fixture.browser->state().selection_revision == revision);
}
TEST_CASE(
    "session changes retire late grants without admitting stale folders") {
  domain::LocalSourceLimits limits;
  limits.maximum_roots = 1;
  Fixture fixture{limits, 1};
  auto gate = std::make_shared<Gate>();
  fixture.factory->observation->grant_gate = gate;
  Release release{gate};
  REQUIRE(fixture.browser->add_folder("/private/folder"));
  REQUIRE(gate->await());
  REQUIRE(fixture.browser->activate_session(
      domain::SessionId::from("next").value()));
  REQUIRE_FALSE(fixture.browser->state().granting);
  REQUIRE(fixture.browser->state().folders.empty());
  REQUIRE(fixture.browser->occupied_grants() == 1);
  REQUIRE(fixture.browser->occupied_workers() == 1);
  REQUIRE_FALSE(fixture.browser->add_folder("/private/replacement"));
  gate->release();
  REQUIRE(until([&] {
    return fixture.browser->occupied_workers() == 0 &&
           fixture.browser->occupied_grants() == 0;
  }));
  REQUIRE(fixture.browser->poll());
  REQUIRE(fixture.browser->state().folders.empty());
  REQUIRE(fixture.factory->observation->grants == 1);
}
TEST_CASE("cancel and session replacement discard blocked reads without "
          "changing evidence") {
  Fixture fixture;
  fixture.grant();
  fixture.select();
  auto gate = std::make_shared<Gate>();
  fixture.factory->observation->read_gate = gate;
  Release release{gate};
  REQUIRE(fixture.browser->add_evidence(root(), "late.txt"));
  REQUIRE(gate->await());
  SECTION("cancel") {
    fixture.browser->cancel_browsing();
    REQUIRE(fixture.browser->state().selection ==
            std::vector{source("notes.txt")});
  }
  SECTION("session") {
    REQUIRE(fixture.browser->activate_session(request().token.session_id));
    REQUIRE(fixture.browser->state().selection.empty());
  }
  REQUIRE_FALSE(fixture.browser->state().reading);
  REQUIRE(fixture.browser->occupied_workers() == 1);
  gate->release();
  REQUIRE(until([&] { return fixture.browser->occupied_workers() == 0; }));
  const auto selection = fixture.browser->state().selection;
  REQUIRE(fixture.browser->poll());
  REQUIRE(fixture.browser->state().selection == selection);
}
TEST_CASE("tray mutation invalidates pending new context but not original "
          "recovery membership") {
  Fixture fixture;
  fixture.grant();
  fixture.select();
  const auto original = fixture.original();
  auto gate = std::make_shared<Gate>();
  fixture.factory->observation->read_gate = gate;
  Release release{gate};
  SECTION("new preparation") {
    auto token = fixture.browser->prepare_selection();
    REQUIRE(token);
    REQUIRE(gate->await());
    REQUIRE(fixture.browser->clear_evidence());
    REQUIRE_FALSE(fixture.browser->poll_context(*token));
    gate->release();
  }
  SECTION("fixed recovery") {
    auto token = fixture.browser->revalidate(original);
    REQUIRE(token);
    REQUIRE(gate->await());
    REQUIRE(fixture.browser->clear_evidence());
    gate->release();
    auto completed = fixture.context(*token);
    REQUIRE(completed.result);
    REQUIRE(completed.result->admission == original);
    REQUIRE(completed.result->candidates.size() == 1);
    REQUIRE(fixture.browser->state().selection.empty());
  }
  REQUIRE(until([&] { return fixture.browser->occupied_workers() == 0; }));
}
TEST_CASE("recovery needs a current explicit grant and exact original bytes") {
  Fixture fixture;
  fixture.grant();
  fixture.select();
  const auto original = fixture.original();
  REQUIRE(fixture.browser->remove_folder(root()));
  auto token = fixture.browser->revalidate(original);
  REQUIRE(token);
  REQUIRE_FALSE(fixture.context(*token).result);
  const auto reads = fixture.factory->observation->reads.load();
  fixture.grant();
  REQUIRE(fixture.factory->observation->reads == reads);
  REQUIRE_FALSE(fixture.browser->state().preparing);
  REQUIRE(fixture.browser->state().selection.empty());
  fixture.factory->observation->changed = true;
  token = fixture.browser->revalidate(original);
  REQUIRE(token);
  REQUIRE_FALSE(fixture.context(*token).result);
  fixture.factory->observation->changed = false;
  token = fixture.browser->revalidate(original);
  REQUIRE(token);
  auto completed = fixture.context(*token);
  REQUIRE(completed.result);
  REQUIRE(completed.result->admission == original);
}
TEST_CASE("navigation and prefix preview never select evidence or prepare "
          "inference") {
  Fixture fixture;
  fixture.grant();
  REQUIRE(fixture.browser->navigate(root()));
  fixture.finish();
  REQUIRE(fixture.browser->state().listing);
  REQUIRE(fixture.browser->state().listing->state ==
          runtime::LocalListingState::partial);
  REQUIRE(fixture.browser->state().selection.empty());
  REQUIRE(fixture.browser->preview(root(), "notes.txt"));
  fixture.finish();
  REQUIRE(fixture.browser->state().preview);
  REQUIRE_FALSE(fixture.browser->state().preview->source);
  REQUIRE(fixture.browser->state().selection.empty());
  REQUIRE(fixture.factory->observation->reads == 0);
  REQUIRE(fixture.factory->observation->exact == 0);
  fixture.select();
  REQUIRE(fixture.factory->observation->exact == 1);
  REQUIRE(fixture.browser->state().selection ==
          std::vector{source("notes.txt")});
  REQUIRE(fixture.factory->observation->reads == 0);
  REQUIRE(fixture.browser->remove_evidence(root(), "notes.txt"));
  REQUIRE(fixture.browser->state().selection.empty());
}

TEST_CASE("clearing an empty tray cancels an unfinished explicit Add") {
  Fixture fixture;
  fixture.grant();
  auto gate = std::make_shared<Gate>();
  fixture.factory->observation->read_gate = gate;
  Release release{gate};
  REQUIRE(fixture.browser->add_evidence(root(), "notes.txt"));
  REQUIRE(gate->await());
  REQUIRE_FALSE(fixture.browser->prepare_selection());
  REQUIRE(fixture.browser->state().selection.empty());
  REQUIRE(fixture.browser->clear_evidence());
  REQUIRE_FALSE(fixture.browser->state().reading);
  gate->release();
  REQUIRE(until([&] { return fixture.browser->occupied_workers() == 0; }));
  REQUIRE(fixture.browser->poll());
  REQUIRE(fixture.browser->state().selection.empty());
}
TEST_CASE("browser teardown revokes authority while a context worker retains "
          "its resolver") {
  Fixture fixture;
  fixture.grant();
  fixture.select();
  auto gate = std::make_shared<Gate>();
  fixture.factory->observation->read_gate = gate;
  Release release{gate};
  REQUIRE(fixture.browser->prepare_selection());
  REQUIRE(gate->await());
  REQUIRE(fixture.factory->observation->revokes == 0);
  // Teardown must return before the blocked read can finish, and authority
  // must already be revoked even though that read owns the registry resolver.
  fixture.browser.reset();
  REQUIRE(fixture.factory->observation->revokes == 1);
  gate->release();
}
