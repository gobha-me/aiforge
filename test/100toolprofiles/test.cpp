#include <aiforge/runtime/tool_launch_policy.hpp>
#include <aiforge/runtime/tool_profiles.hpp>
#include <aiforge/testing/scripted_tool_executor.hpp>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <optional>
#include <ranges>
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
  std::vector<domain::Effect> effects;
  std::vector<domain::CapabilityScope> scopes;
  if (authority_bearing) {
    effects.push_back(domain::Effect::read);
    scopes.push_back({domain::Effect::read, "filesystem.root", "/repo"});
  }
  return {std::move(name),
          "A no-authority test tool",
          {"application/schema+json", R"({"type":"object"})"},
          std::move(effects),
          std::move(scopes)};
}

auto register_tool(
    runtime::ToolRegistry& registry, const std::string& name,
    const bool authority_bearing = false,
    const runtime::ToolCategory category = runtime::ToolCategory::other)
    -> std::shared_ptr<testing::ScriptedToolExecutor> {
  auto executor = std::make_shared<testing::ScriptedToolExecutor>(
      std::vector<testing::ScriptedToolExchange>{});
  REQUIRE(registry.register_tool(declaration(name, authority_bearing), executor,
                                 {}, std::nullopt, category));
  return executor;
}

auto launch_policy(
    const runtime::ToolRegistrySnapshot& tools,
    const runtime::RestrictionLevel restriction =
        runtime::RestrictionLevel::medium,
    const runtime::ApprovalMode approval = runtime::ApprovalMode::allow_all,
    std::vector<std::string> automatic = {})
    -> std::shared_ptr<runtime::ToolPolicy> {
  auto permission_profile =
      domain::PermissionProfileId::from("test-launch-profile").value();
  runtime::ApplicationLaunchContextConfiguration context_configuration;
  context_configuration.selected_restriction = restriction;
  context_configuration.achieved_restriction = restriction;
  context_configuration.unavailable_reason.reset();
  context_configuration.restriction_policy_identity = "test.process-policy.v1";
  context_configuration.approval_mode = approval;
  if (approval == runtime::ApprovalMode::automatic) {
    context_configuration.matcher_policy_identity =
        runtime::exact_tool_allowlist_matcher_identity(automatic).value();
  }
  auto context = runtime::make_application_launch_context(
      std::move(context_configuration));
  REQUIRE(context);
  auto policy = runtime::make_tool_launch_policy(
      tools, {std::move(permission_profile), std::move(*context),
              std::move(automatic)});
  REQUIRE(policy);
  return std::move(*policy);
}

} // namespace

