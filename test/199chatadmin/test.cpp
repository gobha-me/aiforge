#include "../167chatevidence/fixture.hpp"
#include "fixture.hpp"
#include <aiforge/adapters/admin_dialog.hpp>
#include <aiforge/surfaces/slash_commands.hpp>

namespace {
using namespace chat_admin_test;
}

TEST_CASE("Chat Admin unavailable catalog and invalid input never fall back or "
          "submit",
          "[chat-admin][failure]") {
  Fixture f;
  bool configured = false;
  SECTION("unavailable catalog") {
  }
  SECTION("valid catalog with no selected target") {
    configured = true;
  }
  f.open(runtime::ApprovalMode::allow_all, false, configured);
  f.models.unavailable = true;
  for (const auto& command : {"/admin health", "/admin service ../../bad",
                              "/admin logs", "/admin select missing"}) {
    f.command(command);
    f.close();
  }
  f.command("/admin");
  for (const auto& size :
       {std::pair{120, 32}, std::pair{24, 8}, std::pair{60, 18}})
    static_cast<void>(rendered(*f.app, size.first, size.second));
  f.close();
  CHECK(f.catalog->factories == 0);
  CHECK(f.catalog->alpha->observations.load() == 0);
  CHECK(count<RunStarted>(f.history()) == 0);
  f.no_model();
}

TEST_CASE(
    "Chat Admin manual observations reach actual SQLite without model access",
    "[chat-admin][sqlite]") {
  Fixture f;
  bool polling = false;
  SECTION("disabled tick polling") {
  }
  SECTION("ordinary tick polling enabled") {
    polling = true;
  }
  SECTION("runtime wake delivery") {
    f.wake_only = true;
  }
  f.models.supports_tools = false;
  f.open(runtime::ApprovalMode::allow_all, polling, true, true);
  f.models.unavailable = true;
  f.select();
  f.press(U'h');
  f.press(U'r');
  f.completed(1);
  f.press(U's');
  f.press(U'r');
  f.completed(2);
  // The selected cached row supplies its original service identity, not a
  // fabricated current UID. Details reads are explicit.
  f.press(U'd');
  f.press(U'r');
  f.completed(3);
  const auto durable = f.history();
  CHECK(count<HumanObservationRequested>(durable) == 3);
  CHECK(count<OpsObservationRecorded>(durable) == 3);
  CHECK(count<ToolResultRecorded>(durable) == 3);
  CHECK(count<RunCompleted>(durable) == 3);
  CHECK(count<InferenceStarted>(durable) == 0);
  CHECK(f.catalog->alpha->observations.load() == 3);
  CHECK(f.catalog->beta->observations.load() == 0);
  f.press(U's');
  CHECK(rendered(*f.app).find("example.service") != std::string::npos);
  for (unsigned i{}; i < 10; ++i)
    f.step();
  CHECK(count<OpsObservationRecorded>(f.app->events()) == 3);
  CHECK(count<ToolResultRecorded>(f.app->events()) == 3);
  f.no_model();
}

TEST_CASE("Chat Admin approval overlays the view and stays on the manual path",
          "[chat-admin][approval]") {
  Fixture f;
  f.open(runtime::ApprovalMode::prompt);
  f.models.unavailable = true;
  f.select();
  auto* admin = f.app->top_overlay();
  REQUIRE(admin != nullptr);
  f.press(U'h');
  f.press(U'r');
  f.wait([&] { return count<ToolApprovalRequested>(f.app->events()) == 1; });
  REQUIRE(f.app->modal());
  REQUIRE(f.app->top_overlay() != admin);
  CHECK(f.catalog->alpha->observations.load() == 0);
  bool approved = false;
  ApprovalDecision decision = ApprovalDecision::denied;
  SECTION("approve once") {
    approved = true;
    decision = ApprovalDecision::approved;
    f.app->dispatch_event(key(termforge::Key::Down));
    f.app->dispatch_event(key(termforge::Key::Enter));
  }
  SECTION("deny default") {
    f.app->dispatch_event(key(termforge::Key::Enter));
  }
  SECTION("cancel approval") {
    decision = ApprovalDecision::cancelled;
    f.app->dispatch_event(key(termforge::Key::Escape));
  }
  f.wait([&] { return count<ToolApprovalDecided>(f.app->events()) == 1; });
  if (approved)
    f.completed(1);
  else
    f.wait([&] {
      return count<RunFailed>(f.app->events()) +
                 count<RunCancelled>(f.app->events()) ==
             1;
    });
  CHECK(f.app->top_overlay() == admin);
  REQUIRE(f.app->modal());
  const auto durable = f.history();
  CHECK(count<OpsObservationRecorded>(durable) == (approved ? 1 : 0));
  CHECK(count<ToolResultRecorded>(durable) == (approved ? 1 : 0));
  CHECK(count<ToolApprovalDecided>(durable) == 1);
  const auto found = std::ranges::find_if(durable, [](const auto& event) {
    return std::holds_alternative<ToolApprovalDecided>(event.payload);
  });
  REQUIRE(found != durable.end());
  CHECK(std::get<ToolApprovalDecided>(found->payload).decision == decision);
  CHECK(f.catalog->alpha->observations.load() == (approved ? 1 : 0));
  f.close();
  f.no_model();
}

