#include <aiforge/adapters/venice_generation_options.hpp>

#include <utility>

namespace aiforge::adapters {
namespace {

[[nodiscard]] auto configured_web_search(const config::ResolvedConfig& resolved)
    -> std::expected<
        std::pair<VeniceWebSearchSetting, std::optional<config::ConfigSource>>,
        std::string> {
  const auto* entry = resolved.find("venice.web_search");
  if (entry == nullptr || !entry->value) {
    return std::pair{VeniceWebSearchSetting::inherit,
                     std::optional<config::ConfigSource>{}};
  }
  const auto* mode = std::get_if<std::string>(&*entry->value);
  if (mode == nullptr) {
    return std::unexpected("configured Venice web-search mode is not text");
  }
  if (*mode == "auto")
    return std::pair{VeniceWebSearchSetting::automatic, entry->source};
  if (*mode == "on")
    return std::pair{VeniceWebSearchSetting::on, entry->source};
  if (*mode == "off")
    return std::pair{VeniceWebSearchSetting::off, entry->source};
  return std::unexpected(
      "configured Venice web-search mode must be auto, on, or off");
}

[[nodiscard]] auto configured_system_prompt(
    const config::ResolvedConfig& resolved)
    -> std::expected<std::pair<VeniceSystemPromptSetting,
                               std::optional<config::ConfigSource>>,
                     std::string> {
  const auto* entry = resolved.find("venice.include_system_prompt");
  if (entry == nullptr || !entry->value) {
    return std::pair{VeniceSystemPromptSetting::inherit,
                     std::optional<config::ConfigSource>{}};
  }
  const auto* include = std::get_if<bool>(&*entry->value);
  if (include == nullptr) {
    return std::unexpected(
        "configured Venice system-prompt setting is not boolean");
  }
  return std::pair{*include ? VeniceSystemPromptSetting::include
                            : VeniceSystemPromptSetting::exclude,
                   entry->source};
}

} // namespace

auto venice_configured_request_settings(const config::ResolvedConfig& resolved)
    -> std::expected<VeniceConfiguredRequestSettings, std::string> {
  try {
    auto web_search = configured_web_search(resolved);
    if (!web_search) return std::unexpected(std::move(web_search.error()));
    auto system_prompt = configured_system_prompt(resolved);
    if (!system_prompt)
      return std::unexpected(std::move(system_prompt.error()));
    return VeniceConfiguredRequestSettings{
        web_search->first, web_search->second, system_prompt->first,
        system_prompt->second};
  } catch (...) {
    return std::unexpected(
        "failed to resolve Venice request settings internally");
  }
}

auto venice_generation_options(
    const VeniceConfiguredRequestSettings& configured,
    const VeniceRequestSettingOverrides& overrides)
    -> std::expected<backend::GenerationOptions, std::string> {
  try {
    backend::GenerationOptions result;
    const auto web_search =
        overrides.web_search.value_or(configured.web_search);
    if (web_search != VeniceWebSearchSetting::inherit) {
      std::string_view mode;
      switch (web_search) {
        case VeniceWebSearchSetting::automatic: mode = "auto"; break;
        case VeniceWebSearchSetting::on: mode = "on"; break;
        case VeniceWebSearchSetting::off: mode = "off"; break;
        case VeniceWebSearchSetting::inherit: break;
        default: return std::unexpected("Venice web-search setting is invalid");
      }
      result.extensions.emplace(
          std::string{venice_web_search_extension},
          domain::StructuredDataBlock{"application/json",
                                      "\"" + std::string{mode} + "\""});
      if (web_search != VeniceWebSearchSetting::off) {
        result.required_model_capabilities.emplace_back(
            web_search_model_capability);
      }
    }

    const auto system_prompt =
        overrides.system_prompt.value_or(configured.system_prompt);
    if (system_prompt != VeniceSystemPromptSetting::inherit) {
      if (system_prompt != VeniceSystemPromptSetting::include &&
          system_prompt != VeniceSystemPromptSetting::exclude) {
        return std::unexpected("Venice system-prompt setting is invalid");
      }
      result.extensions.emplace(
          std::string{venice_system_prompt_extension},
          domain::StructuredDataBlock{
              "application/json",
              system_prompt == VeniceSystemPromptSetting::include ? "true"
                                                                  : "false"});
    }
    return result;
  } catch (...) {
    return std::unexpected(
        "failed to construct Venice generation options internally");
  }
}

auto venice_effective_request_options(
    const VeniceConfiguredRequestSettings& configured,
    const VeniceRequestSettingOverrides& overrides)
    -> std::expected<std::vector<domain::EffectiveRequestOption>, std::string> {
  const auto web_search = overrides.web_search.value_or(configured.web_search);
  const auto system_prompt =
      overrides.system_prompt.value_or(configured.system_prompt);
  const auto web_source =
      overrides.web_search ? domain::RequestOptionSource::session_override
      : configured.web_search == VeniceWebSearchSetting::inherit
          ? domain::RequestOptionSource::provider_default
          : domain::RequestOptionSource::configuration;
  const auto prompt_source =
      overrides.system_prompt ? domain::RequestOptionSource::session_override
      : configured.system_prompt == VeniceSystemPromptSetting::inherit
          ? domain::RequestOptionSource::provider_default
          : domain::RequestOptionSource::configuration;
  const auto web_value = [&]() -> std::optional<std::string> {
    switch (web_search) {
      case VeniceWebSearchSetting::inherit: return std::nullopt;
      case VeniceWebSearchSetting::automatic: return "auto";
      case VeniceWebSearchSetting::on: return "on";
      case VeniceWebSearchSetting::off: return "off";
    }
    return "invalid";
  }();
  const auto prompt_value = [&]() -> std::optional<std::string> {
    switch (system_prompt) {
      case VeniceSystemPromptSetting::inherit: return std::nullopt;
      case VeniceSystemPromptSetting::include: return "true";
      case VeniceSystemPromptSetting::exclude: return "false";
    }
    return "invalid";
  }();
  if (web_value == "invalid" || prompt_value == "invalid") {
    return std::unexpected("Venice request setting is invalid");
  }
  return std::vector<domain::EffectiveRequestOption>{
      {std::string{venice_web_search_extension}, web_value, web_source},
      {std::string{venice_system_prompt_extension}, prompt_value,
       prompt_source}};
}

auto venice_generation_options(const config::ResolvedConfig& resolved)
    -> std::expected<backend::GenerationOptions, std::string> {
  auto configured = venice_configured_request_settings(resolved);
  if (!configured) return std::unexpected(std::move(configured.error()));
  return venice_generation_options(*configured);
}

} // namespace aiforge::adapters
