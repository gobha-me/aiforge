#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>

#include <aiforge/adapters/process_tool.hpp>
#include <aiforge/testing/scripted_artifact_store.hpp>

namespace {

using namespace std::chrono_literals;
using namespace aiforge;
using Json = nlohmann::json;

template <typename IdType> auto make_id(const std::string& value) -> IdType {
  return IdType::from(value).value();
}

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    auto pattern =
        (std::filesystem::temp_directory_path() / "aiforge-process-XXXXXX")
            .string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    const auto* result = ::mkdtemp(buffer.data());
    REQUIRE(result != nullptr);
    m_path = result;
  }
  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(m_path, error);
  }
  TemporaryDirectory(const TemporaryDirectory&) = delete;
  auto operator=(const TemporaryDirectory&) -> TemporaryDirectory& = delete;
  [[nodiscard]] auto path() const -> const std::filesystem::path& {
    return m_path;
  }

 private:
  std::filesystem::path m_path;
};

struct StoredCall {
  storage::ArtifactWrite write;
  std::string content;
};

class RecordingArtifactStore final : public storage::ArtifactStore {
 public:
  explicit RecordingArtifactStore(
      std::optional<storage::ArtifactStoreError> failure = std::nullopt)
      : m_failure(std::move(failure)) {}

  auto put(storage::ArtifactWrite write,
           const std::span<const std::byte> content, std::stop_token)
      -> std::expected<domain::ArtifactMetadata,
                       storage::ArtifactStoreError> override {
    std::string value;
    value.resize(content.size());
    for (std::size_t index = 0; index < content.size(); ++index) {
      value[index] = std::to_integer<char>(content[index]);
    }
    m_calls.push_back({write, value});
    if (m_failure) return std::unexpected(*m_failure);
    return domain::ArtifactMetadata{write.artifact_id,
                                    write.media_type,
                                    static_cast<std::uint64_t>(content.size()),
                                    "test-digest-" +
                                        std::to_string(content.size()),
                                    write.producing_invocation_id,
                                    std::nullopt,
                                    std::nullopt};
  }

  [[nodiscard]] auto calls() const -> const std::vector<StoredCall>& {
    return m_calls;
  }

 private:
  std::optional<storage::ArtifactStoreError> m_failure;
  std::vector<StoredCall> m_calls;
};

auto limits() -> adapters::ProcessToolLimits {
  return {8,           32,   16U * 1024U, 8,   8, 2s, 256U * 1024U,
          32U * 1024U, 1024, 32,          50ms};
}

auto configuration(const std::filesystem::path& executable,
                   const std::filesystem::path& root,
                   adapters::ProcessToolLimits process_limits = limits())
    -> adapters::ProcessToolConfiguration {
  return {{executable},
          {root},
          {root},
          {{"SAFE_VALUE", "secret-value-123"}},
          process_limits};
}

auto arguments(const std::filesystem::path& executable,
               const std::filesystem::path& root, std::vector<std::string> argv,
               std::vector<std::string> environment = {},
               const std::uint64_t timeout_ms = 1000,
               const std::uint64_t output_bytes = 128U * 1024U,
               std::vector<std::string> writable = {})
    -> domain::StructuredDataBlock {
  Json value{{"executable", executable.generic_string()},
             {"arguments", std::move(argv)},
             {"working_directory", root.generic_string()},
             {"readable_roots", Json::array({root.generic_string()})},
             {"writable_roots", std::move(writable)},
             {"environment", std::move(environment)},
             {"stdin", "closed"},
             {"timeout_ms", timeout_ms},
             {"output_bytes", output_bytes}};
  return {"application/json", value.dump()};
}

struct Execution {
  runtime::ValidatedToolArguments validated;
  std::vector<runtime::ToolProgress> progress;
  runtime::ToolResult result;
};

