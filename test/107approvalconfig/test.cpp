#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

#include <aiforge/adapters/pinned_repository_root_authority.hpp>
#include <aiforge/config/config.hpp>
#include <aiforge/runtime/automatic_approval_matcher.hpp>
#include <aiforge/runtime/repository_read_tool.hpp>
#include <aiforge/testing/scripted_exact_source_editor.hpp>
#include <aiforge/testing/scripted_repository_snapshot_source.hpp>

namespace {

using namespace aiforge;

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    auto pattern =
        (std::filesystem::temp_directory_path() / "aiforge-approval-XXXXXX")
            .string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    const auto* created = ::mkdtemp(writable.data());
    REQUIRE(created != nullptr);
    m_path = created;
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  auto operator=(const TemporaryDirectory&) -> TemporaryDirectory& = delete;

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(m_path, ignored);
  }

  [[nodiscard]] auto path() const -> const std::filesystem::path& {
    return m_path;
  }

 private:
  std::filesystem::path m_path;
};

auto write_file(const std::filesystem::path& path,
                const std::string_view content = "content") -> void {
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  REQUIRE(output);
  output.write(content.data(), static_cast<std::streamsize>(content.size()));
  REQUIRE(output);
}

template <typename Id> auto id(const std::string_view value) -> Id {
  return Id::from(std::string{value}).value();
}

auto request(std::string invocation, std::string tool_name,
             const std::string_view arguments)
    -> runtime::AutomaticApprovalMatchRequest {
  auto canonical = runtime::canonicalize_validated_tool_arguments(
      {"application/json", std::string{arguments}});
  REQUIRE(canonical);
  return {id<domain::SessionId>("session"),
          id<domain::RunId>("run"),
          id<domain::InvocationId>(invocation),
          std::move(tool_name),
          std::move(*canonical),
          runtime::RestrictionLevel::high,
          {},
          {}};
}

auto constraints(const std::uint64_t matches, const std::uint32_t precedence)
    -> config::AutomaticApprovalRuleConstraintsConfig {
  return {{"high"}, matches, std::nullopt, precedence};
}

auto digest(std::string value, const std::uint64_t bytes)
    -> domain::ContentDigest {
  return {"sha256", std::move(value), bytes};
}

auto snapshot(const std::string& root) -> domain::RepositorySnapshot {
  return {{id<domain::RepositoryId>("repository-1"), root},
          domain::VcsState{"git", "sha256", domain::VcsHeadKind::branch, "main",
                           "bbbbbbbbbbbbbbbb"},
          {},
          digest("aaaaaaaaaaaaaaaa", 64),
          std::chrono::sys_time<std::chrono::milliseconds>{
              std::chrono::milliseconds{100}}};
}

} // namespace

TEST_CASE("pinned repository authority rejects symlinks and root replacement",
          "[automatic-approval][repository][failure]") {
  TemporaryDirectory temporary;
  const auto repository = temporary.path() / "repository";
  REQUIRE(std::filesystem::create_directories(repository / "src"));
  write_file(repository / "src" / "file.cpp");
  std::filesystem::create_directory_symlink(repository / "src",
                                            repository / "src" / "linked");
  REQUIRE(std::filesystem::is_symlink(repository / "src" / "linked"));

  const auto authority =
      adapters::open_pinned_repository_root_authority(repository);
  REQUIRE(authority);
  REQUIRE((*authority)->identity().starts_with("sha256:"));
  REQUIRE((*authority)->contains("src", "src/file.cpp").value());
  REQUIRE_FALSE((*authority)->contains("docs", "src/file.cpp").value());
  REQUIRE_FALSE((*authority)->contains("src", "src/linked/file.cpp"));
  REQUIRE_FALSE((*authority)->contains("src", "src/missing.cpp"));
  REQUIRE_FALSE((*authority)->contains("src", "../src/file.cpp"));

  const auto symlinked_root = temporary.path() / "repository-link";
  std::filesystem::create_directory_symlink(repository, symlinked_root);
  REQUIRE(std::filesystem::is_symlink(symlinked_root));
  REQUIRE_FALSE(
      adapters::open_pinned_repository_root_authority(symlinked_root));
  REQUIRE_FALSE(adapters::open_pinned_repository_root_authority(
      std::filesystem::path{"/" + std::string(4097U, 'x')}));

  const auto original = temporary.path() / "original-repository";
  std::filesystem::rename(repository, original);
  REQUIRE(std::filesystem::is_directory(original));
  REQUIRE(std::filesystem::create_directories(repository / "src"));
  write_file(repository / "src" / "file.cpp", "replacement");
  REQUIRE_FALSE((*authority)->contains("src", "src/file.cpp"));
}

