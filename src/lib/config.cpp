#include <aiforge/config/config.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <concepts>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <ranges>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include <aiforge/detail/utf8_text.hpp>
#include <aiforge/runtime/tool_profiles.hpp>

namespace aiforge::config {
namespace {

[[nodiscard]] auto diagnostic(const ConfigDiagnosticCode code,
                              const ConfigSource source, std::string key,
                              std::string message) -> ConfigDiagnostic {
  return {code, source, std::move(key), std::move(message)};
}

[[nodiscard]] auto valid_key_id(const std::string_view id) -> bool {
  if (id.empty() || id.front() == '.' || id.back() == '.') return false;
  bool component_start = true;
  for (const unsigned char character : id) {
    if (character == '.') {
      if (component_start) return false;
      component_start = true;
      continue;
    }
    if (component_start) {
      if ((character < 'a' || character > 'z') && character != '_')
        return false;
      component_start = false;
      continue;
    }
    if ((character < 'a' || character > 'z') &&
        (character < '0' || character > '9') && character != '_') {
      return false;
    }
  }
  return !component_start;
}

[[nodiscard]] auto kind_matches(const ConfigValueKind kind,
                                const ConfigValue& value) -> bool {
  switch (kind) {
    case ConfigValueKind::boolean: return std::holds_alternative<bool>(value);
    case ConfigValueKind::signed_integer:
      return std::holds_alternative<std::int64_t>(value);
    case ConfigValueKind::unsigned_integer:
      return std::holds_alternative<std::uint64_t>(value);
    case ConfigValueKind::text:
      return std::holds_alternative<std::string>(value);
    case ConfigValueKind::text_list:
      return std::holds_alternative<std::vector<std::string>>(value);
    case ConfigValueKind::text_map:
      return std::holds_alternative<ConfigTextMap>(value);
    case ConfigValueKind::automatic_approval_rules:
      return std::holds_alternative<AutomaticApprovalRulesConfig>(value);
  }
  return false;
}

[[nodiscard]] auto valid_utf8(const std::string_view value) -> bool {
  std::size_t index{};
  while (index < value.size()) {
    const auto lead = static_cast<unsigned char>(value[index]);
    std::size_t continuation_count{};
    std::uint32_t code_point{};
    if (lead <= 0x7fU) {
      ++index;
      continue;
    }
    if (lead >= 0xc2U && lead <= 0xdfU) {
      continuation_count = 1;
      code_point = lead & 0x1fU;
    } else if (lead >= 0xe0U && lead <= 0xefU) {
      continuation_count = 2;
      code_point = lead & 0x0fU;
    } else if (lead >= 0xf0U && lead <= 0xf4U) {
      continuation_count = 3;
      code_point = lead & 0x07U;
    } else {
      return false;
    }
    if (index + continuation_count >= value.size()) return false;
    for (std::size_t offset = 1; offset <= continuation_count; ++offset) {
      const auto byte = static_cast<unsigned char>(value[index + offset]);
      if ((byte & 0xc0U) != 0x80U) return false;
      code_point = (code_point << 6U) | (byte & 0x3fU);
    }
    if ((continuation_count == 2 && code_point < 0x800U) ||
        (continuation_count == 3 && code_point < 0x10000U) ||
        (code_point >= 0xd800U && code_point <= 0xdfffU) ||
        code_point > 0x10ffffU) {
      return false;
    }
    index += continuation_count + 1;
  }
  return true;
}

[[nodiscard]] auto valid_automatic_approval_constraints(
    const AutomaticApprovalRuleConstraintsConfig& constraints) -> bool {
  if (constraints.allowed_restrictions.empty() ||
      constraints.allowed_restrictions.size() > 4U ||
      constraints.maximum_matches == 0 ||
      (constraints.expires_after_milliseconds &&
       *constraints.expires_after_milliseconds == 0)) {
    return false;
  }
  std::unordered_set<std::string_view> restrictions;
  for (const auto& restriction : constraints.allowed_restrictions) {
    if ((restriction != "none" && restriction != "low" &&
         restriction != "medium" && restriction != "high") ||
        !restrictions.insert(restriction).second) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] auto valid_automatic_approval_rules(
    const AutomaticApprovalRulesConfig& configuration,
    const ConfigKeySpec& spec) -> bool {
  if (configuration.rules.size() > spec.maximum_list_items) return false;
  return std::ranges::all_of(configuration.rules, [&](const auto& rule) {
    return std::visit(
        [&]<typename Rule>(const Rule& concrete) {
          if (concrete.tool_name.empty() ||
              concrete.tool_name.size() > spec.maximum_text_bytes ||
              !detail::is_safe_utf8_text(concrete.tool_name) ||
              concrete.tool_name.find_first_of("\r\n\t") != std::string::npos ||
              !valid_automatic_approval_constraints(concrete.constraints)) {
            return false;
          }
          if constexpr (std::same_as<Rule, ExactAutomaticApprovalRuleConfig>) {
            return !concrete.canonical_arguments_json.empty() &&
                   concrete.canonical_arguments_json.size() <=
                       spec.maximum_text_bytes &&
                   valid_utf8(concrete.canonical_arguments_json);
          } else {
            return concrete.tool_name == "read_repository_file" &&
                   concrete.allowed_relative_path.size() <=
                       spec.maximum_text_bytes &&
                   detail::is_safe_utf8_text(concrete.allowed_relative_path) &&
                   concrete.allowed_relative_path.find_first_of("\r\n\t") ==
                       std::string::npos;
          }
        },
        rule);
  });
}

// clang-format off
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Closed variants require explicit bounded validation branches.
[[nodiscard]] auto validate_value(const ConfigKeySpec& spec,
                                  const ConfigValue& value,
                                  const ConfigSource source)
    -> std::expected<void, ConfigDiagnostic> {
  // clang-format on
  if (!kind_matches(spec.value_kind, value)) {
    return std::unexpected(
        diagnostic(ConfigDiagnosticCode::invalid_value, source, spec.id,
                   "the value type does not match the configuration key"));
  }
  if (spec.value_kind == ConfigValueKind::text_map &&
      source != ConfigSource::file) {
    return std::unexpected(
        diagnostic(ConfigDiagnosticCode::invalid_value, source, spec.id,
                   "configuration maps are supported only by the file source"));
  }
  if (spec.value_kind == ConfigValueKind::automatic_approval_rules &&
      source != ConfigSource::file &&
      source != ConfigSource::compiled_default) {
    return std::unexpected(diagnostic(
        ConfigDiagnosticCode::invalid_value, source, spec.id,
        "automatic approval rules are supported only by the file source"));
  }
  if (const auto* text = std::get_if<std::string>(&value);
      text != nullptr && text->size() > spec.maximum_text_bytes) {
    return std::unexpected(diagnostic(ConfigDiagnosticCode::value_too_large,
                                      source, spec.id,
                                      "the text value exceeds its byte limit"));
  }
  if (const auto* text = std::get_if<std::string>(&value);
      text != nullptr && !valid_utf8(*text)) {
    return std::unexpected(diagnostic(ConfigDiagnosticCode::invalid_value,
                                      source, spec.id,
                                      "the text value is not valid UTF-8"));
  }
  if (const auto* list = std::get_if<std::vector<std::string>>(&value)) {
    if (list->size() > spec.maximum_list_items) {
      return std::unexpected(diagnostic(ConfigDiagnosticCode::too_many_values,
                                        source, spec.id,
                                        "the list exceeds its item limit"));
    }
    if (std::ranges::any_of(*list, [&](const auto& item) {
          return item.size() > spec.maximum_text_bytes;
        })) {
      return std::unexpected(diagnostic(ConfigDiagnosticCode::value_too_large,
                                        source, spec.id,
                                        "a list item exceeds its byte limit"));
    }
    if (std::ranges::any_of(
            *list, [](const auto& item) { return !valid_utf8(item); })) {
      return std::unexpected(diagnostic(ConfigDiagnosticCode::invalid_value,
                                        source, spec.id,
                                        "a list item is not valid UTF-8"));
    }
  }
  if (const auto* map = std::get_if<ConfigTextMap>(&value)) {
    if (map->size() > spec.maximum_list_items) {
      return std::unexpected(diagnostic(ConfigDiagnosticCode::too_many_values,
                                        source, spec.id,
                                        "the map exceeds its entry limit"));
    }
    std::unordered_set<std::string_view> keys;
    for (const auto& entry : *map) {
      if (entry.key.size() > spec.maximum_text_bytes ||
          entry.value.size() > spec.maximum_text_bytes) {
        return std::unexpected(
            diagnostic(ConfigDiagnosticCode::value_too_large, source, spec.id,
                       "a map key or value exceeds its byte limit"));
      }
      if (!detail::is_safe_utf8_text(entry.key) ||
          !detail::is_safe_utf8_text(entry.value) ||
          entry.key.find_first_of("\r\n\t") != std::string::npos ||
          entry.value.find_first_of("\r\n\t") != std::string::npos) {
        return std::unexpected(
            diagnostic(ConfigDiagnosticCode::invalid_value, source, spec.id,
                       "a map key or value is not safe single-line UTF-8"));
      }
      if (!keys.insert(entry.key).second) {
        return std::unexpected(
            diagnostic(ConfigDiagnosticCode::invalid_value, source, spec.id,
                       "configuration map keys must be unique"));
      }
    }
  }
  if (const auto* rules = std::get_if<AutomaticApprovalRulesConfig>(&value);
      rules != nullptr && !valid_automatic_approval_rules(*rules, spec)) {
    return std::unexpected(
        diagnostic(ConfigDiagnosticCode::invalid_value, source, spec.id,
                   "automatic approval rules are malformed or overbound"));
  }
  return {};
}

template <typename Integer>
[[nodiscard]] auto parse_integer(const std::string_view value)
    -> std::optional<Integer> {
  Integer result{};
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), result, 10);
  if (error != std::errc{} || end != value.data() + value.size()) {
    return std::nullopt;
  }
  return result;
}

[[nodiscard]] auto rank(const ConfigSource source) -> int {
  switch (source) {
    case ConfigSource::command_line: return 4;
    case ConfigSource::environment: return 3;
    case ConfigSource::file: return 2;
    case ConfigSource::compiled_default: return 1;
  }
  return 0;
}

[[nodiscard]] auto find_spec(const ConfigRegistry& registry,
                             const std::string_view key)
    -> const ConfigKeySpec* {
  const auto found = std::ranges::find(registry.keys, key, &ConfigKeySpec::id);
  return found == registry.keys.end() ? nullptr : &*found;
}

[[nodiscard]] auto process_namespace_key(const std::string_view key) -> bool {
  constexpr std::string_view prefix{"tools.process"};
  return key == prefix ||
         (key.size() > prefix.size() && key.starts_with(prefix) &&
          key[prefix.size()] == '.');
}

template <typename Value>
[[nodiscard]] auto process_config_value(const ResolvedConfig& resolved,
                                        const std::string_view key)
    -> std::expected<const Value*, ConfigDiagnostic> {
  const auto* entry = resolved.find(key);
  if (entry == nullptr) {
    return std::unexpected(diagnostic(
        ConfigDiagnosticCode::invalid_registry, ConfigSource::compiled_default,
        std::string{key}, "a process configuration key is missing"));
  }
  if (!entry->value) return nullptr;
  const auto source = entry->source.value_or(ConfigSource::compiled_default);
  if (source != ConfigSource::file &&
      source != ConfigSource::compiled_default) {
    return std::unexpected(diagnostic(
        ConfigDiagnosticCode::invalid_value, source, std::string{key},
        "process capability configuration is file-backed only"));
  }
  const auto* value = std::get_if<Value>(&*entry->value);
  if (value == nullptr) {
    return std::unexpected(diagnostic(
        ConfigDiagnosticCode::invalid_value, source, std::string{key},
        "a process configuration value has the wrong type"));
  }
  return value;
}

[[nodiscard]] auto valid_process_path(const std::string& value) -> bool {
  if (value.empty() || value.size() > 4096U ||
      !detail::is_safe_utf8_text(value) ||
      value.find_first_of("\r\n\t") != std::string::npos) {
    return false;
  }
  const std::filesystem::path path{value};
  return path.is_absolute() && path.generic_string() == value &&
         path.lexically_normal().generic_string() == value;
}

[[nodiscard]] auto valid_environment_name(const std::string_view name) -> bool {
  if (name.empty() || name.size() > 255U || name.front() == '=' ||
      (name.front() >= '0' && name.front() <= '9')) {
    return false;
  }
  return std::ranges::all_of(name, [](const unsigned char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '_';
  });
}

template <typename Value>
[[nodiscard]] auto unique_values(const std::vector<Value>& values) -> bool {
  std::unordered_set<Value> unique;
  return std::ranges::all_of(
      values, [&](const auto& value) { return unique.insert(value).second; });
}

[[nodiscard]] auto path_is_within(const std::string_view parent_text,
                                  const std::string_view child_text) -> bool {
  const std::filesystem::path parent{parent_text};
  const std::filesystem::path child{child_text};
  auto parent_part = parent.begin();
  auto child_part = child.begin();
  for (; parent_part != parent.end() && child_part != child.end();
       ++parent_part, ++child_part) {
    if (*parent_part != *child_part) return false;
  }
  return parent_part == parent.end();
}

} // namespace

