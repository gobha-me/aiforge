#pragma once

#include <aiforge/backend/backend.hpp>
#include <aiforge/config/config.hpp>
#include <aiforge/domain/provenance.hpp>

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aiforge::adapters {

inline constexpr std::string_view venice_web_search_extension{
    "venice.chat.web-search"};
inline constexpr std::string_view venice_system_prompt_extension{
    "venice.chat.include-system-prompt"};
inline constexpr std::string_view web_search_model_capability{"web-search"};

enum class VeniceWebSearchSetting {
  inherit,
  automatic,
  on,
  off,
};

enum class VeniceSystemPromptSetting {
  inherit,
  include,
  exclude,
};

struct VeniceConfiguredRequestSettings {
  VeniceWebSearchSetting web_search{VeniceWebSearchSetting::inherit};
  std::optional<config::ConfigSource> web_search_source;
  VeniceSystemPromptSetting system_prompt{VeniceSystemPromptSetting::inherit};
  std::optional<config::ConfigSource> system_prompt_source;
  auto operator==(const VeniceConfiguredRequestSettings&) const
      -> bool = default;
};

struct VeniceRequestSettingOverrides {
  std::optional<VeniceWebSearchSetting> web_search;
  std::optional<VeniceSystemPromptSetting> system_prompt;
  auto operator==(const VeniceRequestSettingOverrides&) const -> bool = default;
};

[[nodiscard]] auto venice_configured_request_settings(
    const config::ResolvedConfig& resolved)
    -> std::expected<VeniceConfiguredRequestSettings, std::string>;

[[nodiscard]] auto venice_generation_options(
    const VeniceConfiguredRequestSettings& configured,
    const VeniceRequestSettingOverrides& overrides = {})
    -> std::expected<backend::GenerationOptions, std::string>;

[[nodiscard]] auto venice_effective_request_options(
    const VeniceConfiguredRequestSettings& configured,
    const VeniceRequestSettingOverrides& overrides = {})
    -> std::expected<std::vector<domain::EffectiveRequestOption>, std::string>;

[[nodiscard]] auto venice_generation_options(
    const config::ResolvedConfig& resolved)
    -> std::expected<backend::GenerationOptions, std::string>;

} // namespace aiforge::adapters
