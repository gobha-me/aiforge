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
auto event(const std::uint64_t sequence, Payload payload)
    -> domain::RunEvent {
  return {{make_id<domain::EventId>("event-" + std::to_string(sequence)),
           make_id<domain::RunId>("run"), sequence, 1,
           domain::EventTimestamp{std::chrono::milliseconds{sequence}},
           std::nullopt, std::nullopt, std::nullopt},
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
