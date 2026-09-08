#pragma once

#include <aiforge/domain/ids.hpp>
#include <cstdint>

namespace aiforge::surfaces {

enum class ChatEvidenceWorkPurpose {
  submit,
  inspection,
  summary_preview,
  summary_apply,
  continuation,
  recovery,
  answer,
  approval
};

// Owner-thread operation identity. This is neither a durable source proof nor
// permission to dispatch a provider request or tool.
struct ChatEvidenceWorkToken {
  domain::SessionId session_id;
  domain::ModelId model_id;
  std::uint64_t generation{};
  ChatEvidenceWorkPurpose purpose{ChatEvidenceWorkPurpose::submit};
  auto operator==(const ChatEvidenceWorkToken&) const -> bool = default;
};

} // namespace aiforge::surfaces
