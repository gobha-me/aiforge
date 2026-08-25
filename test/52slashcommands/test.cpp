#include <aiforge/surfaces/slash_commands.hpp>

#include <catch2/catch_test_macros.hpp>
#include <expected>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace aiforge::surfaces;

auto available(const SlashCommandContext&) -> bool { return true; }
auto unavailable(const SlashCommandContext&) -> bool { return false; }

auto show(std::string_view arguments, const SlashCommandContext&)
    -> std::expected<SlashCommandResult, SlashCommandError> {
  return SlashCommandResult{SlashCommandAction::show_help,
                            std::string{arguments}};
}

auto throws(std::string_view, const SlashCommandContext&)
    -> std::expected<SlashCommandResult, SlashCommandError> {
  throw std::runtime_error{"secret handler detail"};
}

auto fails(std::string_view, const SlashCommandContext&)
    -> std::expected<SlashCommandResult, SlashCommandError> {
  return std::unexpected(SlashCommandError{
      SlashCommandErrorCode::handler_failure, "handler rejected request"});
}

auto spec(std::string id, std::string name,
          SlashCommandAvailability predicate = available,
          SlashCommandHandler handler = show) -> SlashCommandSpec {
  return {std::move(id), std::move(name), "[value]", "Test command.",
          predicate, handler};
}

}  // namespace

TEST_CASE("slash registries reject invalid and ambiguous definitions",
          "[slash][registry][failure]") {
  auto missing = spec("missing", "missing");
  missing.handler = nullptr;
  auto registry = SlashCommandRegistry::create({missing});
  REQUIRE_FALSE(registry);
  REQUIRE(registry.error().code ==
          SlashCommandRegistryErrorCode::missing_handler);

  missing = spec("missing", "missing");
  missing.available = nullptr;
  registry = SlashCommandRegistry::create({missing});
  REQUIRE_FALSE(registry);
  REQUIRE(registry.error().code ==
          SlashCommandRegistryErrorCode::missing_availability);

  registry = SlashCommandRegistry::create(
      {spec("same", "one"), spec("same", "two")});
  REQUIRE_FALSE(registry);
  REQUIRE(registry.error().code == SlashCommandRegistryErrorCode::duplicate_id);

  registry = SlashCommandRegistry::create(
      {spec("one", "same"), spec("two", "same")});
  REQUIRE_FALSE(registry);
  REQUIRE(registry.error().code ==
          SlashCommandRegistryErrorCode::duplicate_name);

  registry = SlashCommandRegistry::create({spec("Bad", "valid")});
  REQUIRE_FALSE(registry);
  REQUIRE(registry.error().code == SlashCommandRegistryErrorCode::invalid_id);

  registry = SlashCommandRegistry::create({spec("valid", "bad\x1b")});
  REQUIRE_FALSE(registry);
  REQUIRE(registry.error().code == SlashCommandRegistryErrorCode::invalid_name);
}

TEST_CASE(
    "slash dispatch rejects malformed, bounded, unknown, and unavailable input",
    "[slash][dispatch][failure]") {
  auto registry = SlashCommandRegistry::create(
      {spec("known", "known"), spec("blocked", "blocked", unavailable)});
  REQUIRE(registry);

  const auto plain = registry->dispatch("ordinary prompt");
  REQUIRE(plain);
  REQUIRE_FALSE(plain->has_value());

  auto result = registry->dispatch("/", {}, {64, 16});
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == SlashCommandErrorCode::invalid_input);

  result = registry->dispatch("/known value", {}, {6, 16});
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == SlashCommandErrorCode::input_too_large);

  result = registry->dispatch(std::string{"/known \xC3", 8});
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == SlashCommandErrorCode::invalid_input);

  result = registry->dispatch("/known bad\x1bvalue");
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == SlashCommandErrorCode::invalid_input);

  result = registry->dispatch("/missing");
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == SlashCommandErrorCode::unknown_command);

  result = registry->dispatch("/blocked");
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == SlashCommandErrorCode::unavailable_command);
}

TEST_CASE("handler exceptions are contained and redacted",
          "[slash][dispatch][failure]") {
  auto registry = SlashCommandRegistry::create(
      {spec("throw", "throw", available, throws),
       spec("fail", "fail", available, fails)});
  REQUIRE(registry);
  const auto result = registry->dispatch("/throw");
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == SlashCommandErrorCode::internal_failure);
  REQUIRE(result.error().message.find("secret") == std::string::npos);

  const auto failed = registry->dispatch("/fail");
  REQUIRE_FALSE(failed);
  REQUIRE(failed.error().code == SlashCommandErrorCode::handler_failure);
}

