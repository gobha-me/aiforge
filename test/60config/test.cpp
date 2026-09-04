#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <barrier>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include <aiforge/cli/command_registry.hpp>
#include <aiforge/config/config.hpp>
#include <aiforge/config/file_store.hpp>
#include <aiforge/config/provenance.hpp>

namespace {

using namespace aiforge::config;

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    auto pattern =
        (std::filesystem::temp_directory_path() / "aiforge-config-XXXXXX")
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

class EnvironmentGuard final {
 public:
  EnvironmentGuard(std::string name, std::optional<std::string> value)
      : m_name(std::move(name)) {
    if (const auto* current = std::getenv(m_name.c_str())) m_original = current;
    if (value) {
      REQUIRE(::setenv(m_name.c_str(), value->c_str(), 1) == 0);
    } else {
      REQUIRE(::unsetenv(m_name.c_str()) == 0);
    }
  }

  EnvironmentGuard(const EnvironmentGuard&) = delete;
  auto operator=(const EnvironmentGuard&) -> EnvironmentGuard& = delete;

  ~EnvironmentGuard() {
    if (m_original) {
      static_cast<void>(::setenv(m_name.c_str(), m_original->c_str(), 1));
    } else {
      static_cast<void>(::unsetenv(m_name.c_str()));
    }
  }

 private:
  std::string m_name;
  std::optional<std::string> m_original;
};

[[nodiscard]] auto test_registry() -> ConfigRegistry {
  return {{
      {"model", ConfigValueKind::text, "AIFORGE_MODEL", ConfigValue{"default"},
       false, true, 32, 4},
      {"enabled", ConfigValueKind::boolean, "AIFORGE_ENABLED",
       ConfigValue{true}, false, true, 32, 4},
      {"count", ConfigValueKind::signed_integer, std::nullopt,
       ConfigValue{std::int64_t{9}}, false, true, 32, 4},
      {"tags", ConfigValueKind::text_list, std::nullopt,
       ConfigValue{std::vector<std::string>{"default"}}, false, true, 8, 4},
      {"optional", ConfigValueKind::text, std::nullopt, std::nullopt, false,
       true, 32, 4},
      {"credential", ConfigValueKind::text, "SECRET_TOKEN", std::nullopt, true,
       false, 32, 4},
  }};
}

[[nodiscard]] auto candidate(std::string key, ConfigValue value)
    -> ConfigCandidate {
  return {std::move(key), std::move(value), std::nullopt};
}

[[nodiscard]] auto rejected(std::string key, const ConfigSource source)
    -> ConfigCandidate {
  return {key, std::nullopt,
          ConfigDiagnostic{ConfigDiagnosticCode::invalid_value, source,
                           std::move(key), "the source value is invalid"}};
}

auto write_file(const std::filesystem::path& path, const std::string_view value)
    -> void {
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  REQUIRE(output);
  output.write(value.data(), static_cast<std::streamsize>(value.size()));
  REQUIRE(output);
  output.close();
  REQUIRE(output);
}

[[nodiscard]] auto read_file(const std::filesystem::path& path) -> std::string {
  std::ifstream input{path, std::ios::binary};
  REQUIRE(input);
  return {std::istreambuf_iterator<char>{input},
          std::istreambuf_iterator<char>{}};
}

[[nodiscard]] auto run_cli(std::vector<std::string_view> arguments,
                           std::string& output, std::string& error) -> int {
  std::ostringstream output_stream;
  std::ostringstream error_stream;
  const auto result =
      aiforge::cli::run_cli(arguments, output_stream, error_stream);
  output = output_stream.str();
  error = error_stream.str();
  return result;
}

template <typename Id>
[[nodiscard]] auto id(const std::string_view value) -> Id {
  return *Id::from(std::string{value});
}

} // namespace

TEST_CASE("configuration registries reject ambiguous or unsafe schemas",
          "[config][failure]") {
  auto registry = test_registry();
  registry.keys.push_back(registry.keys.front());
  auto result = validate_registry(registry);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == ConfigDiagnosticCode::duplicate_key);

  registry = test_registry();
  registry.keys[1].environment_name = registry.keys[0].environment_name;
  result = validate_registry(registry);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          ConfigDiagnosticCode::duplicate_environment_binding);

  registry = test_registry();
  registry.keys.front().id = "Bad..key";
  result = validate_registry(registry);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == ConfigDiagnosticCode::invalid_registry);

  registry = test_registry();
  registry.keys.back().file_writable = true;
  result = validate_registry(registry);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == ConfigDiagnosticCode::invalid_registry);
}

TEST_CASE("paid image tool model is explicit optional configuration",
          "[config][image-tool][failure]") {
  const auto unresolved = resolve_config(builtin_config_registry(), {});
  REQUIRE(unresolved);
  const auto absent = resolve_image_tool_model(*unresolved);
  REQUIRE(absent);
  REQUIRE_FALSE(*absent);

  const ConfigLayer configured{ConfigSource::file,
                               {candidate(std::string{image_tool_model_key},
                                          std::string{"exact-image-model"})},
                               {}};
  const auto resolved =
      resolve_config(builtin_config_registry(), {&configured, 1});
  REQUIRE(resolved);
  const auto selected = resolve_image_tool_model(*resolved);
  REQUIRE(selected);
  REQUIRE(*selected == id<aiforge::domain::ModelId>("exact-image-model"));

  const ConfigLayer invalid{
      ConfigSource::file,
      {candidate(std::string{image_tool_model_key}, std::string{})},
      {}};
  const auto rejected =
      resolve_config(builtin_config_registry(), {&invalid, 1});
  REQUIRE(rejected);
  const auto invalid_model = resolve_image_tool_model(*rejected);
  REQUIRE_FALSE(invalid_model);
  REQUIRE(invalid_model.error().code == ConfigDiagnosticCode::invalid_value);
}

