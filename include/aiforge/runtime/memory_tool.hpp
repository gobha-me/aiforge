#pragma once

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include <aiforge/domain/memory.hpp>
#include <aiforge/runtime/tool_registry.hpp>

namespace aiforge::runtime {

struct MemoryProposalDraft {
  domain::MemoryOwnerKind owner_kind{domain::MemoryOwnerKind::global};
  domain::MemoryKind kind{domain::MemoryKind::user_preference};
  std::string content;
  std::string rationale;
  std::string evidence_excerpt;
  std::optional<domain::MemoryRecordId> replacement_record_id;
  std::vector<domain::MemoryRecordId> overlap_record_ids;
  auto operator==(const MemoryProposalDraft&) const -> bool = default;
};

struct MemoryToolConfiguration {
  bool global_enabled{};
  bool project_enabled{};
  bool persona_capable{};
  std::optional<domain::PersonaId> persona_id;
  domain::MemoryLimits limits{};
  auto operator==(const MemoryToolConfiguration&) const -> bool = default;
};

[[nodiscard]] auto parse_memory_proposal_draft(
    const domain::StructuredDataBlock& arguments,
    const MemoryToolConfiguration& configuration)
    -> std::expected<MemoryProposalDraft, ToolExecutionError>;
[[nodiscard]] auto memory_tool_declaration(
    const MemoryToolConfiguration& configuration) -> backend::ToolDeclaration;
[[nodiscard]] auto register_memory_tool(ToolRegistry& registry,
                                        MemoryToolConfiguration configuration)
    -> std::expected<void, ToolRegistryError>;
[[nodiscard]] auto bind_memory_tool(
    const ToolRegistrySnapshot& tools, MemoryToolConfiguration configuration,
    std::optional<std::string> expected_registration_digest = std::nullopt)
    -> std::expected<ToolRegistrySnapshot, ToolRegistryError>;

} // namespace aiforge::runtime