auto execute(runtime::ToolRegistry& registry,
             const domain::StructuredDataBlock& arguments,
             std::stop_token stop_token = {})
    -> std::expected<Execution, runtime::ToolExecutionError> {
  auto snapshot = registry.snapshot();
  REQUIRE(snapshot);
  const auto* registered = snapshot->find("run_process");
  REQUIRE(registered != nullptr);
  auto validated = registered->executor->validate(arguments);
  if (!validated) return std::unexpected(validated.error());
  auto stream = registered->executor->start(
      {make_id<domain::InvocationId>("process-call"), std::nullopt,
       "run_process", *validated, validated->required_scopes,
       registered->limits},
      stop_token);
  if (!stream) return std::unexpected(stream.error());
  Execution execution{*validated, {}, {}};
  for (;;) {
    auto next = (*stream)->next(stop_token);
    if (!next) return std::unexpected(next.error());
    if (!*next) break;
    if (auto* progress = std::get_if<runtime::ToolProgress>(&**next)) {
      execution.progress.push_back(std::move(*progress));
    } else if (auto* result = std::get_if<runtime::ToolResult>(&**next)) {
      execution.result = std::move(*result);
    }
  }
  return execution;
}

auto result_json(const runtime::ToolResult& result) -> Json {
  REQUIRE_FALSE(result.content.empty());
  const auto* structured =
      std::get_if<domain::StructuredDataBlock>(&result.content.front());
  REQUIRE(structured != nullptr);
  REQUIRE(structured->media_type ==
          "application/vnd.aiforge.process-result+json");
  return Json::parse(structured->data);
}

auto fixture() -> std::filesystem::path {
  return std::filesystem::path{PROCESS_TEST_FIXTURE};
}

} // namespace

