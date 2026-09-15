#include <aiforge/adapters/configured_admin_sources.hpp>
#include <aiforge/adapters/linux_ops_observation_source.hpp>
#include <aiforge/config/file_store.hpp>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {
unsigned source_creations{};
std::optional<aiforge::runtime::OpsSourcePreparationIdentity> source_identity;
} // namespace
// Strong no-IO fixture: any accidental source construction is counted, while
// the archived native procfs implementation is not linked into this executable.
namespace aiforge::adapters {
auto LinuxOpsObservationSource::create(
    domain::OpsTargetId target, domain::OpsConfigurationRevision revision)
    -> std::expected<std::shared_ptr<LinuxOpsObservationSource>,
                     runtime::OpsObservationSourceError> {
  ++source_creations;
  source_identity.emplace(runtime::OpsSourcePreparationIdentity{
      std::move(target), std::move(revision),
      domain::OpsTargetKind::linux_local});
  return std::unexpected(runtime::OpsObservationSourceError::unavailable);
}
} // namespace aiforge::adapters

namespace {
using namespace aiforge;
using SourceError = runtime::OpsObservationSourceError;
using Code = surfaces::ManualOpsErrorCode;
template <class T> auto id(std::string value) -> T {
  auto parsed = T::from(std::move(value));
  REQUIRE(parsed);
  return std::move(*parsed);
}
auto sample() -> config::OpsTargetsConfig {
  return {
      {{"desktop", "Configured desktop", config::LinuxLocalTargetConfig{}},
       {"cluster", "Private cluster",
        config::StaticKubernetesTargetConfig{"/nonexistent/private-kubeconfig",
                                             "chosen-context", "chosen"}}}};
}
auto resolve(std::optional<config::OpsTargetsConfig> records = {})
    -> config::ResolvedConfig {
  config::ConfigLayer layer{config::ConfigSource::file, {}, {}};
  if (records)
    layer.candidates.push_back({std::string{config::ops_targets_key},
                                config::ConfigValue{std::move(*records)},
                                {}});
  const std::array layers{std::move(layer)};
  auto resolved =
      config::resolve_config(config::builtin_config_registry(), layers);
  REQUIRE(resolved);
  return std::move(*resolved);
}
class TemporaryConfig final {
 public:
  TemporaryConfig() {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "aiforge-admin-catalog-XXXXXX")
                       .string();
    std::vector<char> bytes(pattern.begin(), pattern.end());
    bytes.push_back('\0');
    const auto* made = ::mkdtemp(bytes.data());
    REQUIRE(made != nullptr);
    root = made;
    REQUIRE(std::filesystem::create_directory(root / "aiforge"));
    std::filesystem::permissions(root / "aiforge",
                                 std::filesystem::perms::owner_all);
  }
  ~TemporaryConfig() {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }
  auto path() const -> std::filesystem::path {
    return root / "aiforge" / "config.json";
  }
  auto write(std::string_view document) const -> void {
    std::ofstream out{path()};
    out << document;
    out.close();
    REQUIRE(out);
    std::filesystem::permissions(path(),
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write);
  }
  std::filesystem::path root;
};
class ConfigEnvironment final {
 public:
  explicit ConfigEnvironment(const std::filesystem::path& path) {
    if (const auto* current = std::getenv("XDG_CONFIG_HOME"))
      m_original = current;
    REQUIRE(::setenv("XDG_CONFIG_HOME", path.c_str(), 1) == 0);
  }
  ~ConfigEnvironment() {
    if (m_original)
      static_cast<void>(::setenv("XDG_CONFIG_HOME", m_original->c_str(), 1));
    else
      static_cast<void>(::unsetenv("XDG_CONFIG_HOME"));
  }

 private:
  std::optional<std::string> m_original;
};
} // namespace

TEST_CASE(
    "Configured Admin rejects invalid ops instead of falling back local") {
  source_creations = 0;
  auto records = sample();
  SECTION("duplicate") {
    records.targets[1].id = records.targets[0].id;
  }
  SECTION("reserved local") {
    records.targets[0].id = "local";
  }
  SECTION("unsafe label") {
    records.targets[0].display_name = "private\033text";
  }
  SECTION("unsafe target") {
    records.targets[0].id = "../desktop";
  }
  SECTION("too many entries") {
    for (unsigned i = 2; i < 33; ++i)
      records.targets.push_back({"target-" + std::to_string(i), "Target",
                                 config::LinuxLocalTargetConfig{}});
  }
  const auto result = adapters::make_configured_admin_sources(resolve(records));
  REQUIRE_FALSE(result);
  CHECK(result.error().code == Code::invalid_input);
  CHECK(source_creations == 0);
}

TEST_CASE(
    "Rejected ops diagnostics cannot be hidden by an otherwise empty config") {
  auto resolved = resolve();
  resolved.diagnostics.push_back({config::ConfigDiagnosticCode::invalid_value,
                                  config::ConfigSource::file, "ops.targets",
                                  "private fixture diagnostic"});
  const auto result = adapters::make_configured_admin_sources(resolved);
  REQUIRE_FALSE(result);
  CHECK(result.error() == surfaces::ManualOpsFailure{Code::invalid_input});
}

