#include <aiforge/runtime/tool_profiles.hpp>
#include <aiforge/testing/scripted_tool_executor.hpp>

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace aiforge;

auto profile_id(const std::string& value) -> domain::ToolProfileId {
  return domain::ToolProfileId::from(value).value();
}

auto profile(std::string id = "custom", std::string name = "Custom",
             std::vector<std::string> tools = {}) -> runtime::ToolProfile {
  return {profile_id(id), std::move(name), std::move(tools)};
}

auto declaration(std::string name, const bool authority_bearing = false)
    -> backend::ToolDeclaration {
  return {std::move(name),
          "A no-authority test tool",
          {"application/schema+json", R"({"type":"object"})"},
          authority_bearing ? std::vector{domain::Effect::read}
                            : std::vector<domain::Effect>{},
          authority_bearing
              ? std::vector{domain::CapabilityScope{domain::Effect::read,
                                                    "filesystem.root", "/repo"}}
              : std::vector<domain::CapabilityScope>{}};
}

auto register_tool(runtime::ToolRegistry& registry, const std::string& name,
                   const bool authority_bearing = false)
    -> std::shared_ptr<testing::ScriptedToolExecutor> {
  auto executor = std::make_shared<testing::ScriptedToolExecutor>(
      std::vector<testing::ScriptedToolExchange>{});
  REQUIRE(
      registry.register_tool(declaration(name, authority_bearing), executor));
  return executor;
}

} // namespace

TEST_CASE("built-in tool profiles have explicit bounded membership",
          "[tool-profile]") {
  const auto profiles = runtime::builtin_tool_profiles();
  REQUIRE(runtime::validate_tool_profiles(profiles));
  REQUIRE(profiles.size() == 3);
  REQUIRE(profiles[0].profile_id == profile_id("essentials"));
  REQUIRE(profiles[0].name == "Essentials");
  REQUIRE(profiles[0].tool_names ==
          std::vector<std::string>{"ask_user", "propose_memory"});
  REQUIRE(profiles[1].profile_id == profile_id("repository-read"));
  REQUIRE(profiles[1].name == "Repository read");
  REQUIRE(profiles[1].tool_names ==
          std::vector<std::string>{"ask_user", "propose_memory",
                                   "read_repository_file"});
  REQUIRE(profiles[2].profile_id == profile_id("off"));
  REQUIRE(profiles[2].name == "Off");
  REQUIRE(profiles[2].tool_names.empty());
  REQUIRE(runtime::tool_profile_availability_reason_text(
              runtime::ToolProfileAvailabilityReason::tool_not_registered) ==
          "tool is not registered in this runtime");
}

TEST_CASE(
    "tool profile validation rejects malformed ambiguous and unbounded data",
    "[tool-profile][failure]") {
  auto result = runtime::validate_tool_profiles({});
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          runtime::ToolProfileErrorCode::invalid_profile);

  auto valid = profile();
  result = runtime::validate_tool_profiles(std::span{&valid, 1}, {});
  REQUIRE(result);
  result =
      runtime::validate_tool_profiles(std::span{&valid, 1}, {0, 1, 1, 1, 1, 1});
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == runtime::ToolProfileErrorCode::invalid_limits);

  auto invalid_id = profile("Bad");
  result = runtime::validate_tool_profiles(std::span{&invalid_id, 1});
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          runtime::ToolProfileErrorCode::invalid_profile);

  auto long_id = profile(std::string(65, 'a'));
  result = runtime::validate_tool_profiles(std::span{&long_id, 1});
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          runtime::ToolProfileErrorCode::invalid_profile);

  auto unsafe_name = profile("unsafe", "Unsafe\x1b");
  result = runtime::validate_tool_profiles(std::span{&unsafe_name, 1});
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          runtime::ToolProfileErrorCode::invalid_profile);

  auto empty_name = profile("empty-name", "");
  result = runtime::validate_tool_profiles(std::span{&empty_name, 1});
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          runtime::ToolProfileErrorCode::invalid_profile);

  auto empty_tool = profile("empty-tool", "Empty tool", {""});
  result = runtime::validate_tool_profiles(std::span{&empty_tool, 1});
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          runtime::ToolProfileErrorCode::invalid_profile);

  auto wildcard = profile("wildcard", "Wildcard", {"ask_*"});
  result = runtime::validate_tool_profiles(std::span{&wildcard, 1});
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          runtime::ToolProfileErrorCode::invalid_profile);

  auto duplicated_tool =
      profile("duplicate-tool", "Duplicate tool", {"ask_user", "ask_user"});
  result = runtime::validate_tool_profiles(std::span{&duplicated_tool, 1});
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == runtime::ToolProfileErrorCode::duplicate_tool);

  const std::vector duplicated_profile{profile("same", "First"),
                                       profile("same", "Second")};
  result = runtime::validate_tool_profiles(duplicated_profile);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          runtime::ToolProfileErrorCode::duplicate_profile);

  const std::vector duplicated_name{profile("first", "Same"),
                                    profile("second", "Same")};
  result = runtime::validate_tool_profiles(duplicated_name);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          runtime::ToolProfileErrorCode::duplicate_profile);

  auto too_many_tools = profile("many", "Many", {"one", "two"});
  result = runtime::validate_tool_profiles(std::span{&too_many_tools, 1},
                                           {1, 64, 128, 1, 128, 1024});
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          runtime::ToolProfileErrorCode::resource_exhausted);

  result = runtime::validate_tool_profiles(std::span{&valid, 1},
                                           {1, 64, 128, 1, 128, 4});
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          runtime::ToolProfileErrorCode::resource_exhausted);
}

