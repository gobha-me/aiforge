#include <aiforge/adapters/process_chat_assembly.hpp>

#include <array>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

using namespace aiforge;

class UnusedArtifactStore final : public storage::ArtifactStore {
 public:
  auto put(storage::ArtifactWrite, std::span<const std::byte>, std::stop_token)
      -> std::expected<domain::ArtifactMetadata,
                       storage::ArtifactStoreError> override {
    return std::unexpected(storage::ArtifactStoreError{
        storage::ArtifactStoreErrorCode::internal_failure,
        "unexpected artifact write", false});
  }
};

[[nodiscard]] auto settings(const bool unrestricted_network = true)
    -> config::ProcessConfigSettings {
  config::ProcessConfigSettings value;
  value.executable_allowlist = {
      std::filesystem::canonical("/usr/bin/true").generic_string()};
  value.readable_roots = {std::filesystem::canonical("/tmp").generic_string()};
  value.writable_roots = value.readable_roots;
  value.unrestricted_network = unrestricted_network;
  return value;
}

[[nodiscard]] auto matcher_identity(const runtime::ApprovalMode approval)
    -> std::optional<std::string> {
  if (approval == runtime::ApprovalMode::automatic) {
    return std::string{"matcher-v1"};
  }
  return std::nullopt;
}

[[nodiscard]] auto assemble(
    runtime::ToolRegistry& registry,
    std::optional<config::ProcessConfigSettings> configured,
    const bool durable_session, const runtime::RestrictionLevel restriction,
    const runtime::ApprovalMode approval, storage::ArtifactStore* artifacts,
    std::size_t& establishment_calls,
    adapters::ProcessEnvironmentLookup lookup = {}) {
  return adapters::assemble_process_chat_tool(
      registry,
      {.durable_session = durable_session,
       .settings = std::move(configured),
       .restriction = restriction,
       .approval = approval,
       .matcher_policy_identity = matcher_identity(approval),
       .artifact_store = artifacts,
       .environment_lookup = std::move(lookup),
       .establish_launcher =
           [&establishment_calls](
               adapters::LinuxProcessLauncherConfiguration configuration) {
             ++establishment_calls;
             return adapters::establish_linux_process_launcher(
                 std::move(configuration));
           }});
}

TEST_CASE("production process assembly exposes only achieved none authority",
          "[process][chat][restriction][failure]") {
  constexpr std::array restrictions{
      runtime::RestrictionLevel::none, runtime::RestrictionLevel::low,
      runtime::RestrictionLevel::medium, runtime::RestrictionLevel::high};
  constexpr std::array approvals{runtime::ApprovalMode::prompt,
                                 runtime::ApprovalMode::automatic,
                                 runtime::ApprovalMode::allow_all};
  UnusedArtifactStore artifacts;
  for (const auto restriction : restrictions) {
    for (const auto approval : approvals) {
      CAPTURE(static_cast<int>(restriction), static_cast<int>(approval));
      runtime::ToolRegistry registry;
      std::size_t establishment_calls{};
      auto result = assemble(registry, settings(), true, restriction, approval,
                             &artifacts, establishment_calls);
      REQUIRE(result);
      REQUIRE(establishment_calls == 1);
      REQUIRE(result->launch_context.selected_restriction() == restriction);
      REQUIRE(result->launch_context.approval_mode() == approval);
      REQUIRE(result->launch_context.matcher_policy_identity() ==
              matcher_identity(approval));

      auto snapshot = registry.snapshot();
      REQUIRE(snapshot);
      REQUIRE(snapshot->find_unavailable("run_shell") != nullptr);
      REQUIRE(snapshot->find("run_shell") == nullptr);
      if (restriction == runtime::RestrictionLevel::none) {
        REQUIRE(result->process_registered);
        REQUIRE(snapshot->find("run_process") != nullptr);
        REQUIRE(snapshot->find_unavailable("run_process") == nullptr);
        REQUIRE(result->launch_context.achieved_restriction() == restriction);
      } else {
        REQUIRE_FALSE(result->process_registered);
        REQUIRE(snapshot->find("run_process") == nullptr);
        const auto* unavailable = snapshot->find_unavailable("run_process");
        REQUIRE(unavailable != nullptr);
        REQUIRE(unavailable->unavailability.reason ==
                runtime::ToolUnavailableReason::restriction_unavailable);
        REQUIRE(unavailable->unavailability.restriction);
        REQUIRE(unavailable->unavailability.restriction->selected_restriction ==
                restriction);
        REQUIRE_FALSE(result->launch_context.achieved_restriction());
      }
    }
  }
}