auto validate_config_value(const ConfigKeySpec& spec, const ConfigValue& value,
                           const ConfigSource source)
    -> std::expected<void, ConfigDiagnostic> {
  return validate_value(spec, value, source);
}

auto ResolvedConfig::find(const std::string_view key) const
    -> const ResolvedConfigEntry* {
  const auto found = std::ranges::find(entries, key, &ResolvedConfigEntry::key);
  return found == entries.end() ? nullptr : &*found;
}

auto validate_registry(const ConfigRegistry& registry)
    -> std::expected<void, ConfigDiagnostic> {
  std::unordered_set<std::string> ids;
  std::unordered_set<std::string> environment_names;
  for (const auto& spec : registry.keys) {
    if (!valid_key_id(spec.id) || spec.maximum_text_bytes == 0 ||
        spec.maximum_list_items == 0) {
      return std::unexpected(
          diagnostic(ConfigDiagnosticCode::invalid_registry,
                     ConfigSource::compiled_default, spec.id,
                     "a configuration key specification is invalid"));
    }
    if (!ids.insert(spec.id).second) {
      return std::unexpected(diagnostic(
          ConfigDiagnosticCode::duplicate_key, ConfigSource::compiled_default,
          spec.id, "configuration key IDs must be unique"));
    }
    if (spec.environment_name &&
        (spec.environment_name->empty() ||
         !environment_names.insert(*spec.environment_name).second)) {
      return std::unexpected(
          diagnostic(ConfigDiagnosticCode::duplicate_environment_binding,
                     ConfigSource::compiled_default, spec.id,
                     "environment bindings must be nonempty and unique"));
    }
    if (spec.compiled_default) {
      if (auto valid = validate_value(spec, *spec.compiled_default,
                                      ConfigSource::compiled_default);
          !valid) {
        return std::unexpected(std::move(valid.error()));
      }
    }
    if (spec.sensitive && spec.file_writable) {
      return std::unexpected(diagnostic(
          ConfigDiagnosticCode::invalid_registry,
          ConfigSource::compiled_default, spec.id,
          "sensitive keys cannot be writable configuration-file values"));
    }
  }
  return {};
}

