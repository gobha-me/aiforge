#include <aiforge/runtime/repository_read_tool.hpp>
#include <aiforge/runtime/tool_launch_policy.hpp>
#include <aiforge/testing/scripted_exact_source_editor.hpp>
#include <aiforge/testing/scripted_repository_snapshot_source.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace aiforge;

template <typename Id> auto id(std::string value) -> Id {
  auto parsed = Id::from(std::move(value));
  REQUIRE(parsed);
  return std::move(*parsed);
}

auto digest(std::string value, const std::uint64_t bytes)
    -> domain::ContentDigest {
  return {"sha256", std::move(value), bytes};
}

auto snapshot(std::string root = "/work/repository")
    -> domain::RepositorySnapshot {
  return {{id<domain::RepositoryId>("repository-1"), std::move(root)},
          domain::VcsState{"git", "sha256", domain::VcsHeadKind::branch, "main",
                           "bbbbbbbbbbbbbbbb"},
          {},
          digest("aaaaaaaaaaaaaaaa", 64),
          std::chrono::sys_time<std::chrono::milliseconds>{
              std::chrono::milliseconds{100}}};
}

auto configuration() -> runtime::RepositoryReadToolConfiguration {
  return {"/work/repository"};
}

auto arguments(std::string data) -> domain::StructuredDataBlock {
  return {"application/json", std::move(data)};
}

auto register_tool(testing::ScriptedRepositorySnapshotSource& snapshots,
                   testing::ScriptedExactSourceEditor& sources,
                   runtime::RepositoryReadToolConfiguration config =
                       configuration()) -> runtime::RegisteredTool {
  sources.couple_to(snapshots, true);
  runtime::ToolRegistry registry;
  REQUIRE(runtime::register_repository_read_tool(registry, snapshots, sources,
                                                 std::move(config)));
  auto registered = registry.snapshot();
  REQUIRE(registered);
  const auto* tool = registered->find("read_repository_file");
  REQUIRE(tool != nullptr);
  return *tool;
}

auto start(const runtime::RegisteredTool& tool,
           domain::StructuredDataBlock value,
           const std::stop_token stop_token = {})
    -> std::expected<std::unique_ptr<runtime::ToolExecutionStream>,
                     runtime::ToolExecutionError> {
  auto validated = tool.executor->validate(value);
  if (!validated) return std::unexpected(std::move(validated.error()));
  auto granted_scopes = validated->required_scopes;
  return tool.executor->start({id<domain::InvocationId>("invocation-1"),
                               std::nullopt, "read_repository_file",
                               std::move(*validated), std::move(granted_scopes),
                               tool.limits},
                              stop_token);
}

} // namespace

TEST_CASE("repository-read registration rejects unsafe roots and limits") {
  testing::ScriptedRepositorySnapshotSource snapshots;
  testing::ScriptedExactSourceEditor sources;
  sources.couple_to(snapshots, true);
  runtime::ToolRegistry registry;

  auto config = configuration();
  config.repository_root = "relative/repository";
  auto result = runtime::register_repository_read_tool(registry, snapshots,
                                                       sources, config);
  REQUIRE_FALSE(result);
  CHECK(result.error().code ==
        runtime::ToolRegistryErrorCode::invalid_declaration);

  config = configuration();
  config.repository_root = "/work/repository/../elsewhere";
  REQUIRE_FALSE(runtime::register_repository_read_tool(registry, snapshots,
                                                       sources, config));

  config = configuration();
  config.repository_root = std::string{"/work/\xE2\x80\xAErepo"};
  REQUIRE_FALSE(runtime::register_repository_read_tool(registry, snapshots,
                                                       sources, config));

  config = configuration();
  config.maximum_argument_bytes = 0;
  REQUIRE_FALSE(runtime::register_repository_read_tool(registry, snapshots,
                                                       sources, config));

  config = configuration();
  config.maximum_result_bytes = 4U * 1024U * 1024U + 1U;
  REQUIRE_FALSE(runtime::register_repository_read_tool(registry, snapshots,
                                                       sources, config));

  config = configuration();
  config.read_limits.maximum_source_bytes = 1024U * 1024U + 1U;
  REQUIRE_FALSE(runtime::register_repository_read_tool(registry, snapshots,
                                                       sources, config));

  config = configuration();
  config.snapshot_limits.command_timeout = std::chrono::seconds{20};
  config.snapshot_limits.observation_timeout = std::chrono::seconds{10};
  REQUIRE_FALSE(runtime::register_repository_read_tool(registry, snapshots,
                                                       sources, config));

  config = configuration();
  config.snapshot_limits.maximum_total_bytes = 64U * 1024U * 1024U + 1U;
  REQUIRE_FALSE(runtime::register_repository_read_tool(registry, snapshots,
                                                       sources, config));

  auto snapshot_result = registry.snapshot();
  REQUIRE(snapshot_result);
  CHECK(snapshot_result->empty());

  testing::ScriptedExactSourceEditor unrestricted{{}, {}, false};
  unrestricted.couple_to(snapshots, false);
  REQUIRE_FALSE(runtime::register_repository_read_tool(
      registry, snapshots, unrestricted, configuration()));
  snapshot_result = registry.snapshot();
  REQUIRE(snapshot_result);
  CHECK(snapshot_result->empty());

  testing::ScriptedRepositorySnapshotSource unsafe_observer{{}, false};
  testing::ScriptedExactSourceEditor safe_editor;
  safe_editor.couple_to(unsafe_observer, true);
  REQUIRE_FALSE(runtime::register_repository_read_tool(
      registry, unsafe_observer, safe_editor, configuration()));

  testing::ScriptedRepositorySnapshotSource other_observer;
  testing::ScriptedExactSourceEditor mismatched_editor;
  mismatched_editor.couple_to(snapshots, true);
  REQUIRE_FALSE(runtime::register_repository_read_tool(
      registry, other_observer, mismatched_editor, configuration()));
}

