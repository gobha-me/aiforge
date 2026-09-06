#pragma once

#include <chrono>
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
  automatic_approval_rules,
};

struct ConfigTextMapEntry {
  std::string key;
  std::string value;

  auto operator==(const ConfigTextMapEntry&) const -> bool = default;
};

using ConfigTextMap = std::vector<ConfigTextMapEntry>;

struct AutomaticApprovalRuleConstraintsConfig {
  std::vector<std::string> allowed_restrictions;
  std::uint64_t maximum_matches{};
  std::optional<std::uint64_t> expires_after_milliseconds;
  std::uint32_t precedence{};

  auto operator==(const AutomaticApprovalRuleConstraintsConfig&) const
      -> bool = default;
};

struct ExactAutomaticApprovalRuleConfig {
  std::string tool_name;
  // Strict JSON is canonicalized by the file adapter and validated again by
  // the runtime matcher compiler. No JSON-library type crosses this boundary.
  std::string canonical_arguments_json;
  AutomaticApprovalRuleConstraintsConfig constraints;

  auto operator==(const ExactAutomaticApprovalRuleConfig&) const
      -> bool = default;
};

struct RepositoryPathAutomaticApprovalRuleConfig {
  std::string tool_name;
  std::string allowed_relative_path;
  AutomaticApprovalRuleConstraintsConfig constraints;

  auto operator==(const RepositoryPathAutomaticApprovalRuleConfig&) const
      -> bool = default;
};

using AutomaticApprovalRuleConfig =
    std::variant<ExactAutomaticApprovalRuleConfig,
                 RepositoryPathAutomaticApprovalRuleConfig>;

struct AutomaticApprovalRulesConfig {
  std::vector<AutomaticApprovalRuleConfig> rules;

  auto operator==(const AutomaticApprovalRulesConfig&) const -> bool = default;
};

using ConfigValue = std::variant<bool, std::int64_t, std::uint64_t, std::string,
                                 std::vector<std::string>, ConfigTextMap,
                                 AutomaticApprovalRulesConfig>;

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
inline constexpr std::string_view image_tool_model_key{"tools.image.model"};
inline constexpr std::string_view automatic_approval_rules_key{
    "tools.approval.automatic_rules"};
inline constexpr std::string_view user_global_instructions_enabled_key{
    "instructions.global.enabled"};
inline constexpr std::string_view process_executables_key{
    "tools.process.executables"};
inline constexpr std::string_view process_readable_roots_key{
    "tools.process.readable_roots"};
inline constexpr std::string_view process_writable_roots_key{
    "tools.process.writable_roots"};
inline constexpr std::string_view process_environment_key{
    "tools.process.environment"};
inline constexpr std::string_view process_unrestricted_network_key{
    "tools.process.unrestricted_network"};
inline constexpr std::string_view
    process_allowlist_automatic_approval_maximum_matches_key{
        "tools.process.allowlist_automatic_approval.maximum_matches"};
inline constexpr std::string_view process_limit_executables_key{
    "tools.process.limits.executables"};
inline constexpr std::string_view process_limit_arguments_key{
    "tools.process.limits.arguments"};
inline constexpr std::string_view process_limit_argument_bytes_key{
    "tools.process.limits.argument_bytes"};
inline constexpr std::string_view process_limit_roots_key{
    "tools.process.limits.roots"};
inline constexpr std::string_view process_limit_environment_variables_key{
    "tools.process.limits.environment_variables"};
inline constexpr std::string_view process_limit_timeout_ms_key{
    "tools.process.limits.timeout_ms"};
inline constexpr std::string_view process_limit_output_bytes_key{
    "tools.process.limits.output_bytes"};
inline constexpr std::string_view process_limit_inline_output_bytes_key{
    "tools.process.limits.inline_output_bytes"};
inline constexpr std::string_view process_limit_progress_chunk_bytes_key{
    "tools.process.limits.progress_chunk_bytes"};
inline constexpr std::string_view process_limit_progress_events_key{
    "tools.process.limits.progress_events"};
inline constexpr std::string_view process_limit_termination_grace_ms_key{
    "tools.process.limits.termination_grace_ms"};

struct ProcessConfigLimits {
  std::size_t executables{64};
  std::size_t arguments{256};
  std::size_t argument_bytes{std::size_t{256} * 1024U};
  std::size_t roots{64};
  std::size_t environment_variables{64};
  std::chrono::milliseconds timeout{std::chrono::seconds{120}};
  std::size_t output_bytes{std::size_t{8} * 1024U * 1024U};
  std::size_t inline_output_bytes{std::size_t{32} * 1024U};
  std::size_t progress_chunk_bytes{std::size_t{4} * 1024U};
  std::size_t progress_events{64};
  std::chrono::milliseconds termination_grace{std::chrono::milliseconds{100}};
  auto operator==(const ProcessConfigLimits&) const -> bool = default;
};

struct ProcessConfigSettings {
  std::vector<std::string> executable_allowlist;
  std::vector<std::string> readable_roots;
  std::vector<std::string> writable_roots;
  // Names are inherited from the application environment by the production
  // adapter. Values never enter configuration or configuration provenance.
  std::vector<std::string> inherited_environment_names;
  ProcessConfigLimits limits{};
  bool unrestricted_network{};
  std::optional<std::uint64_t> allowlist_automatic_approval_maximum_matches;
  auto operator==(const ProcessConfigSettings&) const -> bool = default;
};

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

// Absence disables the paid image tool. A present value is one exact model ID;
// model catalog capability and pricing checks remain a runtime concern.
[[nodiscard]] auto resolve_image_tool_model(const ResolvedConfig& resolved)
    -> std::expected<std::optional<domain::ModelId>, ConfigDiagnostic>;

// Missing configuration is the explicit deny-all automatic policy. Invalid
// values anywhere in this security-sensitive namespace fail closed instead of
// falling through to the empty policy.
[[nodiscard]] auto resolve_automatic_approval_rules(
    const ResolvedConfig& resolved)
    -> std::expected<AutomaticApprovalRulesConfig, ConfigDiagnostic>;

// An absent value means the process tool is not configured. Process authority
// is file-backed only: command-line and environment candidates are rejected.
// Launch restriction and approval mode remain separate application-lifetime
// controls and are intentionally absent from this value.
[[nodiscard]] auto resolve_process_config_settings(
    const ResolvedConfig& resolved)
    -> std::expected<std::optional<ProcessConfigSettings>, ConfigDiagnostic>;

} // namespace aiforge::config
