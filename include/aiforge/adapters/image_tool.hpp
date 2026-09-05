#pragma once

#include <chrono>
#include <cstddef>
#include <expected>
#include <string>

#include <aiforge/adapters/image_backend.hpp>
#include <aiforge/model/catalog.hpp>
#include <aiforge/runtime/tool_registry.hpp>

namespace aiforge::adapters {

struct ImageToolConfiguration {
  domain::ModelId model_id;
  domain::ToolSpendQuote spend_quote;
  std::string artifact_root;
  std::string network_host;
  ImageBackendOptions image_options{};
  // Covers the 1 MiB prompt plus worst-case JSON string escaping and fixed
  // runtime-owned model/format framing under the kernel's 8 MiB ceiling.
  std::size_t maximum_argument_bytes{std::size_t{3} * 1024U * 1024U};
  std::chrono::milliseconds timeout{std::chrono::minutes{2}};
};

// Resolves one exact image model from a non-stale, unexpired catalog snapshot.
// The returned quote is pinned to its positive image type and USD generation
// price; no fallback or suggestion path exists.
[[nodiscard]] auto resolve_image_tool_configuration(
    const model::CatalogSnapshot& snapshot,
    const domain::ModelId& configured_model, std::string artifact_root,
    std::string network_host, domain::EventTimestamp now,
    std::chrono::hours catalog_time_to_live = std::chrono::hours{24})
    -> std::expected<ImageToolConfiguration, runtime::ToolRegistryError>;

[[nodiscard]] auto image_tool_declaration(
    const ImageToolConfiguration& configuration)
    -> std::expected<backend::ToolDeclaration, runtime::ToolRegistryError>;

[[nodiscard]] auto register_image_tool(runtime::ToolRegistry& registry,
                                       backend::ImageGenerator& generator,
                                       storage::ArtifactStore& artifact_store,
                                       ImageToolConfiguration configuration)
    -> std::expected<void, runtime::ToolRegistryError>;

} // namespace aiforge::adapters
