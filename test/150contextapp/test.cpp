#include "../144chatsummary/fixture.hpp"
#include <aiforge/adapters/interactive_chat_app.hpp>
#include <condition_variable>
#include <mutex>
namespace {
using namespace chat_summary_test;
struct StreamEndGate {
  std::mutex mutex;
  std::condition_variable changed;
  bool entered{}, released{};
  auto wait() -> void {
    std::unique_lock lock{mutex};
    entered = true;
    changed.notify_all();
    changed.wait(lock, [&] { return released; });
  }
  auto await() -> bool {
    std::unique_lock lock{mutex};
    return changed.wait_for(lock, std::chrono::seconds{3},
                            [&] { return entered; });
  }
  auto release() -> void {
    {
      const std::lock_guard lock{mutex};
      released = true;
    }
    changed.notify_all();
  }
};
class GatedStream final : public backend::BackendStream {
 public:
  GatedStream(std::unique_ptr<backend::BackendStream> stream,
              std::shared_ptr<StreamEndGate> gate)
      : m_stream(std::move(stream)), m_gate(std::move(gate)) {}
  auto next(std::stop_token stop)
      -> std::expected<std::optional<backend::BackendEvent>,
                       backend::BackendError> override {
    if (m_finished) m_gate->wait();
    auto result = m_stream->next(stop);
    if (result && *result &&
        std::holds_alternative<backend::ResponseFinished>(**result))
      m_finished = true;
    return result;
  }

 private:
  std::unique_ptr<backend::BackendStream> m_stream;
  std::shared_ptr<StreamEndGate> m_gate;
  bool m_finished{};
};
class GatedBackend final : public backend::Backend {
 public:
  explicit GatedBackend(summary_kernel_test::Store& store) : m_backend(store) {}
  std::shared_ptr<StreamEndGate> end_gate;
  auto requests() -> std::vector<backend::BackendRequest> {
    return m_backend.requests();
  }
  auto start(backend::BackendRequest request, std::stop_token stop)
      -> std::expected<std::unique_ptr<backend::BackendStream>,
                       backend::BackendError> override {
    auto stream = m_backend.start(std::move(request), stop);
    if (!stream || !end_gate) return stream;
    return std::make_unique<GatedStream>(std::move(*stream), end_gate);
  }

 private:
  chat_summary_test::Backend m_backend;
};
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
auto running(adapters::InteractiveChatApp& app) -> bool {
  termforge::Screen screen{120, 30};
  app.on_render(screen);
  std::string prefix;
  for (int x = 0; x < 7; ++x)
    prefix += screen.text_at(x, 29);
  return prefix == "Running";
}
struct AppFixture {
  summary_kernel_test::Store store;
  GatedBackend backend{store};
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
  auto inference_finished() -> bool {
    const auto requests = backend.requests();
    return !requests.empty() &&
           std::ranges::any_of(app->events(), [&](const auto& event) {
             const auto* finished =
                 std::get_if<domain::InferenceFinished>(&event.payload);
             return finished != nullptr &&
                    finished->inference_id == requests.back().inference_id;
           });
  }
  auto drain() -> void {
    for (unsigned count{}; count < 1000; ++count) {
      app->on_tick(std::chrono::milliseconds{1});
      // ResponseFinished records InferenceFinished before provider EOF. The
      // composer stays disabled until that trailing stream work is drained.
      if (inference_finished() && !running(*app)) return;
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    FAIL("Context app did not become input-ready after inference completion");
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
  auto gate = std::make_shared<StreamEndGate>();
  f.backend.end_gate = gate;
  struct Release {
    std::shared_ptr<StreamEndGate> gate;
    ~Release() { gate->release(); }
  } release{gate};
  command(*f.app, "/context summary generate source-run");
  REQUIRE(gate->await());
  f.app->on_tick(std::chrono::milliseconds{1});
  REQUIRE(f.inference_finished());
  REQUIRE(running(*f.app));
  CHECK(f.app->status_text().find("Summary generation finished") !=
        std::string::npos);
  gate->release();
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
  INFO(f.app->status_text());
  REQUIRE(f.app->status_text().find("Preview ready") != std::string::npos);
  command(*f.app, "/context summary apply");
  INFO(f.app->status_text());
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
