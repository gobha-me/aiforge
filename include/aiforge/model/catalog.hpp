#pragma once

#include <aiforge/backend/backend.hpp>
#include <aiforge/domain/ids.hpp>
#include <aiforge/domain/pricing.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aiforge::model {

enum class Capability {
  tool_calling,
  vision,
  multiple_images,
  video_input,
  audio_input,
  reasoning,
  reasoning_effort,
  response_schema,
  log_probabilities,
  web_search,
  x_search,
  tee_attestation,
  end_to_end_encryption,
  optimized_for_code,
};

struct CapabilitySupport {
  Capability capability{Capability::tool_calling};
  std::optional<bool> supported;
  auto operator==(const CapabilitySupport&) const -> bool = default;
};

struct Price {
  std::optional<domain::DecimalAmount> usd;
  std::optional<domain::DecimalAmount> diem;
  auto operator==(const Price&) const -> bool = default;
};

struct PriceTier {
  std::optional<Price> input;
  std::optional<Price> output;
  std::optional<Price> cache_input;
  std::optional<Price> cache_write;
  auto operator==(const PriceTier&) const -> bool = default;
};

struct Pricing {
  explicit Pricing(PriceTier base_value = {}) : base(std::move(base_value)) {}
  PriceTier base;
  std::optional<std::uint64_t> extended_threshold_tokens;
  std::optional<PriceTier> extended;
  std::optional<Price> generation;
  auto operator==(const Pricing&) const -> bool = default;
};

struct CatalogEntry {
  CatalogEntry(domain::ModelId model_id, std::string model_type)
      : id(std::move(model_id)), type(std::move(model_type)) {}
  domain::ModelId id;
  std::string type;
  std::optional<std::string> name;
  std::optional<std::uint64_t> context_window_tokens;
  std::optional<std::uint64_t> maximum_output_tokens;
  bool offline{};
  std::vector<std::string> traits;
  std::vector<CapabilitySupport> capabilities;
  std::optional<Pricing> pricing;
  auto operator==(const CatalogEntry&) const -> bool = default;
};

enum class CatalogOrigin { live, fresh_cache, stale_cache };

struct CatalogSnapshot {
  CatalogSnapshot(std::chrono::sys_time<std::chrono::milliseconds> fetched,
                  std::vector<CatalogEntry> values = {})
      : fetched_at(fetched), entries(std::move(values)) {}
  std::chrono::sys_time<std::chrono::milliseconds> fetched_at;
  std::vector<CatalogEntry> entries;
  CatalogOrigin origin{CatalogOrigin::live};
  std::vector<std::string> warnings;
  std::string source_id{"model-catalog"};
  std::optional<std::string> source_revision;
  auto operator==(const CatalogSnapshot&) const -> bool = default;
};

enum class CatalogErrorCode {
  invalid_data,
  unavailable,
  cancelled,
  storage,
  internal_failure,
};

struct CatalogError {
  CatalogErrorCode code{CatalogErrorCode::internal_failure};
  std::string message;
  bool retryable{};
  auto operator==(const CatalogError&) const -> bool = default;
};

struct CatalogLimits {
  std::size_t maximum_entries{4096};
  std::size_t maximum_text_bytes{4096};
  std::size_t maximum_traits_per_entry{256};
  auto operator==(const CatalogLimits&) const -> bool = default;
};

class CatalogSource {
 public:
  virtual ~CatalogSource() = default;
  [[nodiscard]] virtual auto fetch(std::stop_token stop_token)
      -> std::expected<CatalogSnapshot, CatalogError> = 0;
};

class CatalogCache {
 public:
  virtual ~CatalogCache() = default;
  [[nodiscard]] virtual auto load(std::stop_token stop_token)
      -> std::expected<std::optional<CatalogSnapshot>, CatalogError> = 0;
  [[nodiscard]] virtual auto store(const CatalogSnapshot& snapshot,
                                   std::stop_token stop_token)
      -> std::expected<void, CatalogError> = 0;
};

using CatalogClock = std::function<
    std::chrono::sys_time<std::chrono::milliseconds>()>;

class CatalogService final : public backend::ModelContextProvider {
 public:
  CatalogService(CatalogSource& source, CatalogCache* cache = nullptr,
                 std::chrono::hours time_to_live = std::chrono::hours{24},
                 CatalogClock clock = {}, CatalogLimits limits = {});

  [[nodiscard]] auto snapshot(std::stop_token stop_token = {})
      -> std::expected<std::reference_wrapper<const CatalogSnapshot>,
                       CatalogError>;
  [[nodiscard]] auto lookup(const domain::ModelId& model_id,
                            std::stop_token stop_token)
      -> std::expected<backend::ModelContextInfo,
                       backend::BackendError> override;
  auto clear_memory_cache() noexcept -> void;

 private:
  CatalogSource& m_source;
  CatalogCache* m_cache{};
  std::chrono::hours m_time_to_live;
  CatalogClock m_clock;
  CatalogLimits m_limits;
  std::optional<CatalogSnapshot> m_snapshot;
};

[[nodiscard]] auto validate_catalog(const CatalogSnapshot& snapshot,
                                    CatalogLimits limits = {})
    -> std::expected<void, CatalogError>;
[[nodiscard]] auto find_model(const CatalogSnapshot& snapshot,
                              const domain::ModelId& model_id,
                              std::string_view type = {}) noexcept
    -> const CatalogEntry*;
[[nodiscard]] auto suggest_models(const CatalogSnapshot& snapshot,
                                  std::string_view requested,
                                  std::size_t limit = 3,
                                  std::string_view type = "text")
    -> std::vector<std::string>;
[[nodiscard]] auto capability_name(Capability capability) noexcept
    -> std::string_view;

}  // namespace aiforge::model
