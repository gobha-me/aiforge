#include <aiforge/config/config.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <concepts>
#include <cstdlib>
#include <limits>
#include <ranges>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

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

[[nodiscard]] auto validate_value(const ConfigKeySpec& spec,
                                  const ConfigValue& value,
                                  const ConfigSource source)
    -> std::expected<void, ConfigDiagnostic> {
  if (!kind_matches(spec.value_kind, value)) {
    return std::unexpected(
        diagnostic(ConfigDiagnosticCode::invalid_value, source, spec.id,
                   "the value type does not match the configuration key"));
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

} // namespace

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
  }
  if (auto valid = validate_value(spec, parsed, source); !valid) {
    return std::unexpected(std::move(valid.error()));
  }
  return parsed;
}

auto format_config_value(const ConfigValue& value) -> std::string {
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
      {"memory.global.capture", ConfigValueKind::text,
       std::string{"AIFORGE_MEMORY_GLOBAL_CAPTURE"},
       ConfigValue{std::string{"off"}}, false, true, 16, 1},
      {"memory.project.capture", ConfigValueKind::text,
       std::string{"AIFORGE_MEMORY_PROJECT_CAPTURE"},
       ConfigValue{std::string{"review"}}, false, true, 16, 1},
      {"memory.context.max_tokens", ConfigValueKind::unsigned_integer,
       std::string{"AIFORGE_MEMORY_CONTEXT_MAX_TOKENS"},
       ConfigValue{std::uint64_t{2048}}, false, true, 32, 1},
  }};
  return registry;
}

} // namespace aiforge::config
