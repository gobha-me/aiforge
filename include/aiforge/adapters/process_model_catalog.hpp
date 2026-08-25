#pragma once

#include <aiforge/adapters/json_model_catalog_cache.hpp>
#include <aiforge/adapters/venice_model_catalog_source.hpp>
#include <aiforge/model/catalog.hpp>

#include <expected>
#include <memory>

namespace aiforge::adapters {

class ProcessModelCatalog final {
 public:
  [[nodiscard]] static auto create()
      -> std::expected<std::unique_ptr<ProcessModelCatalog>,
                       model::CatalogError>;

  [[nodiscard]] auto service() noexcept -> model::CatalogService& {
    return m_service;
  }

 private:
  explicit ProcessModelCatalog(std::filesystem::path cache_path);

  VeniceModelCatalogSource m_source;
  JsonModelCatalogCache m_cache;
  model::CatalogService m_service;
};

}  // namespace aiforge::adapters
