#include "json_ops_targets.hpp"
#include <algorithm>
#include <array>
#include <nlohmann/json.hpp>

namespace aiforge::config::detail {
namespace {
using Json = nlohmann::json;
auto fields(const Json& value, std::initializer_list<std::string_view> names)
    -> bool {
  return value.is_object() && value.size() == names.size() &&
         std::ranges::all_of(names,
                             [&](auto name) { return value.contains(name); });
}
auto string_field(const Json& value, std::string_view name, std::size_t maximum)
    -> bool {
  const auto found = value.find(name);
  return found != value.end() && found->is_string() &&
         found->get_ref<const std::string&>().size() <= maximum;
}
auto parse_target(const Json& record) -> std::optional<OpsTargetConfig> {
  if (!fields(record, {"id", "display_name", "source"}) ||
      !string_field(record, "id", 64) ||
      !string_field(record, "display_name", 128))
    return std::nullopt;
  const auto& source = record.at("source");
  if (!source.is_object() || !string_field(source, "kind", 32))
    return std::nullopt;
  OpsTargetConfig result{record.at("id").get<std::string>(),
                         record.at("display_name").get<std::string>(),
                         LinuxLocalTargetConfig{}};
  const auto& kind = source.at("kind").get_ref<const std::string&>();
  if (kind == "linux_local" && fields(source, {"kind"})) return result;
  if (kind != "kubernetes_static" ||
      !fields(source, {"kind", "config_file", "context", "namespace"}) ||
      !string_field(source, "config_file", 4096) ||
      !string_field(source, "context", 256) ||
      !string_field(source, "namespace", 63))
    return std::nullopt;
  result.source =
      StaticKubernetesTargetConfig{source.at("config_file").get<std::string>(),
                                   source.at("context").get<std::string>(),
                                   source.at("namespace").get<std::string>()};
  return result;
}
} // namespace

auto parse_ops_targets_json(const Json& value, const ConfigKeySpec& spec)
    -> std::expected<ConfigValue, ConfigDiagnostic> {
  const auto invalid = [&] {
    return std::unexpected(ConfigDiagnostic{
        ConfigDiagnosticCode::invalid_value, ConfigSource::file, spec.id,
        "Ops target configuration is malformed or overbound"});
  };
  try {
    if (!value.is_array() ||
        value.size() > std::min(spec.maximum_list_items, std::size_t{32}))
      return invalid();
    OpsTargetsConfig result;
    for (const auto& record : value) {
      auto target = parse_target(record);
      if (!target) return invalid();
      result.targets.push_back(std::move(*target));
    }
    if (auto valid = validate_ops_targets(result, spec, ConfigSource::file);
        !valid)
      return std::unexpected(valid.error());
    return ConfigValue{std::move(result)};
  } catch (...) {
    return std::unexpected(ConfigDiagnostic{
        ConfigDiagnosticCode::invalid_value, ConfigSource::file, {}, {}});
  }
}
auto ops_targets_json(const OpsTargetsConfig& configured) -> Json {
  auto result = Json::array();
  for (const auto& target : configured.targets) {
    Json source{{"kind", "linux_local"}};
    if (const auto* kube =
            std::get_if<StaticKubernetesTargetConfig>(&target.source))
      source = {{"kind", "kubernetes_static"},
                {"config_file", kube->config_file},
                {"context", kube->context},
                {"namespace", kube->name_space}};
    result.push_back({{"id", target.id},
                      {"display_name", target.display_name},
                      {"source", std::move(source)}});
  }
  return result;
}
} // namespace aiforge::config::detail