TEST_CASE("process assembly establishes once before gating tool authority",
          "[process][chat][lifetime][network][failure]") {
  UnusedArtifactStore artifacts;
  const auto verify = [&](std::optional<config::ProcessConfigSettings> value,
                          const bool durable,
                          const runtime::ToolUnavailableReason reason,
                          storage::ArtifactStore* store) {
    runtime::ToolRegistry registry;
    std::size_t establishment_calls{};
    std::size_t environment_lookups{};
    auto result = assemble(
        registry, std::move(value), durable, runtime::RestrictionLevel::none,
        runtime::ApprovalMode::prompt, store, establishment_calls,
        [&environment_lookups](std::string_view) {
          ++environment_lookups;
          return std::optional<std::string>{"secret"};
        });
    REQUIRE(result);
    REQUIRE_FALSE(result->process_registered);
    REQUIRE(establishment_calls == 1);
    REQUIRE(environment_lookups == 0);
    auto snapshot = registry.snapshot();
    REQUIRE(snapshot);
    REQUIRE(snapshot->find("run_process") == nullptr);
    REQUIRE(snapshot->find_unavailable("run_process") != nullptr);
    REQUIRE(snapshot->find_unavailable("run_process")->unavailability.reason ==
            reason);
  };

  verify(std::nullopt, true, runtime::ToolUnavailableReason::not_configured,
         &artifacts);
  verify(settings(), false,
         runtime::ToolUnavailableReason::durable_session_required, &artifacts);
  verify(settings(false), true,
         runtime::ToolUnavailableReason::unrestricted_network_required,
         &artifacts);
  verify(settings(), true,
         runtime::ToolUnavailableReason::runtime_dependency_or_path_unavailable,
         nullptr);
}

TEST_CASE("missing inherited environment is unavailable without value leakage",
          "[process][chat][environment][failure]") {
  auto configured = settings();
  configured.inherited_environment_names = {"AIFORGE_TEST_SECRET"};
  runtime::ToolRegistry registry;
  UnusedArtifactStore artifacts;
  std::size_t establishment_calls{};
  std::vector<std::string> names;
  auto result =
      assemble(registry, configured, true, runtime::RestrictionLevel::none,
               runtime::ApprovalMode::prompt, &artifacts, establishment_calls,
               [&names](const std::string_view name) {
                 names.emplace_back(name);
                 return std::optional<std::string>{};
               });
  REQUIRE(result);
  REQUIRE(establishment_calls == 1);
  REQUIRE(names == std::vector<std::string>{"AIFORGE_TEST_SECRET"});
  auto snapshot = registry.snapshot();
  REQUIRE(snapshot);
  REQUIRE(snapshot->find_unavailable("run_process") != nullptr);
  REQUIRE(
      snapshot->find_unavailable("run_process")->unavailability.reason ==
      runtime::ToolUnavailableReason::runtime_dependency_or_path_unavailable);
}

TEST_CASE("configured environment values stay outside declarations and events",
          "[process][chat][environment][security]") {
  auto configured = settings();
  configured.inherited_environment_names = {"AIFORGE_TEST_SECRET"};
  runtime::ToolRegistry registry;
  UnusedArtifactStore artifacts;
  std::size_t establishment_calls{};
  std::vector<std::string> names;
  constexpr std::string_view secret{"assembly-secret-value"};
  auto result =
      assemble(registry, configured, true, runtime::RestrictionLevel::none,
               runtime::ApprovalMode::prompt, &artifacts, establishment_calls,
               [&names](const std::string_view name) {
                 names.emplace_back(name);
                 return std::optional<std::string>{"assembly-secret-value"};
               });
  REQUIRE(result);
  REQUIRE(result->process_registered);
  REQUIRE(establishment_calls == 1);
  REQUIRE(names == std::vector<std::string>{"AIFORGE_TEST_SECRET"});
  auto snapshot = registry.snapshot();
  REQUIRE(snapshot);
  const auto* tool = snapshot->find("run_process");
  REQUIRE(tool != nullptr);
  REQUIRE(tool->declaration.description.find(secret) == std::string::npos);
  REQUIRE(tool->declaration.input_schema.data.find(secret) ==
          std::string::npos);
  const auto executable = configured.executable_allowlist.front();
  const auto root = configured.readable_roots.front();
  const domain::StructuredDataBlock arguments{
      "application/json",
      "{\"arguments\":[],\"environment\":[\"AIFORGE_TEST_SECRET\"],"
      "\"executable\":\"" +
          executable + "\",\"output_bytes\":1024,\"readable_roots\":[\"" +
          root +
          "\"],\"stdin\":\"closed\",\"timeout_ms\":1000,"
          "\"working_directory\":\"" +
          root + "\",\"writable_roots\":[\"" + root + "\"]}"};
  const auto validated = tool->executor->validate(arguments);
  REQUIRE(validated);
  REQUIRE(validated->value.data.find(secret) == std::string::npos);
}

