#pragma once

#include <aiforge/model/catalog.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <string>

namespace aiforge::adapters {

struct VeniceModelCatalogOptions {
  std::string base_url{"https://api.venice.ai/api/v1"};
  std::optional<std::chrono::milliseconds> connect_timeout;
  std::optional<std::chrono::milliseconds> read_timeout;
  std::optional<std::chrono::milliseconds> write_timeout;
};

class VeniceModelCatalogSource final : public model::CatalogSource {
 public:
  explicit VeniceModelCatalogSource(VeniceModelCatalogOptions options = {});
  ~VeniceModelCatalogSource() override;

  VeniceModelCatalogSource(const VeniceModelCatalogSource&) = delete;
  auto operator=(const VeniceModelCatalogSource&)
      -> VeniceModelCatalogSource& = delete;
  VeniceModelCatalogSource(VeniceModelCatalogSource&&) noexcept;
  auto operator=(VeniceModelCatalogSource&&) noexcept
      -> VeniceModelCatalogSource&;

  [[nodiscard]] auto fetch(std::stop_token stop_token)
      -> std::expected<model::CatalogSnapshot, model::CatalogError> override;

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

}  // namespace aiforge::adapters