TEST_CASE("process declaration and validation fail closed",
          "[process][validation][failure]") {
  TemporaryDirectory temporary;
  RecordingArtifactStore artifacts;
  auto config = configuration(fixture(), temporary.path());
  auto declaration = adapters::process_tool_declaration(config);
  REQUIRE(declaration);
  REQUIRE(declaration->name == "run_process");
  REQUIRE(declaration->effects ==
          std::vector<domain::Effect>{
              domain::Effect::execute, domain::Effect::read,
              domain::Effect::network, domain::Effect::write});
  REQUIRE(std::ranges::contains(declaration->capability_scopes,
                                domain::CapabilityScope{domain::Effect::network,
                                                        "network.unrestricted",
                                                        "new-sockets"}));

  runtime::ToolRegistry registry;
  REQUIRE(adapters::register_process_tool(registry, artifacts, config));
  auto snapshot = registry.snapshot();
  REQUIRE(snapshot);
  const auto* registration = snapshot->find("run_process");
  REQUIRE(registration != nullptr);
  const runtime::ToolExecutorContract process_contract{
      "aiforge.adapters.run_process", "1"};
  REQUIRE(registration->executor_contract == process_contract);
  auto* executor = registration->executor.get();

  const auto valid = executor->validate(
      arguments(fixture(), temporary.path(), {}, {}, 1000, 128U * 1024U,
                {temporary.path().generic_string()}));
  REQUIRE(valid);
  REQUIRE(std::ranges::contains(valid->required_scopes,
                                domain::CapabilityScope{domain::Effect::network,
                                                        "network.unrestricted",
                                                        "new-sockets"}));
  REQUIRE(
      std::ranges::contains(valid->required_effects, domain::Effect::network));

  for (auto malformed :
       std::vector<domain::StructuredDataBlock>{{"text/plain", "{}"},
                                                {"application/json", "{"},
                                                {"application/json", "{}"}}) {
    INFO(malformed.data);
    REQUIRE_FALSE(executor->validate(malformed));
  }

  auto strict_json = arguments(fixture(), temporary.path(), {}).data;
  strict_json.insert(1, "/* comment */");
  REQUIRE_FALSE(executor->validate({"application/json", strict_json}));
  auto duplicate_key = arguments(fixture(), temporary.path(), {}).data;
  duplicate_key.insert(1, "\"stdin\":\"closed\",");
  REQUIRE_FALSE(executor->validate({"application/json", duplicate_key}));

  auto bad = Json::parse(arguments(fixture(), temporary.path(), {}).data);
  bad["executable"] = "relative";
  REQUIRE_FALSE(executor->validate({"application/json", bad.dump()}));
  bad = Json::parse(arguments(fixture(), temporary.path(), {}).data);
  bad["working_directory"] = "/tmp/../outside";
  REQUIRE_FALSE(executor->validate({"application/json", bad.dump()}));
  bad = Json::parse(arguments(fixture(), temporary.path(), {}).data);
  bad["environment"] = Json::array({"NOT_ALLOWED"});
  REQUIRE_FALSE(executor->validate({"application/json", bad.dump()}));
  bad = Json::parse(arguments(fixture(), temporary.path(), {}).data);
  bad["stdin"] = "inherit";
  REQUIRE_FALSE(executor->validate({"application/json", bad.dump()}));
  bad = Json::parse(arguments(fixture(), temporary.path(), {}).data);
  bad["timeout_ms"] = 0;
  REQUIRE_FALSE(executor->validate({"application/json", bad.dump()}));
  bad = Json::parse(arguments(fixture(), temporary.path(), {}).data);
  bad["unknown"] = true;
  REQUIRE_FALSE(executor->validate({"application/json", bad.dump()}));

  auto too_many = std::vector<std::string>(33, "argument");
  REQUIRE_FALSE(
      executor->validate(arguments(fixture(), temporary.path(), too_many)));
  auto at_boundary = std::vector<std::string>(32, "argument");
  REQUIRE(
      executor->validate(arguments(fixture(), temporary.path(), at_boundary)));
  REQUIRE_FALSE(executor->validate(
      arguments(fixture(), temporary.path(), {std::string(16U * 1024U, 'x')})));

  auto missing_directory =
      Json::parse(arguments(fixture(), temporary.path(), {}).data);
  missing_directory["working_directory"] =
      (temporary.path() / "missing").generic_string();
  auto validated =
      executor->validate({"application/json", missing_directory.dump()});
  REQUIRE(validated);
  auto missing =
      executor->start({make_id<domain::InvocationId>("missing-directory"),
                       std::nullopt, "run_process", *validated,
                       validated->required_scopes, registration->limits},
                      {});
  REQUIRE_FALSE(missing);
  REQUIRE(missing.error().code == runtime::ToolExecutionErrorCode::unavailable);

  validated = executor->validate(arguments(fixture(), temporary.path(), {}));
  REQUIRE(validated);
  auto ungranted = executor->start({make_id<domain::InvocationId>("ungranted"),
                                    std::nullopt,
                                    "run_process",
                                    *validated,
                                    {},
                                    registration->limits},
                                   {});
  REQUIRE_FALSE(ungranted);
  REQUIRE(ungranted.error().code ==
          runtime::ToolExecutionErrorCode::unavailable);

  validated =
      executor->validate(arguments(fixture(), temporary.path(), {}, {}, 1000,
                                   1024, {temporary.path().generic_string()}));
  REQUIRE(validated);
  auto read_only_grants = validated->required_scopes;
  std::erase_if(read_only_grants, [](const auto& capability) {
    return capability.effect == domain::Effect::write;
  });
  auto unwritable = executor->start(
      {make_id<domain::InvocationId>("unwritable"), std::nullopt, "run_process",
       *validated, std::move(read_only_grants), registration->limits},
      {});
  REQUIRE_FALSE(unwritable);
  REQUIRE(unwritable.error().code ==
          runtime::ToolExecutionErrorCode::unavailable);

  auto relative_config = config;
  relative_config.readable_roots = {"relative"};
  REQUIRE_FALSE(adapters::process_tool_declaration(relative_config));

  auto oversized_environment = config;
  oversized_environment.environment_allowlist.front().value =
      std::string(config.limits.argument_bytes, 's');
  REQUIRE_FALSE(adapters::process_tool_declaration(oversized_environment));

  auto combined_environment = config;
  combined_environment.environment_allowlist.front().value =
      std::string(9U * 1024U, 's');
  runtime::ToolRegistry combined_registry;
  REQUIRE(adapters::register_process_tool(combined_registry, artifacts,
                                          combined_environment));
  auto combined_snapshot = combined_registry.snapshot();
  REQUIRE(combined_snapshot);
  const auto* combined = combined_snapshot->find("run_process");
  REQUIRE(combined != nullptr);
  REQUIRE_FALSE(combined->executor->validate(
      arguments(fixture(), temporary.path(), {std::string(7U * 1024U, 'a')},
                {"SAFE_VALUE"})));
}

