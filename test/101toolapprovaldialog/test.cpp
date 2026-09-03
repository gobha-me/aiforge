#include <aiforge/adapters/tool_approval_dialog.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <termforge/core/screen.hpp>
#include <termforge/core/types.hpp>

namespace {

using namespace aiforge;

auto key(const termforge::Key value) -> termforge::Event {
  termforge::KeyEvent event;
  event.key = value;
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

auto request(std::string tool_name = "read_repository_file")
    -> adapters::PendingToolApprovalView {
  return {std::move(tool_name),
          {domain::Effect::read},
          {{domain::Effect::read, "filesystem.root", "/work/repository"}}};
}

} // namespace

TEST_CASE("tool approval rejects invalid and unbounded presentation input",
          "[adapter][tool-approval][dialog][failure]") {
  SECTION("limits must be positive") {
    termforge::ChoiceWizardDialog dialog;
    adapters::ToolApprovalDialogLimits limits;
    limits.maximum_scopes = 0;
    adapters::ToolApprovalDialogController controller{dialog, limits};
    const auto result = controller.present(request(), [](auto) {});
    REQUIRE_FALSE(result);
    CHECK(result.error().code ==
          adapters::ToolApprovalDialogErrorCode::invalid_limits);
  }

  SECTION("tool name is bounded and control safe") {
    termforge::ChoiceWizardDialog dialog;
    adapters::ToolApprovalDialogLimits limits;
    limits.maximum_tool_name_bytes = 4;
    adapters::ToolApprovalDialogController controller{dialog, limits};
    CHECK(controller.present(request("safe"), [](auto) {}));

    termforge::ChoiceWizardDialog over_dialog;
    adapters::ToolApprovalDialogController over{over_dialog, limits};
    CHECK_FALSE(over.present(request("overs"), [](auto) {}));

    for (const auto& unsafe :
         {std::string{"bad\nname"}, std::string{"bad\x1b", 4},
          std::string{"bad\xc2\x85", 5}, std::string{"bad\xe2\x80\xae", 6}}) {
      termforge::ChoiceWizardDialog unsafe_dialog;
      adapters::ToolApprovalDialogController unsafe_controller{unsafe_dialog};
      CHECK_FALSE(unsafe_controller.present(request(unsafe), [](auto) {}));
    }
  }

  SECTION("effects and scopes must be exact and non-duplicated") {
    termforge::ChoiceWizardDialog dialog;
    adapters::ToolApprovalDialogController controller{dialog};
    auto input = request();
    input.effects.clear();
    CHECK_FALSE(controller.present(input, [](auto) {}));

    input = request();
    input.effects.push_back(domain::Effect::read);
    CHECK_FALSE(controller.present(input, [](auto) {}));

    input = request();
    input.effects = {static_cast<domain::Effect>(255)};
    input.scopes.clear();
    CHECK_FALSE(controller.present(input, [](auto) {}));

    input = request();
    input.scopes.front().effect = domain::Effect::write;
    CHECK_FALSE(controller.present(input, [](auto) {}));

    input = request();
    input.scopes.push_back(input.scopes.front());
    CHECK_FALSE(controller.present(input, [](auto) {}));

    input = request();
    input.scopes.front().kind = "unsafe\tfield";
    CHECK_FALSE(controller.present(input, [](auto) {}));

    input = request();
    input.scopes.front().value = "unsafe\xe2\x80\xae";
    CHECK_FALSE(controller.present(input, [](auto) {}));
  }

  SECTION("effect, scope, and field count bounds are enforced") {
    adapters::ToolApprovalDialogLimits limits;
    limits.maximum_effects = 1;
    limits.maximum_scopes = 1;
    limits.maximum_scope_kind_bytes = 3;
    limits.maximum_scope_value_bytes = 4;

    termforge::ChoiceWizardDialog effects_dialog;
    adapters::ToolApprovalDialogController effects_controller{effects_dialog,
                                                              limits};
    auto input = request();
    input.effects.push_back(domain::Effect::network);
    CHECK_FALSE(effects_controller.present(input, [](auto) {}));

    termforge::ChoiceWizardDialog scopes_dialog;
    adapters::ToolApprovalDialogController scopes_controller{scopes_dialog,
                                                             limits};
    input = request();
    input.scopes.front() = {domain::Effect::read, "key", "root"};
    input.scopes.push_back({domain::Effect::read, "alt", "else"});
    CHECK_FALSE(scopes_controller.present(input, [](auto) {}));

    termforge::ChoiceWizardDialog kind_dialog;
    adapters::ToolApprovalDialogController kind_controller{kind_dialog, limits};
    input = request();
    input.scopes.front() = {domain::Effect::read, "kind", "root"};
    CHECK_FALSE(kind_controller.present(input, [](auto) {}));

    termforge::ChoiceWizardDialog value_dialog;
    adapters::ToolApprovalDialogController value_controller{value_dialog,
                                                            limits};
    input.scopes.front() = {domain::Effect::read, "key", "roots"};
    CHECK_FALSE(value_controller.present(input, [](auto) {}));
  }

  SECTION("total text accepts the exact boundary and rejects one byte over") {
    adapters::ToolApprovalDialogLimits limits;
    limits.maximum_total_text_bytes = 8;
    termforge::ChoiceWizardDialog exact_dialog;
    adapters::ToolApprovalDialogController exact{exact_dialog, limits};
    auto input = request("tool");
    input.scopes = {{domain::Effect::read, "k", "abc"}};
    CHECK(exact.present(input, [](auto) {}));

    termforge::ChoiceWizardDialog over_dialog;
    adapters::ToolApprovalDialogController over{over_dialog, limits};
    input.scopes.front().value += "d";
    CHECK_FALSE(over.present(input, [](auto) {}));
  }
}

TEST_CASE("tool approval defaults to invocation-only denial",
          "[adapter][tool-approval][dialog]") {
  termforge::ChoiceWizardDialog dialog;
  adapters::ToolApprovalDialogController controller{dialog};
  std::vector<runtime::ToolApprovalResolution> resolutions;
  REQUIRE(controller.present(request(), [&](auto resolution) {
    resolutions.push_back(std::move(resolution));
  }));

  termforge::Screen screen{100, 24};
  dialog.draw(screen);
  const auto rendered = screen_text(screen);
  CHECK(rendered.find("Tool: read_repository_file") != std::string::npos);
  CHECK(rendered.find("Effects: read") != std::string::npos);
  CHECK(rendered.find("filesystem.root: /work/repository") !=
        std::string::npos);
  CHECK(rendered.find("Deny") != std::string::npos);
  CHECK(rendered.find("Allow once") != std::string::npos);

  REQUIRE(dialog.on_event(key(termforge::Key::Enter)));
  REQUIRE(resolutions.size() == 1);
  CHECK(resolutions.front().decision == domain::ApprovalDecision::denied);
  CHECK(resolutions.front().granted_scopes.empty());
  CHECK(resolutions.front().lifetime ==
        domain::ApprovalGrantLifetime::invocation);
  CHECK_FALSE(controller.active());
  CHECK_FALSE(controller.was_cancelled());
  CHECK_FALSE(controller.last_error());

  static_cast<void>(dialog.on_event(key(termforge::Key::Enter)));
  CHECK(resolutions.size() == 1);
}

TEST_CASE("tool approval grants only exact scopes for one invocation",
          "[adapter][tool-approval][dialog]") {
  termforge::ChoiceWizardDialog dialog;
  adapters::ToolApprovalDialogController controller{dialog};
  auto input = request();
  input.effects.push_back(domain::Effect::network);
  input.scopes.push_back({domain::Effect::network, "host", "api.example.test"});
  const auto expected_scopes = input.scopes;
  std::optional<runtime::ToolApprovalResolution> resolution;
  REQUIRE(controller.present(
      std::move(input), [&](auto value) { resolution = std::move(value); }));

  termforge::Screen tiny{1, 1};
  dialog.draw(tiny);
  tiny.resize(60, 18);
  dialog.draw(tiny);
  REQUIRE(dialog.on_event(key(termforge::Key::Down)));
  REQUIRE(dialog.on_event(key(termforge::Key::Enter)));

  REQUIRE(resolution);
  CHECK(resolution->decision == domain::ApprovalDecision::approved);
  CHECK(resolution->granted_scopes == expected_scopes);
  CHECK(resolution->lifetime == domain::ApprovalGrantLifetime::invocation);
  CHECK_FALSE(controller.last_error());
}

TEST_CASE("tool approval cancellation grants nothing and permits reuse",
          "[adapter][tool-approval][dialog][failure]") {
  termforge::ChoiceWizardDialog dialog;
  adapters::ToolApprovalDialogController controller{dialog};
  std::vector<runtime::ToolApprovalResolution> resolutions;
  REQUIRE(controller.present(request(), [&](auto resolution) {
    resolutions.push_back(std::move(resolution));
  }));
  CHECK_FALSE(controller.present(request(), [](auto) {}));

  REQUIRE(dialog.on_event(key(termforge::Key::Escape)));
  REQUIRE(resolutions.size() == 1);
  CHECK(resolutions.front() == (runtime::ToolApprovalResolution{
                                   domain::ApprovalDecision::cancelled,
                                   {},
                                   domain::ApprovalGrantLifetime::invocation}));
  CHECK(controller.was_cancelled());

  REQUIRE(controller.present(request(), [&](auto resolution) {
    resolutions.push_back(std::move(resolution));
  }));
  termforge::Screen reopened{80, 20};
  dialog.draw(reopened);
  REQUIRE(dialog.on_event(key(termforge::Key::Enter)));
  REQUIRE(resolutions.size() == 2);
  CHECK(resolutions.back().decision == domain::ApprovalDecision::denied);
}

TEST_CASE("tool approval contains callback exceptions",
          "[adapter][tool-approval][dialog][failure]") {
  termforge::ChoiceWizardDialog dialog;
  adapters::ToolApprovalDialogController controller{dialog};
  REQUIRE(controller.present(request(), [](auto) { throw 1; }));
  CHECK_NOTHROW(dialog.on_event(key(termforge::Key::Enter)));
  REQUIRE(controller.last_error());
  CHECK(controller.last_error()->code ==
        adapters::ToolApprovalDialogErrorCode::callback_failure);
  CHECK_FALSE(controller.active());
}
