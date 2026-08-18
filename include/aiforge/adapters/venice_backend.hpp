#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>

#include <aiforge/backend/backend.hpp>

namespace aiforge::adapters {

struct VeniceBackendOptions {
  std::string api_key;
  std::string base_url{"https://api.venice.ai/api/v1"};
  std::optional<std::chrono::milliseconds> connect_timeout;
  std::optional<std::chrono::milliseconds> read_timeout;
  std::optional<std::chrono::milliseconds> write_timeout;
  std::size_t pending_events{256};
};

class VeniceBackend final : public backend::Backend {
 public:
  explicit VeniceBackend(VeniceBackendOptions options);
  ~VeniceBackend() override;

  VeniceBackend(const VeniceBackend&) = delete;
  auto operator=(const VeniceBackend&) -> VeniceBackend& = delete;
  VeniceBackend(VeniceBackend&&) noexcept;
  auto operator=(VeniceBackend&&) noexcept -> VeniceBackend&;

  [[nodiscard]] auto start(backend::BackendRequest request,
                           std::stop_token stop_token)
      -> std::expected<std::unique_ptr<backend::BackendStream>,
                       backend::BackendError> override;

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

}  // namespace aiforge::adapters
