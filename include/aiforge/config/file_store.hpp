#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>

#include <aiforge/config/config.hpp>

namespace aiforge::config {

enum class ConfigFileErrorCode {
  missing_home,
  invalid_base_path,
  path_escape,
  not_regular,
  insecure_permissions,
  too_large,
  malformed,
  duplicate_key,
  lock_failed,
  read_failed,
  write_failed,
  sync_failed,
  rename_failed,
};

struct ConfigFileError {
  ConfigFileErrorCode code{ConfigFileErrorCode::read_failed};
  std::filesystem::path path;
  std::string message;
  bool effect_may_have_applied{};
};

struct ConfigPathEnvironment {
  std::optional<std::filesystem::path> xdg_config_home;
  std::optional<std::filesystem::path> home;
};

[[nodiscard]] auto resolve_config_path(const ConfigPathEnvironment& environment)
    -> std::expected<std::filesystem::path, ConfigFileError>;

class ConfigFileStore {
 public:
  virtual ~ConfigFileStore() = default;

  [[nodiscard]] virtual auto load(const ConfigRegistry& registry) const
      -> std::expected<ConfigLayer, ConfigFileError> = 0;
  [[nodiscard]] virtual auto set(const ConfigRegistry& registry,
                                 std::string_view key, const ConfigValue& value)
      -> std::expected<void, ConfigFileError> = 0;
  [[nodiscard]] virtual auto unset(const ConfigRegistry& registry,
                                   std::string_view key)
      -> std::expected<void, ConfigFileError> = 0;
  [[nodiscard]] virtual auto update_text_map_entry(
      const ConfigRegistry& registry, std::string_view key,
      std::string map_entry_key, std::optional<std::string> value)
      -> std::expected<void, ConfigFileError> = 0;
};

class JsonConfigFileStore final : public ConfigFileStore {
 public:
  explicit JsonConfigFileStore(std::filesystem::path path);

  [[nodiscard]] auto path() const -> const std::filesystem::path&;
  [[nodiscard]] auto load(const ConfigRegistry& registry) const
      -> std::expected<ConfigLayer, ConfigFileError> override;
  [[nodiscard]] auto set(const ConfigRegistry& registry, std::string_view key,
                         const ConfigValue& value)
      -> std::expected<void, ConfigFileError> override;
  [[nodiscard]] auto unset(const ConfigRegistry& registry, std::string_view key)
      -> std::expected<void, ConfigFileError> override;
  [[nodiscard]] auto update_text_map_entry(const ConfigRegistry& registry,
                                           std::string_view key,
                                           std::string map_entry_key,
                                           std::optional<std::string> value)
      -> std::expected<void, ConfigFileError> override;

 private:
  std::filesystem::path m_path;
};

[[nodiscard]] auto process_config_path()
    -> std::expected<std::filesystem::path, ConfigFileError>;

} // namespace aiforge::config
