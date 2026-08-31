#include <aiforge/adapters/venice_generation_options.hpp>

#include <utility>

namespace aiforge::adapters {

auto venice_generation_options(const config::ResolvedConfig& resolved)
    -> std::expected<backend::GenerationOptions, std::string> {
  try {
    backend::GenerationOptions result;
    const auto* entry = resolved.find("venice.web_search");
    if (entry == nullptr || !entry->value) return result;

    const auto* mode = std::get_if<std::string>(&*entry->value);
    if (mode == nullptr ||
        (*mode != "auto" && *mode != "on" && *mode != "off")) {
      return std::unexpected(
          "configured Venice web-search mode must be auto, on, or off");
    }
    result.extensions.emplace(
        std::string{venice_web_search_extension},
        domain::StructuredDataBlock{"application/json", "\"" + *mode + "\""});
    if (*mode != "off") {
      result.required_model_capabilities.emplace_back(
          web_search_model_capability);
    }
    return result;
  } catch (...) {
    return std::unexpected(
        "failed to construct Venice generation options internally");
  }
}

} // namespace aiforge::adapters