TEST_CASE("one slash registry drives listing completion and exact dispatch",
          "[slash]") {
  auto registry = SlashCommandRegistry::create(
      {spec("alpha", "alpha"), spec("agent", "agent"),
       spec("blocked", "blocked", unavailable)});
  REQUIRE(registry);

  const auto listed = registry->describe();
  REQUIRE(listed);
  REQUIRE(listed->size() == 3);
  REQUIRE((*listed)[0].name == "alpha");
  REQUIRE((*listed)[1].name == "agent");
  REQUIRE_FALSE((*listed)[2].available);

  const auto matches = registry->complete("/a");
  const std::vector<std::string> available_names{"alpha", "agent"};
  REQUIRE(matches == available_names);
  REQUIRE(registry->complete("/blocked")->empty());

  for (const auto& name : available_names) {
    const auto dispatched = registry->dispatch('/' + name);
    REQUIRE(dispatched);
    REQUIRE(dispatched->has_value());
  }

  const auto unsafe_help = registry->describe("bad\x1btarget");
  REQUIRE_FALSE(unsafe_help);
  REQUIRE(unsafe_help.error().code == SlashCommandErrorCode::invalid_input);

  const auto exact = registry->dispatch("/alpha value");
  REQUIRE(exact);
  REQUIRE(exact->has_value());
  REQUIRE((*exact)->action == SlashCommandAction::show_help);
  REQUIRE((*exact)->subject == "value");

  const auto prefix = registry->dispatch("/al");
  REQUIRE_FALSE(prefix);
  REQUIRE(prefix.error().code == SlashCommandErrorCode::unknown_command);
}

TEST_CASE("builtin slash commands expose bounded neutral actions", "[slash]") {
  const auto& registry = builtin_slash_command_registry();
  const auto listed = registry.describe();
  REQUIRE(listed);
  REQUIRE(listed->size() == 7);
  REQUIRE((*listed)[0].name == "help");
  REQUIRE((*listed)[1].name == "quit");
  REQUIRE((*listed)[2].name == "clear");
  REQUIRE((*listed)[3].name == "edit");
  REQUIRE((*listed)[4].name == "session");
  REQUIRE((*listed)[5].name == "persona");
  REQUIRE((*listed)[6].name == "model");

  const auto help = registry.dispatch("/help /clear");
  REQUIRE(help);
  REQUIRE((*help)->action == SlashCommandAction::show_help);
  REQUIRE((*help)->subject == "clear");

  const auto quit = registry.dispatch("/quit now");
  REQUIRE_FALSE(quit);
  REQUIRE(quit.error().code == SlashCommandErrorCode::invalid_arguments);

  const auto edit = registry.dispatch(
      "/edit", {.editor_available = false, .stop_token = {}});
  REQUIRE_FALSE(edit);
  REQUIRE(edit.error().code == SlashCommandErrorCode::unavailable_command);

  const auto active = registry.dispatch(
      "/help", {.run_active = true, .stop_token = {}});
  REQUIRE_FALSE(active);
  REQUIRE(active.error().code == SlashCommandErrorCode::unavailable_command);

  const auto sessions = registry.dispatch("/session");
  REQUIRE(sessions);
  REQUIRE((*sessions)->action == SlashCommandAction::list_sessions);
  REQUIRE_FALSE((*sessions)->subject);

  const auto listed_sessions = registry.dispatch("/session list\t");
  REQUIRE(listed_sessions);
  REQUIRE((*listed_sessions)->action == SlashCommandAction::list_sessions);

  const auto resumed = registry.dispatch("/session resume session-42");
  REQUIRE(resumed);
  REQUIRE((*resumed)->action == SlashCommandAction::resume_session);
  REQUIRE((*resumed)->subject == "session-42");

  const auto created = registry.dispatch("/session new");
  REQUIRE(created);
  REQUIRE((*created)->action == SlashCommandAction::new_session);

  const auto personas = registry.dispatch("/persona");
  REQUIRE(personas);
  REQUIRE((*personas)->action == SlashCommandAction::list_personas);

  const auto selected = registry.dispatch("/persona set reviewer");
  REQUIRE(selected);
  REQUIRE((*selected)->action == SlashCommandAction::select_persona);
  REQUIRE((*selected)->subject == "reviewer");

  const auto disabled = registry.dispatch("/persona off");
  REQUIRE(disabled);
  REQUIRE((*disabled)->action == SlashCommandAction::disable_persona);

  const auto picker = registry.dispatch("/model");
  REQUIRE(picker);
  REQUIRE((*picker)->action == SlashCommandAction::choose_model);
  REQUIRE_FALSE((*picker)->subject);

  const auto model = registry.dispatch("/model text-model");
  REQUIRE(model);
  REQUIRE((*model)->action == SlashCommandAction::choose_model);
  REQUIRE((*model)->subject == "text-model");

  const auto invalid_model = registry.dispatch("/model two models");
  REQUIRE_FALSE(invalid_model);
  REQUIRE(invalid_model.error().code == SlashCommandErrorCode::invalid_arguments);

  for (const auto invalid : {"/persona set", "/persona off now",
                             "/persona set two names", "/persona unknown"}) {
    const auto rejected = registry.dispatch(invalid);
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == SlashCommandErrorCode::invalid_arguments);
  }

  for (const auto invalid : {"/session resume", "/session list extra",
                             "/session unknown"}) {
    const auto rejected = registry.dispatch(invalid);
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == SlashCommandErrorCode::invalid_arguments);
  }

  const auto active_session = registry.dispatch(
      "/session list", {.run_active = true, .stop_token = {}});
  REQUIRE_FALSE(active_session);
  REQUIRE(active_session.error().code ==
          SlashCommandErrorCode::unavailable_command);

  std::stop_source cancelled;
  cancelled.request_stop();
  const auto stopped = registry.dispatch(
      "/help", {.stop_token = cancelled.get_token()});
  REQUIRE_FALSE(stopped);
  REQUIRE(stopped.error().code == SlashCommandErrorCode::cancelled);
}