TEST_CASE("process execution preserves argv and isolates its environment",
          "[process][execution][environment]") {
  TemporaryDirectory temporary;
  RecordingArtifactStore artifacts;
  runtime::ToolRegistry registry;
  REQUIRE(adapters::register_process_tool(
      registry, artifacts, configuration(fixture(), temporary.path())));
  REQUIRE(::setenv("UNLISTED_VALUE", "ambient-secret", 1) == 0);
  const auto result =
      execute(registry, arguments(fixture(), temporary.path(),
                                  {"inspect", "literal;still-one",
                                   "$(not-a-shell)", "line\nbreak"},
                                  {"SAFE_VALUE"}));
  REQUIRE(::unsetenv("UNLISTED_VALUE") == 0);
  REQUIRE(result);
  REQUIRE(result->validated.required_effects ==
          std::vector<domain::Effect>{domain::Effect::execute,
                                      domain::Effect::read,
                                      domain::Effect::network});
  REQUIRE(result->validated.required_scopes.size() == 3);
  REQUIRE(std::ranges::contains(result->validated.required_scopes,
                                domain::CapabilityScope{domain::Effect::network,
                                                        "network.unrestricted",
                                                        "new-sockets"}));
  const auto value = result_json(result->result);
  REQUIRE(value.at("status") == "exited");
  REQUIRE(value.at("exit_code") == 7);
  const auto output = value.at("stdout").at("text").get<std::string>();
  REQUIRE(output.find("literal;still-one") != std::string::npos);
  REQUIRE(output.find("$(not-a-shell)") != std::string::npos);
  REQUIRE(output.find("line\nbreak") != std::string::npos);
  REQUIRE(output.find("safe=****************") != std::string::npos);
  REQUIRE(output.find("secret-value-123") == std::string::npos);
  REQUIRE(output.find("unlisted=<unset>") != std::string::npos);
  REQUIRE(output.find("stdin=eof") != std::string::npos);
  REQUIRE(value.at("stderr").at("text") == "stderr=separate\n");
  REQUIRE(artifacts.calls().empty());

#ifndef _WIN32
  std::array<int, 2> descriptors{-1, -1};
  REQUIRE(::pipe(descriptors.data()) == 0);
  const auto ambient = ::fcntl(descriptors[0], F_DUPFD, 32);
  REQUIRE(ambient >= 32);
  static_cast<void>(::close(descriptors[0]));
  static_cast<void>(::close(descriptors[1]));
  auto isolated =
      execute(registry, arguments(fixture(), temporary.path(),
                                  {"descriptor", std::to_string(ambient)}));
  static_cast<void>(::close(ambient));
  REQUIRE(isolated);
  REQUIRE(result_json(isolated->result).at("stdout").at("text") ==
          "descriptor=closed\n");
#endif
}

