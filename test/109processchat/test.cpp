#include <aiforge/adapters/process_chat_assembly.hpp>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <aiforge/runtime/run_kernel.hpp>
#include <aiforge/runtime/tool_launch_policy.hpp>
#include <aiforge/testing/application_launch_context.hpp>
#include <aiforge/testing/scripted_backend.hpp>
#include <aiforge/testing/scripted_process_launcher.hpp>

namespace {

using namespace aiforge;
using namespace std::chrono_literals;

template <typename Id> auto make_id(const std::string& value) -> Id {
  return Id::from(value).value();
}

class UnusedArtifactStore final : public storage::ArtifactStore {
 public:
  auto put(storage::ArtifactWrite, std::span<const std::byte>, std::stop_token)
      -> std::expected<domain::ArtifactMetadata,
                       storage::ArtifactStoreError> override {
    ++writes;
    return std::unexpected(storage::ArtifactStoreError{
        storage::ArtifactStoreErrorCode::internal_failure,
        "unexpected artifact write", false});
  }

  std::size_t writes{};
};

class MemorySessionStore final : public storage::SessionStore {
 public:
  domain::SessionId session_id{make_id<domain::SessionId>("session")};
  domain::EventTimestamp created{1ms};
  std::vector<domain::RunEvent> events;

  auto create_session(storage::SessionCreate session, std::stop_token)
      -> std::expected<void, storage::SessionStoreError> override {
    session_id = std::move(session.session_id);
    created = session.created_at;
    return {};
  }

  auto open_session(const domain::SessionId& requested, std::stop_token)
      -> std::expected<storage::SessionInfo,
                       storage::SessionStoreError> override {
    if (requested != session_id) {
      return std::unexpected(storage::SessionStoreError{
          storage::SessionStoreErrorCode::not_found, "missing", false});
    }
    const auto last =
        events.empty() ? created : events.back().metadata.timestamp;
    return storage::SessionInfo{
        session_id, created, last,
        events.empty() ? 0 : events.back().metadata.sequence, 0};
  }

  auto list_sessions(std::size_t, std::stop_token)
      -> std::expected<std::vector<storage::SessionInfo>,
                       storage::SessionStoreError> override {
    return std::vector<storage::SessionInfo>{};
  }

  auto append_events(const domain::SessionId& requested,
                     const std::span<const domain::RunEvent> additions,
                     std::stop_token)
      -> std::expected<void, storage::SessionStoreError> override {
    if (requested != session_id) {
      return std::unexpected(storage::SessionStoreError{
          storage::SessionStoreErrorCode::not_found, "missing", false});
    }
    events.insert(events.end(), additions.begin(), additions.end());
    return {};
  }

  auto replay_events(const domain::SessionId& requested, std::stop_token)
      -> std::expected<std::vector<domain::RunEvent>,
                       storage::SessionStoreError> override {
    if (requested != session_id) {
      return std::unexpected(storage::SessionStoreError{
          storage::SessionStoreErrorCode::not_found, "missing", false});
    }
    return events;
  }
};

class WakeCounter final : public runtime::RunWakeSink {
 public:
  auto wake() noexcept -> void override {
    {
      std::lock_guard lock(m_mutex);
      ++m_count;
    }
    m_changed.notify_all();
  }

  auto wait_for_change(const std::size_t previous) -> void {
    std::unique_lock lock(m_mutex);
    static_cast<void>(
        m_changed.wait_for(lock, 1s, [&] { return m_count > previous; }));
  }

  [[nodiscard]] auto count() -> std::size_t {
    std::lock_guard lock(m_mutex);
    return m_count;
  }

 private:
  std::mutex m_mutex;
  std::condition_variable m_changed;
  std::size_t m_count{};
};

class CountingPolicy final : public runtime::ToolPolicy {
 public:
  explicit CountingPolicy(std::shared_ptr<runtime::ToolPolicy> delegate)
      : m_delegate(std::move(delegate)) {}

  auto evaluate(const runtime::ToolPolicyRequest& request)
      -> std::expected<runtime::ToolPolicyResolution,
                       runtime::ToolPolicyError> override {
    ++evaluations;
    return m_delegate->evaluate(request);
  }

  auto approve(const runtime::ToolPolicyRequest& request,
               runtime::ToolPolicyApproval approval)
      -> std::expected<runtime::ToolPolicyResolution,
                       runtime::ToolPolicyError> override {
    return m_delegate->approve(request, std::move(approval));
  }

  [[nodiscard]] auto provenance() const noexcept
      -> const domain::ToolPolicyProvenance* override {
    return m_delegate->provenance();
  }

  [[nodiscard]] auto selected_restriction() const noexcept
      -> std::optional<runtime::RestrictionLevel> override {
    return m_delegate->selected_restriction();
  }

  std::size_t evaluations{};

