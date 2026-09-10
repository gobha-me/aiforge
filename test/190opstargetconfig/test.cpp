#include <aiforge/config/config.hpp>
#include <aiforge/config/file_store.hpp>
#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sys/stat.h>
#include <vector>

namespace {
using namespace aiforge::config;
auto spec() -> ConfigKeySpec {
  const auto& keys = builtin_config_registry().keys;
  auto found = std::ranges::find(keys, ops_targets_key, &ConfigKeySpec::id);
  REQUIRE(found != keys.end());
  return *found;
}
auto sample() -> OpsTargetsConfig {
  return {{{"desktop", "Desktop", LinuxLocalTargetConfig{}},
           {"cluster1", "Cluster one",
            StaticKubernetesTargetConfig{"/not-opened/kubeconfig",
                                         "explicit-context", "default"}}}};
}
class Fixture final {
 public:
  Fixture() {
    auto pattern =
        (std::filesystem::temp_directory_path() / "aiforge-ops-config-XXXXXX")
            .string();
    std::vector<char> bytes(pattern.begin(), pattern.end());
    bytes.push_back('\0');
    const auto* made = ::mkdtemp(bytes.data());
    REQUIRE(made != nullptr);
    root = made;
  }
  ~Fixture() {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }
  auto path() const -> std::filesystem::path { return root / "config.json"; }
  auto write(std::string_view document) const -> void {
    std::ofstream out(path());
    out << document;
    out.close();
    REQUIRE(out);
    REQUIRE(::chmod(path().c_str(), 0600) == 0);
  }
  auto resolve() const -> std::expected<OpsTargetsConfig, ConfigDiagnostic> {
    auto layer = JsonConfigFileStore{path()}.load(builtin_config_registry());
    REQUIRE(layer);
    const std::array layers{*layer};
    auto resolved = resolve_config(builtin_config_registry(), layers);
    REQUIRE(resolved);
    return resolve_ops_targets(*resolved);
  }

 private:
  std::filesystem::path root;
};
} // namespace

TEST_CASE("Ops target metadata refuses invalid authority and fields") {
  for (auto source : {ConfigSource::command_line, ConfigSource::environment,
                      ConfigSource::compiled_default})
    CHECK_FALSE(validate_config_value(spec(), ConfigValue{sample()}, source));
  const std::vector<std::function<void(OpsTargetsConfig&)>> changes{
      [](auto& c) { c.targets[0].id = "local"; },
      [](auto& c) { c.targets[0].id = ""; },
      [](auto& c) { c.targets[0].id = "bad/id"; },
      [](auto& c) { c.targets[0].id = "bad-"; },
      [](auto& c) { c.targets[0].id = "bad_"; },
      [](auto& c) { c.targets[0].id = std::string(65, 'a'); },
      [](auto& c) { c.targets[1].id = c.targets[0].id; },
      [](auto& c) { c.targets[0].display_name = ""; },
      [](auto& c) { c.targets[0].display_name = std::string(129, 'a'); },
      [](auto& c) { c.targets[0].display_name = "label\x1b[31m"; },
      [](auto& c) {
        std::get<StaticKubernetesTargetConfig>(c.targets[1].source)
            .config_file = "relative";
      },
      [](auto& c) {
        std::get<StaticKubernetesTargetConfig>(c.targets[1].source)
            .config_file = "/" + std::string(4096, 'a');
      },
      [](auto& c) {
        std::get<StaticKubernetesTargetConfig>(c.targets[1].source)
            .context.clear();
      },
      [](auto& c) {
        std::get<StaticKubernetesTargetConfig>(c.targets[1].source).context =
            std::string(257, 'a');
      },
      [](auto& c) {
        std::get<StaticKubernetesTargetConfig>(c.targets[1].source).name_space =
            "Upper";
      },
      [](auto& c) {
        std::get<StaticKubernetesTargetConfig>(c.targets[1].source).name_space =
            "ends-";
      },
      [](auto& c) {
        std::get<StaticKubernetesTargetConfig>(c.targets[1].source).name_space =
            std::string(64, 'a');
      },
  };
  for (const auto& change : changes) {
    auto configured = sample();
    change(configured);
    CHECK_FALSE(validate_ops_targets(configured, spec(), ConfigSource::file));
  }
  ResolvedConfig wrong{{{std::string{ops_targets_key},
                         ConfigValue{true},
                         ConfigSource::file,
                         false,
                         {}}},
                       {}};
  CHECK_FALSE(resolve_ops_targets(wrong));
  wrong = {{},
           {{ConfigDiagnosticCode::unknown_key, ConfigSource::file, "ops.typo",
             "shape failure"}}};
  CHECK_FALSE(resolve_ops_targets(wrong));
}

