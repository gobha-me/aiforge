#include <aiforge/adapters/persona_editor_dialog.hpp>

#include <catch2/catch_test_macros.hpp>

#include <concepts>
#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include <termforge/core/screen.hpp>
#include <termforge/core/types.hpp>

namespace {

using namespace aiforge;

auto create_request(std::string text = {}, const std::size_t maximum = 64)
    -> persona::PersonaCreate {
  persona::PersonaCreate request{
      {"Reviewer", persona::PersonaFileKind::markdown, std::move(text)}, {}};
  request.limits.maximum_file_bytes = maximum;
  return request;
}

auto key(const termforge::Key value) -> termforge::Event {
  termforge::KeyEvent event;
  event.key = value;
  event.action = termforge::KeyAction::Press;
  return event;
}

auto character(const char32_t value) -> termforge::Event {
  termforge::KeyEvent event;
  event.key = termforge::Key::Char;
  event.ch = value;
  event.action = termforge::KeyAction::Press;
  return event;
}

auto screen_text(const termforge::Screen& screen) -> std::string {
  std::string result;
  for (int row{}; row < screen.rows(); ++row) {
    for (int column{}; column < screen.cols(); ++column) {
      const auto value = screen.text_at(column, row);
      result += value.empty() ? " " : std::string{value};
    }
    result += '\n';
  }
  return result;
}

auto receipt_for(const adapters::PersonaEditorSubmission& submission)
    -> std::expected<persona::PersonaWriteReceipt,
                     persona::PersonaEditorError> {
  return std::visit(
      [](const auto& request) -> std::expected<persona::PersonaWriteReceipt,
                                               persona::PersonaEditorError> {
        using Request = std::decay_t<decltype(request)>;
        if constexpr (std::same_as<Request, persona::PersonaCreate>) {
          auto prepared = persona::prepare_persona_create(request);
          if (!prepared) return std::unexpected(std::move(prepared.error()));
          return persona::PersonaWriteReceipt{std::nullopt,
                                              prepared->reference};
        } else {
          auto prepared = persona::prepare_persona_replace(request);
          if (!prepared) return std::unexpected(std::move(prepared.error()));
          return persona::PersonaWriteReceipt{request.expected,
                                              prepared->reference};
        }
      },
      submission);
}

} // namespace

TEST_CASE("persona editor reviews bounded multiline content before saving",
          "[adapter][persona][dialog]") {
  adapters::PersonaEditorDialog dialog;
  dialog.set_submission(create_request(), true);
  int save_calls{};
  int close_calls{};
  std::optional<adapters::PersonaEditorDialogResult> result;
  dialog.on_save([&](adapters::PersonaEditorSubmission submission) {
    ++save_calls;
    return receipt_for(submission);
  });
  dialog.on_result([&](adapters::PersonaEditorDialogResult value) {
    result = std::move(value);
  });
  dialog.on_close([&] { ++close_calls; });

  termforge::Screen screen{100, 30};
  dialog.draw(screen);
  REQUIRE(dialog.on_event(termforge::PasteEvent{"one\r\ntwo"}));
  REQUIRE(dialog.draft_text() == "one\ntwo");
  REQUIRE(dialog.on_event(key(termforge::Key::Tab)));
  REQUIRE(dialog.on_event(key(termforge::Key::Enter)));

  REQUIRE(dialog.stage() == adapters::PersonaEditorDialogStage::preview);
  REQUIRE(dialog.preview());
  CHECK(dialog.preview()->reference.name == "Reviewer");
  CHECK(dialog.preview()->reference.source_location == "personas/Reviewer.md");
  CHECK(dialog.preview()->reference.content_digest.byte_size == 7);
  CHECK(dialog.preview()->reference.content_digest.value.size() == 64);
  CHECK(save_calls == 0);

  screen.clear();
  dialog.draw(screen);
  const auto preview = screen_text(screen);
  CHECK(preview.find("Name: Reviewer") != std::string::npos);
  CHECK(preview.find("Source: personas/Reviewer.md") != std::string::npos);
  CHECK(preview.find("Bytes: 7 / 64") != std::string::npos);
  CHECK(preview.find("SHA-256:") != std::string::npos);
  CHECK(preview.find("Selected: yes") != std::string::npos);

  REQUIRE(dialog.on_event(key(termforge::Key::Enter)));
  CHECK(save_calls == 1);
  CHECK(close_calls == 1);
  REQUIRE(result);
  REQUIRE(result->receipt);
  CHECK_FALSE(result->effect_may_have_applied);
  CHECK(result->receipt->resulting == dialog.preview()->reference);
}