TEST_CASE("launcher and path failures stay closed without aborting Chat",
          "[process][chat][platform][failure]") {
  UnusedArtifactStore artifacts;
  for (const auto code :
       {adapters::LinuxProcessLauncherEstablishmentErrorCode::
            unsupported_platform,
        adapters::LinuxProcessLauncherEstablishmentErrorCode::binding_failed}) {
    CAPTURE(static_cast<int>(code));
    runtime::ToolRegistry registry;
    auto result = adapters::assemble_process_chat_tool(
        registry, {.durable_session = true,
                   .settings = settings(),
                   .restriction = runtime::RestrictionLevel::none,
                   .approval = runtime::ApprovalMode::prompt,
                   .artifact_store = &artifacts,
                   .establish_launcher =
                       [code](adapters::LinuxProcessLauncherConfiguration)
                       -> std::expected<
                           adapters::LinuxProcessLauncherEstablishment,
                           adapters::LinuxProcessLauncherEstablishmentError> {
                     return std::unexpected(
                         adapters::LinuxProcessLauncherEstablishmentError{
                             code, "synthetic setup failure"});
                   }});
    REQUIRE(result);
    auto snapshot = registry.snapshot();
    REQUIRE(snapshot);
    REQUIRE(snapshot->find("run_process") == nullptr);
    REQUIRE(snapshot->find_unavailable("run_process") != nullptr);
    REQUIRE(snapshot->find_unavailable("run_process")->unavailability.reason ==
            (code == adapters::LinuxProcessLauncherEstablishmentErrorCode::
                         unsupported_platform
                 ? runtime::ToolUnavailableReason::unsupported_platform
                 : runtime::ToolUnavailableReason::
                       runtime_dependency_or_path_unavailable));
  }

  runtime::ToolRegistry invalid_path_registry;
  auto invalid_path = settings();
  invalid_path.executable_allowlist = {"/definitely/missing/aiforge-tool"};
  std::size_t establishment_calls{};
  auto unavailable =
      assemble(invalid_path_registry, invalid_path, true,
               runtime::RestrictionLevel::none, runtime::ApprovalMode::prompt,
               &artifacts, establishment_calls);
  REQUIRE(unavailable);
  REQUIRE(establishment_calls == 1);
  auto snapshot = invalid_path_registry.snapshot();
  REQUIRE(snapshot);
  REQUIRE(snapshot->find("run_process") == nullptr);
  REQUIRE(snapshot->find_unavailable("run_process") != nullptr);
  REQUIRE(
      snapshot->find_unavailable("run_process")->unavailability.reason ==
      runtime::ToolUnavailableReason::runtime_dependency_or_path_unavailable);
}

TEST_CASE("unexpected launcher context cannot become process authority",
          "[process][chat][contract][failure]") {
  runtime::ToolRegistry registry;
  UnusedArtifactStore artifacts;
  std::size_t establishment_calls{};
  auto result = adapters::assemble_process_chat_tool(
      registry,
      {.durable_session = true,
       .settings = settings(),
       .restriction = runtime::RestrictionLevel::high,
       .approval = runtime::ApprovalMode::prompt,
       .artifact_store = &artifacts,
       .establish_launcher =
           [&establishment_calls](
               adapters::LinuxProcessLauncherConfiguration configuration) {
             ++establishment_calls;
             configuration.restriction = runtime::RestrictionLevel::none;
             return adapters::establish_linux_process_launcher(
                 std::move(configuration));
           }});
  REQUIRE(result);
  REQUIRE(establishment_calls == 1);
  REQUIRE_FALSE(result->process_registered);
  REQUIRE(result->launch_context.selected_restriction() ==
          runtime::RestrictionLevel::high);
  REQUIRE_FALSE(result->launch_context.achieved_restriction());
  auto snapshot = registry.snapshot();
  REQUIRE(snapshot);
  REQUIRE(snapshot->find("run_process") == nullptr);
  REQUIRE(snapshot->find_unavailable("run_process") != nullptr);
}

TEST_CASE("allow-list approval generation is explicit and bounded",
          "[process][chat][approval][failure]") {
  auto configured = settings();
  configured.executable_allowlist.push_back("/usr/bin/printf");
  REQUIRE(adapters::configured_process_automatic_approval_rules(configured)
              .empty());

  configured.allowlist_automatic_approval_maximum_matches = 7;
  const auto rules =
      adapters::configured_process_automatic_approval_rules(configured);
  REQUIRE(rules.size() == configured.executable_allowlist.size());
  for (std::size_t index{}; index < rules.size(); ++index) {
    const auto* rule =
        std::get_if<runtime::ProcessExecutableApprovalRule>(&rules[index]);
    REQUIRE(rule != nullptr);
    REQUIRE(rule->executable == configured.executable_allowlist[index]);
    REQUIRE(rule->constraints.maximum_matches == 7);
    REQUIRE(rule->constraints.allowed_restrictions ==
            std::vector{runtime::RestrictionLevel::none,
                        runtime::RestrictionLevel::low});
  }
}

} // namespace