auto parse_config_value(const ConfigKeySpec& spec,
                        const std::span<const std::string_view> values,
                        const ConfigSource source)
    -> std::expected<ConfigValue, ConfigDiagnostic> {
  const auto invalid = [&]() {
    return std::unexpected(
        diagnostic(ConfigDiagnosticCode::invalid_value, source, spec.id,
                   "the value is invalid for the configuration key"));
  };
  // Structured values have no delimiter-based command-line or environment
  // grammar. They are accepted only as native values by the JSON adapter.
  if (spec.value_kind == ConfigValueKind::text_map ||
      spec.value_kind == ConfigValueKind::automatic_approval_rules) {
    return invalid();
  }
  if (spec.value_kind != ConfigValueKind::text_list && values.size() != 1) {
    return invalid();
  }

  ConfigValue parsed;
  switch (spec.value_kind) {
    case ConfigValueKind::boolean: {
      std::string normalized{values.front()};
      std::ranges::transform(normalized, normalized.begin(),
                             [](const unsigned char value) {
                               return static_cast<char>(std::tolower(value));
                             });
      if (normalized == "true" || normalized == "1" || normalized == "on" ||
          normalized == "yes") {
        parsed = true;
      } else if (normalized == "false" || normalized == "0" ||
                 normalized == "off" || normalized == "no") {
        parsed = false;
      } else {
        return invalid();
      }
      break;
    }
    case ConfigValueKind::signed_integer: {
      const auto integer = parse_integer<std::int64_t>(values.front());
      if (!integer) return invalid();
      parsed = *integer;
      break;
    }
    case ConfigValueKind::unsigned_integer: {
      const auto integer = parse_integer<std::uint64_t>(values.front());
      if (!integer) return invalid();
      parsed = *integer;
      break;
    }
    case ConfigValueKind::text: parsed = std::string{values.front()}; break;
    case ConfigValueKind::text_list: {
      std::vector<std::string> list;
      list.reserve(values.size());
      for (const auto value : values)
        list.emplace_back(value);
      parsed = std::move(list);
      break;
    }
    case ConfigValueKind::text_map:
    case ConfigValueKind::automatic_approval_rules: return invalid();
  }
  if (auto valid = validate_value(spec, parsed, source); !valid) {
    return std::unexpected(std::move(valid.error()));
  }
  return parsed;
}

