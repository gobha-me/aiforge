#pragma once

#include <aiforge/model/catalog.hpp>

#include <filesystem>

namespace aiforge::adapters {

class JsonModelCatalogCache final : public model::CatalogCache {
 public:
  explicit JsonModelCatalogCache(std::filesystem::path path,
                                 std::size_t maximum_bytes = 8U * 1024U * 1024U)
      : m_path(std::move(path)), m_maximum_bytes(maximum_bytes) {}

  [[nodiscard]] auto load(std::stop_token stop_token)
      -> std::expected<std::optional<model::CatalogSnapshot>,
                       model::CatalogError> override;
  [[nodiscard]] auto store(const model::CatalogSnapshot& snapshot,
                           std::stop_token stop_token)
      -> std::expected<void, model::CatalogError> override;

  [[nodiscard]] auto path() const noexcept -> const std::filesystem::path& {
    return m_path;
  }

 private:
  std::filesystem::path m_path;
  std::size_t m_maximum_bytes;
};

[[nodiscard]] auto process_model_catalog_cache_path()
    -> std::expected<std::filesystem::path, model::CatalogError>;

} // namespace aiforge::adapters
