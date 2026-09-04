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

auto available(const SlashCommandContext&) -> bool {
  return true;
}
auto unavailable(const SlashCommandContext&) -> bool {
  return false;
}

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
  return {std::move(id),   std::move(name), "[value]",
          "Test command.", predicate,       handler};
}

} // namespace

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

  registry =
      SlashCommandRegistry::create({spec("same", "one"), spec("same", "two")});
  REQUIRE_FALSE(registry);
  REQUIRE(registry.error().code == SlashCommandRegistryErrorCode::duplicate_id);

  registry =
      SlashCommandRegistry::create({spec("one", "same"), spec("two", "same")});
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
  auto registry =
      SlashCommandRegistry::create({spec("throw", "throw", available, throws),
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
  REQUIRE(listed->size() == 15);
  REQUIRE((*listed)[0].name == "help");
  REQUIRE((*listed)[1].name == "quit");
  REQUIRE((*listed)[2].name == "clear");
  REQUIRE((*listed)[3].name == "edit");
  REQUIRE((*listed)[4].name == "session");
  REQUIRE((*listed)[5].name == "persona");
  REQUIRE((*listed)[6].name == "character");
  REQUIRE((*listed)[7].name == "model");
  REQUIRE((*listed)[8].name == "settings");
  REQUIRE((*listed)[9].name == "tools");
  REQUIRE((*listed)[10].name == "reasoning");
  REQUIRE((*listed)[11].name == "usage");
  REQUIRE((*listed)[12].name == "plan");
  REQUIRE((*listed)[13].name == "tasks");
  REQUIRE((*listed)[14].name == "memory");

  const auto memory = registry.dispatch("/memory search convention");
  REQUIRE(memory);
  REQUIRE((*memory)->action == SlashCommandAction::manage_memory);
  REQUIRE((*memory)->subject == "search convention");

  const auto help = registry.dispatch("/help /clear");
  REQUIRE(help);
  REQUIRE((*help)->action == SlashCommandAction::show_help);
  REQUIRE((*help)->subject == "clear");

  const auto quit = registry.dispatch("/quit now");
  REQUIRE_FALSE(quit);
  REQUIRE(quit.error().code == SlashCommandErrorCode::invalid_arguments);

  const auto edit =
      registry.dispatch("/edit", {.editor_available = false, .stop_token = {}});
  REQUIRE_FALSE(edit);
  REQUIRE(edit.error().code == SlashCommandErrorCode::unavailable_command);

  const auto active =
      registry.dispatch("/help", {.run_active = true, .stop_token = {}});
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

  const auto managed = registry.dispatch("/persona manage");
  REQUIRE(managed);
  REQUIRE((*managed)->action == SlashCommandAction::manage_personas);
  const auto active_manager = registry.dispatch(
      "/persona manage", {.run_active = true, .stop_token = {}});
  REQUIRE_FALSE(active_manager);
  REQUIRE(active_manager.error().code ==
          SlashCommandErrorCode::unavailable_command);

  const auto character_picker = registry.dispatch("/character");
  REQUIRE(character_picker);
  REQUIRE((*character_picker)->action ==
          SlashCommandAction::choose_provider_character);
  REQUIRE_FALSE((*character_picker)->subject);

  const auto character = registry.dispatch("/character set alan-watts");
  REQUIRE(character);
  REQUIRE((*character)->action ==
          SlashCommandAction::choose_provider_character);
  REQUIRE((*character)->subject == "alan-watts");

  const auto disabled_character = registry.dispatch("/character off");
  REQUIRE(disabled_character);
  REQUIRE((*disabled_character)->action ==
          SlashCommandAction::disable_provider_character);

  const auto active_character =
      registry.dispatch("/character", {.run_active = true, .stop_token = {}});
  REQUIRE_FALSE(active_character);
  REQUIRE(active_character.error().code ==
          SlashCommandErrorCode::unavailable_command);

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
  REQUIRE(invalid_model.error().code ==
          SlashCommandErrorCode::invalid_arguments);

  const auto usage = registry.dispatch("/usage");
  REQUIRE(usage);
  REQUIRE((*usage)->action == SlashCommandAction::show_usage);

  const auto settings = registry.dispatch("/settings");
  REQUIRE(settings);
  REQUIRE((*settings)->action == SlashCommandAction::manage_request_settings);
  const auto invalid_settings = registry.dispatch("/settings now");
  REQUIRE_FALSE(invalid_settings);
  REQUIRE(invalid_settings.error().code ==
          SlashCommandErrorCode::invalid_arguments);
  const auto active_settings =
      registry.dispatch("/settings", {.run_active = true, .stop_token = {}});
  REQUIRE_FALSE(active_settings);
  REQUIRE(active_settings.error().code ==
          SlashCommandErrorCode::unavailable_command);

  const auto tools = registry.dispatch("/tools");
  REQUIRE(tools);
  REQUIRE((*tools)->action == SlashCommandAction::manage_tool_profile);
  REQUIRE_FALSE((*tools)->subject);
  const auto essentials = registry.dispatch("/tools profile essentials");
  REQUIRE(essentials);
  REQUIRE((*essentials)->action == SlashCommandAction::select_tool_profile);
  REQUIRE((*essentials)->subject == "essentials");
  const auto tools_off = registry.dispatch("/tools off");
  REQUIRE(tools_off);
  REQUIRE((*tools_off)->action == SlashCommandAction::disable_tools);
  REQUIRE_FALSE((*tools_off)->subject);
  const auto active_tools =
      registry.dispatch("/tools", {.run_active = true, .stop_token = {}});
  REQUIRE_FALSE(active_tools);
  REQUIRE(active_tools.error().code ==
          SlashCommandErrorCode::unavailable_command);

  for (const auto argument : {"show", "hide"}) {
    const auto reasoning =
        registry.dispatch("/reasoning " + std::string{argument});
    REQUIRE(reasoning);
    REQUIRE((*reasoning)->action ==
            SlashCommandAction::set_reasoning_visibility);
    REQUIRE((*reasoning)->subject == argument);
  }
  for (const auto invalid :
       {"/reasoning", "/reasoning auto", "/reasoning show now"}) {
    const auto rejected = registry.dispatch(invalid);
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == SlashCommandErrorCode::invalid_arguments);
  }
  const auto active_reasoning = registry.dispatch(
      "/reasoning show", {.run_active = true, .stop_token = {}});
  REQUIRE_FALSE(active_reasoning);
  REQUIRE(active_reasoning.error().code ==
          SlashCommandErrorCode::unavailable_command);

  const auto invalid_usage = registry.dispatch("/usage now");
  REQUIRE_FALSE(invalid_usage);
  REQUIRE(invalid_usage.error().code ==
          SlashCommandErrorCode::invalid_arguments);

  const auto plan = registry.dispatch("/plan");
  REQUIRE(plan);
  REQUIRE((*plan)->action == SlashCommandAction::show_plan);

  const auto tasks = registry.dispatch("/tasks");
  REQUIRE(tasks);
  REQUIRE((*tasks)->action == SlashCommandAction::show_tasks);

  for (const auto invalid : {"/plan now", "/tasks now"}) {
    const auto rejected = registry.dispatch(invalid);
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == SlashCommandErrorCode::invalid_arguments);
  }

  for (const auto invalid :
       {"/persona set", "/persona off now", "/persona manage now",
        "/persona set two names", "/persona unknown"}) {
    const auto rejected = registry.dispatch(invalid);
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == SlashCommandErrorCode::invalid_arguments);
  }

  for (const auto invalid :
       {"/character set", "/character off now", "/character set two slugs",
        "/character unknown"}) {
    const auto rejected = registry.dispatch(invalid);
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == SlashCommandErrorCode::invalid_arguments);
  }

  for (const auto invalid :
       {"/tools profile", "/tools profile two names", "/tools essentials",
        "/tools off now", "/tools profile Bad"}) {
    const auto rejected = registry.dispatch(invalid);
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == SlashCommandErrorCode::invalid_arguments);
  }

  for (const auto invalid :
       {"/session resume", "/session list extra", "/session unknown"}) {
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
  const auto stopped =
      registry.dispatch("/help", {.stop_token = cancelled.get_token()});
  REQUIRE_FALSE(stopped);
  REQUIRE(stopped.error().code == SlashCommandErrorCode::cancelled);
}