// clang-format off
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Closed variants retain explicit deterministic text formats.
auto format_config_value(const ConfigValue& value) -> std::string {
  // clang-format on
  return std::visit(
      []<typename Value>(const Value& concrete) -> std::string {
        if constexpr (std::same_as<Value, bool>) {
          return concrete ? "true" : "false";
        } else if constexpr (std::same_as<Value, std::string>) {
          return concrete;
        } else if constexpr (std::same_as<Value, std::vector<std::string>>) {
          std::string result;
          for (const auto& item : concrete) {
            if (!result.empty()) result.append(",");
            result.append(item);
          }
          return result;
        } else if constexpr (std::same_as<Value, ConfigTextMap>) {
          std::string result;
          for (const auto& entry : concrete) {
            if (!result.empty()) result.append(",");
            result.append(entry.key);
            result.append("=");
            result.append(entry.value);
          }
          return result;
        } else if constexpr (std::same_as<Value,
                                          AutomaticApprovalRulesConfig>) {
          return std::to_string(concrete.rules.size()) +
                 (concrete.rules.size() == 1U ? " rule" : " rules");
        } else {
          return std::to_string(concrete);
        }
      },
      value);
}

auto config_source_name(const ConfigSource source) -> std::string_view {
  switch (source) {
    case ConfigSource::command_line: return "command line";
    case ConfigSource::environment: return "environment";
    case ConfigSource::file: return "file";
    case ConfigSource::compiled_default: return "default";
  }
  return "unknown";
}

auto resolve_config(const ConfigRegistry& registry,
                    const std::span<const ConfigLayer> layers)
    -> std::expected<ResolvedConfig, ConfigDiagnostic> {
  if (auto valid = validate_registry(registry); !valid) {
    return std::unexpected(std::move(valid.error()));
  }

  ResolvedConfig result;
  result.entries.reserve(registry.keys.size());
  for (const auto& spec : registry.keys) {
    result.entries.push_back(
        {spec.id, std::nullopt, std::nullopt, spec.sensitive, {}});
  }

  struct RankedCandidate {
    ConfigSource source;
    const ConfigCandidate* candidate;
  };
  std::unordered_map<std::string, std::vector<RankedCandidate>> candidates;
  std::unordered_set<std::string> source_keys;

  for (const auto& layer : layers) {
    result.diagnostics.insert(result.diagnostics.end(),
                              layer.diagnostics.begin(),
                              layer.diagnostics.end());
    for (const auto& candidate : layer.candidates) {
      const auto* spec = find_spec(registry, candidate.key);
      if (spec == nullptr) {
        auto error = diagnostic(
            ConfigDiagnosticCode::unknown_key, layer.source, candidate.key,
            "the source contains an unknown configuration key");
        if (layer.source == ConfigSource::command_line) {
          return std::unexpected(std::move(error));
        }
        result.diagnostics.push_back(std::move(error));
        continue;
      }
      const auto identity =
          std::to_string(rank(layer.source)) + "\n" + candidate.key;
      if (!source_keys.insert(identity).second) {
        auto error = diagnostic(
            ConfigDiagnosticCode::duplicate_source_value, layer.source,
            candidate.key,
            "the source supplied the configuration key more than once");
        if (layer.source == ConfigSource::command_line) {
          return std::unexpected(std::move(error));
        }
        result.diagnostics.push_back(std::move(error));
        continue;
      }
      candidates[candidate.key].push_back({layer.source, &candidate});
    }
  }

  for (std::size_t index = 0; index < registry.keys.size(); ++index) {
    const auto& spec = registry.keys[index];
    auto& entry = result.entries[index];
    auto key_candidates = std::move(candidates[spec.id]);
    if (spec.compiled_default) {
      static_cast<void>(key_candidates.emplace_back(
          RankedCandidate{ConfigSource::compiled_default, nullptr}));
    }
    std::ranges::sort(key_candidates, [](const auto& left, const auto& right) {
      return rank(left.source) > rank(right.source);
    });

    bool selected{};
    for (const auto& ranked : key_candidates) {
      if (ranked.source == ConfigSource::compiled_default) {
        if (!selected) {
          entry.value = spec.compiled_default;
          entry.source = ConfigSource::compiled_default;
          entry.decisions.push_back(
              {ranked.source, CandidateDisposition::selected, std::nullopt});
          selected = true;
        } else {
          entry.decisions.push_back(
              {ranked.source, CandidateDisposition::shadowed, std::nullopt});
        }
        continue;
      }
      const auto& candidate = *ranked.candidate;
      if (candidate.rejection) {
        auto error = *candidate.rejection;
        error.source = ranked.source;
        error.key = spec.id;
        if (ranked.source == ConfigSource::command_line) {
          return std::unexpected(std::move(error));
        }
        result.diagnostics.push_back(error);
        entry.decisions.push_back(
            {ranked.source, CandidateDisposition::rejected, error.code});
        continue;
      }
      if (!candidate.value) {
        auto error =
            diagnostic(ConfigDiagnosticCode::invalid_value, ranked.source,
                       spec.id, "the source candidate has no value");
        if (ranked.source == ConfigSource::command_line) {
          return std::unexpected(std::move(error));
        }
        result.diagnostics.push_back(error);
        entry.decisions.push_back(
            {ranked.source, CandidateDisposition::rejected, error.code});
        continue;
      }
      if (spec.sensitive && ranked.source == ConfigSource::file) {
        auto error = diagnostic(
            ConfigDiagnosticCode::sensitive_value, ranked.source, spec.id,
            "sensitive values are excluded from configuration files");
        result.diagnostics.push_back(error);
        entry.decisions.push_back(
            {ranked.source, CandidateDisposition::rejected, error.code});
        continue;
      }
      if (auto valid = validate_value(spec, *candidate.value, ranked.source);
          !valid) {
        auto error = std::move(valid.error());
        if (ranked.source == ConfigSource::command_line) {
          return std::unexpected(std::move(error));
        }
        result.diagnostics.push_back(error);
        entry.decisions.push_back(
            {ranked.source, CandidateDisposition::rejected, error.code});
        continue;
      }
      if (!selected) {
        entry.value = candidate.value;
        entry.source = ranked.source;
        entry.decisions.push_back(
            {ranked.source, CandidateDisposition::selected, std::nullopt});
        selected = true;
      } else {
        entry.decisions.push_back(
            {ranked.source, CandidateDisposition::shadowed, std::nullopt});
      }
    }
  }
  return result;
}

