#include <aiforge/adapters/tool_approval_dialog.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
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
          {{domain::Effect::read, "filesystem.root", "/work/repository"}},
          {"aiforge.canonical-tool-json.v1",
           {"application/json", R"({"path":"README.md"})"}},
          domain::ToolRestrictionLevel::none,
          domain::ToolRestrictionLevel::none,
          domain::ToolApprovalMode::prompt,
          runtime::ToolApprovalSupplySource::per_invocation,
          {4096, 16, std::chrono::seconds{5}}};
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
    termforge::ChoiceWizardDialog exact_dialog;
    auto input = request("tool");
    input.scopes = {{domain::Effect::read, "k", "abc"}};
    limits.maximum_total_text_bytes =
        input.tool_name.size() + input.canonical_arguments.value.data.size() +
        input.scopes.front().kind.size() + input.scopes.front().value.size();
    adapters::ToolApprovalDialogController exact_at_boundary{exact_dialog,
                                                             limits};
    CHECK(exact_at_boundary.present(input, [](auto) {}));

    termforge::ChoiceWizardDialog over_dialog;
    adapters::ToolApprovalDialogController over{over_dialog, limits};
    input.scopes.front().value += "d";
    CHECK_FALSE(over.present(input, [](auto) {}));
  }

  SECTION("canonical arguments accept the exact boundary and reject one over") {
    auto input = request();
    adapters::ToolApprovalDialogLimits limits;
    limits.maximum_canonical_argument_bytes =
        input.canonical_arguments.value.data.size();
    termforge::ChoiceWizardDialog exact_dialog;
    adapters::ToolApprovalDialogController exact{exact_dialog, limits};
    CHECK(exact.present(input, [](auto) {}));

    --limits.maximum_canonical_argument_bytes;
    termforge::ChoiceWizardDialog over_dialog;
    adapters::ToolApprovalDialogController over{over_dialog, limits};
    CHECK_FALSE(over.present(input, [](auto) {}));
  }

  SECTION("approval provenance and executor limits must be consistent") {
    for (const auto mode : {domain::ToolApprovalMode::automatic,
                            domain::ToolApprovalMode::allow_all}) {
      auto input = request();
      input.approval_mode = mode;
      termforge::ChoiceWizardDialog dialog;
      adapters::ToolApprovalDialogController controller{dialog};
      CHECK_FALSE(controller.present(input, [](auto) {}));
    }

    auto input = request();
    input.supply_source = runtime::ToolApprovalSupplySource::implicit;
    termforge::ChoiceWizardDialog implicit_dialog;
    adapters::ToolApprovalDialogController implicit{implicit_dialog};
    CHECK_FALSE(implicit.present(input, [](auto) {}));

    input = request();
    input.achieved_restriction = domain::ToolRestrictionLevel::low;
    termforge::ChoiceWizardDialog mismatch_dialog;
    adapters::ToolApprovalDialogController mismatch{mismatch_dialog};
    CHECK_FALSE(mismatch.present(input, [](auto) {}));

    input = request();
    input.executor_limits.timeout = std::chrono::milliseconds{0};
    termforge::ChoiceWizardDialog limits_dialog;
    adapters::ToolApprovalDialogController invalid_limits{limits_dialog};
    CHECK_FALSE(invalid_limits.present(input, [](auto) {}));
  }

  SECTION("canonical arguments must be exact safe JSON") {
    auto input = request();
    input.canonical_arguments.value.data = R"({ "path":"README.md"})";
    termforge::ChoiceWizardDialog noncanonical_dialog;
    adapters::ToolApprovalDialogController noncanonical{noncanonical_dialog};
    CHECK_FALSE(noncanonical.present(input, [](auto) {}));

    input = request();
    input.canonical_arguments.value.data =
        std::string{"{\"path\":\"bad"} + "\xe2\x80\xae" + "\"}";
    termforge::ChoiceWizardDialog unsafe_dialog;
    adapters::ToolApprovalDialogController unsafe{unsafe_dialog};
    CHECK_FALSE(unsafe.present(input, [](auto) {}));
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

  termforge::Screen screen{180, 40};
  dialog.draw(screen);
  const auto rendered = screen_text(screen);
  CHECK(rendered.find("Tool: read_repository_file") != std::string::npos);
  CHECK(rendered.find("Effects: read") != std::string::npos);
  CHECK(rendered.find("filesystem.root: /work/repository") !=
        std::string::npos);
  CHECK(rendered.find("Selected restriction: none") != std::string::npos);
  CHECK(rendered.find("Achieved restriction: none") != std::string::npos);
  CHECK(rendered.find("Approval: prompt / per invocation") !=
        std::string::npos);
  CHECK(rendered.find(R"({"path":"README.md"})") != std::string::npos);
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
