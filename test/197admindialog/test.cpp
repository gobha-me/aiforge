#include <aiforge/adapters/admin_dialog.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {
using namespace aiforge;
using namespace aiforge::surfaces;
template <class T> auto id(std::string value) -> T {
  auto result = T::from(std::move(value));
  REQUIRE(result);
  return std::move(*result);
}
class Controls final : public AdminControls {
 public:
  AdminState state;
  std::vector<AdminAction> actions;
  bool refuse{};
  Controls() {
    state.targets.push_back({id<domain::OpsTargetId>("local"), "Local Linux",
                             domain::OpsTargetKind::linux_local});
    state.session = id<domain::SessionId>("session");
    state.session_epoch = 1;
    state.phase = AdminPhase::idle;
  }
  auto execute(const AdminAction& value)
      -> std::expected<void, ManualOpsFailure> override {
    actions.push_back(value);
    if (refuse)
      return std::unexpected(ManualOpsFailure{ManualOpsErrorCode::busy});
    return {};
  }
  auto inspect() const noexcept -> const AdminState& override { return state; }
};
auto key(char32_t character) -> termforge::Event {
  return termforge::KeyEvent{termforge::Key::Char, character};
}
auto render(adapters::AdminDialog& dialog, int cols, int rows) -> std::string {
  termforge::Screen screen{cols, rows};
  dialog.draw(screen);
  std::string result;
  for (int row = 0; row < rows; ++row) {
    for (int col = 0; col < cols; ++col)
      result += screen.text_at(col, row);
    result += '\n';
  }
  return result;
}
auto click(adapters::AdminDialog& dialog, std::string_view label) -> bool {
  termforge::Screen screen{120, 32};
  dialog.draw(screen);
  for (int row = 0; row < 32; ++row)
    for (int col = 0; col < 120; ++col) {
      std::string text;
      for (int i = 0; i < static_cast<int>(label.size()) && col + i < 120; ++i)
        text += screen.text_at(col + i, row);
      if (text == label)
        return dialog.on_event(termforge::MouseEvent{col, row, 0, true});
    }
  return false;
}
auto committed(std::string event = "observed") -> CommittedOpsObservation {
  domain::OpsObservationRequest request{
      id<domain::OpsOwnerId>("owner"),
      id<domain::SessionId>("session"),
      id<domain::OpsRequestId>("request"),
      {id<domain::OpsTargetId>("local"),
       id<domain::OpsConfigurationRevision>("revision"),
       domain::LinuxOpsIdentity{domain::LinuxExecutionScope::container,
                                "12345678-1234-1234-1234-123456789abc", 42,
                                43}},
      7,
      domain::OpsObservationOperation::linux_services,
      {},
      1,
      {}};
  domain::OpsObservation observation{
      request,
      domain::EventTimestamp{std::chrono::milliseconds{1000}},
      domain::EventTimestamp{std::chrono::milliseconds{1001}},
      domain::OpsObservationCompleteness::complete,
      0,
      0,
      {},
      domain::LinuxServicesObservation{
          {{{"example.service", {}},
            domain::OpsServiceState::failed,
            domain::OpsObservationReason::failed_exit,
            7,
            3}}}};
  REQUIRE(domain::validate_recorded_ops_observation(observation));
  return {{id<domain::RunId>("run"), id<domain::InvocationId>("invocation")},
          id<domain::EventId>(std::move(event)),
          id<domain::EventId>("result"),
          std::move(observation)};
}
} // namespace
TEST_CASE("Admin rendering and view navigation send no operation",
          "[admin][dialog]") {
  Controls controls;
  adapters::AdminDialog dialog{controls};
  for (const auto& [cols, rows] :
       {std::pair{120, 32}, {80, 24}, {40, 12}, {20, 5}, {2, 2}, {1, 1}}) {
    for (const bool toolbar : {true, false}) {
      dialog.set_toolbar_visible(toolbar);
      for (const char32_t view : {U't', U'h', U's', U'd'}) {
        REQUIRE(dialog.on_event(key(view)));
        REQUIRE(dialog.refresh());
        CHECK_FALSE(render(dialog, cols, rows).empty());
      }
    }
  }
  CHECK(controls.actions.empty());
  REQUIRE(dialog.on_event(termforge::KeyEvent{termforge::Key::Escape}));
  REQUIRE(controls.actions.size() == 1);
  CHECK(std::holds_alternative<AdminCloseView>(controls.actions.front()));
}
TEST_CASE("Admin explicit buttons retain typed intent and failed status",
          "[admin][dialog]") {
  Controls controls;
  adapters::AdminDialog dialog{controls};
  REQUIRE(click(dialog, "[ Use target ]"));
  REQUIRE(controls.actions.size() == 1);
  CHECK(std::get<AdminSelectTarget>(controls.actions.back()).target.value() ==
        "local");
  REQUIRE(dialog.on_event(key(U'h')));
  REQUIRE(click(dialog, "[ Read ]"));
  REQUIRE(controls.actions.size() == 2);
  CHECK(std::holds_alternative<AdminReadHealth>(controls.actions.back()));
  REQUIRE(dialog.on_event(key(U's')));
  REQUIRE(click(dialog, "[ Read ]"));
  CHECK(std::holds_alternative<AdminReadServices>(controls.actions.back()));
  controls.refuse = true;
  CHECK_FALSE(dialog.execute(AdminReadHealth{}));
  CHECK(dialog.status() == "Session or source worker is busy");
  REQUIRE(click(dialog, "[ Cancel ]"));
  CHECK(std::holds_alternative<AdminCancel>(controls.actions.back()));
}
TEST_CASE("Admin service actions keep the displayed snapshot identity",
          "[admin][dialog]") {
  Controls controls;
  controls.state.snapshots[1] = committed();
  adapters::AdminDialog dialog{controls};
  REQUIRE(dialog.on_event(key(U's')));
  CHECK(dialog.display_text().find("observed") != std::string::npos);
  CHECK(dialog.display_text().find("example.service") != std::string::npos);
  // Replace the controller cache without refreshing the displayed row.
  controls.state.snapshots[1] = committed("replacement");
  REQUIRE(click(dialog, "example.service"));
  REQUIRE_FALSE(controls.actions.empty());
  const auto& action =
      std::get<AdminReadCachedService>(controls.actions.back());
  CHECK(action.session.value() == "session");
  CHECK(action.inventory_event.value() == "observed");
  CHECK(action.selection_generation == 7);
  CHECK(action.row == 0);
}
TEST_CASE("Admin preserves evidence identity and hides malformed labels",
          "[admin][dialog]") {
  Controls controls;
  controls.state.targets.front().display_name = "bad\033[31m";
  controls.state.snapshots[1] = committed();
  controls.state.pending_target = id<domain::OpsTargetId>("other");
  adapters::AdminDialog dialog{controls};
  REQUIRE(dialog.on_event(key(U's')));
  const auto before = dialog.display_text();
  controls.refuse = true;
  CHECK_FALSE(dialog.execute(AdminReadServices{}));
  CHECK(dialog.display_text() == before);
  CHECK(render(dialog, 120, 32).find('\033') == std::string::npos);
  ++controls.state.session_epoch;
  controls.state.snapshots = {};
  REQUIRE(dialog.refresh());
  CHECK(dialog.display_text().find("example.service") == std::string::npos);
}