TEST_CASE(
    "Chat Admin actions during an ordinary run neither drain nor cancel it",
    "[chat-admin][interleave]") {
  Fixture f;
  f.open();
  f.backend.unavailable = false;
  f.command("ordinary prompt");
  f.wait([&] { return f.backend.state->nexts.load() >= 2; });
  REQUIRE(f.backend.state->gate->await());
  REQUIRE(f.backend.state->starts.load() == 1);
  const auto before = f.history();
  f.command("/admin");
  REQUIRE(f.app->modal());
  f.press(U'h');
  f.press(U'r');
  f.press(U't');
  f.press(U'u');
  for (unsigned i{}; i < 10; ++i)
    f.step();
  f.press(U'c');
  f.close();
  f.command("/admin service ../../bad");
  f.close();
  CHECK(f.catalog->factories == 0);
  CHECK(f.catalog->alpha->observations.load() == 0);
  CHECK(count<RunCancelRequested>(f.history()) == 0);
  CHECK(count<RunCancelled>(f.history()) == 0);
  CHECK(f.history() == before);
  CHECK(f.backend.state->starts.load() == 1);
  f.command("retain this busy draft");
  CHECK(rendered(*f.app).find("retain this busy draft") != std::string::npos);
  CHECK(f.history() == before);
  CHECK(f.backend.state->starts.load() == 1);
  f.command("/admin");
  REQUIRE(f.app->modal());
  f.close();
  CHECK(count<RunCancelled>(f.history()) == 0);
  // Normal runtime wakes keep their preexisting ordinary-drain semantics.
  f.app->dispatch_event(termforge::ErrorEvent{
      termforge::Severity::Info, "aiforge.runtime", "events-ready"});
  CHECK(f.history().size() > before.size());
  CHECK(f.backend.state->starts.load() == 1);
  CHECK(count<RunCancelled>(f.history()) == 0);
}

TEST_CASE("Chat Admin failed candidate preserves selected target and last good "
          "evidence",
          "[chat-admin][selection]") {
  Fixture f;
  f.catalog->beta->preparation_failure = true;
  f.open();
  f.select();
  f.press(U'h');
  f.press(U'r');
  f.completed(1);
  f.close();
  f.command("/admin select beta");
  f.wait([&] {
    return f.catalog->beta->preparations.load() == 1 &&
           rendered(*f.app).find("Admin observation failed") !=
               std::string::npos;
  });
  CHECK(rendered(*f.app).find("Active target: alpha") != std::string::npos);
  f.press(U'h');
  CHECK(rendered(*f.app).find("alpha") != std::string::npos);
  f.press(U'r');
  f.completed(2);
  const auto durable = f.history();
  for (const auto& event : durable)
    if (const auto* observation =
            std::get_if<OpsObservationRecorded>(&event.payload))
      CHECK(observation->observation.request.target.target_id ==
            id<OpsTargetId>("alpha"));
  CHECK(f.catalog->beta->observations.load() == 0);
  CHECK(f.app->status_text() != "Admin operation unavailable");
  f.no_model();
}

TEST_CASE(
    "Chat Admin cancelled held preparation cannot bind after late completion",
    "[chat-admin][lifetime]") {
  Fixture f;
  auto gate = std::make_shared<Gate>();
  f.catalog->beta->preparation_gate = gate;
  f.open();
  f.select();
  f.close();
  f.command("/admin select beta");
  REQUIRE(gate->await());
  f.press(U'c');
  gate->release();
  f.wait([&] { return f.catalog->beta->destroyed.load() == 1; });
  CHECK(rendered(*f.app).find("Active target: alpha") != std::string::npos);
  f.press(U'h');
  f.press(U'r');
  f.completed(1);
  CHECK(f.catalog->alpha->observations.load() == 1);
  CHECK(f.catalog->beta->observations.load() == 0);
  f.no_model();
}