TEST_CASE("all present-layer permutations resolve by fixed precedence",
          "[config][precedence]") {
  const auto registry = test_registry();
  for (unsigned mask = 0; mask < 8; ++mask) {
    std::vector<ConfigLayer> layers;
    if ((mask & 1U) != 0U) {
      layers.push_back(
          {ConfigSource::file, {candidate("model", std::string{"file"})}, {}});
    }
    if ((mask & 2U) != 0U) {
      layers.push_back({ConfigSource::environment,
                        {candidate("model", std::string{"environment"})},
                        {}});
    }
    if ((mask & 4U) != 0U) {
      layers.push_back({ConfigSource::command_line,
                        {candidate("model", std::string{"command"})},
                        {}});
    }
    const auto resolved = resolve_config(registry, layers);
    REQUIRE(resolved);
    const auto* model = resolved->find("model");
    REQUIRE(model != nullptr);
    const auto expected = (mask & 4U) != 0U   ? "command"
                          : (mask & 2U) != 0U ? "environment"
                          : (mask & 1U) != 0U ? "file"
                                              : "default";
    REQUIRE(std::get<std::string>(*model->value) == expected);
  }
}

TEST_CASE("default-like values remain explicit and invalid sources are visible",
          "[config][failure][precedence]") {
  const auto registry = test_registry();
  ConfigLayer file{ConfigSource::file,
                   {candidate("model", std::string{"file"}),
                    candidate("enabled", false),
                    candidate("count", std::int64_t{0}),
                    candidate("tags", std::vector<std::string>{})},
                   {}};
  ConfigLayer environment{ConfigSource::environment,
                          {rejected("model", ConfigSource::environment)},
                          {}};
  auto resolved = resolve_config(registry, {&file, 1});
  REQUIRE(resolved);
  REQUIRE(std::get<bool>(*resolved->find("enabled")->value) == false);
  REQUIRE(std::get<std::int64_t>(*resolved->find("count")->value) == 0);
  REQUIRE(std::get<std::vector<std::string>>(*resolved->find("tags")->value)
              .empty());
  REQUIRE_FALSE(resolved->find("optional")->value);
  REQUIRE(file.candidates.size() == 4);
  REQUIRE(file.candidates.front().key == "model");

  const std::array layers{environment, file};
  REQUIRE(layers.size() == 2);
  REQUIRE(layers[1].candidates.size() == 4);
  resolved = resolve_config(registry, layers);
  REQUIRE(resolved);
  REQUIRE(std::get<std::string>(*resolved->find("model")->value) == "file");
  REQUIRE_FALSE(resolved->diagnostics.empty());
  REQUIRE(resolved->diagnostics.front().message.find("environment") ==
          std::string::npos);

  const ConfigLayer command{ConfigSource::command_line,
                            {rejected("model", ConfigSource::command_line)},
                            {}};
  resolved = resolve_config(registry, {&command, 1});
  REQUIRE_FALSE(resolved);
  REQUIRE(resolved.error().code == ConfigDiagnosticCode::invalid_value);
}

TEST_CASE("value parsing enforces type cardinality and byte limits",
          "[config][failure]") {
  const auto registry = test_registry();
  const auto& enabled = registry.keys[1];
  auto value =
      parse_config_value(enabled, std::array<std::string_view, 1>{"false"},
                         ConfigSource::command_line);
  REQUIRE(value);
  REQUIRE(std::get<bool>(*value) == false);

  value = parse_config_value(enabled, std::array<std::string_view, 1>{"maybe"},
                             ConfigSource::command_line);
  REQUIRE_FALSE(value);
  REQUIRE(value.error().code == ConfigDiagnosticCode::invalid_value);

  const auto& tags = registry.keys[3];
  value = parse_config_value(
      tags, std::array<std::string_view, 5>{"a", "b", "c", "d", "e"},
      ConfigSource::file);
  REQUIRE_FALSE(value);
  REQUIRE(value.error().code == ConfigDiagnosticCode::too_many_values);

  const std::string oversized(33, 'x');
  value = parse_config_value(registry.keys.front(),
                             std::array<std::string_view, 1>{oversized},
                             ConfigSource::file);
  REQUIRE_FALSE(value);
  REQUIRE(value.error().code == ConfigDiagnosticCode::value_too_large);

  const std::string invalid_utf8{"\xc3\x28", 2};
  value = parse_config_value(registry.keys.front(),
                             std::array<std::string_view, 1>{invalid_utf8},
                             ConfigSource::environment);
  REQUIRE_FALSE(value);
  REQUIRE(value.error().code == ConfigDiagnosticCode::invalid_value);
}

TEST_CASE("environment input is typed and sensitive values stay marked",
          "[config][environment]") {
  EnvironmentGuard enabled{"AIFORGE_ENABLED", std::string{"false"}};
  EnvironmentGuard credential{"SECRET_TOKEN", std::string{"do-not-render"}};
  const auto registry = test_registry();
  const auto layer = environment_config_layer(registry);
  REQUIRE(layer);
  const std::array layers{*layer};
  const auto resolved = resolve_config(registry, layers);
  REQUIRE(resolved);
  REQUIRE(std::get<bool>(*resolved->find("enabled")->value) == false);
  const auto* secret = resolved->find("credential");
  REQUIRE(secret != nullptr);
  REQUIRE(secret->sensitive);
  REQUIRE(secret->source == ConfigSource::environment);
  for (const auto& diagnostic : resolved->diagnostics) {
    REQUIRE(diagnostic.message.find("do-not-render") == std::string::npos);
  }
}