TEST_CASE("Admin close is dispatched once per showing", "[admin][dialog]") {
  Controls controls;
  adapters::AdminDialog dialog{controls};
  unsigned closed{};
  dialog.on_close([&closed] { ++closed; });
  static_cast<void>(render(dialog, 120, 32));
  REQUIRE(dialog.on_event(termforge::KeyEvent{termforge::Key::Escape}));
  REQUIRE(dialog.on_event(termforge::KeyEvent{termforge::Key::Escape}));
  CHECK(closed == 1);
  REQUIRE(controls.actions.size() == 1);
  CHECK(std::holds_alternative<AdminCloseView>(controls.actions.front()));
  static_cast<void>(render(dialog, 120, 32));
  REQUIRE(dialog.on_event(termforge::KeyEvent{termforge::Key::Escape}));
  CHECK(closed == 2);
  CHECK(controls.actions.size() == 2);
}

TEST_CASE("Admin F10 restores and focuses the hidden menu", "[admin][dialog]") {
  Controls controls;
  adapters::AdminDialog dialog{controls};
  dialog.set_toolbar_visible(false);
  static_cast<void>(render(dialog, 120, 32));
  REQUIRE(dialog.on_event(termforge::KeyEvent{termforge::Key::F10}));
  static_cast<void>(render(dialog, 120, 32));
  REQUIRE(dialog.on_event(termforge::KeyEvent{termforge::Key::Enter}));
  CHECK(render(dialog, 120, 32).find("Use highlighted target") !=
        std::string::npos);
  CHECK(controls.actions.empty());
  REQUIRE(dialog.on_event(termforge::KeyEvent{termforge::Key::Escape}));
  CHECK(controls.actions.empty());
}

TEST_CASE("Malformed Admin evidence cannot create a service action",
          "[admin][dialog]") {
  Controls controls;
  auto invalid = committed();
  auto& services =
      std::get<domain::LinuxServicesObservation>(invalid.observation.payload);
  services.services.front().identity.unit_name = "bad\033.service";
  controls.state.snapshots[1] = std::move(invalid);
  adapters::AdminDialog dialog{controls};
  REQUIRE(dialog.on_event(key(U's')));
  CHECK(dialog.display_text() ==
        "Committed observation could not be rendered.");
  CHECK(render(dialog, 120, 32).find('\033') == std::string::npos);
  REQUIRE(dialog.on_event(key(U'd')));
  REQUIRE(click(dialog, "[ Read ]"));
  CHECK(controls.actions.empty());
  CHECK(dialog.status() == "Read loaded services and choose a service first");
}

TEST_CASE("Admin explicit actions remain reachable with no visible buttons",
          "[admin][dialog]") {
  Controls controls;
  adapters::AdminDialog dialog{controls};
  dialog.set_toolbar_visible(false);
  static_cast<void>(render(dialog, 1, 1));
  REQUIRE(dialog.on_event(key(U'u')));
  REQUIRE(controls.actions.size() == 1);
  CHECK(std::get<AdminSelectTarget>(controls.actions.back()).target.value() ==
        "local");
  REQUIRE(dialog.on_event(key(U'h')));
  REQUIRE(dialog.on_event(key(U'r')));
  CHECK(std::holds_alternative<AdminReadHealth>(controls.actions.back()));
  REQUIRE(dialog.on_event(key(U'c')));
  CHECK(std::holds_alternative<AdminCancel>(controls.actions.back()));
}