auto environment_config_layer(const ConfigRegistry& registry)
    -> std::expected<ConfigLayer, ConfigDiagnostic> {
  if (auto valid = validate_registry(registry); !valid) {
    return std::unexpected(std::move(valid.error()));
  }
  ConfigLayer layer{ConfigSource::environment, {}, {}};
  for (const auto& spec : registry.keys) {
    if (!spec.environment_name) continue;
    const auto* raw = std::getenv(spec.environment_name->c_str());
    if (raw == nullptr) continue;
    std::string storage{raw};
    std::vector<std::string> list_storage;
    std::vector<std::string_view> values;
    if (spec.value_kind == ConfigValueKind::text_list) {
      if (!storage.empty()) {
        std::size_t start{};
        while (start <= storage.size()) {
          const auto end = storage.find(',', start);
          list_storage.emplace_back(storage.substr(start, end - start));
          if (end == std::string::npos) break;
          start = end + 1;
        }
      }
      values.reserve(list_storage.size());
      for (const auto& value : list_storage)
        values.push_back(value);
    } else {
      values.push_back(storage);
    }
    auto parsed = parse_config_value(spec, values, ConfigSource::environment);
    if (parsed) {
      layer.candidates.push_back({spec.id, std::move(*parsed), std::nullopt});
    } else {
      layer.candidates.push_back(
          {spec.id, std::nullopt, std::move(parsed.error())});
    }
  }
  return layer;
}