TEST_CASE("configuration provenance keeps decisions and drops sensitive values",
          "[config][provenance][failure]") {
  const auto registry = test_registry();
  const ConfigLayer file{ConfigSource::file,
                         {candidate("model", std::string{"file-model"}),
                          rejected("count", ConfigSource::file)},
                         {}};
  const ConfigLayer environment{
      ConfigSource::environment,
      {candidate("model", std::string{"environment-model"}),
       candidate("credential", std::string{"do-not-record"})},
      {}};
  const std::array layers{file, environment};
  const auto resolved = resolve_config(registry, layers);
  REQUIRE(resolved);

  const auto provenance = configuration_provenance(*resolved);
  REQUIRE(provenance.size() == resolved->entries.size());

  const auto find = [&](const std::string_view key) {
    const auto found = std::ranges::find(
        provenance, key, &aiforge::domain::ConfigurationProvenanceEntry::key);
    REQUIRE(found != provenance.end());
    return *found;
  };

  // A sensitive key keeps presence, source, and its decision trail. Its
  // resolved value is structurally absent, not filtered by a message check.
  const auto credential = find("credential");
  REQUIRE(credential.sensitive);
  REQUIRE(credential.value_present);
  REQUIRE_FALSE(credential.value.has_value());
  REQUIRE(credential.source == aiforge::domain::ProvenanceSource::environment);
  REQUIRE_FALSE(credential.decisions.empty());

  // Environment wins over file, and the shadowed candidate stays visible.
  const auto model = find("model");
  REQUIRE_FALSE(model.sensitive);
  REQUIRE(model.value == "environment-model");
  REQUIRE(model.source == aiforge::domain::ProvenanceSource::environment);
  REQUIRE(model.decisions.size() == resolved->find("model")->decisions.size());
  REQUIRE(std::ranges::any_of(model.decisions, [](const auto& decision) {
    return decision.source == aiforge::domain::ProvenanceSource::file &&
           decision.disposition ==
               aiforge::domain::ProvenanceDisposition::shadowed;
  }));

  // A rejected candidate carries its diagnostic code, without its text.
  const auto count = find("count");
  REQUIRE(std::ranges::any_of(count.decisions, [](const auto& decision) {
    return decision.disposition ==
               aiforge::domain::ProvenanceDisposition::rejected &&
           decision.diagnostic_code ==
               aiforge::domain::ProvenanceDiagnosticCode::invalid_value;
  }));

  const auto optional_key = find("optional");
  REQUIRE_FALSE(optional_key.value_present);
  REQUIRE_FALSE(optional_key.value.has_value());
}

TEST_CASE("configuration path resolution follows XDG without relative escape",
          "[config][path][failure]") {
  auto result = resolve_config_path(
      {std::filesystem::path{"/tmp/xdg"}, std::filesystem::path{"/home/user"}});
  REQUIRE(result == std::filesystem::path{"/tmp/xdg/aiforge/config.json"});

  result = resolve_config_path(
      {std::filesystem::path{"relative"}, std::filesystem::path{"/home/user"}});
  REQUIRE(result ==
          std::filesystem::path{"/home/user/.config/aiforge/config.json"});

  result = resolve_config_path({std::nullopt, std::nullopt});
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == ConfigFileErrorCode::missing_home);
}

TEST_CASE(
    "JSON adapter rejects malformed duplicate oversized and symlink inputs",
    "[config][file][failure]") {
  TemporaryDirectory temporary;
  const auto app = temporary.path() / "aiforge";
  REQUIRE(std::filesystem::create_directory(app));
  REQUIRE(::chmod(app.c_str(), 0700) == 0);
  const auto path = app / "config.json";
  JsonConfigFileStore store{path};

  write_file(path, R"({"model":"one","model":"two"})");
  auto loaded = store.load(builtin_config_registry());
  REQUIRE_FALSE(loaded);
  REQUIRE(loaded.error().code == ConfigFileErrorCode::duplicate_key);

  write_file(path, "{\"model\":\"");
  loaded = store.load(builtin_config_registry());
  REQUIRE_FALSE(loaded);
  REQUIRE(loaded.error().code == ConfigFileErrorCode::malformed);

  write_file(path, std::string{"{\"model\":\"\xc3\x28\"}", 14});
  loaded = store.load(builtin_config_registry());
  REQUIRE_FALSE(loaded);
  REQUIRE(loaded.error().code == ConfigFileErrorCode::malformed);

  write_file(path, std::string(1024U * 1024U + 1U, 'x'));
  loaded = store.load(builtin_config_registry());
  REQUIRE_FALSE(loaded);
  REQUIRE(loaded.error().code == ConfigFileErrorCode::too_large);

  std::filesystem::remove(path);
  const auto target = temporary.path() / "target.json";
  write_file(target, R"({"model":"target"})");
  std::filesystem::create_symlink(target, path);
  REQUIRE(std::filesystem::is_symlink(std::filesystem::symlink_status(path)));
  loaded = store.load(builtin_config_registry());
  REQUIRE_FALSE(loaded);
  REQUIRE(loaded.error().code == ConfigFileErrorCode::path_escape);

  const auto linked_app = temporary.path() / "linked-aiforge";
  std::filesystem::create_directory_symlink(app, linked_app);
  loaded = JsonConfigFileStore{linked_app / "config.json"}.load(
      builtin_config_registry());
  REQUIRE_FALSE(loaded);
  REQUIRE(loaded.error().code == ConfigFileErrorCode::path_escape);
}

