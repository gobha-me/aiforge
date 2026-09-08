#include "../144chatsummary/fixture.hpp"
#include "../167chatevidence/fixture.hpp"
#include <aiforge/adapters/interactive_chat_app.hpp>

namespace {
using namespace aiforge;
using chat_evidence_test::Gate;
using chat_evidence_test::Release;
using chat_evidence_test::until;
using chat_summary_test::id;
class Editor final : public surfaces::DraftEditor {
 public:
  auto edit(std::string_view text, std::stop_token)
      -> std::expected<std::string, surfaces::DraftEditorError> override {
    return std::string{text};
  }
};
auto key(termforge::Key value, char32_t character = 0, bool ctrl = false)
    -> termforge::Event {
  return termforge::KeyEvent{value, character, ctrl,
                             false, false,     termforge::KeyAction::Press};
}
auto rendered(adapters::InteractiveChatApp& app, int cols = 120, int rows = 32)
    -> std::string {
  termforge::Screen screen{cols, rows};
  app.on_render(screen);
  std::string text;
  for (int y = 0; y < rows; ++y) {
    for (int x = 0; x < cols; ++x)
      text += screen.text_at(x, y);
    text += '\n';
  }
  return text;
}
struct Fixture {
  summary_kernel_test::Store store;
  chat_summary_test::Backend backend{store};
  chat_summary_test::Models models;
  Editor editor;
  std::shared_ptr<chat_evidence_test::Factory> factory{
      std::make_shared<chat_evidence_test::Factory>()};
  std::unique_ptr<adapters::InteractiveChatApp> app;
  Fixture() {
    adapters::InteractiveChatAppOptions options;
    options.live_wake_enabled = false;
    options.local_source_factory = factory;
    auto identity = std::make_shared<std::uint64_t>(1000);
    options.session_dependencies.identity_suffix_source = [identity] {
      return ++*identity;
    };
    app = adapters::make_interactive_chat_app(
        backend, models, &store,
        {id<domain::ModelId>("model"), surfaces::ChatSessionOpen::Mode::resume,
         id<domain::SessionId>("session")},
        editor, {}, std::move(options));
    REQUIRE(app->ready());
    static_cast<void>(rendered(*app));
    app->on_start();
  }
  auto command(std::string text) -> void {
    app->on_event(termforge::PasteEvent{std::move(text)});
    app->on_event(key(termforge::Key::Enter));
  }
  auto status(std::string_view text) -> void {
    REQUIRE(until([&] {
      app->on_tick(std::chrono::milliseconds{1});
      return app->status_text().find(text) != std::string_view::npos;
    }));
    REQUIRE_FALSE(app->failure_state());
  }
  auto grant() -> void {
    command("/files add-folder /private/folder");
    status("Folder added");
  }
  auto select() -> void {
    command("/files add 1 notes.txt");
    status("File added");
  }
};
} // namespace

TEST_CASE("app keeps failed and cancelled local preparation from submitting "
          "the draft") {
  Fixture fixture;
  fixture.grant();
  fixture.select();
  SECTION("changed source") {
    fixture.factory->observation->changed = true;
    fixture.command("Keep this draft");
    REQUIRE(until([&] {
      fixture.app->on_tick(std::chrono::milliseconds{1});
      return fixture.factory->observation->reads > 0 &&
             fixture.app->status_text().find("Preparing") ==
                 std::string_view::npos;
    }));
  }
  SECTION("cancel blocked source") {
    auto gate = std::make_shared<Gate>();
    fixture.factory->observation->read_gate = gate;
    Release release{gate};
    fixture.command("Keep this draft");
    REQUIRE(gate->await());
    REQUIRE(rendered(*fixture.app, 52, 18).find("Keep this draft") !=
            std::string::npos);
    fixture.app->on_event(key(termforge::Key::Escape));
    REQUIRE(fixture.app->status_text().find("cancelled") !=
            std::string_view::npos);
    REQUIRE(fixture.backend.requests().empty());
    gate->release();
    for (unsigned index{}; index < 20; ++index)
      fixture.app->on_tick(std::chrono::milliseconds{1});
  }
  REQUIRE(fixture.backend.requests().empty());
  REQUIRE(rendered(*fixture.app).find("Keep this draft") != std::string::npos);
  REQUIRE(fixture.factory->observation->owner_io == 0);
  REQUIRE_FALSE(fixture.app->failure_state());
}
TEST_CASE("app navigation preview and explicit Add are separate from final "
          "submission") {
  Fixture fixture;
  fixture.grant();
  fixture.command("/files open 1");
  fixture.status("Partial listing");
  fixture.command("/files preview 1 notes.txt");
  fixture.status("Preview prefix");
  REQUIRE(fixture.backend.requests().empty());
  REQUIRE(fixture.factory->observation->exact == 0);
  REQUIRE(rendered(*fixture.app).find("Selection tray: 0 files") !=
          std::string::npos);
  fixture.select();
  REQUIRE(fixture.factory->observation->exact == 1);
  REQUIRE(fixture.backend.requests().empty());
  fixture.command("Use my selected file");
  REQUIRE(until([&] {
    fixture.app->on_tick(std::chrono::milliseconds{1});
    return !fixture.backend.requests().empty();
  }));
  const auto requests = fixture.backend.requests();
  REQUIRE(requests.size() == 1);
  REQUIRE(std::ranges::any_of(
      requests.front().context.entries, [](const auto& entry) {
        return entry.message.role == domain::Role::evidence &&
               std::ranges::any_of(
                   entry.message.content, [](const auto& block) {
                     const auto* text = std::get_if<domain::TextBlock>(&block);
                     return text != nullptr && text->text == "hello";
                   });
      }));
  REQUIRE(fixture.factory->observation->owner_io == 0);
  REQUIRE_FALSE(fixture.app->failure_state());
}
TEST_CASE("hidden toolbar retains Context navigation and commands without "
          "sending drafts") {
  Fixture fixture;
  fixture.command("/context toolbar hide");
  fixture.app->on_event(termforge::PasteEvent{"Unsent draft"});
  fixture.app->on_event(key(termforge::Key::Char, U'g', true));
  REQUIRE(rendered(*fixture.app).find("Context") != std::string::npos);
  fixture.app->on_event(key(termforge::Key::Escape));
  REQUIRE(rendered(*fixture.app).find("Unsent draft") != std::string::npos);
  REQUIRE(fixture.backend.requests().empty());
  REQUIRE(fixture.factory->observation->grants == 0);
}
TEST_CASE("app teardown immediately revokes local grants while exact reads "
          "remain blocked") {
  Fixture fixture;
  fixture.grant();
  fixture.select();
  auto gate = std::make_shared<Gate>();
  fixture.factory->observation->read_gate = gate;
  Release release{gate};
  fixture.command("Keep this draft");
  REQUIRE(gate->await());
  fixture.app.reset();
  REQUIRE(fixture.factory->observation->revokes > 0);
  REQUIRE(fixture.backend.requests().empty());
  gate->release();
}
