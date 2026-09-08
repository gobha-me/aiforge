#include "../159foldergrant/fixture.hpp"
#include <aiforge/adapters/local_source_browser_dialog.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {
using namespace folder_grant_test;
using namespace aiforge::surfaces;
using Dialog = adapters::LocalSourceBrowserDialog;
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
  std::atomic<unsigned> lists{}, previews{}, exact{};
  std::shared_ptr<Gate> gate;
  std::string last_path, last_filter;
};
class Reader final : public runtime::LocalSourceLease {
 public:
  Reader(runtime::LocalFolderGrantRequest request,
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
  auto revoke() noexcept -> void override {}
  auto list(runtime::LocalListRequest value, std::stop_token)
      -> std::expected<runtime::LocalListResult,
                       domain::LocalSourceError> override {
    ++m_observation->lists;
    m_observation->last_filter = value.filename_filter;
    if (m_observation->gate) m_observation->gate->wait();
    const auto prefix =
        value.directory.empty() ? std::string{} : value.directory + "/";
    return runtime::LocalListResult{
        value.token,
        value.directory,
        {{prefix + "folder", runtime::LocalEntryKind::directory},
         {prefix + "notes.txt", runtime::LocalEntryKind::regular_file},
         {prefix + "bad\033name", runtime::LocalEntryKind::unsupported}},
        4,
        runtime::LocalListingState::partial,
        1};
  }
  auto preview(runtime::LocalPreviewRequest value, std::stop_token)
      -> std::expected<runtime::LocalPreviewResult,
                       domain::LocalSourceError> override {
    ++m_observation->previews;
    m_observation->last_path = value.relative_path;
    if (m_observation->gate) m_observation->gate->wait();
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
    return runtime::LocalReadResult{value.token, value.expected_source,
                                    "hello"};
  }

 private:
  runtime::LocalFolderGrantRequest m_request;
  std::shared_ptr<Observation> m_observation;
  domain::LocalRootIdentity m_root{root()};
};
class Factory final : public runtime::LocalSourceGrantFactory {
 public:
  std::shared_ptr<Observation> observation{std::make_shared<Observation>()};
  auto guarantees_pinned_read_only_sources() const noexcept -> bool override {
    return true;
  }
  auto grant(const runtime::LocalFolderGrantRequest& value, std::stop_token)
      -> std::expected<runtime::LocalFolderGrantResult,
                       domain::LocalSourceError> override {
    return runtime::LocalFolderGrantResult{
        value.token, root(), std::make_shared<Reader>(value, observation)};
  }
};
auto key(termforge::Key value, char32_t character = 0) -> termforge::Event {
  return termforge::KeyEvent{value, character, false,
                             false, false,     termforge::KeyAction::Press};
}
auto rendered(Dialog& dialog, int cols = 110, int rows = 34) -> std::string {
  termforge::Screen screen{cols, rows};
  dialog.draw(screen);
  std::string result;
  for (int y = 0; y < rows; ++y) {
    for (int x = 0; x < cols; ++x)
      result += screen.text_at(x, y);
    result += '\n';
  }
  return result;
}
auto click_label(Dialog& dialog, std::string_view label) -> bool {
  termforge::Screen screen{110, 34};
  dialog.draw(screen);
  for (int y = 0; y < 34; ++y) {
    for (int x = 0; x < 110; ++x) {
      std::string text;
      for (int offset = 0;
           offset < static_cast<int>(label.size()) && x + offset < 110;
           ++offset)
        text += screen.text_at(x + offset, y);
      if (text == label)
        return dialog.on_event(termforge::MouseEvent{x, y, 0, true});
    }
  }
  return false;
}
struct Fixture {
  std::shared_ptr<Factory> factory{std::make_shared<Factory>()};
  std::unique_ptr<LocalSourceBrowser> browser{
      LocalSourceBrowser::create(factory).value()};
  Dialog dialog{*browser};
  Fixture() {
    REQUIRE(browser->activate_session(request().token.session_id));
    dialog.refresh();
  }
  auto finish() -> void {
    REQUIRE(until([&] {
      auto result = browser->poll();
      INFO((result ? "poll succeeded" : result.error().message));
      REQUIRE(result);
      return !browser->state().granting && !browser->state().reading;
    }));
    REQUIRE(until([&] { return browser->occupied_workers() == 0; }));
    dialog.refresh();
  }
  auto grant() -> void {
    REQUIRE(dialog.execute(LocalBrowseAddFolder{"/private/folder"}));
    finish();
  }
  auto list() -> void {
    REQUIRE(dialog.execute(LocalBrowseNavigate{root(), {}, {}}));
    finish();
  }
};
} // namespace