TEST_CASE("atomic mutations preserve unknown JSON and restrictive permissions",
          "[config][file]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "aiforge" / "config.json";
  JsonConfigFileStore store{path};

  auto changed = store.set(builtin_config_registry(), "model",
                           ConfigValue{std::string{"first"}});
  REQUIRE(changed);
  struct stat directory_info{};
  struct stat file_info{};
  REQUIRE(::stat(path.parent_path().c_str(), &directory_info) == 0);
  REQUIRE(::stat(path.c_str(), &file_info) == 0);
  REQUIRE((directory_info.st_mode & 0777) == 0700);
  REQUIRE((file_info.st_mode & 0777) == 0600);

  write_file(path, R"({"future":{"flag":true},"model":"first"})");
  REQUIRE(::chmod(path.c_str(), 0600) == 0);
  changed = store.set(builtin_config_registry(), "model",
                      ConfigValue{std::string{"second"}});
  REQUIRE(changed);
  const auto contents = read_file(path);
  REQUIRE(contents.find("\"future\"") != std::string::npos);
  REQUIRE(contents.find("\"flag\": true") != std::string::npos);
  REQUIRE(contents.find("\"second\"") != std::string::npos);

  auto loaded = store.load(builtin_config_registry());
  REQUIRE(loaded);
  REQUIRE(loaded->candidates.size() == 1);
  REQUIRE_FALSE(loaded->diagnostics.empty());
  REQUIRE(loaded->diagnostics.front().code ==
          ConfigDiagnosticCode::unknown_key);

  changed = store.unset(builtin_config_registry(), "model");
  REQUIRE(changed);
  changed = store.unset(builtin_config_registry(), "model");
  REQUIRE(changed);
  REQUIRE(read_file(path).find("future") != std::string::npos);
}

TEST_CASE("dotted keys map to nested JSON without disturbing siblings",
          "[config][file]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "aiforge" / "config.json";
  const ConfigRegistry registry{{
      {"ui.tags", ConfigValueKind::text_list, std::nullopt, std::nullopt, false,
       true, 16, 4},
  }};
  JsonConfigFileStore store{path};
  REQUIRE(store.set(registry, "ui.tags",
                    ConfigValue{std::vector<std::string>{"one", "two"}}));
  const auto contents = read_file(path);
  REQUIRE(contents.find("\"ui\"") != std::string::npos);
  REQUIRE(contents.find("\"tags\"") != std::string::npos);

  const auto loaded = store.load(registry);
  REQUIRE(loaded);
  REQUIRE(loaded->candidates.size() == 1);
  const auto& value = *loaded->candidates.front().value;
  REQUIRE(std::get<std::vector<std::string>>(value) ==
          std::vector<std::string>{"one", "two"});
}

TEST_CASE("mutations refuse loose permissions and malformed existing content",
          "[config][file][failure]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "aiforge" / "config.json";
  JsonConfigFileStore store{path};
  REQUIRE(store.set(builtin_config_registry(), "model",
                    ConfigValue{std::string{"safe"}}));

  REQUIRE(::chmod(path.c_str(), 0644) == 0);
  auto loaded = store.load(builtin_config_registry());
  REQUIRE(loaded);
  REQUIRE_FALSE(loaded->diagnostics.empty());
  auto changed = store.set(builtin_config_registry(), "model",
                           ConfigValue{std::string{"replacement"}});
  REQUIRE_FALSE(changed);
  REQUIRE(changed.error().code == ConfigFileErrorCode::insecure_permissions);
  REQUIRE(read_file(path).find("safe") != std::string::npos);

  REQUIRE(::chmod(path.c_str(), 0600) == 0);
  write_file(path, "not-json");
  changed = store.unset(builtin_config_registry(), "model");
  REQUIRE_FALSE(changed);
  REQUIRE(changed.error().code == ConfigFileErrorCode::malformed);
  REQUIRE(read_file(path) == "not-json");
}

TEST_CASE("concurrent writers serialize read-modify-write updates",
          "[config][file][concurrency]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "aiforge" / "config.json";
  const ConfigRegistry registry{{
      {"model", ConfigValueKind::text, std::nullopt, std::nullopt, false, true,
       32, 4},
      {"enabled", ConfigValueKind::boolean, std::nullopt, std::nullopt, false,
       true, 32, 4},
  }};
  std::optional<ConfigFileError> first_error;
  std::optional<ConfigFileError> second_error;
  std::barrier start{3};
  std::jthread first{[&] {
    start.arrive_and_wait();
    auto result = JsonConfigFileStore{path}.set(
        registry, "model", ConfigValue{std::string{"concurrent"}});
    if (!result) first_error = result.error();
  }};
  std::jthread second{[&] {
    start.arrive_and_wait();
    auto result =
        JsonConfigFileStore{path}.set(registry, "enabled", ConfigValue{false});
    if (!result) second_error = result.error();
  }};
  start.arrive_and_wait();
  first.join();
  second.join();
  REQUIRE_FALSE(first_error);
  REQUIRE_FALSE(second_error);

  const auto loaded = JsonConfigFileStore{path}.load(registry);
  REQUIRE(loaded);
  REQUIRE(loaded->candidates.size() == 2);
  const std::array layers{*loaded};
  const auto resolved = resolve_config(registry, layers);
  REQUIRE(resolved);
  REQUIRE(std::get<std::string>(*resolved->find("model")->value) ==
          "concurrent");
  REQUIRE(std::get<bool>(*resolved->find("enabled")->value) == false);
}