TEST_CASE(
    "Configured Admin owns metadata and returns only exact native factories") {
  source_creations = 0;
  auto resolved = resolve(sample());
  auto catalog = adapters::make_configured_admin_sources(resolved);
  REQUIRE(catalog);
  resolved = {};
  REQUIRE((*catalog)->guarantees_owned_metadata());
  const auto choices = (*catalog)->targets();
  REQUIRE(choices.size() == 3);
  CHECK(choices[0].id.value() == "local");
  CHECK(choices[1].display_name == "Configured desktop");
  CHECK(choices[2].kind == domain::OpsTargetKind::kubernetes);
  const auto revision =
      id<domain::OpsConfigurationRevision>("reserved-revision");
  auto missing =
      (*catalog)->factory(id<domain::OpsTargetId>("missing"), revision);
  REQUIRE_FALSE(missing);
  CHECK(missing.error() == SourceError::invalid_result);
  auto unsupported =
      (*catalog)->factory(id<domain::OpsTargetId>("cluster"), revision);
  REQUIRE_FALSE(unsupported);
  CHECK(unsupported.error() == SourceError::unsupported);
  const auto invalid_revision = id<domain::OpsConfigurationRevision>(
      std::string(1, static_cast<char>(0xFF)));
  CHECK_FALSE((*catalog)->factory(id<domain::OpsTargetId>("desktop"),
                                  invalid_revision));
  auto native =
      (*catalog)->factory(id<domain::OpsTargetId>("desktop"), revision);
  REQUIRE(native);
  CHECK((*native)->guarantees_owned_read_only_preparation());
  CHECK((*native)->preparation_identity() ==
        runtime::OpsSourcePreparationIdentity{
            id<domain::OpsTargetId>("desktop"), revision,
            domain::OpsTargetKind::linux_local});
  CHECK(source_creations == 0);
  catalog = {};
  runtime::OpsSourcePreparationRequest request{
      {id<domain::SessionId>("session"), 1, 1,
       (*native)->preparation_identity()},
      std::chrono::steady_clock::now() + std::chrono::seconds{5}};
  auto prepared = (*native)->prepare(request);
  REQUIRE_FALSE(prepared);
  CHECK(prepared.error() == SourceError::unavailable);
  CHECK(source_creations == 1);
  REQUIRE(source_identity);
  CHECK(*source_identity == request.token.selection);
}

TEST_CASE(
    "Strict Admin process load refuses unusable files and invalid records") {
  TemporaryConfig temporary;
  ConfigEnvironment environment{temporary.root};
  source_creations = 0;
  SECTION("malformed document") {
    temporary.write("{private invalid");
  }
  SECTION("duplicate key") {
    temporary.write(R"({"ops":{},"ops":{}})");
  }
  SECTION("inline credentials") {
    temporary.write(
        R"({"ops":{"targets":[{"id":"cluster","display_name":"Cluster","kind":"kubernetes","token":"private-token"}]}})");
  }
  SECTION("wrong ops namespace") {
    temporary.write(R"({"ops":false})");
  }
  SECTION("nonfile") {
    REQUIRE(std::filesystem::create_directory(temporary.path()));
  }
  SECTION("symlink") {
    const auto other = temporary.root / "other.json";
    {
      std::ofstream out{other};
      out << "{}";
    }
    std::filesystem::create_symlink(other, temporary.path());
  }
  auto loaded = adapters::load_process_admin_sources();
  REQUIRE_FALSE(loaded);
  CHECK((loaded.error().code == Code::unavailable ||
         loaded.error().code == Code::invalid_input));
  CHECK(source_creations == 0);
}

TEST_CASE("Successfully absent Admin configuration yields local metadata "
          "without IO") {
  TemporaryConfig temporary;
  ConfigEnvironment environment{temporary.root};
  source_creations = 0;
  SECTION("missing file") {
  }
  SECTION("empty document") {
    temporary.write("{}");
  }
  SECTION("empty ops object") {
    temporary.write(R"({"ops":{}})");
  }
  auto loaded = adapters::load_process_admin_sources();
  REQUIRE(loaded);
  REQUIRE((*loaded)->targets().size() == 1);
  CHECK((*loaded)->targets()[0].id.value() == "local");
  CHECK((*loaded)->targets()[0].display_name == "This environment");
  CHECK(source_creations == 0);
}

TEST_CASE(
    "Strict Admin load retains configured kinds without reading references") {
  TemporaryConfig temporary;
  ConfigEnvironment environment{temporary.root};
  config::JsonConfigFileStore file{temporary.path()};
  REQUIRE(file.set(config::builtin_config_registry(), config::ops_targets_key,
                   config::ConfigValue{sample()}));
  source_creations = 0;
  auto loaded = adapters::load_process_admin_sources();
  REQUIRE(loaded);
  REQUIRE((*loaded)->targets().size() == 3);
  CHECK((*loaded)->targets()[2].id.value() == "cluster");
  CHECK((*loaded)->targets()[2].display_name == "Private cluster");
  auto unsupported =
      (*loaded)->factory(id<domain::OpsTargetId>("cluster"),
                         id<domain::OpsConfigurationRevision>("next"));
  REQUIRE_FALSE(unsupported);
  CHECK(unsupported.error() == SourceError::unsupported);
  CHECK(source_creations == 0);
}
