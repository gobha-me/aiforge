#include "../144chatsummary/fixture.hpp"
#include <aiforge/adapters/conversation_context_dialog.hpp>
#include <aiforge/adapters/transcript_view.hpp>
namespace {
using namespace chat_summary_test;
auto key(termforge::Key value, char32_t character = 0, bool ctrl = false)
    -> termforge::Event {
  return termforge::KeyEvent{value, character, ctrl,
                             false, false,     termforge::KeyAction::Press};
}
auto rendered(adapters::ConversationContextDialog& dialog, int cols = 110,
              int rows = 32) -> std::string {
  termforge::Screen screen{cols, rows};
  dialog.draw(screen);
  std::string text;
  for (int y = 0; y < rows; ++y) {
    for (int x = 0; x < cols; ++x)
      text += screen.text_at(x, y);
    text += '\n';
  }
  return text;
}
} // namespace
TEST_CASE(
    "Context dialog failed previews and cancellation preserve the chat draft",
    "[contextdialog][failure]") {
  Fixture f;
  std::string draft = "Unsubmitted story draft";
  adapters::ConversationContextDialog dialog{*f.chat, [&] { return draft; }};
  REQUIRE(dialog.execute(surfaces::InspectConversation{}));
  const auto before = f.chat->event_log().events();
  const auto missing = id<domain::ConversationSummaryId>("missing");
  CHECK_FALSE(
      dialog.execute(surfaces::PreviewConversationSummary{missing, {}}));
  CHECK_FALSE(dialog.review_available());
  CHECK_FALSE(dialog.execute(surfaces::ApplyConversationSummary{}));
  CHECK(dialog.on_event(key(termforge::Key::Escape)));
  CHECK(draft == "Unsubmitted story draft");
  CHECK(f.chat->event_log().events() == before);
  CHECK(f.backend.requests().empty());
}
TEST_CASE("Context dialog exposes mandatory capacity failure and recovers "
          "after model change",
          "[contextdialog][capacity]") {
  Fixture f;
  f.models.window = 300;
  REQUIRE(f.chat->select_model(id<domain::ModelId>("tiny")));
  adapters::ConversationContextDialog dialog{
      *f.chat, [] { return std::string{"draft"}; }};
  REQUIRE(dialog.execute(surfaces::InspectConversation{}));
  CHECK(rendered(dialog).find("Capacity: 300") != std::string::npos);
  CHECK(dialog.status().find("context inspected") == std::string::npos);
  for (const auto& [cols, rows] :
       {std::pair{1, 1}, std::pair{8, 3}, std::pair{40, 10}})
    CHECK_NOTHROW(rendered(dialog, cols, rows));
  f.models.window = 32768;
  REQUIRE(f.chat->select_model(id<domain::ModelId>("large")));
  REQUIRE(dialog.execute(surfaces::InspectConversation{}));
  CHECK(rendered(dialog).find("Capacity: 32768") != std::string::npos);
  CHECK(f.backend.requests().empty());
}
TEST_CASE("Context menu and slash command select the same durable policy",
          "[contextdialog][parity]") {
  Fixture menu;
  Fixture command;
  adapters::ConversationContextDialog dialog{
      *menu.chat, [] { return std::string{"kept"}; }};
  REQUIRE(dialog.execute(surfaces::InspectConversation{}));
  static_cast<void>(rendered(dialog));
  REQUIRE(dialog.on_event(key(termforge::Key::Enter)));
  static_cast<void>(rendered(dialog));
  REQUIRE(dialog.on_event(key(termforge::Key::Down)));
  REQUIRE(dialog.on_event(key(termforge::Key::Down)));
  REQUIRE(dialog.on_event(key(termforge::Key::Enter)));
  const auto parsed = surfaces::parse_conversation_command("mode rolling");
  REQUIRE(parsed);
  adapters::ConversationContextDialog other{*command.chat,
                                            [] { return std::string{"kept"}; }};
  REQUIRE(other.execute(*parsed));
  const auto actual = menu.chat->conversation_policy();
  const auto expected = command.chat->conversation_policy();
  REQUIRE(actual);
  REQUIRE(expected);
  CHECK(actual->policy == expected->policy);
  CHECK(actual->policy.mode == domain::ConversationMode::rolling);
  CHECK(menu.backend.requests().empty());
  CHECK(command.backend.requests().empty());
}
TEST_CASE("Context preview is invalidated when the unsubmitted draft changes",
          "[contextdialog][stale]") {
  Fixture f;
  const auto candidate = f.candidate();
  REQUIRE(f.chat->set_conversation_policy(0, domain::ConversationMode::rolling,
                                          {}));
  std::string draft = "First draft";
  adapters::ConversationContextDialog dialog{*f.chat, [&] { return draft; }};
  REQUIRE(dialog.execute(
      surfaces::PreviewConversationSummary{candidate.summary_id, {}}));
  REQUIRE(dialog.review_available());
  draft = "Second draft";
  static_cast<void>(rendered(dialog));
  CHECK_FALSE(dialog.review_available());
  const auto before = f.chat->event_log().events();
  CHECK_FALSE(dialog.execute(surfaces::ApplyConversationSummary{}));
  CHECK(f.chat->event_log().events() == before);
  CHECK(draft == "Second draft");
  CHECK(f.backend.requests().size() == 1);
}
TEST_CASE("Context summary editor keeps separate text after stale save and "
          "rejects oversized paste",
          "[contextdialog][edit]") {
  Fixture f;
  const auto candidate = f.candidate();
  std::string draft = "Chat composer stays here";
  adapters::ConversationContextDialog dialog{*f.chat, [&] { return draft; }};
  REQUIRE(
      dialog.execute(surfaces::EditConversationSummary{candidate.summary_id}));
  static_cast<void>(rendered(dialog));
  const auto original = dialog.editing_text();
  REQUIRE(dialog.on_event(termforge::PasteEvent{
      std::string(domain::summary_maximum_text_bytes, 'x')}));
  CHECK(dialog.editing_text() == original);
  REQUIRE(dialog.on_event(termforge::PasteEvent{" edited"}));
  const auto edited = dialog.editing_text();
  REQUIRE(f.chat->set_conversation_policy(0, domain::ConversationMode::rolling,
                                          {}));
  REQUIRE(dialog.on_event(key(termforge::Key::Tab)));
  REQUIRE(dialog.on_event(key(termforge::Key::Enter)));
  CHECK(dialog.editing_text() == edited);
  CHECK(dialog.status().find("stale") != std::string::npos);
  CHECK(draft == "Chat composer stays here");
  CHECK(f.backend.requests().size() == 1);
}
TEST_CASE(
    "Context summary generation remains explicit with review before Apply",
    "[contextdialog][generation]") {
  Fixture f;
  std::string draft = "Unsubmitted instruction";
  adapters::ConversationContextDialog dialog{*f.chat, [&] { return draft; }};
  REQUIRE(dialog.execute(surfaces::InspectConversation{}));
  CHECK(f.backend.requests().empty());
  REQUIRE(dialog.execute(surfaces::GenerateConversationSummary{
      {id<domain::RunId>("source-run")}}));
  CHECK(dialog.status().find("paid request") != std::string::npos);
  f.drain();
  const auto catalog = f.chat->summary_catalog();
  REQUIRE(catalog);
  REQUIRE(catalog->unpublished.size() == 1);
  CHECK(catalog->snapshot.active.empty());
  CHECK_FALSE(dialog.review_available());
  REQUIRE(dialog.execute(surfaces::ReviewConversationSummary{
      catalog->unpublished.front().intent.summary_id}));
  CHECK_FALSE(dialog.execute(surfaces::ApplyConversationSummary{}));
  CHECK(f.backend.requests().size() == 1);
  CHECK(draft == "Unsubmitted instruction");
}