TEST_CASE("persona editor rejects invalid input before Composer mutation",
          "[adapter][persona][dialog][failure]") {
  adapters::PersonaEditorDialog dialog;
  dialog.set_submission(create_request("abc", 5), false);
  termforge::Screen screen{80, 20};
  dialog.draw(screen);

  REQUIRE(dialog.on_event(termforge::PasteEvent{std::string{"bad\x01", 4}}));
  CHECK(dialog.draft_text() == "abc");
  CHECK_FALSE(dialog.error_message().empty());

  REQUIRE(dialog.on_event(termforge::PasteEvent{std::string{"\xc3", 1}}));
  CHECK(dialog.draft_text() == "abc");

  REQUIRE(dialog.on_event(termforge::PasteEvent{std::string{"\xc2\x85", 2}}));
  CHECK(dialog.draft_text() == "abc");

  REQUIRE(dialog.on_event(termforge::PasteEvent{"\r\n"}));
  CHECK(dialog.draft_text() == "abc\n");
  REQUIRE(dialog.on_event(character(U'x')));
  CHECK(dialog.draft_text() == "abc\nx");
  REQUIRE(dialog.on_event(character(U'y')));
  CHECK(dialog.draft_text() == "abc\nx");
  CHECK(dialog.error_message().find("byte limit") != std::string::npos);

  adapters::PersonaEditorDialog unicode_dialog;
  unicode_dialog.set_submission(create_request("safe", 64), false);
  unicode_dialog.draw(screen);
  REQUIRE(unicode_dialog.on_event(character(U'\u202e')));
  CHECK(unicode_dialog.draft_text() == "safe");
  CHECK(unicode_dialog.error_message().find("unsafe controls") !=
        std::string::npos);
}

TEST_CASE("persona editor accepts safe content before metadata review",
          "[adapter][persona][dialog][failure]") {
  auto request = create_request();
  request.draft.name = "invalid name";
  adapters::PersonaEditorDialog dialog;
  dialog.set_submission(std::move(request), false);
  termforge::Screen screen{80, 20};
  dialog.draw(screen);

  REQUIRE(dialog.on_event(termforge::PasteEvent{"safe content"}));
  CHECK(dialog.draft_text() == "safe content");
  REQUIRE(dialog.on_event(key(termforge::Key::Tab)));
  REQUIRE(dialog.on_event(key(termforge::Key::Enter)));
  CHECK(dialog.stage() == adapters::PersonaEditorDialogStage::editing);
  CHECK(dialog.error_message().find("name") != std::string::npos);
}

TEST_CASE("persona editor retains its draft and preview after save failure",
          "[adapter][persona][dialog][failure]") {
  adapters::PersonaEditorDialog dialog;
  dialog.set_submission(create_request("draft"), false);
  int calls{};
  int close_calls{};
  dialog.on_save([&](adapters::PersonaEditorSubmission submission)
                     -> std::expected<persona::PersonaWriteReceipt,
                                      persona::PersonaEditorError> {
    ++calls;
    if (calls == 1) {
      return std::unexpected(persona::PersonaEditorError{
          persona::PersonaEditorErrorCode::permission_denied,
          "persona file is not writable", std::nullopt, false, false});
    }
    return receipt_for(submission);
  });
  dialog.on_close([&] { ++close_calls; });

  termforge::Screen screen{100, 30};
  dialog.draw(screen);
  REQUIRE(dialog.on_event(key(termforge::Key::Tab)));
  REQUIRE(dialog.on_event(key(termforge::Key::Enter)));
  REQUIRE(dialog.stage() == adapters::PersonaEditorDialogStage::preview);
  REQUIRE(dialog.on_event(key(termforge::Key::Enter)));

  CHECK(calls == 1);
  CHECK(close_calls == 0);
  CHECK(dialog.stage() == adapters::PersonaEditorDialogStage::preview);
  CHECK(dialog.draft_text() == "draft");
  CHECK(dialog.preview());
  CHECK(dialog.error_message() == "persona file is not writable");

  screen.clear();
  dialog.draw(screen);
  CHECK(screen_text(screen).find("Error: persona file is not writable") !=
        std::string::npos);
  REQUIRE(dialog.on_event(key(termforge::Key::Enter)));
  CHECK(calls == 2);
  CHECK(close_calls == 1);
}