TEST_CASE("first write creates and syncs a private directory chain",
          "[config][file][durability]") {
  TemporaryDirectory temporary;
  const auto base = temporary.path() / "missing" / "xdg";
  const auto app = base / "aiforge";
  const auto path = app / "config.json";
  REQUIRE_FALSE(std::filesystem::exists(base));

  JsonConfigFileStore store{path};
  REQUIRE(store.update_text_map_entry(builtin_config_registry(),
                                      model_maximum_tool_profiles_key, "model",
                                      "off"));
  REQUIRE(std::filesystem::is_directory(base));
  REQUIRE(std::filesystem::is_directory(app));

  struct stat info{};
  REQUIRE(::stat(base.c_str(), &info) == 0);
  REQUIRE((info.st_mode & 0077) == 0);
  REQUIRE(::stat(app.c_str(), &info) == 0);
  REQUIRE((info.st_mode & 0077) == 0);
  const auto loaded = store.load(builtin_config_registry());
  REQUIRE(loaded);
  const std::array layers{*loaded};
  const auto resolved = resolve_config(builtin_config_registry(), layers);
  REQUIRE(resolved);
  const auto mappings = resolve_tool_profile_maximum_mappings(*resolved);
  REQUIRE(mappings);
  REQUIRE(mappings->models.at(id<aiforge::domain::ModelId>("model")) ==
          id<aiforge::domain::ToolProfileId>("off"));
}

TEST_CASE("concurrent writers merge entries in the same text map",
          "[config][file][concurrency][tools]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "aiforge" / "config.json";
  std::optional<ConfigFileError> first_error;
  std::optional<ConfigFileError> second_error;
  std::barrier start{3};
  std::jthread first{[&] {
    start.arrive_and_wait();
    auto result = JsonConfigFileStore{path}.update_text_map_entry(
        builtin_config_registry(), model_maximum_tool_profiles_key, "model-a",
        "off");
    if (!result) first_error = result.error();
  }};
  std::jthread second{[&] {
    start.arrive_and_wait();
    auto result = JsonConfigFileStore{path}.update_text_map_entry(
        builtin_config_registry(), model_maximum_tool_profiles_key, "model-b",
        "essentials");
    if (!result) second_error = result.error();
  }};
  start.arrive_and_wait();
  first.join();
  second.join();
  REQUIRE_FALSE(first_error);
  REQUIRE_FALSE(second_error);

  const auto loaded = JsonConfigFileStore{path}.load(builtin_config_registry());
  REQUIRE(loaded);
  const std::array layers{*loaded};
  const auto resolved = resolve_config(builtin_config_registry(), layers);
  REQUIRE(resolved);
  const auto mappings = resolve_tool_profile_maximum_mappings(*resolved);
  REQUIRE(mappings);
  REQUIRE(mappings->models.at(id<aiforge::domain::ModelId>("model-a")) ==
          id<aiforge::domain::ToolProfileId>("off"));
  REQUIRE(mappings->models.at(id<aiforge::domain::ModelId>("model-b")) ==
          id<aiforge::domain::ToolProfileId>("essentials"));
}

TEST_CASE("Venice system-prompt config preserves absence and explicit false",
          "[config][venice][failure]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "aiforge" / "config.json";
  JsonConfigFileStore store{path};
  REQUIRE(store.set(builtin_config_registry(), "venice.include_system_prompt",
                    ConfigValue{false}));
  const auto file = store.load(builtin_config_registry());
  REQUIRE(file);
  const std::array layers{*file};
  const auto resolved = resolve_config(builtin_config_registry(), layers);
  REQUIRE(resolved);
  REQUIRE(std::get<bool>(
              *resolved->find("venice.include_system_prompt")->value) == false);
  REQUIRE(resolved->find("venice.include_system_prompt")->source ==
          ConfigSource::file);
  REQUIRE(
      store.unset(builtin_config_registry(), "venice.include_system_prompt"));
  const auto absent = store.load(builtin_config_registry());
  REQUIRE(absent);
  const std::array absent_layers{*absent};
  const auto inherited =
      resolve_config(builtin_config_registry(), absent_layers);
  REQUIRE(inherited);
  REQUIRE_FALSE(inherited->find("venice.include_system_prompt")->value);

  EnvironmentGuard malformed{"AIFORGE_VENICE_INCLUDE_SYSTEM_PROMPT",
                             std::string{"sometimes"}};
  const auto environment = environment_config_layer(builtin_config_registry());
  REQUIRE(environment);
  const std::array malformed_layers{*environment};
  const auto rejected =
      resolve_config(builtin_config_registry(), malformed_layers);
  REQUIRE(rejected);
  REQUIRE_FALSE(rejected->find("venice.include_system_prompt")->value);
  REQUIRE(
      std::ranges::any_of(rejected->diagnostics, [](const auto& diagnostic) {
        return diagnostic.key == "venice.include_system_prompt" &&
               diagnostic.code == ConfigDiagnosticCode::invalid_value;
      }));
}

TEST_CASE("user-global instruction enablement defaults on and fails closed",
          "[config][instructions][failure]") {
  EnvironmentGuard environment{"AIFORGE_INSTRUCTIONS_GLOBAL_ENABLED",
                               std::nullopt};
  const auto defaults = resolve_config(builtin_config_registry(), {});
  REQUIRE(defaults);
  REQUIRE(resolve_user_global_instructions_enabled(*defaults) == true);

  const ConfigLayer disabled{
      ConfigSource::file,
      {{std::string{user_global_instructions_enabled_key}, ConfigValue{false},
        std::nullopt}},
      {}};
  const std::array disabled_layers{disabled};
  const auto resolved =
      resolve_config(builtin_config_registry(), disabled_layers);
  REQUIRE(resolved);
  REQUIRE(resolve_user_global_instructions_enabled(*resolved) == false);

  const ConfigLayer malformed{
      ConfigSource::file,
      {{std::string{user_global_instructions_enabled_key}, std::nullopt,
        ConfigDiagnostic{ConfigDiagnosticCode::invalid_value,
                         ConfigSource::file,
                         std::string{user_global_instructions_enabled_key},
                         "the setting is invalid"}}},
      {}};
  const std::array malformed_layers{malformed};
  const auto rejected =
      resolve_config(builtin_config_registry(), malformed_layers);
  REQUIRE(rejected);
  const auto enabled = resolve_user_global_instructions_enabled(*rejected);
  REQUIRE_FALSE(enabled);
  REQUIRE(enabled.error().code == ConfigDiagnosticCode::invalid_value);
}