TEST_CASE("built-in tool profiles have explicit bounded membership",
          "[tool-profile]") {
  const auto profiles = runtime::builtin_tool_profiles();
  REQUIRE(runtime::validate_tool_profiles(profiles));
  REQUIRE(profiles.size() == 4);
  REQUIRE(profiles[0].profile_id == profile_id("essentials"));
  REQUIRE(profiles[0].name == "Essentials");
  REQUIRE(profiles[0].tool_names ==
          std::vector<std::string>{"ask_user", "propose_memory"});
  REQUIRE(profiles[1].profile_id == profile_id("repository-read"));
  REQUIRE(profiles[1].name == "Repository read");
  REQUIRE(profiles[1].tool_names ==
          std::vector<std::string>{"ask_user", "propose_memory",
                                   "read_repository_file"});
  REQUIRE(profiles[2].profile_id == profile_id("media"));
  REQUIRE(profiles[2].name == "Media");
  REQUIRE(profiles[2].tool_names == std::vector<std::string>{"ask_user",
                                                             "propose_memory",
                                                             "generate_image"});
  REQUIRE(profiles[3].profile_id == profile_id("off"));
  REQUIRE(profiles[3].name == "Off");
  REQUIRE(profiles[3].tool_names.empty());
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

TEST_CASE("category helpers expand only registered selected-profile members",
          "[tool-profile][category]") {
  runtime::ToolRegistry registry;
  static_cast<void>(register_tool(registry, "read_repository_file", true,
                                  runtime::ToolCategory::repository));
  static_cast<void>(register_tool(registry, "ask_user", false,
                                  runtime::ToolCategory::interaction));
  static_cast<void>(register_tool(registry, "outside", false,
                                  runtime::ToolCategory::repository));
  const auto snapshot = registry.snapshot();
  REQUIRE(snapshot);

  const auto repository = runtime::tool_profile_category_members(
      *snapshot, profile_id("repository-read"),
      runtime::ToolCategory::repository);
  REQUIRE(repository == std::vector<std::string>{"read_repository_file"});
  const auto interaction = runtime::tool_profile_category_members(
      *snapshot, profile_id("repository-read"),
      runtime::ToolCategory::interaction);
  REQUIRE(interaction == std::vector<std::string>{"ask_user"});
  REQUIRE_FALSE(runtime::tool_profile_category_members(
      *snapshot, profile_id("missing"), runtime::ToolCategory::repository));
  REQUIRE_FALSE(runtime::tool_profile_category_members(
      *snapshot, profile_id("repository-read"),
      static_cast<runtime::ToolCategory>(100)));
}

TEST_CASE(
    "exact session model and persona subsets only narrow built-in profiles",
    "[tool-profile][narrowing][failure]") {
  runtime::ToolRegistry registry;
  static_cast<void>(register_tool(registry, "read_repository_file", true,
                                  runtime::ToolCategory::repository));
  static_cast<void>(register_tool(registry, "propose_memory"));
  static_cast<void>(register_tool(registry, "ask_user"));
  static_cast<void>(register_tool(registry, "outside"));
  const auto snapshot = registry.snapshot();
  REQUIRE(snapshot);
  const auto policy = launch_policy(*snapshot);

  auto selection = runtime::ToolProfileSelection{
      profile_id("repository-read"),
      std::vector<std::string>{"read_repository_file", "ask_user"},
      profile_id("essentials"), std::nullopt, true};
  auto resolved = runtime::resolve_tool_profile(*snapshot, selection, *policy);
  REQUIRE(resolved);
  REQUIRE(resolved->effective_tools.size() == 1);
  REQUIRE(resolved->effective_tools.declarations().front().name == "ask_user");
  REQUIRE(resolved->effective_tools.find("outside") == nullptr);
  REQUIRE(resolved->selection.desired_tool_names ==
          std::vector<std::string>{"ask_user", "read_repository_file"});
  REQUIRE(resolved->tool_availability ==
          std::vector<runtime::ToolProfileToolAvailability>{
              {"ask_user", runtime::ToolProfileAvailabilityReason::available},
              {"propose_memory",
               runtime::ToolProfileAvailabilityReason::session_tool_disabled},
              {"read_repository_file",
               runtime::ToolProfileAvailabilityReason::model_profile_limit}});

  selection.model_maximum_profile_id = std::nullopt;
  selection.persona_maximum_profile_id = profile_id("off");
  resolved = runtime::resolve_tool_profile(*snapshot, selection, *policy);
  REQUIRE(resolved);
  REQUIRE(resolved->effective_tools.empty());
  REQUIRE(resolved->tool_availability.front().reason ==
          runtime::ToolProfileAvailabilityReason::persona_profile_limit);

  selection.persona_maximum_profile_id = std::nullopt;
  selection.desired_tool_names = std::vector<std::string>{"outside"};
  REQUIRE_FALSE(runtime::resolve_tool_profile(*snapshot, selection, *policy));
  selection.desired_tool_names =
      std::vector<std::string>{"ask_user", "ask_user"};
  resolved = runtime::resolve_tool_profile(*snapshot, selection, *policy);
  REQUIRE_FALSE(resolved);
  REQUIRE(resolved.error().code ==
          runtime::ToolProfileErrorCode::duplicate_tool);

  selection.desired_tool_names = std::nullopt;
  selection.model_maximum_profile_id = profile_id("unknown");
  resolved = runtime::resolve_tool_profile(*snapshot, selection, *policy);
  REQUIRE_FALSE(resolved);
  REQUIRE(resolved.error().code ==
          runtime::ToolProfileErrorCode::unknown_profile);
}

TEST_CASE("authority profiles stay independent from containment and approval",
          "[tool-profile][narrowing][policy]") {
  runtime::ToolRegistry registry;
  static_cast<void>(register_tool(registry, "read_repository_file", true));
  static_cast<void>(register_tool(registry, "propose_memory"));
  static_cast<void>(register_tool(registry, "ask_user"));
  const auto snapshot = registry.snapshot();
  REQUIRE(snapshot);
  auto selection =
      runtime::ToolProfileSelection{profile_id("repository-read"), std::nullopt,
                                    std::nullopt, std::nullopt, true};

  const auto high = launch_policy(*snapshot, runtime::RestrictionLevel::high);
  auto resolved = runtime::resolve_tool_profile(*snapshot, selection, *high);
  REQUIRE(resolved);
  REQUIRE(resolved->effective_tools.size() == 3);
  REQUIRE(resolved->effective_tools.find("read_repository_file") != nullptr);

  const auto medium = launch_policy(*snapshot);
  resolved = runtime::resolve_tool_profile(*snapshot, selection, *medium);
  REQUIRE(resolved);
  REQUIRE(resolved->effective_tools.size() == 3);

  const auto automatic =
      launch_policy(*snapshot, runtime::RestrictionLevel::medium,
                    runtime::ApprovalMode::automatic, {"ask_user"});
  resolved = runtime::resolve_tool_profile(*snapshot, selection, *automatic);
  REQUIRE(resolved);
  REQUIRE(resolved->effective_tools.size() == 3);
  REQUIRE(resolved->effective_tools.find("read_repository_file") != nullptr);

  const auto fallback = runtime::default_tool_policy();
  resolved = runtime::resolve_tool_profile(*snapshot, selection, *fallback);
  REQUIRE(resolved);
  REQUIRE(resolved->effective_tools.size() == 2);
  REQUIRE(resolved->effective_tools.find("read_repository_file") == nullptr);

  selection.model_tool_calling_support = false;
  resolved = runtime::resolve_tool_profile(*snapshot, selection, *medium);
  REQUIRE(resolved);
  REQUIRE(resolved->effective_tools.empty());
  REQUIRE(std::ranges::all_of(
      resolved->tool_availability, [](const auto& availability) {
        return availability.reason == runtime::ToolProfileAvailabilityReason::
                                          model_tool_calling_unsupported;
      }));
}

TEST_CASE("paid image tool requires media selection and authority ceilings",
          "[tool-profile][media][narrowing][policy][failure]") {
  runtime::ToolRegistry registry;
  static_cast<void>(register_tool(registry, "ask_user"));
  static_cast<void>(register_tool(registry, "propose_memory"));
  auto image_executor = std::make_shared<testing::ScriptedToolExecutor>(
      std::vector<testing::ScriptedToolExchange>{});
  const backend::ToolDeclaration image{
      "generate_image",
      "Generate one image",
      {"application/schema+json", R"({"type":"object"})"},
      {domain::Effect::write, domain::Effect::network, domain::Effect::spend},
      {{domain::Effect::write, "filesystem.root", "/state/artifacts"},
       {domain::Effect::network, "network.host", "api.example.test"},
       {domain::Effect::spend, "spend.microunits", "250000"}}};
  REQUIRE(registry.register_tool(image, image_executor, {}, std::nullopt,
                                 runtime::ToolCategory::media));
  const auto snapshot = registry.snapshot();
  REQUIRE(snapshot);

  const auto low = launch_policy(*snapshot, runtime::RestrictionLevel::low);
  auto selection = runtime::ToolProfileSelection{
      profile_id("essentials"), std::nullopt, std::nullopt, std::nullopt, true};
  auto resolved = runtime::resolve_tool_profile(*snapshot, selection, *low);
  REQUIRE(resolved);
  CHECK(resolved->effective_tools.find("generate_image") == nullptr);

  selection.selected_profile_id = profile_id("media");
  const auto medium = launch_policy(*snapshot);
  resolved = runtime::resolve_tool_profile(*snapshot, selection, *medium);
  REQUIRE(resolved);
  CHECK(resolved->effective_tools.find("generate_image") != nullptr);
  REQUIRE(resolved->tool_availability.size() == 3);
  CHECK(resolved->tool_availability.back().reason ==
        runtime::ToolProfileAvailabilityReason::available);

  resolved = runtime::resolve_tool_profile(*snapshot, selection, *low);
  REQUIRE(resolved);
  REQUIRE(resolved->effective_tools.find("generate_image") != nullptr);
  CHECK(resolved->effective_tools.find("generate_image")->executor ==
        image_executor);

  selection.desired_tool_names =
      std::vector<std::string>{"ask_user", "propose_memory"};
  resolved = runtime::resolve_tool_profile(*snapshot, selection, *low);
  REQUIRE(resolved);
  CHECK(resolved->effective_tools.find("generate_image") == nullptr);

  selection.desired_tool_names = std::nullopt;
  selection.model_maximum_profile_id = profile_id("essentials");
  resolved = runtime::resolve_tool_profile(*snapshot, selection, *low);
  REQUIRE(resolved);
  CHECK(resolved->effective_tools.find("generate_image") == nullptr);
  CHECK(resolved->tool_availability.back().reason ==
        runtime::ToolProfileAvailabilityReason::model_profile_limit);
}
