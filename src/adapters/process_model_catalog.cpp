#include <aiforge/adapters/process_model_catalog.hpp>

#include <utility>

namespace aiforge::adapters {

ProcessModelCatalog::ProcessModelCatalog(std::filesystem::path cache_path)
    : m_cache(std::move(cache_path)), m_service(m_source, &m_cache) {}

auto ProcessModelCatalog::create()
    -> std::expected<std::unique_ptr<ProcessModelCatalog>,
                     model::CatalogError> {
  auto path = process_model_catalog_cache_path();
  if (!path) return std::unexpected(std::move(path.error()));
  try {
    return std::unique_ptr<ProcessModelCatalog>{
        new ProcessModelCatalog{std::move(*path)}};
  } catch (...) {
    return std::unexpected(model::CatalogError{
        model::CatalogErrorCode::internal_failure,
        "process model catalog could not be initialized", false});
  }
}

}  // namespace aiforge::adapters