TEST_CASE("malformed persisted Venice request settings fail closed",
          "[config][venice][file][failure]") {
  TemporaryDirectory temporary;
  const auto app = temporary.path() / "aiforge";
  REQUIRE(std::filesystem::create_directories(app));
  REQUIRE(::chmod(app.c_str(), 0700) == 0);
  const auto path = app / "config.json";

  write_file(path, R"({"venice":{"include_system_prompt":"false"}})");
  REQUIRE(::chmod(path.c_str(), 0600) == 0);
  auto file = JsonConfigFileStore{path}.load(builtin_config_registry());
  REQUIRE(file);
  REQUIRE(file->candidates.size() == 1);
  REQUIRE_FALSE(file->candidates.front().value);
  REQUIRE(file->candidates.front().rejection);
  REQUIRE(file->candidates.front().rejection->key ==
          "venice.include_system_prompt");
  REQUIRE(file->candidates.front().rejection->source == ConfigSource::file);
  REQUIRE(file->candidates.front().rejection->code ==
          ConfigDiagnosticCode::invalid_value);
  const std::array prompt_layers{*file};
  auto prompt = resolve_config(builtin_config_registry(), prompt_layers);
  REQUIRE(prompt);
  REQUIRE_FALSE(prompt->find("venice.include_system_prompt")->value);

  write_file(path, R"({"venice":{"web_search":"sometimes"}})");
  REQUIRE(::chmod(path.c_str(), 0600) == 0);
  file = JsonConfigFileStore{path}.load(builtin_config_registry());
  REQUIRE(file);
  REQUIRE(file->candidates.size() == 1);
  REQUIRE(file->candidates.front().key == "venice.web_search");
  REQUIRE(file->candidates.front().value);
  const auto* invalid_web =
      std::get_if<std::string>(&*file->candidates.front().value);
  REQUIRE(invalid_web);
  REQUIRE(*invalid_web == "sometimes");
}

TEST_CASE("tool profile maximum mappings are typed bounded JSON objects",
          "[config][tools][file]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "aiforge" / "config.json";
  JsonConfigFileStore store{path};

  const ConfigTextMap models{{"z-model", "repository-read"},
                             {"a-model", "off"}};
  const ConfigTextMap personas{{"reviewer", "essentials"}};
  REQUIRE(store.set(builtin_config_registry(), model_maximum_tool_profiles_key,
                    ConfigValue{models}));
  REQUIRE(store.set(builtin_config_registry(),
                    persona_maximum_tool_profiles_key, ConfigValue{personas}));

  const auto file = store.load(builtin_config_registry());
  REQUIRE(file);
  const std::array layers{*file};
  const auto resolved = resolve_config(builtin_config_registry(), layers);
  REQUIRE(resolved);
  const auto mappings = resolve_tool_profile_maximum_mappings(*resolved);
  REQUIRE(mappings);
  REQUIRE(mappings->models.size() == 2);
  REQUIRE(mappings->models.at(id<aiforge::domain::ModelId>("a-model")) ==
          id<aiforge::domain::ToolProfileId>("off"));
  REQUIRE(mappings->models.at(id<aiforge::domain::ModelId>("z-model")) ==
          id<aiforge::domain::ToolProfileId>("repository-read"));
  REQUIRE(mappings->personas.at(id<aiforge::domain::PersonaId>("reviewer")) ==
          id<aiforge::domain::ToolProfileId>("essentials"));

  const auto provenance = configuration_provenance(*resolved);
  const auto model_provenance =
      std::ranges::find(provenance, model_maximum_tool_profiles_key,
                        &aiforge::domain::ConfigurationProvenanceEntry::key);
  REQUIRE(model_provenance != provenance.end());
  REQUIRE(model_provenance->value_present);
  REQUIRE_FALSE(model_provenance->value);

  const auto contents = read_file(path);
  REQUIRE(contents.find(R"("maximum_profiles": {)") != std::string::npos);

  const auto empty = resolve_config(builtin_config_registry(), {});
  REQUIRE(empty);
  const auto no_mappings = resolve_tool_profile_maximum_mappings(*empty);
  REQUIRE(no_mappings);
  REQUIRE(no_mappings->models.empty());
  REQUIRE(no_mappings->personas.empty());
}