TEST_CASE("persona editor blocks retry when a failed write may have applied",
          "[adapter][persona][dialog][failure]") {
  adapters::PersonaEditorDialog dialog;
  dialog.set_submission(create_request("draft"), false);
  int calls{};
  std::optional<adapters::PersonaEditorDialogResult> result;
  dialog.on_save([&](adapters::PersonaEditorSubmission)
                     -> std::expected<persona::PersonaWriteReceipt,
                                      persona::PersonaEditorError> {
    ++calls;
    return std::unexpected(persona::PersonaEditorError{
        persona::PersonaEditorErrorCode::durability_failure,
        "directory sync failed", std::nullopt, false, true});
  });
  dialog.on_result([&](adapters::PersonaEditorDialogResult value) {
    result = std::move(value);
  });

  termforge::Screen screen{100, 30};
  dialog.draw(screen);
  REQUIRE(dialog.on_event(key(termforge::Key::Tab)));
  REQUIRE(dialog.on_event(key(termforge::Key::Enter)));
  REQUIRE(dialog.on_event(key(termforge::Key::Enter)));
  REQUIRE(dialog.on_event(key(termforge::Key::Enter)));

  CHECK(calls == 1);
  CHECK(dialog.draft_text() == "draft");
  CHECK(dialog.preview());
  CHECK(dialog.error_message().find("Reload") != std::string::npos);
  REQUIRE(dialog.on_event(key(termforge::Key::Escape)));
  REQUIRE(result);
  CHECK_FALSE(result->receipt);
  CHECK(result->effect_may_have_applied);
}

TEST_CASE("persona editor goes back without losing content and cancel is inert",
          "[adapter][persona][dialog][failure]") {
  adapters::PersonaEditorDialog dialog;
  dialog.set_submission(create_request("unchanged"), false);
  int save_calls{};
  std::optional<adapters::PersonaEditorDialogResult> result;
  dialog.on_save([&](adapters::PersonaEditorSubmission submission) {
    ++save_calls;
    return receipt_for(submission);
  });
  dialog.on_result([&](adapters::PersonaEditorDialogResult value) {
    result = std::move(value);
  });

  termforge::Screen screen{100, 30};
  dialog.draw(screen);
  REQUIRE(dialog.on_event(key(termforge::Key::Tab)));
  REQUIRE(dialog.on_event(key(termforge::Key::Enter)));
  REQUIRE(dialog.on_event(key(termforge::Key::Tab)));
  REQUIRE(dialog.on_event(key(termforge::Key::Enter)));
  CHECK(dialog.stage() == adapters::PersonaEditorDialogStage::editing);
  CHECK(dialog.draft_text() == "unchanged");

  REQUIRE(dialog.on_event(key(termforge::Key::Escape)));
  CHECK(save_calls == 0);
  REQUIRE(result);
  CHECK_FALSE(result->receipt);
  CHECK_FALSE(result->effect_may_have_applied);
}

TEST_CASE("persona editor safely clamps editing and preview to tiny screens",
          "[adapter][persona][dialog][resize]") {
  adapters::PersonaEditorDialog dialog;
  dialog.set_submission(create_request("tiny"), true);

  termforge::Screen tiny{8, 3};
  dialog.draw(tiny);
  CHECK(dialog.rect().x >= 0);
  CHECK(dialog.rect().y >= 0);
  CHECK(dialog.rect().w <= tiny.cols());
  CHECK(dialog.rect().h <= tiny.rows());

  REQUIRE(dialog.on_event(key(termforge::Key::Tab)));
  REQUIRE(dialog.on_event(key(termforge::Key::Enter)));
  REQUIRE(dialog.preview());
  tiny.clear();
  dialog.draw(tiny);
  CHECK(dialog.rect().w <= tiny.cols());
  CHECK(dialog.rect().h <= tiny.rows());
}