TEST_CASE("Transcript summary exclusion preserves the default projection and "
          "reasoning redraw",
          "[contextdialog][transcript]") {
  Fixture f;
  const auto generated = f.generate();
  adapters::TranscriptView default_view;
  REQUIRE(default_view.rebuild(f.chat->event_log().events()));
  adapters::TranscriptView filtered;
  REQUIRE(filtered.rebuild(f.chat->event_log().events(), {generated.run_id}));
  CHECK(filtered.session_projection().runs().size() ==
        default_view.session_projection().runs().size());
  const auto text = [](adapters::TranscriptView& view) {
    termforge::Screen screen{160, 40};
    view.set_geometry({0, 0, 160, 40});
    view.draw(screen);
    std::string value;
    for (int row{}; row < 40; ++row) {
      for (int col{}; col < 160; ++col)
        value += screen.text_at(col, row);
      value += '\n';
    }
    return value;
  };
  CHECK(text(default_view)
            .find("Keep the open task and unresolved constraints.") !=
        std::string::npos);
  CHECK(text(filtered).find("Keep the open task and unresolved constraints.") ==
        std::string::npos);
  CHECK(text(filtered).find("Preserve the open task and its constraints.") !=
        std::string::npos);
  REQUIRE(filtered.set_reasoning_visibility(
      adapters::ReasoningVisibility::expanded));
  CHECK(text(filtered).find("Keep the open task and unresolved constraints.") ==
        std::string::npos);
  REQUIRE(filtered.rebuild(f.chat->event_log().events()));
  CHECK(text(filtered).find("Keep the open task and unresolved constraints.") !=
        std::string::npos);
}

TEST_CASE(
    "Failed replacement preview cannot leave an older summary armed for Apply",
    "[contextdialog][stale]") {
  Fixture f;
  const auto first = f.candidate();
  const auto second = f.candidate();
  const auto edited = f.chat->edit_conversation_summary(
      f.chat->event_log().last_sequence(), version(second),
      std::string(3500, 'x'));
  INFO((edited ? "edited within the producer bound" : edited.error().message));
  REQUIRE(edited);
  f.models.window = 3000;
  REQUIRE(f.chat->select_model(id<domain::ModelId>("small")));
  REQUIRE(f.chat->set_conversation_policy(0, domain::ConversationMode::rolling,
                                          {}));
  adapters::ConversationContextDialog dialog{
      *f.chat, [] { return std::string{"draft"}; }};
  const auto first_preview = dialog.execute(
      surfaces::PreviewConversationSummary{first.summary_id, {}});
  INFO((first_preview ? "first preview fits" : first_preview.error().message));
  REQUIRE(first_preview);
  REQUIRE(dialog.review_available());
  const auto before = f.chat->event_log().events();
  CHECK_FALSE(dialog.execute(
      surfaces::PreviewConversationSummary{second.summary_id, {}}));
  CHECK_FALSE(dialog.review_available());
  CHECK_FALSE(dialog.execute(surfaces::ApplyConversationSummary{}));
  CHECK(f.chat->event_log().events() == before);
  CHECK(f.backend.requests().size() == 2);
}