TEST_CASE("Ops metadata limits are exact and aggregate") {
  OpsTargetsConfig configured;
  for (int i = 0; i < 32; ++i)
    configured.targets.push_back(
        {"target-" + std::to_string(i), "Local", LinuxLocalTargetConfig{}});
  REQUIRE(validate_ops_targets(configured, spec(), ConfigSource::file));
  configured.targets.push_back({"extra", "Local", LinuxLocalTargetConfig{}});
  CHECK_FALSE(validate_ops_targets(configured, spec(), ConfigSource::file));
  auto bounded = spec();
  bounded.maximum_text_bytes = 7;
  configured = {{{"abc", "name", LinuxLocalTargetConfig{}}}};
  CHECK(validate_ops_targets(configured, bounded, ConfigSource::file));
  --bounded.maximum_text_bytes;
  CHECK_FALSE(validate_ops_targets(configured, bounded, ConfigSource::file));
  configured.targets.clear();
  for (int i = 0; i < 32; ++i)
    configured.targets.push_back(
        {"target-" + std::to_string(i), "Kube",
         StaticKubernetesTargetConfig{"/" + std::string(4095, 'a'), "context",
                                      "namespace"}});
  CHECK_FALSE(validate_ops_targets(configured, spec(), ConfigSource::file));
  configured = {{{std::string(64, 'a'), std::string(128, 'b'),
                  StaticKubernetesTargetConfig{"/" + std::string(4095, 'c'),
                                               std::string(256, 'd'),
                                               std::string(63, 'e')}}}};
  CHECK(validate_ops_targets(configured, spec(), ConfigSource::file));
}

TEST_CASE("Ops resolution refuses rejected higher precedence and duplicate "
          "candidates") {
  const ConfigLayer file{
      ConfigSource::file,
      {{std::string{ops_targets_key}, ConfigValue{sample()}, {}}},
      {}};
  const ConfigLayer environment{
      ConfigSource::environment,
      {{std::string{ops_targets_key}, ConfigValue{sample()}, {}}},
      {}};
  auto layers = std::array{file, environment};
  auto resolved = resolve_config(builtin_config_registry(), layers);
  REQUIRE(resolved);
  REQUIRE(resolved->find(ops_targets_key)->value);
  CHECK_FALSE(resolve_ops_targets(*resolved));
  auto duplicate = file;
  duplicate.candidates.push_back(duplicate.candidates.front());
  const std::array duplicates{duplicate};
  resolved = resolve_config(builtin_config_registry(), duplicates);
  REQUIRE(resolved);
  CHECK_FALSE(resolve_ops_targets(*resolved));
}

TEST_CASE("Ops catalog hard aggregate boundary is inclusive") {
  OpsTargetsConfig configured;
  std::size_t total{};
  for (int i = 0; i < 16; ++i) {
    OpsTargetConfig record{
        "target-" + std::to_string(i), "Kube",
        StaticKubernetesTargetConfig{"/" + std::string(4095, 'a'), "c", "n"}};
    total += record.id.size() + record.display_name.size() + 4096 + 2;
    configured.targets.push_back(std::move(record));
  }
  REQUIRE(total > 65536);
  auto& path =
      std::get<StaticKubernetesTargetConfig>(configured.targets.back().source)
          .config_file;
  path.resize(path.size() - (total - 65536));
  REQUIRE(validate_ops_targets(configured, spec(), ConfigSource::file));
  path.push_back('a');
  CHECK_FALSE(validate_ops_targets(configured, spec(), ConfigSource::file));
}

TEST_CASE("Ops JSON configuration rejects ambiguity without reading referenced "
          "files") {
  Fixture fixture;
  for (
      auto bad :
      {R"({"ops":{"targets":{}}})",
       R"({"ops":{"targets":[{"id":"x","display_name":"X","source":{"kind":"ssh"}}]}})",
       R"({"ops":{"targets":[{"id":"x","display_name":"X","source":{"kind":"linux_local","token":"SENTINEL"}}]}})",
       R"({"ops":{"targets":[{"id":"x","display_name":"X","source":{"kind":"kubernetes_static","config_file":"/not-opened","context":"c"}}]}})",
       R"({"ops":{"targets":[{"id":"local","display_name":"X","source":{"kind":"linux_local"}}]}})",
       R"({"ops":{"typo":true}})", R"({"ops":null})", R"({"ops":true})",
       R"({"ops.targets":[]})"}) {
    fixture.write(bad);
    auto result = fixture.resolve();
    REQUIRE_FALSE(result);
    CHECK(result.error().message.find("SENTINEL") == std::string::npos);
  }
  fixture.write(
      R"({"ops":{"targets":[{"id":"x","display_name":"X","source":{"kind":"linux_local","k\u0069nd":"linux_local"}}]}})");
  auto duplicate =
      JsonConfigFileStore{fixture.path()}.load(builtin_config_registry());
  REQUIRE_FALSE(duplicate);
  CHECK(duplicate.error().code == ConfigFileErrorCode::duplicate_key);
}

TEST_CASE("Ops typed file updates roundtrip metadata and preserve "
          "invalid-update baseline") {
  Fixture fixture;
  JsonConfigFileStore store{fixture.path()};
  REQUIRE(store.set(builtin_config_registry(), ops_targets_key,
                    ConfigValue{sample()}));
  auto resolved = fixture.resolve();
  REQUIRE(resolved);
  REQUIRE(resolved->targets.size() == 3);
  CHECK(resolved->targets.front().id == "local");
  CHECK(resolved->targets[1] == sample().targets[0]);
  CHECK(resolved->targets[2] == sample().targets[1]);
  auto invalid = sample();
  invalid.targets[0].id = "local";
  CHECK_FALSE(store.set(builtin_config_registry(), ops_targets_key,
                        ConfigValue{invalid}));
  CHECK(fixture.resolve() == resolved);
  REQUIRE(store.unset(builtin_config_registry(), ops_targets_key));
  resolved = fixture.resolve();
  REQUIRE(resolved);
  REQUIRE(resolved->targets.size() == 1);
  CHECK(resolved->targets.front().display_name == "This environment");
  CHECK(format_config_value(ConfigValue{sample()}) == "2 configured targets");
}
