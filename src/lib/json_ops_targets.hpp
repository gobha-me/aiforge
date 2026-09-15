#pragma once
#include <aiforge/config/config.hpp>
#include <nlohmann/json_fwd.hpp>

namespace aiforge::config::detail {
[[nodiscard]] auto parse_ops_targets_json(const nlohmann::json& value,
                                          const ConfigKeySpec& spec)
    -> std::expected<ConfigValue, ConfigDiagnostic>;
[[nodiscard]] auto ops_targets_json(const OpsTargetsConfig& configured)
    -> nlohmann::json;
} // namespace aiforge::config::detail