TEST_CASE("signals and malformed executables are structured outcomes",
          "[process][result][failure]") {
  TemporaryDirectory temporary;
  RecordingArtifactStore artifacts;
  runtime::ToolRegistry registry;
  REQUIRE(adapters::register_process_tool(
      registry, artifacts, configuration(fixture(), temporary.path())));
  auto signaled =
      execute(registry, arguments(fixture(), temporary.path(), {"signal"}));
  REQUIRE(signaled);
  REQUIRE(result_json(signaled->result).at("status") == "signaled");
  REQUIRE(result_json(signaled->result).at("signal") == SIGTERM);

  const auto malformed = temporary.path() / "malformed-executable";
  {
    std::ofstream output{malformed};
    output << "this is not an executable format\n";
  }
  std::filesystem::permissions(malformed,
                               std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_exec);
  runtime::ToolRegistry malformed_registry;
  REQUIRE(adapters::register_process_tool(
      malformed_registry, artifacts,
      configuration(malformed, temporary.path())));
  auto failed =
      execute(malformed_registry, arguments(malformed, temporary.path(), {}));
  REQUIRE(failed);
  const auto failed_json = result_json(failed->result);
  REQUIRE(failed_json.at("status") == "spawn_failed");
  REQUIRE(failed_json.at("spawn_error") == "invalid_format");
}

TEST_CASE("large and binary output spill to bounded artifacts",
          "[process][artifact][output]") {
  TemporaryDirectory temporary;
  auto process_limits = limits();
  process_limits.inline_output_bytes = 8;
  RecordingArtifactStore artifacts;
  runtime::ToolRegistry registry;
  REQUIRE(adapters::register_process_tool(
      registry, artifacts,
      configuration(fixture(), temporary.path(), process_limits)));

  auto spilled =
      execute(registry, arguments(fixture(), temporary.path(),
                                  {"emit", "100", "x"}, {}, 1000, 1024));
  REQUIRE(spilled);
  const auto value = result_json(spilled->result);
  REQUIRE(value.at("status") == "exited");
  REQUIRE(value.at("stdout").at("bytes") == 100);
  REQUIRE(value.at("stdout").at("text").is_null());
  REQUIRE(spilled->result.created_artifacts.size() == 1);
  REQUIRE(artifacts.calls().size() == 1);
  REQUIRE(artifacts.calls().front().content == std::string(100, 'x'));

  RecordingArtifactStore binary_artifacts;
  runtime::ToolRegistry binary_registry;
  REQUIRE(adapters::register_process_tool(
      binary_registry, binary_artifacts,
      configuration(fixture(), temporary.path(), process_limits)));
  auto binary = execute(binary_registry,
                        arguments(fixture(), temporary.path(), {"binary"}));
  REQUIRE(binary);
  REQUIRE(binary->result.created_artifacts.size() == 1);
  REQUIRE(binary_artifacts.calls().front().content.size() == 4);
  REQUIRE(binary_artifacts.calls().front().content[1] == '\0');

  RecordingArtifactStore secret_artifacts;
  runtime::ToolRegistry secret_registry;
  REQUIRE(adapters::register_process_tool(
      secret_registry, secret_artifacts,
      configuration(fixture(), temporary.path(), process_limits)));
  auto secret =
      execute(secret_registry, arguments(fixture(), temporary.path(),
                                         {"inspect"}, {"SAFE_VALUE"}));
  REQUIRE(secret);
  REQUIRE(secret->progress.empty());
  REQUIRE(secret_artifacts.calls().size() == 2);
  REQUIRE(std::ranges::none_of(secret_artifacts.calls(), [](const auto& call) {
    return call.content.find("secret-value-123") != std::string::npos;
  }));
  REQUIRE(result_json(secret->result).dump().find("secret-value-123") ==
          std::string::npos);
}

