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

TEST_CASE("configured matcher rejects missing authority and overbound values",
          "[automatic-approval][configuration][failure]") {
  config::AutomaticApprovalRulesConfig repository{{
      config::RepositoryPathAutomaticApprovalRuleConfig{
          "read_repository_file", "src", constraints(1, 0)},
  }};
  REQUIRE_FALSE(
      runtime::compile_configured_automatic_approval_matcher(repository));

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
