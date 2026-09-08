#include <aiforge/surfaces/conversation_commands.hpp>
#include <catch2/catch_test_macros.hpp>
#include <stop_token>
using namespace aiforge::surfaces;
TEST_CASE(
    "conversation controls reject malformed ambiguous and unbounded commands",
    "[conversationcommands][failure]") {
  for (const auto* text : {"unknown",
                           "mode",
                           "mode automatic",
                           "mode rolling extra",
                           "pin",
                           "unpin a b",
                           "toolbar maybe",
                           "history extra",
                           "summary generate",
                           "summary generate a a",
                           "summary review",
                           "summary review a b",
                           "summary preview a replace",
                           "summary preview a replace b b",
                           "summary preview a b",
                           "summary apply a",
                           "summary discard extra",
                           "summary unknown a",
                           "summary list a",
                           "mode\nrolling"}) {
    INFO(text);
    CHECK_FALSE(parse_conversation_command(text));
  }
  CHECK_FALSE(parse_conversation_command(std::string{"mode "} + char(0xff)));
  CHECK_FALSE(parse_conversation_command(std::string(65537, 'x')));
  std::string oversized = "summary generate";
  for (unsigned i = 0; i < 129; ++i)
    oversized += " run-" + std::to_string(i);
  CHECK_FALSE(parse_conversation_command(oversized));
  std::stop_source cancelled;
  cancelled.request_stop();
  auto result =
      parse_conversation_command("mode rolling", cancelled.get_token());
  REQUIRE_FALSE(result);
  CHECK(result.error().code == ConversationCommandErrorCode::cancelled);
}
TEST_CASE("conversation commands produce explicit bounded typed actions",
          "[conversationcommands][smoke]") {
  REQUIRE(parse_conversation_command(""));
  CHECK(std::holds_alternative<InspectConversation>(
      *parse_conversation_command("history")));
  CHECK(
      std::get<SetConversationMode>(*parse_conversation_command("mode rolling"))
          .mode == aiforge::domain::ConversationMode::rolling);
  CHECK_FALSE(
      std::get<PinConversationRun>(*parse_conversation_command("unpin run-1"))
          .pinned);
  CHECK_FALSE(
      std::get<SetContextToolbar>(*parse_conversation_command("toolbar hide"))
          .visible);
  CHECK(std::holds_alternative<InspectConversationSummaries>(
      *parse_conversation_command("summary")));
  const auto generated = parse_conversation_command("summary generate one two");
  REQUIRE(generated);
  CHECK(std::get<GenerateConversationSummary>(*generated).run_ids.size() == 2);
  const auto preview =
      parse_conversation_command("summary preview candidate replace old older");
  REQUIRE(preview);
  CHECK(std::get<PreviewConversationSummary>(*preview).replacements.size() ==
        2);
  CHECK(std::holds_alternative<ReviewConversationSummary>(
      *parse_conversation_command("summary review candidate")));
  CHECK(std::holds_alternative<EditConversationSummary>(
      *parse_conversation_command("summary edit candidate")));
  CHECK(std::holds_alternative<DisableConversationSummary>(
      *parse_conversation_command("summary disable candidate")));
  CHECK(std::holds_alternative<ApplyConversationSummary>(
      *parse_conversation_command("summary apply")));
  CHECK(std::holds_alternative<DiscardConversationReview>(
      *parse_conversation_command("summary discard")));
}