 private:
  std::shared_ptr<runtime::ToolPolicy> m_delegate;
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

[[nodiscard]] auto process_arguments(
    const config::ProcessConfigSettings& configured) -> std::string {
  const auto& executable = configured.executable_allowlist.front();
  const auto& root = configured.readable_roots.front();
  return "{\"arguments\":[],\"environment\":[],\"executable\":\"" + executable +
         "\",\"output_bytes\":1024,\"readable_roots\":[\"" + root +
         "\"],\"stdin\":\"closed\",\"timeout_ms\":1000,"
         "\"working_directory\":\"" +
         root + "\",\"writable_roots\":[\"" + root + "\"]}";
}

[[nodiscard]] auto constructed_context() -> domain::ConstructedContext {
  return {{{make_id<domain::ContextEntryId>("runtime-context"),
            domain::ContextEntryKind::instruction,
            domain::InstructionLayer::application_runtime,
            {make_id<domain::MessageId>("runtime-message"),
             domain::Role::system,
             {domain::TextBlock{"runtime contract"}},
             std::nullopt},
            {make_id<domain::ContextSourceId>("runtime-source"), std::nullopt,
             std::nullopt},
            0,
            1,
            2}},
          {},
          {4096, 512, 0},
          2};
}

[[nodiscard]] auto backend_request(std::vector<backend::ToolDeclaration> tools)
    -> backend::BackendRequest {
  return {make_id<domain::InferenceId>("inference"),
          make_id<domain::MessageId>("assistant"),
          make_id<domain::ModelId>("model"),
          constructed_context(),
          std::move(tools),
          {0.25, 128, 42, {}, {}},
          {}};
}

[[nodiscard]] auto run_start(backend::BackendRequest request,
                             const domain::PermissionProfileId& profile)
    -> runtime::RunStart {
  runtime::RunStart result{make_id<domain::RunId>("run"),
                           {make_id<domain::SurfaceId>("test"),
                            make_id<domain::WorkspaceId>("chat"), profile,
                            std::nullopt},
                           {make_id<domain::MessageId>("user"),
                            domain::Role::user,
                            {domain::TextBlock{"hello"}},
                            std::nullopt},
                           std::move(request)};
  result.provenance = domain::RunProvenance{"test-version",
                                            "test-backend",
                                            std::nullopt,
                                            make_id<domain::ModelId>("model"),
                                            std::nullopt,
                                            {},
                                            {},
                                            {}};
  return result;
}

[[nodiscard]] auto process_tool_call(const domain::InvocationId& invocation,
                                     std::string arguments)
    -> testing::StreamScript {
  return {{testing::ScriptedStep{backend::ResponseStarted{"response"}},
           testing::ScriptedStep{backend::ToolCallDelta{
               invocation, "run_process", std::move(arguments)}},
           testing::ScriptedStep{
               backend::ResponseFinished{domain::FinishReason::tool_call}},
           testing::ScriptedStep{testing::EndOfStream{}}}};
}

auto drain_to_inference_boundary(runtime::RunKernel& kernel, WakeCounter& wake)
    -> void {
  std::size_t observed{};
  for (int attempt = 0; attempt < 100 && kernel.active_inference_id();
       ++attempt) {
    const auto drained = kernel.drain();
    INFO((drained ? std::string{} : drained.error().message));
    REQUIRE(drained);
    if (kernel.active_inference_id()) wake.wait_for_change(observed);
    observed = wake.count();
  }
  REQUIRE_FALSE(kernel.active_inference_id());
  REQUIRE(kernel.active_run_id());
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

TEST_CASE("ephemeral process assembly stays unavailable for every launch mode",
          "[process][chat][lifetime][restriction][failure]") {
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
      auto result = assemble(registry, settings(), false, restriction, approval,
                             &artifacts, establishment_calls);
      REQUIRE(result);
      REQUIRE(establishment_calls == 1);
      REQUIRE_FALSE(result->process_registered);
      REQUIRE(result->launch_context.selected_restriction() == restriction);
      REQUIRE(result->launch_context.approval_mode() == approval);
      REQUIRE(result->launch_context.matcher_policy_identity() ==
              matcher_identity(approval));

      const auto snapshot = registry.snapshot();
      REQUIRE(snapshot);
      REQUIRE(snapshot->find("run_process") == nullptr);
      const auto* unavailable = snapshot->find_unavailable("run_process");
      REQUIRE(unavailable != nullptr);
      REQUIRE(unavailable->unavailability.reason ==
              runtime::ToolUnavailableReason::durable_session_required);
      REQUIRE(snapshot->find("run_shell") == nullptr);
      REQUIRE(snapshot->find_unavailable("run_shell") != nullptr);
    }
  }
  REQUIRE(artifacts.writes == 0);
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
                   .matcher_policy_identity = std::nullopt,
                   .artifact_store = &artifacts,
                   .environment_lookup = {},
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
       .matcher_policy_identity = std::nullopt,
       .artifact_store = &artifacts,
       .environment_lookup = {},
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

TEST_CASE("durable process replay invokes no external boundary",
          "[process][chat][storage][replay][security][failure]") {
  const auto configured = settings();
  const auto executable = configured.executable_allowlist.front();
  const auto root = configured.readable_roots.front();
  const auto invocation = make_id<domain::InvocationId>("process-call");
  const auto profile =
      make_id<domain::PermissionProfileId>("process-none-prompt-v1");
  const auto launch_context = testing::available_application_launch_context(
      runtime::RestrictionLevel::none, runtime::ApprovalMode::prompt);
  const runtime::ProcessLauncherContract launcher_contract{
      runtime::RestrictionLevel::none, launch_context.mechanism(),
      *launch_context.restriction_policy_identity()};
  auto launcher = std::make_shared<testing::ScriptedProcessLauncher>(
      launcher_contract, std::vector<testing::ScriptedProcessLaunchExchange>{},
      runtime::ProcessLaunchBounds{},
      std::vector<testing::ScriptedProcessPathPinExchange>{
          {executable, runtime::ProcessFilesystemTargetKind::regular_executable,
           std::string{"executable-identity"}},
          {root, runtime::ProcessFilesystemTargetKind::directory,
           std::string{"root-identity"}}});

  runtime::ToolRegistry registry;
  UnusedArtifactStore artifacts;
  std::size_t establishment_calls{};
  auto assembled = adapters::assemble_process_chat_tool(
      registry,
      {.durable_session = true,
       .settings = configured,
       .restriction = runtime::RestrictionLevel::none,
       .approval = runtime::ApprovalMode::prompt,
       .matcher_policy_identity = std::nullopt,
       .artifact_store = &artifacts,
       .environment_lookup = {},
       .establish_launcher = [launcher, launch_context, &establishment_calls](
                                 adapters::LinuxProcessLauncherConfiguration)
           -> std::expected<adapters::LinuxProcessLauncherEstablishment,
                            adapters::LinuxProcessLauncherEstablishmentError> {
         ++establishment_calls;
         auto bound = runtime::bind_process_launcher(launch_context, launcher);
         if (!bound) {
           return std::unexpected(
               adapters::LinuxProcessLauncherEstablishmentError{
                   adapters::LinuxProcessLauncherEstablishmentErrorCode::
                       binding_failed,
                   bound.error().message});
         }
         return adapters::LinuxProcessLauncherEstablishment{std::move(*bound)};
       }});
  REQUIRE(assembled);
  REQUIRE(assembled->process_registered);
  REQUIRE(establishment_calls == 1);
  REQUIRE_FALSE(assembled->launch_context.matcher_policy_identity());
  REQUIRE(launcher->recorded_path_pins().size() == 2);
  REQUIRE(launcher->remaining_path_pins() == 0);
  REQUIRE(launcher->recorded_requests().empty());
  REQUIRE(artifacts.writes == 0);

  const auto snapshot = registry.snapshot();
  REQUIRE(snapshot);
  const auto make_policy = [&] {
    auto policy = runtime::make_tool_launch_policy(
        *snapshot, {profile, assembled->launch_context, {}});
    REQUIRE(policy);
    return std::make_shared<CountingPolicy>(std::move(*policy));
  };

  const auto arguments = process_arguments(configured);
  const auto initial_request = backend_request(snapshot->declarations());
  testing::ScriptedBackend backend{{
      {initial_request, process_tool_call(invocation, arguments)},
  }};
  MemorySessionStore store;
  WakeCounter wake;
  auto initial_policy = make_policy();
  auto kernel = runtime::RunKernel::open_durable(
      {store.session_id, runtime::DurableSessionMode::create, store.created},
      store, backend, &wake, {}, {}, *snapshot, initial_policy);
  REQUIRE(kernel);
  REQUIRE((*kernel)->start(run_start(initial_request, profile)));
  drain_to_inference_boundary(**kernel, wake);
  REQUIRE((*kernel)->pending_tool_approval());
  const auto pending = *(*kernel)->pending_tool_approval();
  REQUIRE(pending.invocation_id == invocation);
  REQUIRE(initial_policy->evaluations == 1);
  REQUIRE(backend.recorded_requests().size() == 1);
  REQUIRE(launcher->recorded_path_pins().size() == 2);
  REQUIRE(launcher->recorded_requests().empty());
  REQUIRE(artifacts.writes == 0);
  const auto persisted_events = store.events.size();
  kernel->reset();

  testing::ScriptedBackend replay_backend{{}};
  WakeCounter replay_wake;
  auto replay_policy = make_policy();
  auto replayed = runtime::RunKernel::open_durable(
      {store.session_id, runtime::DurableSessionMode::resume, store.created},
      store, replay_backend, &replay_wake, {}, {}, *snapshot, replay_policy);
  REQUIRE(replayed);
  REQUIRE((*replayed)->pending_tool_approval() == pending);
  REQUIRE_FALSE((*replayed)->active_inference_id());
  // Resume re-evaluates the current in-memory policy to reject stale launch
  // authority, but never crosses an executor or provider boundary.
  REQUIRE(replay_policy->evaluations == 1);
  REQUIRE(replay_backend.recorded_requests().empty());
  REQUIRE(replay_backend.remaining_exchanges() == 0);
  REQUIRE(launcher->recorded_path_pins().size() == 2);
  REQUIRE(launcher->recorded_requests().empty());
  REQUIRE(artifacts.writes == 0);
  REQUIRE(store.events.size() == persisted_events);
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