TEST_CASE("stdout and stderr are drained independently under backpressure",
          "[process][output][progress][backpressure]") {
  TemporaryDirectory temporary;
  auto process_limits = limits();
  process_limits.inline_output_bytes = 8;
  process_limits.progress_chunk_bytes = 4U * 1024U;
  process_limits.progress_events = 64;
  RecordingArtifactStore artifacts;
  runtime::ToolRegistry registry;
  REQUIRE(adapters::register_process_tool(
      registry, artifacts,
      configuration(fixture(), temporary.path(), process_limits)));
  auto duplex = execute(registry, arguments(fixture(), temporary.path(),
                                            {"duplex", "131072"}, {}, 1000,
                                            256U * 1024U));
  REQUIRE(duplex);
  const auto value = result_json(duplex->result);
  REQUIRE(value.at("status") == "exited");
  REQUIRE(value.at("stdout").at("bytes") == 131072);
  REQUIRE(value.at("stderr").at("bytes") == 131072);
  REQUIRE(artifacts.calls().size() == 2);
  REQUIRE(artifacts.calls()[0].content == std::string(131072, 'o'));
  REQUIRE(artifacts.calls()[1].content == std::string(131072, 'e'));
  std::set<std::string> progress_streams;
  for (const auto& progress : duplex->progress) {
    const auto* block =
        std::get_if<domain::StructuredDataBlock>(&progress.content.front());
    REQUIRE(block != nullptr);
    progress_streams.insert(Json::parse(block->data).at("stream"));
  }
  REQUIRE(progress_streams == std::set<std::string>{"stderr", "stdout"});
}

TEST_CASE("output limits and timeouts terminate the process group",
          "[process][limits][cancellation][failure]") {
  TemporaryDirectory temporary;
  auto process_limits = limits();
  process_limits.inline_output_bytes = 8;
  RecordingArtifactStore artifacts;
  runtime::ToolRegistry registry;
  REQUIRE(adapters::register_process_tool(
      registry, artifacts,
      configuration(fixture(), temporary.path(), process_limits)));
  auto limited =
      execute(registry, arguments(fixture(), temporary.path(),
                                  {"emit", "100000", "z"}, {}, 1000, 50));
  REQUIRE(limited);
  REQUIRE(result_json(limited->result).at("status") == "output_limit");
  REQUIRE(artifacts.calls().front().content.size() == 50);

  RecordingArtifactStore timeout_artifacts;
  runtime::ToolRegistry timeout_registry;
  REQUIRE(adapters::register_process_tool(
      timeout_registry, timeout_artifacts,
      configuration(fixture(), temporary.path(), process_limits)));
  const auto marker = temporary.path() / "descendant.marker";
  auto timed_out =
      execute(timeout_registry,
              arguments(fixture(), temporary.path(), {"hang", marker.string()},
                        {}, 50, 1024, {temporary.path().generic_string()}));
  REQUIRE(timed_out);
  REQUIRE(result_json(timed_out->result).at("status") == "timed_out");
  REQUIRE(std::filesystem::exists(marker));
  const auto marker_size = std::filesystem::file_size(marker);
  std::this_thread::sleep_for(80ms);
  REQUIRE(std::filesystem::file_size(marker) == marker_size);
}

TEST_CASE("cancellation cleans up and artifact failures stay redacted",
          "[process][cancellation][artifact][failure]") {
  TemporaryDirectory temporary;
  auto process_limits = limits();
  process_limits.inline_output_bytes = 8;
  RecordingArtifactStore artifacts;
  runtime::ToolRegistry registry;
  REQUIRE(adapters::register_process_tool(
      registry, artifacts,
      configuration(fixture(), temporary.path(), process_limits)));
  std::stop_source cancelled_before_start;
  cancelled_before_start.request_stop();
  auto pre_cancelled =
      execute(registry, arguments(fixture(), temporary.path(), {"hang"}),
              cancelled_before_start.get_token());
  REQUIRE_FALSE(pre_cancelled);
  REQUIRE(pre_cancelled.error().code ==
          runtime::ToolExecutionErrorCode::cancelled);

  std::stop_source cancellation;
  std::jthread cancel_after{[&](std::stop_token) {
    std::this_thread::sleep_for(40ms);
    cancellation.request_stop();
  }};
  auto cancelled =
      execute(registry,
              arguments(fixture(), temporary.path(), {"hang"}, {}, 1000, 1024),
              cancellation.get_token());
  REQUIRE_FALSE(cancelled);
  REQUIRE(cancelled.error().code == runtime::ToolExecutionErrorCode::cancelled);

  RecordingArtifactStore failing_artifacts{
      storage::ArtifactStoreError{storage::ArtifactStoreErrorCode::io_failure,
                                  "secret-bearing filesystem detail", true}};
  runtime::ToolRegistry failing_registry;
  REQUIRE(adapters::register_process_tool(
      failing_registry, failing_artifacts,
      configuration(fixture(), temporary.path(), process_limits)));
  auto failed = execute(failing_registry, arguments(fixture(), temporary.path(),
                                                    {"emit", "100", "x"}));
  REQUIRE_FALSE(failed);
  REQUIRE(failed.error().retryable);
  REQUIRE(failed.error().message.find("secret-bearing") == std::string::npos);
}

