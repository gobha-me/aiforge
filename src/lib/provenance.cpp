#include <aiforge/domain/provenance.hpp>

#include <algorithm>
#include <cstdint>
#include <set>
#include <string_view>
#include <utility>

namespace aiforge::domain {
namespace {

[[nodiscard]] auto failure(const RunProvenanceErrorCode code,
                          std::string message)
    -> std::unexpected<RunProvenanceError> {
  return std::unexpected(RunProvenanceError{code, std::move(message)});
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

[[nodiscard]] auto has_control_character(const std::string_view value) -> bool {
  return std::ranges::any_of(value, [](const char character) {
    const auto byte = static_cast<unsigned char>(character);
    return byte < 0x20U || byte == 0x7fU;
  });
}

// Bounded single-line text: no control characters, valid UTF-8, within budget.
[[nodiscard]] auto bounded_text(const std::string_view value,
                                const std::size_t maximum,
                                const bool allow_empty = false) -> bool {
  if (value.empty()) return allow_empty;
  return value.size() <= maximum && !has_control_character(value) &&
         valid_utf8(value);
}

// Identities name software, keys, and locators. They stay in a conservative
// ASCII set so a persisted document cannot smuggle structure or whitespace.
[[nodiscard]] auto valid_identity(const std::string_view value,
                                  const std::size_t maximum) -> bool {
  if (value.empty() || value.size() > maximum) return false;
  return std::ranges::all_of(value, [](const char character) {
    const auto byte = static_cast<unsigned char>(character);
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' ||
           byte == '-' || byte == '+' || byte == ':';
  });
}

// A credential locator may name a path segment, so '/' is additionally allowed.
// Whitespace and '=' remain excluded: an accidental secret is far likelier to
// contain them than a variable name or path is.
[[nodiscard]] auto valid_credential_identity(const std::string_view value,
                                             const std::size_t maximum)
    -> bool {
  if (value.empty() || value.size() > maximum) return false;
  return std::ranges::all_of(value, [](const char character) {
    const auto byte = static_cast<unsigned char>(character);
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' ||
           byte == '-' || byte == ':' || byte == '/';
  });
}

[[nodiscard]] auto valid_configuration_key(const std::string_view value,
                                           const std::size_t maximum) -> bool {
  if (value.empty() || value.size() > maximum) return false;
  return std::ranges::all_of(value, [](const char character) {
    const auto byte = static_cast<unsigned char>(character);
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' ||
           byte == '-';
  });
}

[[nodiscard]] auto validate_configuration(
    const std::vector<ConfigurationProvenanceEntry>& configuration,
    const RunProvenanceLimits& limits) -> std::expected<void, RunProvenanceError> {
  if (configuration.size() > limits.maximum_configuration_entries) {
    return failure(RunProvenanceErrorCode::too_many_entries,
                   "the configuration entry count exceeds its limit");
  }
  std::set<std::string_view> keys;
  for (const auto& entry : configuration) {
    if (!valid_configuration_key(entry.key, limits.maximum_key_bytes)) {
      return failure(RunProvenanceErrorCode::invalid_key,
                     "a configuration key is empty, oversized, or malformed");
    }
    if (!keys.insert(entry.key).second) {
      return failure(RunProvenanceErrorCode::duplicate_key,
                     "configuration key '" + entry.key + "' is duplicated");
    }
    // The load-bearing rule: a sensitive key contributes presence and source,
    // never its resolved value.
    if (entry.sensitive && entry.value) {
      return failure(RunProvenanceErrorCode::sensitive_value_recorded,
                     "configuration key '" + entry.key +
                         "' is sensitive and cannot record a value");
    }
    if (entry.value && !entry.value_present) {
      return failure(RunProvenanceErrorCode::invalid_key,
                     "configuration key '" + entry.key +
                         "' records a value while reporting none resolved");
    }
    if (entry.value) {
      if (entry.value->size() > limits.maximum_value_bytes) {
        return failure(RunProvenanceErrorCode::value_too_large,
                       "the value of configuration key '" + entry.key +
                           "' exceeds its byte limit");
      }
      if (!valid_utf8(*entry.value)) {
        return failure(RunProvenanceErrorCode::invalid_key,
                       "the value of configuration key '" + entry.key +
                           "' is not valid UTF-8");
      }
    }
    if (entry.decisions.size() > limits.maximum_decisions_per_entry) {
      return failure(RunProvenanceErrorCode::too_many_entries,
                     "configuration key '" + entry.key +
                         "' exceeds its decision limit");
    }
  }
  return {};
}

[[nodiscard]] auto validate_tools(const std::vector<ToolProvenanceEntry>& tools,
                                  const RunProvenanceLimits& limits)
    -> std::expected<void, RunProvenanceError> {
  if (tools.size() > limits.maximum_tools) {
    return failure(RunProvenanceErrorCode::too_many_entries,
                   "the tool entry count exceeds its limit");
  }
  std::set<std::string_view> names;
  for (const auto& tool : tools) {
    if (!bounded_text(tool.tool_name, limits.maximum_identity_bytes)) {
      return failure(RunProvenanceErrorCode::invalid_tool,
                     "a tool name is empty, oversized, or malformed");
    }
    if (!names.insert(tool.tool_name).second) {
      return failure(RunProvenanceErrorCode::duplicate_tool,
                     "tool '" + tool.tool_name + "' is duplicated");
    }
    if (tool.declared_effects.size() > limits.maximum_effects_per_tool) {
      return failure(RunProvenanceErrorCode::invalid_tool,
                     "tool '" + tool.tool_name +
                         "' exceeds its declared effect limit");
    }
    if (tool.capability_scopes.size() > limits.maximum_scopes_per_tool) {
      return failure(
          RunProvenanceErrorCode::invalid_tool,
          "tool '" + tool.tool_name + "' exceeds its capability scope limit");
    }
    for (const auto& scope : tool.capability_scopes) {
      if (!bounded_text(scope.kind, limits.maximum_identity_bytes) ||
          !bounded_text(scope.value, limits.maximum_value_bytes)) {
        return failure(
            RunProvenanceErrorCode::invalid_tool,
            "tool '" + tool.tool_name + "' declares a malformed scope");
      }
    }
  }
  return {};
}

[[nodiscard]] auto total_bytes(const RunProvenance& provenance) -> std::size_t {
  std::size_t total = provenance.aiforge_version.size() +
                      provenance.backend_id.size() +
                      provenance.model_id.value().size();
  if (provenance.backend_version) total += provenance.backend_version->size();
  if (provenance.credential_source) {
    total += provenance.credential_source->identity.size();
  }
  for (const auto& entry : provenance.configuration) {
    total += entry.key.size();
    if (entry.value) total += entry.value->size();
  }
  for (const auto& component : provenance.components) {
    total += component.component.size() + component.version.size();
  }
  for (const auto& tool : provenance.tools) {
    total += tool.tool_name.size();
    for (const auto& scope : tool.capability_scopes) {
      total += scope.kind.size() + scope.value.size();
    }
  }
  return total;
}

}  // namespace

auto validate_run_provenance(const RunProvenance& provenance,
                             const RunProvenanceLimits limits)
    -> std::expected<void, RunProvenanceError> {
  if (limits.maximum_configuration_entries == 0 ||
      limits.maximum_decisions_per_entry == 0 || limits.maximum_key_bytes == 0 ||
      limits.maximum_value_bytes == 0 || limits.maximum_identity_bytes == 0 ||
      limits.maximum_components == 0 || limits.maximum_tools == 0 ||
      limits.maximum_effects_per_tool == 0 ||
      limits.maximum_scopes_per_tool == 0 || limits.maximum_total_bytes == 0) {
    return failure(RunProvenanceErrorCode::invalid_limits,
                   "a provenance limit is zero");
  }
  if (!valid_identity(provenance.aiforge_version,
                      limits.maximum_identity_bytes)) {
    return failure(RunProvenanceErrorCode::invalid_identity,
                   "the aiforge version is empty or malformed");
  }
  if (!valid_identity(provenance.backend_id, limits.maximum_identity_bytes)) {
    return failure(RunProvenanceErrorCode::invalid_identity,
                   "the backend identity is empty or malformed");
  }
  if (provenance.backend_version &&
      !valid_identity(*provenance.backend_version,
                      limits.maximum_identity_bytes)) {
    return failure(RunProvenanceErrorCode::invalid_identity,
                   "the backend version is empty or malformed");
  }
  if (provenance.credential_source &&
      !valid_credential_identity(provenance.credential_source->identity,
                                 limits.maximum_identity_bytes)) {
    return failure(RunProvenanceErrorCode::invalid_credential_source,
                   "the credential source identity is empty or malformed");
  }
  if (auto configuration =
          validate_configuration(provenance.configuration, limits);
      !configuration) {
    return configuration;
  }
  if (provenance.components.size() > limits.maximum_components) {
    return failure(RunProvenanceErrorCode::too_many_entries,
                   "the runtime component count exceeds its limit");
  }
  for (const auto& component : provenance.components) {
    if (!valid_identity(component.component, limits.maximum_identity_bytes) ||
        !valid_identity(component.version, limits.maximum_identity_bytes)) {
      return failure(RunProvenanceErrorCode::invalid_component,
                     "a runtime component name or version is malformed");
    }
  }
  if (auto tools = validate_tools(provenance.tools, limits); !tools) {
    return tools;
  }
  if (total_bytes(provenance) > limits.maximum_total_bytes) {
    return failure(RunProvenanceErrorCode::resource_exhausted,
                   "the provenance record exceeds its total byte budget");
  }
  return {};
}

}  // namespace aiforge::domain