TEST_CASE("Local files dialog rejects missing authority and bounded path input "
          "before reads") {
  Fixture f;
  CHECK_FALSE(f.dialog.execute(LocalBrowseNavigate{root(), {}, {}}));
  CHECK_FALSE(f.dialog.execute(LocalBrowseAddFolder{"relative/path"}));
  CHECK(f.factory->observation->lists == 0);
  CHECK(f.browser->state().selection.empty());
  static_cast<void>(rendered(f.dialog));
  REQUIRE(f.dialog.on_event(key(termforge::Key::Tab))); // path input
  REQUIRE(f.dialog.on_event(termforge::PasteEvent{std::string(4097, 'x')}));
  CHECK(f.dialog.status().find("bounded printable") != std::string::npos);
  REQUIRE(f.dialog.on_event(key(termforge::Key::Enter)));
  CHECK_FALSE(f.browser->state().granting);
  CHECK(f.browser->state().folders.empty());
}
TEST_CASE("Local files closing a blocked preview preserves tray and prepared "
          "context") {
  Fixture f;
  f.grant();
  REQUIRE(f.dialog.execute(LocalBrowseAddEvidence{root(), "notes.txt"}));
  f.finish();
  const auto selected = f.browser->state().selection;
  auto token = f.browser->prepare_selection();
  REQUIRE(token);
  std::optional<runtime::LocalContextWorkCompletion> context;
  REQUIRE(until([&] {
    auto result = f.browser->poll_context(*token);
    REQUIRE(result);
    if (*result) context = std::move(**result);
    return context.has_value();
  }));
  REQUIRE(context->result);
  REQUIRE(until([&] { return f.browser->occupied_workers() == 0; }));
  auto gate = std::make_shared<Gate>();
  f.factory->observation->gate = gate;
  Release release{gate};
  REQUIRE(f.dialog.execute(LocalBrowsePreview{root(), "notes.txt"}));
  REQUIRE(gate->await());
  CHECK(f.dialog.on_event(key(termforge::Key::Escape)));
  CHECK(f.browser->state().selection == selected);
  CHECK(context->result->admission.evidence.size() == 1);
  CHECK_FALSE(f.browser->state().reading);
  gate->release();
  f.finish();
  CHECK_FALSE(f.browser->state().preview);
}
TEST_CASE("Local files session replacement clears displayed identities and "
          "ignores late listing") {
  Fixture f;
  f.grant();
  auto gate = std::make_shared<Gate>();
  f.factory->observation->gate = gate;
  Release release{gate};
  REQUIRE(f.dialog.execute(LocalBrowseNavigate{root(), {}, {}}));
  REQUIRE(gate->await());
  REQUIRE(
      f.browser->activate_session(domain::SessionId::from("other").value()));
  f.dialog.refresh();
  CHECK(rendered(f.dialog).find("/private/folder") == std::string::npos);
  gate->release();
  f.finish();
  CHECK_FALSE(f.browser->state().listing);
  CHECK_FALSE(f.dialog.execute(LocalBrowseAddEvidence{root(), "notes.txt"}));
  CHECK(f.browser->state().selection.empty());
}
TEST_CASE("Local files bounded listing and prefix preview cannot silently "
          "select evidence") {
  Fixture f;
  f.grant();
  f.list();
  CHECK(rendered(f.dialog).find("Partial listing") != std::string::npos);
  CHECK(rendered(f.dialog).find("bad\\x1bname") != std::string::npos);
  CHECK(f.browser->state().selection.empty());
  REQUIRE(f.dialog.execute(LocalBrowsePreview{root(), "notes.txt"}));
  f.finish();
  CHECK(rendered(f.dialog).find("Prefix preview") != std::string::npos);
  CHECK(f.browser->state().selection.empty());
  CHECK(f.factory->observation->exact == 0);
  REQUIRE(f.dialog.execute(LocalBrowseAddEvidence{root(), "notes.txt"}));
  f.finish();
  REQUIRE(f.browser->state().selection.size() == 1);
  CHECK(f.factory->observation->exact == 1);
  CHECK(f.browser->state().selection.front() == source("notes.txt"));
  REQUIRE(f.dialog.execute(LocalBrowseRemoveEvidence{root(), "notes.txt"}));
  CHECK(f.browser->state().selection.empty());
}
TEST_CASE(
    "Local files real menu uses the command dispatcher across tiny resize") {
  Fixture menu;
  Fixture command;
  menu.grant();
  command.grant();
  REQUIRE(menu.dialog.execute(LocalBrowseAddEvidence{root(), "notes.txt"}));
  menu.finish();
  REQUIRE(command.dialog.execute(LocalBrowseAddEvidence{root(), "notes.txt"}));
  command.finish();
  for (const auto& [cols, rows] : {std::pair{1, 1}, std::pair{8, 3},
                                   std::pair{40, 12}, std::pair{110, 34}})
    CHECK_NOTHROW(rendered(menu.dialog, cols, rows));
  // Menu starts on Browse; Right selects Evidence, last item explicitly clears.
  REQUIRE(menu.dialog.on_event(key(termforge::Key::Right)));
  REQUIRE(menu.dialog.on_event(key(termforge::Key::Enter)));
  for (int i = 0; i < 3; ++i)
    REQUIRE(menu.dialog.on_event(key(termforge::Key::Down)));
  REQUIRE(menu.dialog.on_event(key(termforge::Key::Enter)));
  REQUIRE(command.dialog.execute(LocalBrowseClearEvidence{}));
  CHECK(menu.browser->state().selection == command.browser->state().selection);
  CHECK(menu.browser->state().selection.empty());
  CHECK(menu.factory->observation->previews ==
        command.factory->observation->previews);
}
TEST_CASE(
    "Local files Add folder path field grants only after explicit Enter") {
  Fixture f;
  static_cast<void>(rendered(f.dialog));
  REQUIRE(f.dialog.on_event(key(termforge::Key::Tab)));
  REQUIRE(f.dialog.on_event(termforge::PasteEvent{"/private/folder"}));
  CHECK_FALSE(f.browser->state().granting);
  REQUIRE(f.dialog.on_event(key(termforge::Key::Enter)));
  f.finish();
  REQUIRE(f.browser->state().folders.size() == 1);
  CHECK(f.browser->state().selection.empty());
  CHECK(f.factory->observation->lists == 0);
}

TEST_CASE(
    "Local files mouse selection and directory navigation never add evidence") {
  Fixture f;
  f.grant();
  f.list();
  REQUIRE(click_label(f.dialog, "[unsupported]"));
  REQUIRE(f.dialog.on_event(key(termforge::Key::Enter)));
  CHECK(f.dialog.status().find("Only regular files") != std::string::npos);
  CHECK(f.factory->observation->previews == 0);
  REQUIRE(click_label(f.dialog, "[file]"));
  CHECK(f.factory->observation->previews == 0);
  REQUIRE(f.dialog.on_event(key(termforge::Key::Enter)));
  f.finish();
  CHECK(f.factory->observation->previews == 1);
  CHECK(f.factory->observation->last_path == "notes.txt");
  CHECK(f.browser->state().selection.empty());
  REQUIRE(click_label(f.dialog, "[dir]"));
  REQUIRE(f.dialog.on_event(key(termforge::Key::Enter)));
  f.finish();
  REQUIRE(f.browser->state().listing);
  CHECK(f.browser->state().listing->directory == "folder");
  CHECK(f.browser->state().selection.empty());
  CHECK(f.factory->observation->exact == 0);
}