TEST_CASE("a replaced executable fails at the effect boundary",
          "[process][toctou][failure]") {
  TemporaryDirectory temporary;
  const auto executable = temporary.path() / "pinned-executable";
  REQUIRE(std::filesystem::copy_file(fixture(), executable));
  std::filesystem::permissions(executable, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace);
  RecordingArtifactStore artifacts;
  runtime::ToolRegistry registry;
  REQUIRE(adapters::register_process_tool(
      registry, artifacts, configuration(executable, temporary.path())));

  const auto old = temporary.path() / "old-executable";
  std::filesystem::rename(executable, old);
  REQUIRE(std::filesystem::copy_file(fixture(), executable));
  std::filesystem::permissions(executable, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace);
  auto replaced =
      execute(registry, arguments(executable, temporary.path(), {"inspect"}));
  REQUIRE_FALSE(replaced);
  REQUIRE(replaced.error().code ==
          runtime::ToolExecutionErrorCode::unavailable);

  const auto declared_root = temporary.path() / "declared-root";
  REQUIRE(std::filesystem::create_directory(declared_root));
  runtime::ToolRegistry root_registry;
  REQUIRE(adapters::register_process_tool(
      root_registry, artifacts, configuration(executable, declared_root)));
  std::filesystem::rename(declared_root, temporary.path() / "old-root");
  REQUIRE(std::filesystem::create_directory(declared_root));
  auto replaced_root =
      execute(root_registry, arguments(executable, declared_root, {"inspect"}));
  REQUIRE_FALSE(replaced_root);
  REQUIRE(replaced_root.error().code ==
          runtime::ToolExecutionErrorCode::unavailable);
}

TEST_CASE("the artifact store fake is strict and deterministic",
          "[process][artifact][fake]") {
  const auto invocation = make_id<domain::InvocationId>("artifact-call");
  const storage::ArtifactWrite write{make_id<domain::ArtifactId>("artifact"),
                                     "application/octet-stream", invocation};
  const testing::ArtifactStoreCall expected{write,
                                            {std::byte{0x01}, std::byte{0x02}}};
  const domain::ArtifactMetadata metadata{
      write.artifact_id, write.media_type, 2,           "digest",
      invocation,        std::nullopt,     std::nullopt};
  testing::ScriptedArtifactStore artifacts{{{expected, metadata}}};
  const std::array content{std::byte{0x01}, std::byte{0x02}};
  auto stored = artifacts.put(write, content);
  REQUIRE(stored == metadata);
  REQUIRE(artifacts.recorded_calls() ==
          std::vector<testing::ArtifactStoreCall>{expected});
  REQUIRE(artifacts.remaining_exchanges() == 0);

  auto exhausted = artifacts.put(write, content);
  REQUIRE_FALSE(exhausted);
  REQUIRE(exhausted.error().code ==
          storage::ArtifactStoreErrorCode::unavailable);
}
