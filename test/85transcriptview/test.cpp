#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <aiforge/adapters/transcript_view.hpp>

namespace {

using namespace aiforge;

template <typename IdType>
auto make_id(const std::string& value) -> IdType {
  return IdType::from(value).value();
}

template <typename Payload>
auto event(const std::uint64_t sequence, Payload payload,
           std::optional<domain::InvocationId> invocation_id = std::nullopt,
           std::string run_id = "run")
    -> domain::RunEvent {
  return {{make_id<domain::EventId>("event-" + std::to_string(sequence)),
           make_id<domain::RunId>(std::move(run_id)), sequence, 1,
           domain::EventTimestamp{std::chrono::milliseconds{sequence}},
           std::nullopt, std::nullopt, std::move(invocation_id)},
          std::move(payload)};
}

auto started() -> domain::RunStarted {
  return {make_id<domain::SurfaceId>("tui"),
          make_id<domain::WorkspaceId>("chat"),
          make_id<domain::PermissionProfileId>("observe"), std::nullopt};
}

template <typename Screen>
auto cell_text(const Screen& screen, const int column, const int row)
    -> std::string_view {
  if constexpr (requires { screen.text_at(column, row); }) {
    return screen.text_at(column, row);
  } else {
    return screen.at(column, row).text;
  }
}

auto row_text(const termforge::Screen& screen, const int row) -> std::string {
  std::string result;
  for (int column = 0; column < screen.cols(); ++column) {
    const auto cell = cell_text(screen, column, row);
    if (cell.empty()) {
      result.push_back(' ');
    } else if (cell != std::string_view{"\0", 1}) {
      result += cell;
    }
  }
  return result;
}

}  // namespace

TEST_CASE("TranscriptView streams styled content without replacing history",
          "[adapter][transcript]") {
  adapters::TranscriptView view;
  view.set_geometry({0, 0, 48, 6});
  const auto inference = make_id<domain::InferenceId>("inference");
  const auto assistant = make_id<domain::MessageId>("assistant");

  REQUIRE(view.apply(event(1, started())));
  REQUIRE(view.apply(event(
      2, domain::UserContentAdded{domain::Message{
             make_id<domain::MessageId>("user"), domain::Role::user,
             {domain::TextBlock{"hello\x1b[31m hidden"}}, std::nullopt}})));
  REQUIRE(view.apply(event(
      3, domain::InferenceStarted{inference, make_id<domain::ModelId>("model")})));
  REQUIRE(view.apply(
      event(4, domain::AssistantContentStarted{assistant, inference})));
  REQUIRE(view.apply(event(
      5, domain::AssistantContentDeltaAdded{
             assistant, inference, domain::TextBlock{"**answer**"}})));
  REQUIRE(view.widget().line_count() == 2);

  termforge::Screen screen{48, 6};
  view.draw(screen);
  REQUIRE(row_text(screen, 0).find("You: hello hidden") != std::string::npos);
  REQUIRE(row_text(screen, 1).find("Assistant: answer") != std::string::npos);
  REQUIRE(termforge::any(screen.at(0, 0).attrs & termforge::Attr::Bold));

  REQUIRE(view.apply(event(
      6, domain::AssistantContentDeltaAdded{
             assistant, inference, domain::TextBlock{" more"}})));
  REQUIRE(view.widget().line_count() == 2);
  screen.clear();
  view.draw(screen);
  REQUIRE(row_text(screen, 1).find("answer more") != std::string::npos);

  REQUIRE(view.apply(
      event(7, domain::AssistantContentFinished{assistant, inference})));
  REQUIRE(view.widget().line_count() == 2);
  REQUIRE(view.apply(event(
      8, domain::InferenceFinished{inference, domain::FinishReason::stop})));
  REQUIRE(view.apply(event(9, domain::RunCompleted{})));
}

TEST_CASE("TranscriptView provides a markup-free plain fallback and resizes",
          "[adapter][transcript][fallback]") {
  adapters::TranscriptView view{adapters::TranscriptRenderMode::plain_text};
  REQUIRE(view.apply(event(1, started())));
  REQUIRE(view.apply(event(
      2, domain::UserContentAdded{domain::Message{
             make_id<domain::MessageId>("user"), domain::Role::user,
             {domain::TextBlock{"# **plain**"}}, std::nullopt}})));

  view.set_geometry({0, 0, 12, 2});
  termforge::Screen screen{12, 2};
  view.draw(screen);
  const auto visible = row_text(screen, 0) + row_text(screen, 1);
  REQUIRE(visible.find("plain") != std::string::npos);
  REQUIRE(visible.find("**") == std::string::npos);
  REQUIRE(screen.at(0, 0).attrs == termforge::Attr::None);

  view.set_geometry({0, 0, 0, 0});
  view.draw(screen);
  view.set_geometry({0, 0, 1, 1});
  screen.resize(1, 1);
  view.draw(screen);
}