TEST_CASE(
    "Chat Admin rejected observation append never exposes a partial success",
    "[chat-admin][storage]") {
  Fixture f;
  bool polling = false;
  SECTION("manual tick path") {
  }
  SECTION("normal model tick polling enabled") {
    polling = true;
  }
  SECTION("manual runtime wake path") {
    f.wake_only = true;
  }
  f.open(runtime::ApprovalMode::allow_all, polling);
  f.select();
  f.store.refuse_observation = true;
  f.press(U'h');
  f.press(U'r');
  f.wait([&] { return f.store.refused != 0; });
  const auto observed_log =
      std::vector<RunEvent>{f.app->events().begin(), f.app->events().end()};
  for (unsigned i{}; i < 10; ++i)
    f.step();
  CHECK(count<OpsObservationRecorded>(f.history()) == 0);
  CHECK(count<ToolResultRecorded>(f.history()) == 0);
  CHECK(count<OpsObservationRecorded>(f.app->events()) == 0);
  CHECK(count<ToolResultRecorded>(f.app->events()) == 0);
  CHECK(std::ranges::equal(f.app->events(), observed_log));
  CHECK(f.store.refused == 1);
  if (const auto failure = f.app->failure_state()) {
    CHECK(failure->kind == cli::CommandFailureKind::runtime);
    CHECK_FALSE(failure->message.empty());
  }
  CHECK(f.catalog->alpha->observations.load() == 1);
  f.no_model();
}

TEST_CASE(
    "Chat Admin failed session candidate retains its usable current selection",
    "[chat-admin][session]") {
  Fixture f;
  f.open();
  f.select();
  f.close();
  SECTION("missing session") {
    f.command("/session resume absent");
  }
  SECTION("replay refusal") {
    REQUIRE(f.store.sqlite->create_session({id<SessionId>("other"), {}}, {}));
    f.store.refuse_replay = true;
    f.command("/session resume other");
  }
  // Failed command leaves its draft available; clear it through normal editing.
  f.app->dispatch_event(key(termforge::Key::Char, U'c', true));
  f.command("/admin");
  REQUIRE(f.app->modal());
  CHECK(rendered(*f.app).find("Active target: alpha") != std::string::npos);
  f.press(U'h');
  f.press(U'r');
  f.completed(1);
  CHECK(count<OpsObservationRecorded>(f.history()) == 1);
  CHECK(f.backend.state->starts.load() == 0);
}

TEST_CASE(
    "Chat Admin teardown releases no borrowed application state into workers",
    "[chat-admin][lifetime]") {
  Fixture f;
  auto gate = std::make_shared<Gate>();
  f.open();
  SECTION("preparing source") {
    f.catalog->alpha->preparation_gate = gate;
    f.command("/admin select alpha");
  }
  SECTION("observing source") {
    f.select();
    f.catalog->alpha->observation_gate = gate;
    f.press(U'h');
    f.press(U'r');
  }
  f.wait_for_gate(gate);
  REQUIRE(gate->await());
  auto retained = f.catalog->alpha;
  f.app.reset();
  gate->release();
  REQUIRE(until([&] { return retained->destroyed.load() == 1; }));
  CHECK(count<OpsObservationRecorded>(f.history()) == 0);
  CHECK(f.backend.state->starts.load() == 0);
}

TEST_CASE("Chat Admin cancellation discards a late source result without "
          "continuation",
          "[chat-admin][cancellation]") {
  Fixture f;
  auto gate = std::make_shared<Gate>();
  f.catalog->alpha->observation_gate = gate;
  f.open();
  f.select();
  f.press(U'h');
  f.press(U'r');
  f.wait_for_gate(gate);
  REQUIRE(gate->await());
  f.press(U'c');
  f.wait([&] { return count<RunCancelled>(f.app->events()) == 1; });
  gate->release();
  for (unsigned i{}; i < 25; ++i)
    f.step();
  CHECK(count<OpsObservationRecorded>(f.history()) == 0);
  CHECK(count<ToolResultRecorded>(f.history()) == 0);
  CHECK(count<RunCancelled>(f.history()) == 1);
  CHECK(count<RunCancelled>(f.app->events()) == 1);
  f.no_model();
}

TEST_CASE("Chat Admin successful session switch requires fresh selection",
          "[chat-admin][session]") {
  Fixture f;
  f.open();
  f.select();
  f.press(U'h');
  f.press(U'r');
  f.completed(1);
  f.close();
  REQUIRE(f.store.sqlite->create_session({id<SessionId>("other"), {}}, {}));
  f.command("/session resume other");
  REQUIRE(f.app->status_text().find("Resumed session other") !=
          std::string_view::npos);
  f.command("/admin");
  CHECK(rendered(*f.app).find("Active target: none") != std::string::npos);
  f.press(U'h');
  const auto* view =
      dynamic_cast<const adapters::AdminDialog*>(f.app->top_overlay());
  REQUIRE(view != nullptr);
  CHECK(view->display_text().find(
            "observation.request.session_id=\"session\"") != std::string::npos);
  CHECK(rendered(*f.app).find("Committed evidence") != std::string::npos);
  f.press(U'r');
  CHECK(count<HumanObservationRequested>(f.history("other")) == 0);
  CHECK(f.catalog->alpha->observations.load() == 1);
  f.close();
  f.select("beta");
  f.press(U'h');
  f.press(U'r');
  f.completed(1);
  const auto replacement = f.history("other");
  CHECK(count<OpsObservationRecorded>(replacement) == 1);
  for (const auto& event : replacement)
    if (const auto* observation =
            std::get_if<OpsObservationRecorded>(&event.payload)) {
      CHECK(observation->observation.request.session_id ==
            id<SessionId>("other"));
      CHECK(observation->observation.request.target.target_id ==
            id<OpsTargetId>("beta"));
    }
  CHECK(count<OpsObservationRecorded>(f.history()) == 1);
  CHECK(f.backend.state->starts.load() == 0);
}

