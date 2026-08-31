#pragma once

#include <aiforge/backend/backend.hpp>
#include <aiforge/config/config.hpp>

#include <expected>
#include <string>
#include <string_view>

namespace aiforge::adapters {

inline constexpr std::string_view venice_web_search_extension{
    "venice.chat.web-search"};
inline constexpr std::string_view web_search_model_capability{"web-search"};

[[nodiscard]] auto venice_generation_options(
    const config::ResolvedConfig& resolved)
    -> std::expected<backend::GenerationOptions, std::string>;

} // namespace aiforge::adapters
