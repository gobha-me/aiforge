#pragma once

#include <aiforge/domain/digest.hpp>
#include <aiforge/domain/ids.hpp>

#include <cstddef>
#include <cstdint>

namespace aiforge::domain {
inline constexpr std::size_t summary_maximum_active = 32;

struct ConversationSummaryVersion {
  ConversationSummaryId summary_id;
  std::uint64_t revision{};
  ContentDigest candidate_digest;
  auto operator==(const ConversationSummaryVersion&) const -> bool = default;
};
} // namespace aiforge::domain
