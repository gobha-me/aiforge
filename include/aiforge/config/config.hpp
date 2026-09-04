#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <aiforge/domain/ids.hpp>

namespace aiforge::config {

enum class ConfigValueKind {
  boolean,
  signed_integer,
  unsigned_integer,
  text,
  text_list,
  text_map,
};

struct ConfigTextMapEntry {
  std::string key;
  std::string value;

  auto operator==(const ConfigTextMapEntry&) const -> bool = default;
};

using ConfigTextMap = std::vector<ConfigTextMapEntry>;

using ConfigValue = std::variant<bool, std::int64_t, std::uint64_t, std::string,
                                 std::vector<std::string>, ConfigTextMap>;

enum class ConfigSource {
  command_line,
  environment,
  file,
  compiled_default,
};

struct ConfigKeySpec {
  std::string id;
  ConfigValueKind value_kind{ConfigValueKind::text};
  std::optional<std::string> environment_name;
  std::optional<ConfigValue> compiled_default;
  bool sensitive{};
  bool file_writable{true};
  std::size_t maximum_text_bytes{64U * 1024U};
  std::size_t maximum_list_items{256};

  auto operator==(const ConfigKeySpec&) const -> bool = default;
};

struct ConfigRegistry {
  std::vector<ConfigKeySpec> keys;
};

enum class ConfigDiagnosticCode {
  invalid_registry,
  duplicate_key,
  duplicate_environment_binding,
  unknown_key,
  invalid_value,
  value_too_large,
  too_many_values,
  sensitive_value,
  duplicate_source_value,
  source_warning,
};

struct ConfigDiagnostic {
  ConfigDiagnosticCode code{ConfigDiagnosticCode::invalid_value};
  ConfigSource source{ConfigSource::compiled_default};
  std::string key;
  // Messages describe shape and policy only. They never contain source values.
  std::string message;

  auto operator==(const ConfigDiagnostic&) const -> bool = default;
};

struct ConfigCandidate {
  std::string key;
  std::optional<ConfigValue> value;
  std::optional<ConfigDiagnostic> rejection;
};

struct ConfigLayer {
  ConfigSource source{ConfigSource::file};
  std::vector<ConfigCandidate> candidates;
  std::vector<ConfigDiagnostic> diagnostics;
};

enum class CandidateDisposition {
  selected,
  shadowed,
  rejected,
};

struct ConfigDecision {
  ConfigSource source{ConfigSource::compiled_default};
  CandidateDisposition disposition{CandidateDisposition::rejected};
  std::optional<ConfigDiagnosticCode> diagnostic_code;

  auto operator==(const ConfigDecision&) const -> bool = default;
};

struct ResolvedConfigEntry {
  std::string key;
  std::optional<ConfigValue> value;
  std::optional<ConfigSource> source;
  bool sensitive{};
  std::vector<ConfigDecision> decisions;
};

struct ResolvedConfig {
  std::vector<ResolvedConfigEntry> entries;
  std::vector<ConfigDiagnostic> diagnostics;

  [[nodiscard]] auto find(std::string_view key) const
      -> const ResolvedConfigEntry*;
};

[[nodiscard]] auto validate_registry(const ConfigRegistry& registry)
    -> std::expected<void, ConfigDiagnostic>;

[[nodiscard]] auto validate_config_value(const ConfigKeySpec& spec,
                                         const ConfigValue& value,
                                         ConfigSource source)
    -> std::expected<void, ConfigDiagnostic>;

[[nodiscard]] auto parse_config_value(const ConfigKeySpec& spec,
                                      std::span<const std::string_view> values,
                                      ConfigSource source)
    -> std::expected<ConfigValue, ConfigDiagnostic>;

[[nodiscard]] auto format_config_value(const ConfigValue& value) -> std::string;
[[nodiscard]] auto config_source_name(ConfigSource source) -> std::string_view;

[[nodiscard]] auto resolve_config(const ConfigRegistry& registry,
                                  std::span<const ConfigLayer> layers)
    -> std::expected<ResolvedConfig, ConfigDiagnostic>;

[[nodiscard]] auto environment_config_layer(const ConfigRegistry& registry)
    -> std::expected<ConfigLayer, ConfigDiagnostic>;

[[nodiscard]] auto builtin_config_registry() -> const ConfigRegistry&;

inline constexpr std::string_view model_maximum_tool_profiles_key{
    "tools.models.maximum_profiles"};
inline constexpr std::string_view persona_maximum_tool_profiles_key{
    "tools.personas.maximum_profiles"};
inline constexpr std::string_view user_global_instructions_enabled_key{
    "instructions.global.enabled"};

struct ToolProfileMaximumMappings {
  std::map<domain::ModelId, domain::ToolProfileId> models;
  std::map<domain::PersonaId, domain::ToolProfileId> personas;

  auto operator==(const ToolProfileMaximumMappings&) const -> bool = default;
};

[[nodiscard]] auto resolve_tool_profile_maximum_mappings(
    const ResolvedConfig& resolved)
    -> std::expected<ToolProfileMaximumMappings, ConfigDiagnostic>;

[[nodiscard]] auto resolve_user_global_instructions_enabled(
    const ResolvedConfig& resolved) -> std::expected<bool, ConfigDiagnostic>;

} // namespace aiforge::config