TEST_CASE("tool slash mutations are typed, bounded, and fail closed",
          "[slash][tools]") {
  const auto& registry = builtin_slash_command_registry();

  const std::vector<std::string> malformed{
      "/tools profile",
      "/tools profile two names",
      "/tools profile Bad",
      "/tools off now",
      "/tools reset now",
      "/tools category",
      "/tools category memory",
      "/tools category memory auto",
      "/tools category memory On",
      "/tools category unknown on",
      "/tools category Bad on",
      "/tools category memory on now",
      "/tools tool",
      "/tools tool ask_user",
      "/tools tool ask_user auto",
      "/tools tool ask*user on",
      "/tools tool ask_user off now",
      "/tools model-max",
      "/tools model-max Bad",
      "/tools model-max inherit now",
      "/tools persona-max",
      "/tools persona-max Bad",
      "/tools persona-max inherit now",
      std::string{"/tools tool safe"} + "\xC2\x85" + " on",
      std::string{"/tools tool safe"} + "\xE2\x80\xAE" + " on",
  };
  for (const auto& command : malformed) {
    CAPTURE(command);
    const auto rejected = registry.dispatch(command);
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == SlashCommandErrorCode::invalid_arguments);
  }

  const std::string maximum_identifier(64, 'a');
  const std::string oversized_identifier(65, 'a');
  const std::string maximum_tool_name(128, 't');
  const std::string oversized_tool_name(129, 't');
  REQUIRE(registry.dispatch("/tools profile " + maximum_identifier));
  REQUIRE_FALSE(registry.dispatch("/tools profile " + oversized_identifier));
  REQUIRE_FALSE(
      registry.dispatch("/tools category " + oversized_identifier + " on"));
  REQUIRE(registry.dispatch("/tools tool " + maximum_tool_name + " on"));
  REQUIRE_FALSE(
      registry.dispatch("/tools tool " + oversized_tool_name + " on"));
  REQUIRE(registry.dispatch("/tools model-max " + maximum_identifier));
  REQUIRE_FALSE(registry.dispatch("/tools model-max " + oversized_identifier));
  REQUIRE(registry.dispatch("/tools persona-max " + maximum_identifier));
  REQUIRE_FALSE(
      registry.dispatch("/tools persona-max " + oversized_identifier));

  for (const auto& command : {
           "/tools profile essentials",
           "/tools off",
           "/tools reset",
           "/tools category memory on",
           "/tools category memory off",
           "/tools tool ask_user on",
           "/tools tool ask_user off",
           "/tools model-max repository-read",
           "/tools model-max inherit",
           "/tools persona-max off",
           "/tools persona-max inherit",
       }) {
    CAPTURE(command);
    const auto rejected =
        registry.dispatch(command, {.run_active = true, .stop_token = {}});
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code ==
            SlashCommandErrorCode::unavailable_command);
  }

  struct ExpectedMutation {
    std::string command;
    SlashCommandAction action;
    std::optional<std::string> subject;
  };
  const std::vector<ExpectedMutation> mutations{
      {"/tools profile repository-read",
       SlashCommandAction::select_tool_profile, "repository-read"},
      {"/tools profile off", SlashCommandAction::select_tool_profile, "off"},
      {"/tools off", SlashCommandAction::disable_tools, std::nullopt},
      {"/tools reset", SlashCommandAction::reset_tool_narrowing, std::nullopt},
      {"/tools category repository on",
       SlashCommandAction::enable_tool_category, "repository"},
      {"/tools category repository off",
       SlashCommandAction::disable_tool_category, "repository"},
      {"/tools tool read_repository_file on", SlashCommandAction::enable_tool,
       "read_repository_file"},
      {"/tools tool read_repository_file off", SlashCommandAction::disable_tool,
       "read_repository_file"},
      {"/tools model-max essentials",
       SlashCommandAction::set_model_tool_profile_maximum, "essentials"},
      {"/tools model-max off",
       SlashCommandAction::set_model_tool_profile_maximum, "off"},
      {"/tools model-max inherit",
       SlashCommandAction::inherit_model_tool_profile_maximum, std::nullopt},
      {"/tools persona-max off",
       SlashCommandAction::set_persona_tool_profile_maximum, "off"},
      {"/tools persona-max essentials",
       SlashCommandAction::set_persona_tool_profile_maximum, "essentials"},
      {"/tools persona-max inherit",
       SlashCommandAction::inherit_persona_tool_profile_maximum, std::nullopt},
  };
  for (const auto& expected : mutations) {
    CAPTURE(expected.command);
    const auto result = registry.dispatch(expected.command);
    REQUIRE(result);
    REQUIRE(result->has_value());
    REQUIRE((*result)->action == expected.action);
    REQUIRE((*result)->subject == expected.subject);
  }

  for (const auto category :
       {"interaction", "memory", "repository", "process", "media", "other"}) {
    CAPTURE(category);
    const auto result =
        registry.dispatch("/tools category " + std::string{category} + " on");
    REQUIRE(result);
    REQUIRE((*result)->action == SlashCommandAction::enable_tool_category);
    REQUIRE((*result)->subject == category);
  }

  const auto provider_tool = registry.dispatch("/tools tool mcp.read-file on");
  REQUIRE(provider_tool);
  REQUIRE((*provider_tool)->action == SlashCommandAction::enable_tool);
  REQUIRE((*provider_tool)->subject == "mcp.read-file");

  const auto descriptions = registry.describe("tools");
  REQUIRE(descriptions);
  REQUIRE(descriptions->size() == 1);
  REQUIRE(descriptions->front().arguments.find("category") !=
          std::string::npos);
  REQUIRE(descriptions->front().arguments.find("model-max") !=
          std::string::npos);
  REQUIRE(descriptions->front().arguments.find("persona-max") !=
          std::string::npos);
}