TEST_CASE(
    "Chat Admin hidden toolbar keeps command access and redraw remains passive",
    "[chat-admin][presentation]") {
  Fixture f;
  f.open();
  f.command("/context toolbar hide");
  f.command("/admin toolbar hide");
  f.close();
  f.command("/admin");
  REQUIRE(f.app->modal());
  for (char32_t view : {U't', U'h', U's', U'd'}) {
    f.press(view);
    for (const auto& size :
         {std::pair{120, 32}, std::pair{24, 8}, std::pair{60, 18}})
      static_cast<void>(rendered(*f.app, size.first, size.second));
  }
  f.app->dispatch_event(key(termforge::Key::F10));
  static_cast<void>(rendered(*f.app));
  f.close();
  CHECK(f.catalog->factories == 0);
  CHECK(f.catalog->alpha->observations.load() == 0);
  CHECK(f.history().empty());
  f.no_model();
}

TEST_CASE("Chat Admin toolbar and slash paths share explicit selection and "
          "read behavior",
          "[chat-admin][menu]") {
  Fixture f;
  f.open();
  REQUIRE(f.click("Admin"));
  REQUIRE(f.click("Open Admin"));
  REQUIRE(f.app->modal());
  CHECK(f.catalog->factories == 0);
  f.press(U'u');
  f.wait([&] {
    return rendered(*f.app).find("Active target: alpha") != std::string::npos;
  });
  CHECK(f.catalog->alpha->observations.load() == 0);
  f.close();
  f.command("/admin health");
  f.completed(1);
  CHECK(rendered(*f.app).find("[ Read ]") != std::string::npos);
  f.close();
  f.command("/admin services");
  f.completed(2);
  CHECK(rendered(*f.app, 60, 24).find("example.service") != std::string::npos);
  CHECK(f.catalog->factories == 1);
  CHECK(f.catalog->alpha->observations.load() == 2);
  f.no_model();
}

TEST_CASE("Chat Admin preparation can retire while unrelated browser work "
          "remains held",
          "[chat-admin][shared-worker]") {
  Fixture f;
  auto files = std::make_shared<chat_evidence_test::Factory>();
  f.local_factory = files;
  auto gate = std::make_shared<Gate>();
  Release release{gate};
  f.open();
  f.command("/files add-folder /private/folder");
  f.wait([&] {
    return f.app->status_text().find("Folder added") != std::string_view::npos;
  });
  files->observation->read_gate = gate;
  f.command("/files preview 1 notes.txt");
  REQUIRE(gate->await());
  f.select();
  f.press(U'h');
  f.press(U'r');
  f.completed(1);
  {
    const std::lock_guard lock{gate->mutex};
    REQUIRE_FALSE(gate->released);
  }
  CHECK(files->observation->previews.load() == 1);
  CHECK(files->observation->owner_io.load() == 0);
  CHECK(f.catalog->alpha->observations.load() == 1);
  f.no_model();
}

TEST_CASE("Chat builtin Admin discovery shares the closed parser while a run "
          "is active",
          "[chat-admin][commands]") {
  const auto& registry = surfaces::builtin_slash_command_registry();
  surfaces::SlashCommandContext context;
  context.run_active = true;
  const auto description = registry.describe("admin", context);
  REQUIRE(description);
  REQUIRE(description->size() == 1);
  CHECK(description->front().available);
  const auto completions = registry.complete("/adm", context);
  REQUIRE(completions);
  CHECK(std::ranges::find(*completions, "admin") != completions->end());
  for (const auto* command :
       {"/admin", "/admin health", "/admin select alpha",
        "/admin service example.service", "/admin toolbar hide"}) {
    const auto result = registry.dispatch(command, context);
    REQUIRE(result);
    REQUIRE(result->has_value());
    CHECK((**result).action == surfaces::SlashCommandAction::manage_admin);
  }
  for (const auto* command :
       {"/admin logs", "/admin service ../../bad", "/admin health extra",
        "/admin select alpha extra"}) {
    const auto result = registry.dispatch(command, context);
    REQUIRE_FALSE(result);
    CHECK(result.error().code ==
          surfaces::SlashCommandErrorCode::invalid_arguments);
  }
}