TEST_CASE("repository-read rejects malformed paths before observing") {
  testing::ScriptedRepositorySnapshotSource snapshots;
  testing::ScriptedExactSourceEditor sources;
  const auto tool = register_tool(snapshots, sources);

  const std::vector<domain::StructuredDataBlock> invalid{
      {"text/plain", R"({"relative_path":"src/main.cpp"})"},
      arguments(""),
      arguments("{"),
      arguments(R"({"relative_path":"src/a","relative_path":"src/b"})"),
      arguments(R"({"relative_path":42})"),
      arguments(R"({"relative_path":"src/main.cpp","extra":true})"),
      arguments(R"({"relative_path":""})"),
      arguments(R"({"relative_path":"/etc/passwd"})"),
      arguments(R"({"relative_path":"../secret"})"),
      arguments(R"({"relative_path":"src/../secret"})"),
      arguments(R"({"relative_path":"src//main.cpp"})"),
      arguments(R"({"relative_path":"src/main.cpp/"})"),
      arguments(R"({"relative_path":"src\\main.cpp"})"),
      arguments(R"({"relative_path":".git/config"})"),
      arguments(R"({"relative_path":"src/.git/config"})"),
      arguments(R"({"relative_path":"src\nmain.cpp"})"),
      arguments(std::string{"{\"relative_path\":\"\xE2\x80\xAE\"}"}),
      arguments(std::string{"{\"relative_path\":\"\xC3\x28\"}"}),
  };
  for (const auto& value : invalid) {
    CAPTURE(value.media_type, value.data);
    const auto result = tool.executor->validate(value);
    REQUIRE_FALSE(result);
    CHECK(result.error().code ==
          runtime::ToolExecutionErrorCode::invalid_arguments);
  }
  CHECK(snapshots.recorded_requests().empty());
  CHECK(sources.recorded_read_requests().empty());
}

TEST_CASE("repository-read bounds arguments before parsing") {
  testing::ScriptedRepositorySnapshotSource snapshots;
  testing::ScriptedExactSourceEditor sources;
  auto config = configuration();
  config.maximum_argument_bytes = 32;
  const auto tool = register_tool(snapshots, sources, config);

  const auto result = tool.executor->validate(
      arguments(R"({"relative_path":"source-name-that-is-too-long.cpp"})"));
  REQUIRE_FALSE(result);
  CHECK(result.error().code ==
        runtime::ToolExecutionErrorCode::invalid_arguments);
  CHECK(snapshots.recorded_requests().empty());
}

TEST_CASE("repository-read does not continue after observation failure") {
  const auto config = configuration();
  const repository::RepositorySnapshotRequest expected_request{
      config.repository_root, config.snapshot_limits};
  testing::ScriptedRepositorySnapshotSource snapshots{{
      {expected_request,
       repository::RepositorySnapshotError{
           repository::RepositorySnapshotErrorCode::unstable,
           "provider path detail", true}},
  }};
  testing::ScriptedExactSourceEditor sources;
  const auto tool = register_tool(snapshots, sources, config);

  auto result = start(tool, arguments(R"({"relative_path":"src/main.cpp"})"));
  REQUIRE_FALSE(result);
  CHECK(result.error().code == runtime::ToolExecutionErrorCode::unavailable);
  CHECK(result.error().retryable);
  CHECK(result.error().message.find("provider path detail") ==
        std::string::npos);
  CHECK(sources.recorded_read_requests().empty());
}

TEST_CASE("repository-read rejects invalid snapshots before exact reads") {
  const auto config = configuration();
  const repository::RepositorySnapshotRequest expected_request{
      config.repository_root, config.snapshot_limits};
  testing::ScriptedRepositorySnapshotSource snapshots{{
      {expected_request, snapshot("/work/other")},
  }};
  testing::ScriptedExactSourceEditor sources;
  const auto tool = register_tool(snapshots, sources, config);

  auto result = start(tool, arguments(R"({"relative_path":"src/main.cpp"})"));
  REQUIRE_FALSE(result);
  CHECK(result.error().code ==
        runtime::ToolExecutionErrorCode::protocol_failure);
  CHECK(sources.recorded_read_requests().empty());
}

TEST_CASE("repository-read rejects non-Git and untracked snapshots") {
  const auto config = configuration();
  const repository::RepositorySnapshotRequest expected_request{
      config.repository_root, config.snapshot_limits};

  auto plain = snapshot();
  plain.vcs.reset();
  testing::ScriptedRepositorySnapshotSource plain_snapshots{{
      {expected_request, plain},
  }};
  testing::ScriptedExactSourceEditor plain_sources;
  const auto plain_tool = register_tool(plain_snapshots, plain_sources, config);
  auto plain_result =
      start(plain_tool, arguments(R"({"relative_path":"src/main.cpp"})"));
  REQUIRE_FALSE(plain_result);
  CHECK(plain_result.error().code ==
        runtime::ToolExecutionErrorCode::protocol_failure);
  CHECK(plain_sources.recorded_read_requests().empty());

  auto untracked = snapshot();
  untracked.changes.push_back({"src/main.cpp", std::nullopt,
                               domain::RepositoryEntryKind::regular_file,
                               domain::RepositoryChangeKind::untracked,
                               domain::RepositoryChangeStage::untracked,
                               std::nullopt, digest("dddddddddddddddd", 4)});
  testing::ScriptedRepositorySnapshotSource untracked_snapshots{{
      {expected_request, untracked},
  }};
  testing::ScriptedExactSourceEditor untracked_sources;
  const auto untracked_tool =
      register_tool(untracked_snapshots, untracked_sources, config);
  auto untracked_result =
      start(untracked_tool, arguments(R"({"relative_path":"src/main.cpp"})"));
  REQUIRE_FALSE(untracked_result);
  CHECK(untracked_result.error().code ==
        runtime::ToolExecutionErrorCode::unavailable);
  CHECK(untracked_sources.recorded_read_requests().empty());
}

TEST_CASE("repository-read maps exact-source failures without partial output") {
  const auto config = configuration();
  const auto baseline = snapshot();
  const repository::RepositorySnapshotRequest snapshot_request{
      config.repository_root, config.snapshot_limits};
  const repository::ExactSourceReadRequest read_request{
      baseline, "src/main.cpp", config.read_limits};

  struct FailureCase {
    repository::ExactSourceEditErrorCode source;
    runtime::ToolExecutionErrorCode expected;
    bool retryable;
  };
  const std::vector<FailureCase> failures{
      {repository::ExactSourceEditErrorCode::not_found,
       runtime::ToolExecutionErrorCode::unavailable, false},
      {repository::ExactSourceEditErrorCode::unsupported_entry,
       runtime::ToolExecutionErrorCode::unavailable, false},
      {repository::ExactSourceEditErrorCode::resource_exhausted,
       runtime::ToolExecutionErrorCode::output_limit, false},
      {repository::ExactSourceEditErrorCode::stale_snapshot,
       runtime::ToolExecutionErrorCode::unavailable, true},
      {repository::ExactSourceEditErrorCode::source_mismatch,
       runtime::ToolExecutionErrorCode::unavailable, true},
      {repository::ExactSourceEditErrorCode::timed_out,
       runtime::ToolExecutionErrorCode::timed_out, true},
      {repository::ExactSourceEditErrorCode::cancelled,
       runtime::ToolExecutionErrorCode::cancelled, false},
      {repository::ExactSourceEditErrorCode::invalid_request,
       runtime::ToolExecutionErrorCode::protocol_failure, false},
      {repository::ExactSourceEditErrorCode::internal_failure,
       runtime::ToolExecutionErrorCode::internal_failure, false},
  };

  for (const auto& failure : failures) {
    testing::ScriptedRepositorySnapshotSource snapshots{{
        {snapshot_request, baseline},
    }};
    testing::ScriptedExactSourceEditor sources{{
        {read_request, repository::ExactSourceEditError{failure.source,
                                                        "private detail",
                                                        {},
                                                        {},
                                                        failure.retryable,
                                                        false}},
    }};
    const auto tool = register_tool(snapshots, sources, config);
    auto result = start(tool, arguments(R"({"relative_path":"src/main.cpp"})"));
    REQUIRE_FALSE(result);
    CHECK(result.error().code == failure.expected);
    CHECK(result.error().retryable == failure.retryable);
    CHECK(result.error().message.find("private detail") == std::string::npos);
    CHECK(sources.remaining_read_exchanges() == 0);
  }
}

TEST_CASE("repository-read rejects inconsistent unsafe and oversized results") {
  auto config = configuration();
  config.maximum_result_bytes = 128;
  const auto baseline = snapshot();
  const repository::RepositorySnapshotRequest snapshot_request{
      config.repository_root, config.snapshot_limits};
  const repository::ExactSourceReadRequest read_request{
      baseline, "src/main.cpp", config.read_limits};

  const std::vector<repository::ExactSourceReadResult> invalid{
      {{domain::snapshot_identity(baseline), "other.cpp",
        digest("cccccccccccccccc", 4), std::nullopt},
       "text"},
      {{domain::snapshot_identity(baseline), "src/main.cpp",
        digest("cccccccccccccccc", 3), std::nullopt},
       "text"},
      {{domain::snapshot_identity(baseline), "src/main.cpp",
        digest("cccccccccccccccc", 4), std::nullopt},
       std::string{"a\xE2\x80\xAE", 4}},
  };
  for (const auto& result_value : invalid) {
    testing::ScriptedRepositorySnapshotSource snapshots{{
        {snapshot_request, baseline},
    }};
    testing::ScriptedExactSourceEditor sources{{
        {read_request, result_value},
    }};
    const auto tool = register_tool(snapshots, sources, config);
    auto result = start(tool, arguments(R"({"relative_path":"src/main.cpp"})"));
    REQUIRE_FALSE(result);
    CHECK(result.error().code ==
          runtime::ToolExecutionErrorCode::protocol_failure);
  }

  const std::string content(96, '\\');
  testing::ScriptedRepositorySnapshotSource snapshots{{
      {snapshot_request, baseline},
  }};
  testing::ScriptedExactSourceEditor sources{{
      {read_request,
       repository::ExactSourceReadResult{
           {domain::snapshot_identity(baseline), "src/main.cpp",
            digest("cccccccccccccccc", content.size()), std::nullopt},
           content}},
  }};
  const auto tool = register_tool(snapshots, sources, config);
  auto oversized =
      start(tool, arguments(R"({"relative_path":"src/main.cpp"})"));
  REQUIRE_FALSE(oversized);
  CHECK(oversized.error().code ==
        runtime::ToolExecutionErrorCode::output_limit);
}

TEST_CASE("repository-read cancellation performs no observation or read") {
  testing::ScriptedRepositorySnapshotSource snapshots;
  testing::ScriptedExactSourceEditor sources;
  const auto tool = register_tool(snapshots, sources);
  std::stop_source cancellation;
  cancellation.request_stop();

  auto result = start(tool, arguments(R"({"relative_path":"src/main.cpp"})"),
                      cancellation.get_token());
  REQUIRE_FALSE(result);
  CHECK(result.error().code == runtime::ToolExecutionErrorCode::cancelled);
  CHECK(snapshots.recorded_requests().empty());
  CHECK(sources.recorded_read_requests().empty());
}

TEST_CASE("repository-read returns exact bounded source identity and content") {
  const auto config = configuration();
  const auto baseline = snapshot();
  const repository::RepositorySnapshotRequest snapshot_request{
      config.repository_root, config.snapshot_limits};
  const repository::ExactSourceReadRequest read_request{
      baseline, "src/main.cpp", config.read_limits};
  const std::string content{"int main() { return 0; }\n"};
  testing::ScriptedRepositorySnapshotSource snapshots{{
      {snapshot_request, baseline},
  }};
  testing::ScriptedExactSourceEditor sources{{
      {read_request,
       repository::ExactSourceReadResult{
           {domain::snapshot_identity(baseline), "src/main.cpp",
            digest("cccccccccccccccc", content.size()), std::nullopt},
           content}},
  }};
  const auto tool = register_tool(snapshots, sources, config);

  CHECK(tool.declaration.effects ==
        std::vector<domain::Effect>{domain::Effect::read});
  CHECK(tool.declaration.capability_scopes ==
        std::vector<domain::CapabilityScope>{
            {domain::Effect::read, "filesystem.root", "/work/repository"}});
  REQUIRE(tool.executor_contract);
  CHECK(tool.executor_contract->identity ==
        "aiforge.runtime.read_repository_file");
  CHECK(tool.executor_contract->version == "2");

  const auto raw_arguments = arguments(R"({"relative_path":"src/main.cpp"})");
  auto validated = tool.executor->validate(raw_arguments);
  REQUIRE(validated);
  CHECK(validated->value == raw_arguments);
  CHECK(validated->required_effects == tool.declaration.effects);
  CHECK(validated->required_scopes ==
        std::vector<domain::CapabilityScope>{
            {domain::Effect::read, "filesystem.root",
             "/work/repository/src/main.cpp"}});

  auto started = start(tool, raw_arguments);
  REQUIRE(started);
  auto result = (*started)->next({});
  REQUIRE(result);
  REQUIRE(result->has_value());
  const auto* completed = std::get_if<runtime::ToolResult>(&result->value());
  REQUIRE(completed != nullptr);
  REQUIRE(completed->content.size() == 1);
  const auto* structured =
      std::get_if<domain::StructuredDataBlock>(&completed->content.front());
  REQUIRE(structured != nullptr);
  CHECK(structured->media_type == "application/json");
  CHECK(structured->data.find(R"("repository_id":"repository-1")") !=
        std::string::npos);
  CHECK(structured->data.find(R"("relative_path":"src/main.cpp")") !=
        std::string::npos);
  CHECK(structured->data.find(R"("value":"cccccccccccccccc")") !=
        std::string::npos);
  CHECK(structured->data.find("int main() { return 0; }\\n") !=
        std::string::npos);
  CHECK(snapshots.remaining_exchanges() == 0);
  CHECK(sources.remaining_read_exchanges() == 0);

  auto ended = (*started)->next({});
  REQUIRE(ended);
  CHECK_FALSE(ended->has_value());
}

TEST_CASE("repository-read prompt exposes only the requested file scope") {
  testing::ScriptedRepositorySnapshotSource snapshots;
  testing::ScriptedExactSourceEditor sources;
  sources.couple_to(snapshots, true);
  runtime::ToolRegistry registry;
  REQUIRE(runtime::register_repository_read_tool(registry, snapshots, sources,
                                                 configuration()));
  auto registered = registry.snapshot();
  REQUIRE(registered);
  const auto* tool = registered->find("read_repository_file");
  REQUIRE(tool != nullptr);
  auto validated = tool->executor->validate(
      arguments(R"({"relative_path":"src/main.cpp"})"));
  REQUIRE(validated);

  const auto profile =
      id<domain::PermissionProfileId>("repository-read-prompt-v1");
  auto policy = runtime::make_tool_launch_policy(
      *registered, {profile,
                    runtime::RestrictionLevel::medium,
                    runtime::ApprovalMode::prompt,
                    {}});
  REQUIRE(policy);
  auto resolution = (*policy)->evaluate(
      {id<domain::SessionId>("session"), id<domain::RunId>("run"),
       id<domain::InvocationId>("invocation"), profile, "read_repository_file",
       validated->required_effects, validated->required_scopes});
  REQUIRE(resolution);
  CHECK(resolution->decision == domain::PolicyDecision::require_approval);
  CHECK(resolution->scopes == validated->required_scopes);
  CHECK(resolution->scopes == std::vector<domain::CapabilityScope>{
                                  {domain::Effect::read, "filesystem.root",
                                   "/work/repository/src/main.cpp"}});
}