TEST_CASE("TranscriptView groups questions from one invocation",
          "[adapter][transcript][questions]") {
  adapters::TranscriptView view{adapters::TranscriptRenderMode::plain_text};
  const auto invocation = make_id<domain::InvocationId>("ask-call");
  const auto first = make_id<domain::QuestionId>("first");
  const auto second = make_id<domain::QuestionId>("second");
  const auto definition = [](const domain::QuestionId& question_id,
                             std::string prompt) {
    return domain::QuestionDefinition{
        question_id, std::move(prompt), domain::QuestionSelection::one,
        {{"yes", "Yes", std::nullopt}}, true, 1, 1, std::nullopt};
  };

  REQUIRE(view.apply(event(1, started())));
  REQUIRE(view.apply(event(
      2, domain::QuestionRequested{definition(first, "First?")}, invocation)));
  REQUIRE(view.apply(event(
      3, domain::QuestionRequested{definition(second, "Second?")}, invocation)));
  REQUIRE(view.projection().items().size() == 2);
  REQUIRE(view.widget().line_count() == 1);

  REQUIRE(view.apply(event(
      4, domain::QuestionAnswered{{first, {"yes"}, std::nullopt}},
      invocation)));
  REQUIRE(view.apply(event(
      5, domain::QuestionAnswered{{second, {"yes"}, std::nullopt}},
      invocation)));
  REQUIRE(view.widget().line_count() == 1);
}

TEST_CASE("TranscriptView rejects worker-thread projection and widget mutation",
          "[adapter][transcript][failure][thread]") {
  adapters::TranscriptView view;
  std::optional<std::expected<void, adapters::TranscriptViewError>> result;
  std::jthread worker{[&] { result = view.apply(event(1, started())); }};
  worker.join();
  REQUIRE(result);
  REQUIRE_FALSE(*result);
  REQUIRE(result->error().code ==
          adapters::TranscriptViewErrorCode::wrong_thread);
  REQUIRE(view.projection().last_sequence() == 0);
  REQUIRE(view.widget().line_count() == 0);
}

TEST_CASE("TranscriptView rebuild rejects malformed replay without losing state",
          "[adapter][transcript][failure][replay]") {
  adapters::TranscriptView view;
  REQUIRE(view.apply(event(1, started())));
  const std::vector malformed{
      event(2, domain::UserContentAdded{domain::Message{
                   make_id<domain::MessageId>("user"), domain::Role::user,
                   {domain::TextBlock{"missing start"}}, std::nullopt}})};
  const auto rebuilt = view.rebuild(malformed);
  REQUIRE_FALSE(rebuilt);
  REQUIRE(view.projection().last_sequence() == 1);
}

TEST_CASE("TranscriptView clear is presentation-only and accepts later runs",
          "[adapter][transcript][clear]") {
  adapters::TranscriptView view{adapters::TranscriptRenderMode::plain_text};
  REQUIRE(view.apply(event(1, started(), std::nullopt, "first-run")));
  REQUIRE(view.apply(event(
      2,
      domain::UserContentAdded{domain::Message{
          make_id<domain::MessageId>("first-user"), domain::Role::user,
          {domain::TextBlock{"retained durably"}}, std::nullopt}},
      std::nullopt, "first-run")));
  REQUIRE(view.widget().line_count() == 1);

  REQUIRE(view.clear_view());
  REQUIRE(view.widget().line_count() == 0);
  REQUIRE(view.session_projection().runs().empty());

  REQUIRE(view.apply(event(4, started(), std::nullopt, "second-run")));
  REQUIRE(view.apply(event(
      5,
      domain::UserContentAdded{domain::Message{
          make_id<domain::MessageId>("second-user"), domain::Role::user,
          {domain::TextBlock{"visible after clear"}}, std::nullopt}},
      std::nullopt, "second-run")));
  REQUIRE(view.widget().line_count() == 1);
  REQUIRE(view.session_projection().runs().size() == 1);
}

TEST_CASE("TranscriptView renders sequential runs incrementally and on replay",
          "[adapter][transcript][session]") {
  const auto first_user = domain::UserContentAdded{domain::Message{
      make_id<domain::MessageId>("first-user"), domain::Role::user,
      {domain::TextBlock{"first"}}, std::nullopt}};
  const auto second_user = domain::UserContentAdded{domain::Message{
      make_id<domain::MessageId>("second-user"), domain::Role::user,
      {domain::TextBlock{"second"}}, std::nullopt}};
  const std::vector events{
      event(1, started(), std::nullopt, "first-run"),
      event(2, first_user, std::nullopt, "first-run"),
      event(3, domain::RunCompleted{}, std::nullopt, "first-run"),
      event(4, started(), std::nullopt, "second-run"),
      event(5, second_user, std::nullopt, "second-run"),
  };

  adapters::TranscriptView incremental{
      adapters::TranscriptRenderMode::plain_text};
  for (const auto& value : events) REQUIRE(incremental.apply(value));
  REQUIRE(incremental.session_projection().runs().size() == 2);
  REQUIRE(incremental.widget().line_count() == 2);

  adapters::TranscriptView replayed{
      adapters::TranscriptRenderMode::plain_text};
  REQUIRE(replayed.rebuild(events));
  REQUIRE(replayed.session_projection().runs().size() == 2);
  REQUIRE(replayed.widget().line_count() == 2);

  replayed.set_geometry({0, 0, 32, 3});
  termforge::Screen screen{32, 3};
  replayed.draw(screen);
  const auto visible = row_text(screen, 0) + row_text(screen, 1) +
                       row_text(screen, 2);
  REQUIRE(visible.find("You: first") != std::string::npos);
  REQUIRE(visible.find("You: second") != std::string::npos);
}