auto builtin_config_registry() -> const ConfigRegistry& {
  static const ConfigRegistry registry{{
      {"model", ConfigValueKind::text, std::string{"AIFORGE_MODEL"},
       std::nullopt, false, true, 1024, 1},
      {"venice.web_search", ConfigValueKind::text,
       std::string{"AIFORGE_VENICE_WEB_SEARCH"}, std::nullopt, false, true, 4,
       1},
      {"venice.include_system_prompt", ConfigValueKind::boolean,
       std::string{"AIFORGE_VENICE_INCLUDE_SYSTEM_PROMPT"}, std::nullopt, false,
       true, 5, 1},
      {std::string{user_global_instructions_enabled_key},
       ConfigValueKind::boolean,
       std::string{"AIFORGE_INSTRUCTIONS_GLOBAL_ENABLED"}, ConfigValue{true},
       false, true, 5, 1},
      {"memory.global.capture", ConfigValueKind::text,
       std::string{"AIFORGE_MEMORY_GLOBAL_CAPTURE"},
       ConfigValue{std::string{"off"}}, false, true, 16, 1},
      {"memory.project.capture", ConfigValueKind::text,
       std::string{"AIFORGE_MEMORY_PROJECT_CAPTURE"},
       ConfigValue{std::string{"review"}}, false, true, 16, 1},
      {"memory.persona.capture", ConfigValueKind::text,
       std::string{"AIFORGE_MEMORY_PERSONA_CAPTURE"},
       ConfigValue{std::string{"review"}}, false, true, 16, 1},
      {"memory.context.max_tokens", ConfigValueKind::unsigned_integer,
       std::string{"AIFORGE_MEMORY_CONTEXT_MAX_TOKENS"},
       ConfigValue{std::uint64_t{2048}}, false, true, 32, 1},
      {std::string{process_executables_key}, ConfigValueKind::text_list,
       std::nullopt, std::nullopt, false, true, 4096, 64},
      {std::string{process_readable_roots_key}, ConfigValueKind::text_list,
       std::nullopt, std::nullopt, false, true, 4096, 64},
      {std::string{process_writable_roots_key}, ConfigValueKind::text_list,
       std::nullopt, std::nullopt, false, true, 4096, 64},
      {std::string{process_environment_key}, ConfigValueKind::text_list,
       std::nullopt, std::nullopt, false, true, 255, 64},
      {std::string{process_unrestricted_network_key}, ConfigValueKind::boolean,
       std::nullopt, ConfigValue{false}, false, true, 5, 1},
      {std::string{process_allowlist_automatic_approval_maximum_matches_key},
       ConfigValueKind::unsigned_integer, std::nullopt,
       ConfigValue{std::uint64_t{0}}, false, true, 32, 1},
      {std::string{process_limit_executables_key},
       ConfigValueKind::unsigned_integer, std::nullopt,
       ConfigValue{std::uint64_t{64}}, false, true, 32, 1},
      {std::string{process_limit_arguments_key},
       ConfigValueKind::unsigned_integer, std::nullopt,
       ConfigValue{std::uint64_t{256}}, false, true, 32, 1},
      {std::string{process_limit_argument_bytes_key},
       ConfigValueKind::unsigned_integer, std::nullopt,
       ConfigValue{std::uint64_t{256} * 1024U}, false, true, 32, 1},
      {std::string{process_limit_roots_key}, ConfigValueKind::unsigned_integer,
       std::nullopt, ConfigValue{std::uint64_t{64}}, false, true, 32, 1},
      {std::string{process_limit_environment_variables_key},
       ConfigValueKind::unsigned_integer, std::nullopt,
       ConfigValue{std::uint64_t{64}}, false, true, 32, 1},
      {std::string{process_limit_timeout_ms_key},
       ConfigValueKind::unsigned_integer, std::nullopt,
       ConfigValue{std::uint64_t{120'000}}, false, true, 32, 1},
      {std::string{process_limit_output_bytes_key},
       ConfigValueKind::unsigned_integer, std::nullopt,
       ConfigValue{std::uint64_t{8} * 1024U * 1024U}, false, true, 32, 1},
      {std::string{process_limit_inline_output_bytes_key},
       ConfigValueKind::unsigned_integer, std::nullopt,
       ConfigValue{std::uint64_t{32} * 1024U}, false, true, 32, 1},
      {std::string{process_limit_progress_chunk_bytes_key},
       ConfigValueKind::unsigned_integer, std::nullopt,
       ConfigValue{std::uint64_t{4} * 1024U}, false, true, 32, 1},
      {std::string{process_limit_progress_events_key},
       ConfigValueKind::unsigned_integer, std::nullopt,
       ConfigValue{std::uint64_t{64}}, false, true, 32, 1},
      {std::string{process_limit_termination_grace_ms_key},
       ConfigValueKind::unsigned_integer, std::nullopt,
       ConfigValue{std::uint64_t{100}}, false, true, 32, 1},
      {std::string{model_maximum_tool_profiles_key}, ConfigValueKind::text_map,
       std::nullopt, std::nullopt, false, true, domain::ModelId::max_size, 256},
      {std::string{persona_maximum_tool_profiles_key},
       ConfigValueKind::text_map, std::nullopt, std::nullopt, false, true,
       domain::PersonaId::max_size, 256},
      {std::string{image_tool_model_key}, ConfigValueKind::text,
       std::string{"AIFORGE_TOOLS_IMAGE_MODEL"}, std::nullopt, false, true,
       domain::ModelId::max_size, 1},
      {std::string{automatic_approval_rules_key},
       ConfigValueKind::automatic_approval_rules, std::nullopt, std::nullopt,
       false, true, std::size_t{64U} * 1024U, 256},
  }};
  return registry;
}

// clang-format off
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Both identity namespaces are validated before associations are returned.
auto resolve_tool_profile_maximum_mappings(const ResolvedConfig& resolved)
    -> std::expected<ToolProfileMaximumMappings, ConfigDiagnostic> {
  // clang-format on
  constexpr std::array keys{model_maximum_tool_profiles_key,
                            persona_maximum_tool_profiles_key};
  const auto affects_mapping_namespace = [&](const std::string_view key) {
    if (std::ranges::find(keys, key) != keys.end()) return true;
    constexpr std::array prefixes{std::string_view{"tools.models"},
                                  std::string_view{"tools.personas"}};
    return std::ranges::any_of(prefixes, [&](const auto prefix) {
      return key == prefix ||
             (key.size() > prefix.size() && key.starts_with(prefix) &&
              key[prefix.size()] == '.');
    });
  };
  for (const auto& issue : resolved.diagnostics) {
    if (affects_mapping_namespace(issue.key)) return std::unexpected(issue);
  }

  const auto parse_profile = [](const std::string& value,
                                const ConfigSource source,
                                const std::string_view key)
      -> std::expected<domain::ToolProfileId, ConfigDiagnostic> {
    auto profile_id = domain::ToolProfileId::from(value);
    if (!profile_id ||
        std::ranges::find(runtime::builtin_tool_profiles(), *profile_id,
                          &runtime::ToolProfile::profile_id) ==
            runtime::builtin_tool_profiles().end()) {
      return std::unexpected(diagnostic(
          ConfigDiagnosticCode::invalid_value, source, std::string{key},
          "a maximum tool profile reference is not a built-in profile"));
    }
    return std::move(*profile_id);
  };

  ToolProfileMaximumMappings result;
  const auto parse =
      [&]<typename Id>(const std::string_view key,
                       std::map<Id, domain::ToolProfileId>& destination)
      -> std::expected<void, ConfigDiagnostic> {
    const auto* entry = resolved.find(key);
    if (entry == nullptr) {
      return std::unexpected(
          diagnostic(ConfigDiagnosticCode::invalid_registry,
                     ConfigSource::compiled_default, std::string{key},
                     "the maximum tool profile configuration key is missing"));
    }
    if (!entry->value) return {};
    const auto source = entry->source.value_or(ConfigSource::file);
    const auto& registry = builtin_config_registry();
    const auto spec = std::ranges::find(registry.keys, key, &ConfigKeySpec::id);
    if (spec == registry.keys.end()) {
      return std::unexpected(diagnostic(
          ConfigDiagnosticCode::invalid_registry,
          ConfigSource::compiled_default, std::string{key},
          "the maximum tool profile configuration key is unregistered"));
    }
    if (auto valid = validate_config_value(*spec, *entry->value, source);
        !valid) {
      return std::unexpected(std::move(valid.error()));
    }
    const auto* mappings = std::get_if<ConfigTextMap>(&*entry->value);
    if (mappings == nullptr) {
      return std::unexpected(diagnostic(
          ConfigDiagnosticCode::invalid_value, source, std::string{key},
          "the maximum tool profile mapping has the wrong type"));
    }
    for (const auto& mapping : *mappings) {
      auto id = Id::from(mapping.key);
      if (!id) {
        return std::unexpected(diagnostic(
            ConfigDiagnosticCode::invalid_value, source, std::string{key},
            "a maximum tool profile mapping identity is invalid"));
      }
      auto profile = parse_profile(mapping.value, source, key);
      if (!profile) return std::unexpected(std::move(profile.error()));
      if (!destination.emplace(std::move(*id), std::move(*profile)).second) {
        return std::unexpected(diagnostic(
            ConfigDiagnosticCode::invalid_value, source, std::string{key},
            "maximum tool profile mapping identities must be unique"));
      }
    }
    return {};
  };

  if (auto models = parse(model_maximum_tool_profiles_key, result.models);
      !models) {
    return std::unexpected(std::move(models.error()));
  }
  if (auto personas = parse(persona_maximum_tool_profiles_key, result.personas);
      !personas) {
    return std::unexpected(std::move(personas.error()));
  }
  return result;
}

auto resolve_user_global_instructions_enabled(const ResolvedConfig& resolved)
    -> std::expected<bool, ConfigDiagnostic> {
  for (const auto& issue : resolved.diagnostics) {
    if (issue.key == user_global_instructions_enabled_key ||
        issue.key == "instructions" || issue.key == "instructions.global" ||
        issue.key.starts_with("instructions.global.")) {
      return std::unexpected(issue);
    }
  }
  const auto* entry = resolved.find(user_global_instructions_enabled_key);
  if (entry == nullptr || !entry->value) {
    return std::unexpected(diagnostic(
        ConfigDiagnosticCode::invalid_registry, ConfigSource::compiled_default,
        std::string{user_global_instructions_enabled_key},
        "the user-global instruction enable setting is missing"));
  }
  const auto* enabled = std::get_if<bool>(&*entry->value);
  if (enabled == nullptr) {
    return std::unexpected(diagnostic(
        ConfigDiagnosticCode::invalid_value,
        entry->source.value_or(ConfigSource::file),
        std::string{user_global_instructions_enabled_key},
        "the user-global instruction enable setting has the wrong type"));
  }
  return *enabled;
}

auto resolve_image_tool_model(const ResolvedConfig& resolved)
    -> std::expected<std::optional<domain::ModelId>, ConfigDiagnostic> {
  for (const auto& issue : resolved.diagnostics) {
    if (issue.key == image_tool_model_key || issue.key == "tools" ||
        issue.key == "tools.image" || issue.key.starts_with("tools.image.")) {
      return std::unexpected(issue);
    }
  }
  const auto* entry = resolved.find(image_tool_model_key);
  if (entry == nullptr) {
    return std::unexpected(diagnostic(
        ConfigDiagnosticCode::invalid_registry, ConfigSource::compiled_default,
        std::string{image_tool_model_key},
        "the image tool model configuration key is missing"));
  }
  if (!entry->value) return std::nullopt;
  const auto source = entry->source.value_or(ConfigSource::file);
  const auto* text = std::get_if<std::string>(&*entry->value);
  if (text == nullptr) {
    return std::unexpected(
        diagnostic(ConfigDiagnosticCode::invalid_value, source,
                   std::string{image_tool_model_key},
                   "the image tool model setting has the wrong type"));
  }
  auto model_id = domain::ModelId::from(*text);
  if (!model_id) {
    return std::unexpected(
        diagnostic(ConfigDiagnosticCode::invalid_value, source,
                   std::string{image_tool_model_key},
                   "the image tool model setting is invalid"));
  }
  return std::optional<domain::ModelId>{std::move(*model_id)};
}

auto resolve_automatic_approval_rules(const ResolvedConfig& resolved)
    -> std::expected<AutomaticApprovalRulesConfig, ConfigDiagnostic> {
  constexpr std::string_view namespace_key{"tools.approval"};
  for (const auto& issue : resolved.diagnostics) {
    if (issue.key == namespace_key ||
        issue.key == automatic_approval_rules_key ||
        (issue.key.size() > namespace_key.size() &&
         issue.key.starts_with(namespace_key) &&
         issue.key[namespace_key.size()] == '.')) {
      return std::unexpected(issue);
    }
  }
  const auto* entry = resolved.find(automatic_approval_rules_key);
  if (entry == nullptr || !entry->value) return AutomaticApprovalRulesConfig{};
  const auto* configured =
      std::get_if<AutomaticApprovalRulesConfig>(&*entry->value);
  if (configured == nullptr) {
    return std::unexpected(
        diagnostic(ConfigDiagnosticCode::invalid_value,
                   entry->source.value_or(ConfigSource::compiled_default),
                   std::string{automatic_approval_rules_key},
                   "automatic approval rules have an invalid value type"));
  }
  return *configured;
}

// clang-format off
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Every process capability and hard bound is validated before assembly receives authority.
auto resolve_process_config_settings(const ResolvedConfig& resolved)
    -> std::expected<std::optional<ProcessConfigSettings>, ConfigDiagnostic> {
  // clang-format on
  try {
    for (const auto& issue : resolved.diagnostics) {
      if (process_namespace_key(issue.key)) return std::unexpected(issue);
    }

    const auto executables = process_config_value<std::vector<std::string>>(
        resolved, process_executables_key);
    if (!executables) return std::unexpected(executables.error());
    const auto readable = process_config_value<std::vector<std::string>>(
        resolved, process_readable_roots_key);
    if (!readable) return std::unexpected(readable.error());
    const auto writable = process_config_value<std::vector<std::string>>(
        resolved, process_writable_roots_key);
    if (!writable) return std::unexpected(writable.error());
    const auto environment = process_config_value<std::vector<std::string>>(
        resolved, process_environment_key);
    if (!environment) return std::unexpected(environment.error());
    const auto network =
        process_config_value<bool>(resolved, process_unrestricted_network_key);
    if (!network) return std::unexpected(network.error());
    const auto maximum_matches = process_config_value<std::uint64_t>(
        resolved, process_allowlist_automatic_approval_maximum_matches_key);
    if (!maximum_matches) return std::unexpected(maximum_matches.error());

    const auto list_or_empty = [](const auto* value) {
      return value == nullptr ? std::vector<std::string>{} : *value;
    };
    ProcessConfigSettings settings;
    settings.executable_allowlist = list_or_empty(*executables);
    settings.readable_roots = list_or_empty(*readable);
    settings.writable_roots = list_or_empty(*writable);
    settings.inherited_environment_names = list_or_empty(*environment);
    settings.unrestricted_network = network.value() != nullptr && **network;
    if (maximum_matches.value() != nullptr && **maximum_matches != 0) {
      settings.allowlist_automatic_approval_maximum_matches = **maximum_matches;
    }

    std::size_t file_entries{};
    for (const auto& entry : resolved.entries) {
      if (process_namespace_key(entry.key) &&
          entry.source == ConfigSource::file) {
        ++file_entries;
      }
    }
    const auto* executable_entry = resolved.find(process_executables_key);
    const bool explicit_empty_executables =
        executable_entry != nullptr &&
        executable_entry->source == ConfigSource::file &&
        settings.executable_allowlist.empty();
    if (settings.executable_allowlist.empty()) {
      if (file_entries > static_cast<std::size_t>(explicit_empty_executables)) {
        return std::unexpected(diagnostic(
            ConfigDiagnosticCode::invalid_value, ConfigSource::file,
            std::string{process_executables_key},
            "disabled process configuration must not contain partial "
            "authority"));
      }
      return std::nullopt;
    }
    if (settings.readable_roots.empty()) {
      return std::unexpected(
          diagnostic(ConfigDiagnosticCode::invalid_value, ConfigSource::file,
                     std::string{process_readable_roots_key},
                     "enabled process configuration requires readable roots"));
    }

    const auto invalid_paths = [](const std::vector<std::string>& paths) {
      return !unique_values(paths) ||
             std::ranges::any_of(paths, [](const auto& path) {
               return !valid_process_path(path);
             });
    };
    if (invalid_paths(settings.executable_allowlist) ||
        invalid_paths(settings.readable_roots) ||
        invalid_paths(settings.writable_roots)) {
      return std::unexpected(
          diagnostic(ConfigDiagnosticCode::invalid_value, ConfigSource::file,
                     std::string{process_executables_key},
                     "process paths must be unique normalized absolute paths"));
    }
    if (!unique_values(settings.inherited_environment_names) ||
        std::ranges::any_of(
            settings.inherited_environment_names,
            [](const auto& name) { return !valid_environment_name(name); })) {
      return std::unexpected(
          diagnostic(ConfigDiagnosticCode::invalid_value, ConfigSource::file,
                     std::string{process_environment_key},
                     "process environment names are invalid or duplicated"));
    }
    if (std::ranges::any_of(
            settings.writable_roots, [&](const auto& writable_root) {
              return std::ranges::none_of(
                  settings.readable_roots, [&](const auto& readable_root) {
                    return path_is_within(readable_root, writable_root);
                  });
            })) {
      return std::unexpected(diagnostic(
          ConfigDiagnosticCode::invalid_value, ConfigSource::file,
          std::string{process_writable_roots_key},
          "every writable process root must be covered by a readable root"));
    }

    constexpr ProcessConfigLimits maximums;
    const auto assign_limit =
        [&]<typename Value>(
            const std::string_view key, const Value maximum,
            Value& destination) -> std::expected<void, ConfigDiagnostic> {
      const auto configured =
          process_config_value<std::uint64_t>(resolved, key);
      if (!configured) return std::unexpected(configured.error());
      if (*configured == nullptr || **configured == 0 ||
          **configured > static_cast<std::uint64_t>(maximum)) {
        return std::unexpected(diagnostic(
            ConfigDiagnosticCode::invalid_value,
            (*configured == nullptr)
                ? ConfigSource::compiled_default
                : resolved.find(key)->source.value_or(ConfigSource::file),
            std::string{key},
            "a process limit must be positive and within its hard ceiling"));
      }
      destination = static_cast<Value>(**configured);
      return {};
    };
    if (auto result =
            assign_limit(process_limit_executables_key, maximums.executables,
                         settings.limits.executables);
        !result) {
      return std::unexpected(result.error());
    }
    if (auto result =
            assign_limit(process_limit_arguments_key, maximums.arguments,
                         settings.limits.arguments);
        !result) {
      return std::unexpected(result.error());
    }
    if (auto result = assign_limit(process_limit_argument_bytes_key,
                                   maximums.argument_bytes,
                                   settings.limits.argument_bytes);
        !result) {
      return std::unexpected(result.error());
    }
    if (auto result = assign_limit(process_limit_roots_key, maximums.roots,
                                   settings.limits.roots);
        !result) {
      return std::unexpected(result.error());
    }
    if (auto result = assign_limit(process_limit_environment_variables_key,
                                   maximums.environment_variables,
                                   settings.limits.environment_variables);
        !result) {
      return std::unexpected(result.error());
    }
    auto timeout_count = settings.limits.timeout.count();
    if (auto result = assign_limit(process_limit_timeout_ms_key,
                                   maximums.timeout.count(), timeout_count);
        !result) {
      return std::unexpected(result.error());
    }
    settings.limits.timeout = std::chrono::milliseconds{timeout_count};
    if (auto result =
            assign_limit(process_limit_output_bytes_key, maximums.output_bytes,
                         settings.limits.output_bytes);
        !result) {
      return std::unexpected(result.error());
    }
    if (auto result = assign_limit(process_limit_inline_output_bytes_key,
                                   maximums.inline_output_bytes,
                                   settings.limits.inline_output_bytes);
        !result) {
      return std::unexpected(result.error());
    }
    if (auto result = assign_limit(process_limit_progress_chunk_bytes_key,
                                   maximums.progress_chunk_bytes,
                                   settings.limits.progress_chunk_bytes);
        !result) {
      return std::unexpected(result.error());
    }
    if (auto result = assign_limit(process_limit_progress_events_key,
                                   maximums.progress_events,
                                   settings.limits.progress_events);
        !result) {
      return std::unexpected(result.error());
    }
    auto grace_count = settings.limits.termination_grace.count();
    if (auto result =
            assign_limit(process_limit_termination_grace_ms_key,
                         maximums.termination_grace.count(), grace_count);
        !result) {
      return std::unexpected(result.error());
    }
    settings.limits.termination_grace = std::chrono::milliseconds{grace_count};

    if (settings.executable_allowlist.size() > settings.limits.executables ||
        settings.readable_roots.size() > settings.limits.roots ||
        settings.writable_roots.size() > settings.limits.roots ||
        settings.inherited_environment_names.size() >
            settings.limits.environment_variables ||
        settings.limits.inline_output_bytes > settings.limits.output_bytes ||
        settings.limits.progress_chunk_bytes > settings.limits.output_bytes) {
      return std::unexpected(
          diagnostic(ConfigDiagnosticCode::invalid_value, ConfigSource::file,
                     "tools.process.limits",
                     "process configuration exceeds its selected limits"));
    }
    constexpr std::uint64_t maximum_automatic_matches{1'000'000};
    const auto per_executable_matches =
        settings.allowlist_automatic_approval_maximum_matches;
    if (per_executable_matches &&
        (*per_executable_matches > maximum_automatic_matches ||
         settings.executable_allowlist.size() >
             maximum_automatic_matches / *per_executable_matches)) {
      return std::unexpected(diagnostic(
          ConfigDiagnosticCode::invalid_value, ConfigSource::file,
          std::string{process_allowlist_automatic_approval_maximum_matches_key},
          "process allowlist automatic approval accounting exceeds its "
          "aggregate bound"));
    }
    return std::optional<ProcessConfigSettings>{std::move(settings)};
  } catch (...) {
    return std::unexpected(
        diagnostic(ConfigDiagnosticCode::invalid_value, ConfigSource::file,
                   "tools.process", "process configuration failed internally"));
  }
}

} // namespace aiforge::config
