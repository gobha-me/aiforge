#include "../144chatsummary/fixture.hpp"
#include <aiforge/adapters/interactive_chat_app.hpp>
namespace {
using namespace chat_summary_test;
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
auto rendered(adapters::InteractiveChatApp& app, int cols = 120, int rows = 30)
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
auto command(adapters::InteractiveChatApp& app, std::string text) -> void {
  app.on_event(termforge::PasteEvent{std::move(text)});
  app.on_event(key(termforge::Key::Enter));
}
struct AppFixture {
  summary_kernel_test::Store store;
  Backend backend{store};
  Models models;
  Editor editor;
  std::unique_ptr<adapters::InteractiveChatApp> app;
  explicit AppFixture(std::string source_text = {},
                      std::uint64_t window = 32768) {
    models.window = window;
    if (!source_text.empty())
      for (auto& event : store.history)
        if (auto* user = std::get_if<domain::UserContentAdded>(&event.payload))
          user->message.content = {domain::TextBlock{source_text}};
    adapters::InteractiveChatAppOptions options;
    options.live_wake_enabled = false;
    auto counter = std::make_shared<std::uint64_t>(1000);
    options.session_dependencies.identity_suffix_source = [counter] {
      return ++*counter;
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
  auto drain() -> void {
    for (unsigned count{}; count < 1000; ++count) {
      app->on_tick(std::chrono::milliseconds{1});
      const auto requests = backend.requests();
      if (!requests.empty() &&
          std::ranges::any_of(app->events(), [&](const auto& event) {
            const auto* finished =
                std::get_if<domain::InferenceFinished>(&event.payload);
            return finished != nullptr &&
                   finished->inference_id == requests.back().inference_id;
          }))
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
  }
};
} // namespace
TEST_CASE(
    "Hidden Context toolbar retains Ctrl+G access and the unsubmitted composer",
    "[contextapp][toolbar]") {
  AppFixture f;
  CHECK(rendered(*f.app).find("Context") != std::string::npos);
  command(*f.app, "/context toolbar hide");
  CHECK(f.app->status_text().find("Toolbar hidden") != std::string::npos);
  f.app->on_event(termforge::PasteEvent{"Keep this exact draft"});
  f.app->on_event(key(termforge::Key::Char, U'g', true));
  REQUIRE(f.app->modal());
  CHECK(rendered(*f.app).find("Keep this exact draft") != std::string::npos);
  f.app->on_event(key(termforge::Key::Escape));
  CHECK_FALSE(f.app->modal());
  CHECK(rendered(*f.app).find("Keep this exact draft") != std::string::npos);
  CHECK(f.backend.requests().empty());
}
TEST_CASE(
    "Invalid context command and cancelled modal cannot clear the composer",
    "[contextapp][failure]") {
  AppFixture f;
  command(*f.app, "/context summary preview missing");
  CHECK(rendered(*f.app).find("/context summary preview missing") !=
        std::string::npos);
  CHECK(f.backend.requests().empty());
  f.app->on_event(key(termforge::Key::Char, U'c', true));
  f.app->on_event(termforge::PasteEvent{"Draft before toolbar"});
  f.app->on_event(key(termforge::Key::F10));
  f.app->on_event(key(termforge::Key::Enter));
  static_cast<void>(rendered(*f.app));
  f.app->on_event(key(termforge::Key::Enter));
  REQUIRE(f.app->modal());
  f.app->on_event(key(termforge::Key::Escape));
  CHECK(rendered(*f.app).find("Draft before toolbar") != std::string::npos);
  CHECK(f.backend.requests().empty());
}
TEST_CASE("Context summary operation retains accounting and hides canonical "
          "prompt from ordinary transcript",
          "[contextapp][summary]") {
  AppFixture f;
  command(*f.app, "/context summary generate source-run");
  f.drain();
  REQUIRE(f.backend.requests().size() == 1);
  const auto visible = rendered(*f.app);
  CHECK(visible.find("Summarize the selected") == std::string::npos);
  CHECK(visible.find("Keep the open task and unresolved constraints.") ==
        std::string::npos);
  CHECK(f.app->status_text().find("Summary generation finished") !=
        std::string::npos);
  CHECK(std::ranges::any_of(f.app->events(), [](const auto& event) {
    return std::holds_alternative<domain::InferenceStarted>(event.payload);
  }));
  CHECK_FALSE(std::ranges::any_of(f.app->events(), [](const auto& event) {
    return std::holds_alternative<domain::ConversationSummaryActivated>(
        event.payload);
  }));
  f.app->on_event(key(termforge::Key::Escape));
  command(*f.app, "/clear");
  CHECK(std::ranges::any_of(f.app->events(), [](const auto& event) {
    return std::holds_alternative<
        domain::ConversationSummaryGenerationIntentRecorded>(event.payload);
  }));
  CHECK(f.backend.requests().size() == 1);
}
TEST_CASE("Context toolbar tiny resize preserves its keyboard command path",
          "[contextapp][resize]") {
  AppFixture f;
  for (const auto& [cols, rows] :
       {std::pair{1, 1}, std::pair{20, 2}, std::pair{80, 24}})
    CHECK_NOTHROW(rendered(*f.app, cols, rows));
  f.app->on_event(key(termforge::Key::Char, U'g', true));
  REQUIRE(f.app->modal());
  f.app->on_event(key(termforge::Key::Escape));
  CHECK_FALSE(f.app->modal());
  CHECK(f.backend.requests().empty());
}

TEST_CASE("Context commands can preview and apply without opening a modal or "
          "submitting their text",
          "[contextapp][commands]") {
  AppFixture f;
  command(*f.app, "/context summary generate source-run");
  f.drain();
  REQUIRE(f.backend.requests().size() == 1);
  std::optional<domain::ConversationSummaryId> summary;
  for (const auto& event : f.app->events())
    if (const auto* intent =
            std::get_if<domain::ConversationSummaryGenerationIntentRecorded>(
                &event.payload))
      summary = intent->intent.summary_id;
  REQUIRE(summary);
  command(*f.app, "/context mode rolling");
  command(*f.app, "/context summary review " + std::string{summary->value()});
  command(*f.app, "/context summary preview " + std::string{summary->value()});
  CHECK_FALSE(f.app->modal());
  CHECK(f.app->status_text().find("Preview ready") != std::string::npos);
  command(*f.app, "/context summary apply");
  CHECK(std::ranges::any_of(f.app->events(), [](const auto& event) {
    return std::holds_alternative<domain::ConversationSummaryActivated>(
        event.payload);
  }));
  CHECK(f.backend.requests().size() == 1);
  CHECK(rendered(*f.app).find("/context summary apply") == std::string::npos);
}

TEST_CASE("Context rolling menu recovers an unfit full-history submission "
          "without losing its draft",
          "[contextapp][capacity]") {
  const std::string original(10000, 'x');
  AppFixture f{original, 2000};
  const auto before = f.store.history;
  command(*f.app, "Retry this input");
  CHECK(f.backend.requests().empty());
  CHECK(f.store.history == before);
  CHECK(rendered(*f.app).find("Retry this input") != std::string::npos);
  f.app->on_event(key(termforge::Key::Char, U'g', true));
  REQUIRE(f.app->modal());
  termforge::Screen dialog_screen{120, 32};
  f.app->top_overlay()->draw(dialog_screen);
  f.app->on_event(key(termforge::Key::Enter));
  f.app->top_overlay()->draw(dialog_screen);
  f.app->on_event(key(termforge::Key::Down));
  f.app->on_event(key(termforge::Key::Down));
  f.app->on_event(key(termforge::Key::Enter));
  f.app->on_event(key(termforge::Key::Escape));
  CHECK_FALSE(f.app->modal());
  CHECK(rendered(*f.app).find("Retry this input") != std::string::npos);
  f.app->on_event(key(termforge::Key::Enter));
  f.drain();
  REQUIRE(f.backend.requests().size() == 1);
  bool original_retained{};
  for (const auto& event : f.app->events())
    if (const auto* user =
            std::get_if<domain::UserContentAdded>(&event.payload))
      for (const auto& content : user->message.content)
        if (const auto* text = std::get_if<domain::TextBlock>(&content);
            text != nullptr && text->text == original)
          original_retained = true;
  CHECK(original_retained);
  CHECK(std::ranges::any_of(f.app->events(), [](const auto& event) {
    const auto* start = std::get_if<domain::RunStarted>(&event.payload);
    return start != nullptr && start->conversation_admission &&
           start->conversation_admission->mode ==
               domain::ConversationMode::rolling;
  }));
}