TEST_CASE("production configuration compiles nonempty exact and path rules",
          "[automatic-approval][configuration]") {
  TemporaryDirectory temporary;
  const auto repository = temporary.path() / "repository";
  REQUIRE(std::filesystem::create_directories(repository / "src"));
  write_file(repository / "src" / "file.cpp");
  const auto authority =
      adapters::open_pinned_repository_root_authority(repository);
  REQUIRE(authority);

  config::AutomaticApprovalRulesConfig configured{{
      config::ExactAutomaticApprovalRuleConfig{"lookup", R"({"id":1})",
                                               constraints(1, 2)},
      config::RepositoryPathAutomaticApprovalRuleConfig{
          "read_repository_file", "src", constraints(2, 1)},
  }};
  const auto matcher = runtime::compile_configured_automatic_approval_matcher(
      configured, *authority);
  REQUIRE(matcher);
  REQUIRE(std::ranges::equal(
      (*matcher)->tool_names(),
      std::vector<std::string>{"lookup", "read_repository_file"}));
  REQUIRE((*matcher)->match(request("exact", "lookup", R"({"id":1})")).value());
  REQUIRE_FALSE((*matcher)
                    ->match(request("exact-miss", "lookup", R"({"id":2})"))
                    .value());
  REQUIRE((*matcher)
              ->match(request("path", "read_repository_file",
                              R"({"relative_path":"src/file.cpp"})"))
              .value());

  const auto empty = runtime::compile_configured_automatic_approval_matcher({});
  REQUIRE(empty);
  REQUIRE((*empty)->tool_names().empty());
  REQUIRE_FALSE(
      (*empty)->match(request("deny", "lookup", R"({"id":1})")).value());
}

TEST_CASE("automatic repository effect never reopens a replaced root",
          "[automatic-approval][repository][effect][failure]") {
  TemporaryDirectory temporary;
  const auto root = temporary.path() / "repository";
  REQUIRE(std::filesystem::create_directories(root / "src"));
  write_file(root / "src" / "main.cpp", "approved bytes");

  auto authority = adapters::open_pinned_repository_root_authority(root);
  REQUIRE(authority);
  auto rule = runtime::make_repository_read_approval_rule(
      *authority, "src", {{runtime::RestrictionLevel::high}, 1, {}, 0});
  REQUIRE(rule);
  auto matcher = runtime::compile_automatic_approval_matcher({*rule});
  REQUIRE(matcher);
  auto decision =
      (*matcher)->match(request("invocation", "read_repository_file",
                                R"({"relative_path":"src/main.cpp"})"));
  REQUIRE(decision);
  REQUIRE(decision->has_value());

  testing::ScriptedRepositorySnapshotSource snapshots;
  testing::ScriptedExactSourceEditor sources;
  sources.couple_to(snapshots, true);
  runtime::ToolRegistry registry;
  runtime::RepositoryReadToolConfiguration configuration{root.generic_string()};
  REQUIRE(runtime::register_repository_read_tool(
      registry, snapshots, sources, configuration, *authority,
      snapshot(root.generic_string())));
  auto tools = registry.snapshot();
  REQUIRE(tools);
  const auto* tool = tools->find("read_repository_file");
  REQUIRE(tool != nullptr);
  auto validated = tool->executor->validate(
      {"application/json", R"({"relative_path":"src/main.cpp"})"});
  REQUIRE(validated);

  const auto original = temporary.path() / "original";
  std::filesystem::rename(root, original);
  REQUIRE(std::filesystem::create_directories(root / "src"));
  write_file(root / "src" / "main.cpp", "replacement bytes");

  auto started = tool->executor->start({id<domain::InvocationId>("invocation"),
                                        std::nullopt,
                                        "read_repository_file",
                                        std::move(*validated),
                                        {},
                                        tool->limits},
                                       {});
  REQUIRE_FALSE(started);
  CHECK(started.error().code == runtime::ToolExecutionErrorCode::unavailable);
  CHECK(snapshots.recorded_requests().empty());
  CHECK(sources.recorded_read_requests().empty());
}

