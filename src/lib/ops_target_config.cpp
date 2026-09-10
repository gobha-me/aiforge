#include <aiforge/config/config.hpp>

#include <aiforge/detail/utf8_text.hpp>
#include <algorithm>
#include <unordered_set>

namespace aiforge::config {
namespace {
auto invalid(ConfigSource source) -> std::unexpected<ConfigDiagnostic> {
  return std::unexpected(ConfigDiagnostic{
      ConfigDiagnosticCode::invalid_value, source, std::string{ops_targets_key},
      "Ops target configuration is malformed or overbound"});
}
auto text(std::string_view value, std::size_t maximum) -> bool {
  return !value.empty() && value.size() <= maximum &&
         detail::is_safe_utf8_text(value) &&
         std::ranges::none_of(value, [](unsigned char byte) {
           return byte < 32 || byte == 127;
         });
}
auto alphanumeric(char byte) -> bool {
  return (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9');
}
auto target_id(std::string_view value) -> bool {
  return !value.empty() && value.size() <= 64 && value != "local" &&
         alphanumeric(value.front()) && alphanumeric(value.back()) &&
         std::ranges::all_of(value, [](char byte) {
           return alphanumeric(byte) || byte == '-' || byte == '_';
         });
}
auto name_space(std::string_view value) -> bool {
  return !value.empty() && value.size() <= 63 && alphanumeric(value.front()) &&
         alphanumeric(value.back()) &&
         std::ranges::all_of(value, [](char byte) {
           return alphanumeric(byte) || byte == '-';
         });
}
auto valid_record(const OpsTargetConfig& target, std::size_t& remaining)
    -> bool {
  const auto consume = [&](std::string_view value) {
    if (value.size() > remaining) return false;
    remaining -= value.size();
    return true;
  };
  if (!target_id(target.id) || !text(target.display_name, 128) ||
      !consume(target.id) || !consume(target.display_name))
    return false;
  const auto* kube = std::get_if<StaticKubernetesTargetConfig>(&target.source);
  if (kube == nullptr) return !target.source.valueless_by_exception();
  return text(kube->config_file, 4096) && kube->config_file.front() == '/' &&
         text(kube->context, 256) && name_space(kube->name_space) &&
         consume(kube->config_file) && consume(kube->context) &&
         consume(kube->name_space);
}
} // namespace

auto validate_ops_targets(const OpsTargetsConfig& configured,
                          const ConfigKeySpec& spec, ConfigSource source)
    -> std::expected<void, ConfigDiagnostic> {
  try {
    if (source != ConfigSource::file ||
        configured.targets.size() >
            std::min(spec.maximum_list_items, std::size_t{32}))
      return invalid(source);
    std::size_t remaining =
        std::min(spec.maximum_text_bytes, std::size_t{64} * 1024U);
    std::unordered_set<std::string_view> ids;
    for (const auto& target : configured.targets)
      if (!valid_record(target, remaining) || !ids.insert(target.id).second)
        return invalid(source);
    return {};
  } catch (...) {
    return std::unexpected(
        ConfigDiagnostic{ConfigDiagnosticCode::invalid_value, source, {}, {}});
  }
}

auto resolve_ops_targets(const ResolvedConfig& resolved)
    -> std::expected<OpsTargetsConfig, ConfigDiagnostic> {
  try {
    for (const auto& issue : resolved.diagnostics)
      if (issue.key == "ops" || issue.key.starts_with("ops."))
        return std::unexpected(issue);
    OpsTargetsConfig result{
        {{"local", "This environment", LinuxLocalTargetConfig{}}}};
    const auto* entry = resolved.find(ops_targets_key);
    if (entry == nullptr || !entry->value) return result;
    const auto source = entry->source.value_or(ConfigSource::compiled_default);
    const auto* configured = std::get_if<OpsTargetsConfig>(&*entry->value);
    if (configured == nullptr) return invalid(source);
    const auto& keys = builtin_config_registry().keys;
    const auto spec =
        std::ranges::find(keys, ops_targets_key, &ConfigKeySpec::id);
    if (spec == keys.end()) return invalid(source);
    if (auto valid = validate_ops_targets(*configured, *spec, source); !valid)
      return std::unexpected(valid.error());
    result.targets.insert(result.targets.end(), configured->targets.begin(),
                          configured->targets.end());
    return result;
  } catch (...) {
    return std::unexpected(ConfigDiagnostic{
        ConfigDiagnosticCode::invalid_value, ConfigSource::file, {}, {}});
  }
}
} // namespace aiforge::config