TEST_CASE("malformed tool profile maximum mappings fail closed",
          "[config][tools][file][failure]") {
  TemporaryDirectory temporary;
  const auto app = temporary.path() / "aiforge";
  REQUIRE(std::filesystem::create_directories(app));
  REQUIRE(::chmod(app.c_str(), 0700) == 0);
  const auto path = app / "config.json";

  const auto resolve_file = [&]() {
    auto file = JsonConfigFileStore{path}.load(builtin_config_registry());
    REQUIRE(file);
    const std::array layers{*file};
    auto resolved = resolve_config(builtin_config_registry(), layers);
    REQUIRE(resolved);
    return resolve_tool_profile_maximum_mappings(*resolved);
  };

  write_file(path, R"({"tools":{"models":{"maximum_profiles":[]}}})");
  REQUIRE(::chmod(path.c_str(), 0600) == 0);
  REQUIRE_FALSE(resolve_file());

  write_file(path,
             R"({"tools":{"models":{"maximum_profile":{"model":"off"}}}})");
  REQUIRE(::chmod(path.c_str(), 0600) == 0);
  REQUIRE_FALSE(resolve_file());

  write_file(path,
             R"({"tools":{"models":{"maximum_profiles":{"model":"custom"}}}})");
  REQUIRE(::chmod(path.c_str(), 0600) == 0);
  auto result = resolve_file();
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == ConfigDiagnosticCode::invalid_value);

  write_file(path, R"({"tools":{"models":{"maximum_profiles":{"":"off"}}}})");
  REQUIRE(::chmod(path.c_str(), 0600) == 0);
  REQUIRE_FALSE(resolve_file());

  write_file(
      path,
      R"({"tools":{"personas":{"maximum_profiles":{"bad\u0001id":"off"}}}})");
  REQUIRE(::chmod(path.c_str(), 0600) == 0);
  REQUIRE_FALSE(resolve_file());

  write_file(
      path,
      R"({"tools":{"models":{"maximum_profiles":{"model":"off","model":"essentials"}}}})");
  REQUIRE(::chmod(path.c_str(), 0600) == 0);
  const auto duplicate =
      JsonConfigFileStore{path}.load(builtin_config_registry());
  REQUIRE_FALSE(duplicate);
  REQUIRE(duplicate.error().code == ConfigFileErrorCode::duplicate_key);
}

TEST_CASE("tool profile maximum mapping writes reject unsafe sizes atomically",
          "[config][tools][file][failure]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "aiforge" / "config.json";
  JsonConfigFileStore store{path};
  REQUIRE(store.set(builtin_config_registry(), model_maximum_tool_profiles_key,
                    ConfigValue{ConfigTextMap{{"safe", "off"}}}));
  const auto original = read_file(path);

  ConfigTextMap excessive;
  for (std::size_t index = 0; index < 257; ++index) {
    excessive.push_back({"model-" + std::to_string(index), "off"});
  }
  auto changed =
      store.set(builtin_config_registry(), model_maximum_tool_profiles_key,
                ConfigValue{std::move(excessive)});
  REQUIRE_FALSE(changed);
  REQUIRE(changed.error().code == ConfigFileErrorCode::malformed);
  REQUIRE(read_file(path) == original);

  changed =
      store.set(builtin_config_registry(), model_maximum_tool_profiles_key,
                ConfigValue{ConfigTextMap{{std::string(129, 'm'), "off"}}});
  REQUIRE_FALSE(changed);
  REQUIRE(changed.error().code == ConfigFileErrorCode::malformed);
  REQUIRE(read_file(path) == original);

  changed =
      store.set(builtin_config_registry(), model_maximum_tool_profiles_key,
                ConfigValue{ConfigTextMap{{"duplicate", "off"},
                                          {"duplicate", "essentials"}}});
  REQUIRE_FALSE(changed);
  REQUIRE(changed.error().code == ConfigFileErrorCode::malformed);
  REQUIRE(read_file(path) == original);

  const auto& registry = builtin_config_registry();
  const auto spec = std::ranges::find(
      registry.keys, model_maximum_tool_profiles_key, &ConfigKeySpec::id);
  REQUIRE(spec != registry.keys.end());
  const std::array textual{std::string_view{"model=off"}};
  REQUIRE_FALSE(parse_config_value(*spec, textual, ConfigSource::environment));
  REQUIRE_FALSE(spec->environment_name);

  const ConfigLayer command_line{
      ConfigSource::command_line,
      {{std::string{model_maximum_tool_profiles_key},
        ConfigValue{ConfigTextMap{{"model", "off"}}}, std::nullopt}},
      {}};
  const std::array command_layers{command_line};
  REQUIRE_FALSE(resolve_config(builtin_config_registry(), command_layers));
}

TEST_CASE("maximum-size tool mappings keep bounded run provenance",
          "[config][tools][provenance]") {
  ConfigTextMap mappings;
  mappings.reserve(256);
  for (std::size_t index = 0; index < 256; ++index) {
    mappings.push_back({"model-" + std::to_string(index), "off"});
  }
  const ConfigLayer file{ConfigSource::file,
                         {{std::string{model_maximum_tool_profiles_key},
                           ConfigValue{std::move(mappings)}, std::nullopt}},
                         {}};
  const std::array layers{file};
  const auto resolved = resolve_config(builtin_config_registry(), layers);
  REQUIRE(resolved);
  const auto configuration = configuration_provenance(*resolved);
  const auto model_entry =
      std::ranges::find(configuration, model_maximum_tool_profiles_key,
                        &aiforge::domain::ConfigurationProvenanceEntry::key);
  REQUIRE(model_entry != configuration.end());
  REQUIRE(model_entry->value_present);
  REQUIRE_FALSE(model_entry->value);

  const aiforge::domain::RunProvenance provenance{
      "test-version",
      "test-backend",
      std::nullopt,
      id<aiforge::domain::ModelId>("model"),
      std::nullopt,
      configuration,
      {},
      {}};
  REQUIRE(aiforge::domain::validate_run_provenance(provenance));
}