TEST_CASE("automatic repository effect reads through the pinned descriptor",
          "[automatic-approval][repository][effect]") {
  TemporaryDirectory temporary;
  const auto root = temporary.path() / "repository";
  REQUIRE(std::filesystem::create_directories(root / "src"));
  write_file(root / "src" / "main.cpp", "descriptor bytes");

  auto authority = adapters::open_pinned_repository_root_authority(root);
  REQUIRE(authority);
  testing::ScriptedRepositorySnapshotSource snapshots;
  testing::ScriptedExactSourceEditor sources;
  sources.couple_to(snapshots, true);
  runtime::ToolRegistry registry;
  runtime::RepositoryReadToolConfiguration configuration{root.generic_string()};
  REQUIRE(runtime::register_repository_read_tool(
      registry, snapshots, sources, configuration, *authority,
      snapshot(root.generic_string())));
  auto tools = registry.snapshot();
  REQUIRE(tools);
  const auto* tool = tools->find("read_repository_file");
  REQUIRE(tool != nullptr);
  auto validated = tool->executor->validate(
      {"application/json", R"({"relative_path":"src/main.cpp"})"});
  REQUIRE(validated);

  auto started = tool->executor->start({id<domain::InvocationId>("invocation"),
                                        std::nullopt,
                                        "read_repository_file",
                                        std::move(*validated),
                                        {},
                                        tool->limits},
                                       {});
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
  CHECK(structured->data.find("descriptor bytes") != std::string::npos);
  CHECK(snapshots.recorded_requests().empty());
  CHECK(sources.recorded_read_requests().empty());
}

TEST_CASE("configured matcher rejects missing authority and overbound values",
          "[automatic-approval][configuration][failure]") {
  config::AutomaticApprovalRulesConfig repository{{
      config::RepositoryPathAutomaticApprovalRuleConfig{
          "read_repository_file", "src", constraints(1, 0)},
  }};
  REQUIRE_FALSE(
      runtime::compile_configured_automatic_approval_matcher(repository));

  config::AutomaticApprovalRulesConfig exact_repository_read{{
      config::ExactAutomaticApprovalRuleConfig{
          "read_repository_file", R"({"relative_path":"src/main.cpp"})",
          constraints(1, 0)},
  }};
  REQUIRE_FALSE(runtime::compile_configured_automatic_approval_matcher(
      exact_repository_read));

  auto expiry = constraints(1, 0);
  expiry.expires_after_milliseconds =
      static_cast<std::uint64_t>(
          runtime::AutomaticApprovalMatcherLimits{}.maximum_expiry.count()) +
      1U;
  config::AutomaticApprovalRulesConfig overbound{{
      config::ExactAutomaticApprovalRuleConfig{"lookup", "{}", expiry},
  }};
  REQUIRE_FALSE(
      runtime::compile_configured_automatic_approval_matcher(overbound));

  auto too_many = constraints(
      runtime::AutomaticApprovalMatcherLimits{}.maximum_total_matches + 1U, 0);
  overbound = {
      {config::ExactAutomaticApprovalRuleConfig{"lookup", "{}", too_many}}};
  REQUIRE_FALSE(
      runtime::compile_configured_automatic_approval_matcher(overbound));
}