TEST_CASE("profile resolution preserves registry order and exact executors",
          "[tool-profile]") {
  runtime::ToolRegistry registry;
  const auto memory_executor = register_tool(registry, "propose_memory");
  static_cast<void>(register_tool(registry, "unrelated"));
  const auto ask_executor = register_tool(registry, "ask_user");
  const auto snapshot = registry.snapshot();
  REQUIRE(snapshot);

  const auto resolved =
      runtime::resolve_tool_profile(*snapshot, profile_id("essentials"), true);
  REQUIRE(resolved);
  REQUIRE(resolved->selected_profile.profile_id == profile_id("essentials"));
  REQUIRE(resolved->effective_tools.size() == 2);
  REQUIRE(resolved->effective_tools.declarations()[0].name == "propose_memory");
  REQUIRE(resolved->effective_tools.declarations()[1].name == "ask_user");
  REQUIRE(resolved->effective_tools.find("unrelated") == nullptr);
  REQUIRE(resolved->effective_tools.find("propose_memory")->executor ==
          memory_executor);
  REQUIRE(resolved->effective_tools.find("ask_user")->executor == ask_executor);
  REQUIRE(resolved->tool_availability ==
          std::vector<runtime::ToolProfileToolAvailability>{
              {"ask_user", runtime::ToolProfileAvailabilityReason::available},
              {"propose_memory",
               runtime::ToolProfileAvailabilityReason::available}});
}

TEST_CASE(
    "missing and unsupported profile tools fail closed with exact reasons",
    "[tool-profile][failure]") {
  runtime::ToolRegistry registry;
  static_cast<void>(register_tool(registry, "ask_user"));
  const auto snapshot = registry.snapshot();
  REQUIRE(snapshot);

  const auto unsupported =
      runtime::resolve_tool_profile(*snapshot, profile_id("essentials"), false);
  REQUIRE(unsupported);
  REQUIRE(unsupported->selected_profile.profile_id == profile_id("essentials"));
  REQUIRE(unsupported->effective_tools.empty());
  REQUIRE(unsupported->tool_availability ==
          std::vector<runtime::ToolProfileToolAvailability>{
              {"ask_user", runtime::ToolProfileAvailabilityReason::
                               model_tool_calling_unsupported},
              {"propose_memory",
               runtime::ToolProfileAvailabilityReason::tool_not_registered}});

  const auto unknown = runtime::resolve_tool_profile(
      *snapshot, profile_id("essentials"), std::nullopt);
  REQUIRE(unknown);
  REQUIRE(unknown->selected_profile.profile_id == profile_id("essentials"));
  REQUIRE(unknown->effective_tools.empty());
  REQUIRE(
      unknown->tool_availability ==
      std::vector<runtime::ToolProfileToolAvailability>{
          {"ask_user",
           runtime::ToolProfileAvailabilityReason::model_tool_calling_unknown},
          {"propose_memory",
           runtime::ToolProfileAvailabilityReason::tool_not_registered}});
}

TEST_CASE("essentials rejects authority-bearing name collisions",
          "[tool-profile][failure]") {
  runtime::ToolRegistry registry;
  static_cast<void>(register_tool(registry, "ask_user", true));
  static_cast<void>(register_tool(registry, "propose_memory"));
  const auto snapshot = registry.snapshot();
  REQUIRE(snapshot);

  const auto resolved =
      runtime::resolve_tool_profile(*snapshot, profile_id("essentials"), true);
  REQUIRE(resolved);
  REQUIRE(resolved->effective_tools.size() == 1);
  REQUIRE(resolved->effective_tools.find("ask_user") == nullptr);
  REQUIRE(resolved->effective_tools.find("propose_memory") != nullptr);
  REQUIRE(
      resolved->tool_availability ==
      std::vector<runtime::ToolProfileToolAvailability>{
          {"ask_user",
           runtime::ToolProfileAvailabilityReason::profile_contract_mismatch},
          {"propose_memory",
           runtime::ToolProfileAvailabilityReason::available}});
}

TEST_CASE("off and unknown profile selection never enroll registry tools",
          "[tool-profile][failure]") {
  runtime::ToolRegistry registry;
  static_cast<void>(register_tool(registry, "ask_user"));
  static_cast<void>(register_tool(registry, "propose_memory"));
  static_cast<void>(register_tool(registry, "unrelated"));
  const auto snapshot = registry.snapshot();
  REQUIRE(snapshot);

  const auto off =
      runtime::resolve_tool_profile(*snapshot, profile_id("off"), true);
  REQUIRE(off);
  REQUIRE(off->selected_profile.profile_id == profile_id("off"));
  REQUIRE(off->effective_tools.empty());
  REQUIRE(off->tool_availability.empty());

  const auto unknown =
      runtime::resolve_tool_profile(*snapshot, profile_id("missing"), true);
  REQUIRE_FALSE(unknown);
  REQUIRE(unknown.error().code ==
          runtime::ToolProfileErrorCode::unknown_profile);
}