TEST_CASE("config CLI keeps content and diagnostics on their streams",
          "[config][commands]") {
  TemporaryDirectory temporary;
  EnvironmentGuard xdg{"XDG_CONFIG_HOME", temporary.path().string()};
  EnvironmentGuard model{"AIFORGE_MODEL", std::nullopt};
  EnvironmentGuard web_search{"AIFORGE_VENICE_WEB_SEARCH", std::nullopt};
  EnvironmentGuard system_prompt{"AIFORGE_VENICE_INCLUDE_SYSTEM_PROMPT",
                                 std::nullopt};
  EnvironmentGuard image_model{"AIFORGE_TOOLS_IMAGE_MODEL", std::nullopt};
  std::string output;
  std::string error;

  const auto schema = aiforge::cli::make_parser_schema(
      aiforge::cli::builtin_command_registry());
  REQUIRE(schema);
  const auto parsed = aiforge::cli::ArgumentParser{}.parse(
      *schema,
      std::array<std::string_view, 4>{"config", "set", "model", "test-model"},
      {32, 256, 1024});
  REQUIRE(parsed);

  REQUIRE(run_cli({"config", "show"}, output, error) == 0);
  REQUIRE(output == "model\t<unset>\tunset\n"
                    "venice.web_search\t<unset>\tunset\n"
                    "venice.include_system_prompt\t<unset>\tunset\n"
                    "instructions.global.enabled\ttrue\tdefault\n"
                    "memory.global.capture\toff\tdefault\n"
                    "memory.project.capture\treview\tdefault\n"
                    "memory.context.max_tokens\t2048\tdefault\n"
                    "tools.models.maximum_profiles\t<unset>\tunset\n"
                    "tools.personas.maximum_profiles\t<unset>\tunset\n"
                    "tools.image.model\t<unset>\tunset\n");
  REQUIRE(error.empty());

  REQUIRE(run_cli({"config", "set", "model", "test-model"}, output, error) ==
          0);
  REQUIRE(output.empty());
  REQUIRE(error.empty());

  REQUIRE(run_cli({"config", "get", "model"}, output, error) == 0);
  REQUIRE(output == "test-model\n");
  REQUIRE(error.empty());

  REQUIRE(run_cli({"config", "get", "unknown"}, output, error) == 2);
  REQUIRE(output.empty());
  REQUIRE(error.find("unknown configuration key") != std::string::npos);

  REQUIRE(run_cli({"config", "unset", "model"}, output, error) == 0);
  REQUIRE(run_cli({"config", "get", "model"}, output, error) == 1);
  REQUIRE(output.empty());
  REQUIRE(error.find("unset") != std::string::npos);
}

TEST_CASE("malformed files are diagnostic for reads but never overwritten",
          "[config][commands][failure]") {
  TemporaryDirectory temporary;
  EnvironmentGuard xdg{"XDG_CONFIG_HOME", temporary.path().string()};
  EnvironmentGuard model{"AIFORGE_MODEL", std::nullopt};
  EnvironmentGuard web_search{"AIFORGE_VENICE_WEB_SEARCH", std::nullopt};
  EnvironmentGuard system_prompt{"AIFORGE_VENICE_INCLUDE_SYSTEM_PROMPT",
                                 std::nullopt};
  EnvironmentGuard image_model{"AIFORGE_TOOLS_IMAGE_MODEL", std::nullopt};
  const auto app = temporary.path() / "aiforge";
  REQUIRE(std::filesystem::create_directory(app));
  REQUIRE(::chmod(app.c_str(), 0700) == 0);
  const auto path = app / "config.json";
  write_file(path, "broken");
  REQUIRE(::chmod(path.c_str(), 0600) == 0);
  std::string output;
  std::string error;

  REQUIRE(run_cli({"config", "show"}, output, error) == 0);
  REQUIRE(output == "model\t<unset>\tunset\n"
                    "venice.web_search\t<unset>\tunset\n"
                    "venice.include_system_prompt\t<unset>\tunset\n"
                    "instructions.global.enabled\ttrue\tdefault\n"
                    "memory.global.capture\toff\tdefault\n"
                    "memory.project.capture\treview\tdefault\n"
                    "memory.context.max_tokens\t2048\tdefault\n"
                    "tools.models.maximum_profiles\t<unset>\tunset\n"
                    "tools.personas.maximum_profiles\t<unset>\tunset\n"
                    "tools.image.model\t<unset>\tunset\n");
  REQUIRE(error.find("warning") != std::string::npos);

  REQUIRE(run_cli({"config", "set", "model", "replacement"}, output, error) ==
          1);
  REQUIRE(output.empty());
  REQUIRE(error.find("strict UTF-8 JSON") != std::string::npos);
  REQUIRE(read_file(path) == "broken");
}

TEST_CASE("read-only resolution survives an unavailable config home",
          "[config][commands][failure]") {
  EnvironmentGuard xdg{"XDG_CONFIG_HOME", std::nullopt};
  EnvironmentGuard home{"HOME", std::nullopt};
  EnvironmentGuard model{"AIFORGE_MODEL", std::string{"environment-model"}};
  EnvironmentGuard web_search{"AIFORGE_VENICE_WEB_SEARCH", std::nullopt};
  EnvironmentGuard system_prompt{"AIFORGE_VENICE_INCLUDE_SYSTEM_PROMPT",
                                 std::nullopt};
  EnvironmentGuard image_model{"AIFORGE_TOOLS_IMAGE_MODEL", std::nullopt};
  std::string output;
  std::string error;

  REQUIRE(run_cli({"config", "show"}, output, error) == 0);
  REQUIRE(output == "model\tenvironment-model\tenvironment\n"
                    "venice.web_search\t<unset>\tunset\n"
                    "venice.include_system_prompt\t<unset>\tunset\n"
                    "instructions.global.enabled\ttrue\tdefault\n"
                    "memory.global.capture\toff\tdefault\n"
                    "memory.project.capture\treview\tdefault\n"
                    "memory.context.max_tokens\t2048\tdefault\n"
                    "tools.models.maximum_profiles\t<unset>\tunset\n"
                    "tools.personas.maximum_profiles\t<unset>\tunset\n"
                    "tools.image.model\t<unset>\tunset\n");
  REQUIRE(error.find("warning") != std::string::npos);
  REQUIRE(error.find("environment-model") == std::string::npos);
}
