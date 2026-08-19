#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>

#include <aiforge/domain/repository_evidence.hpp>

namespace aiforge::repository {

struct ContextParcelLimits {
  std::size_t maximum_items{1024};
  std::size_t maximum_purpose_bytes{4096};
  std::size_t maximum_path_bytes{4096};
  std::size_t maximum_metadata_bytes{4096};
  std::size_t maximum_provenance_references{4096};
  std::size_t maximum_content_blocks_per_item{1024};
  std::uint64_t maximum_item_bytes{64U * 1024U * 1024U};
  std::uint64_t maximum_total_bytes{256U * 1024U * 1024U};
  std::uint64_t maximum_total_tokens{1024U * 1024U};
  auto operator==(const ContextParcelLimits&) const -> bool = default;
};

enum class ContextParcelErrorCode {
  invalid_limits,
  invalid_parcel,
  invalid_item,
  invalid_source,
  invalid_range,
  invalid_reference,
  invalid_provenance,
  conflicting_provenance,
  invalid_freshness,
  duplicate_item,
  resource_exhausted,
  overflow,
  internal_failure,
};

struct ContextParcelError {
  ContextParcelErrorCode code{ContextParcelErrorCode::invalid_parcel};
  std::string message;
  std::optional<domain::EvidenceId> evidence_id;
  auto operator==(const ContextParcelError&) const -> bool = default;
};

struct ContextParcelEstimate {
  std::size_t item_count{};
  std::uint64_t inline_bytes{};
  std::uint64_t represented_bytes{};
  std::uint64_t estimated_tokens{};
  auto operator==(const ContextParcelEstimate&) const -> bool = default;
};

[[nodiscard]] auto validate_context_parcel(
    const domain::ContextParcel& parcel,
    const ContextParcelLimits& limits = {})
    -> std::expected<ContextParcelEstimate, ContextParcelError>;

}  // namespace aiforge::repository
