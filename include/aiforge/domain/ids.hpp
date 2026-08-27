#pragma once

#include <cstddef>
#include <compare>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace aiforge::domain {

enum class IdError {
  empty,
  too_long,
  control_character,
};

template <typename Tag>
class Id final {
 public:
  [[nodiscard]] static auto from(std::string value) -> std::expected<Id, IdError> {
    if (value.empty()) return std::unexpected(IdError::empty);
    if (value.size() > max_size) return std::unexpected(IdError::too_long);
    for (const unsigned char character : value) {
      if (character < 0x20U || character == 0x7FU) {
        return std::unexpected(IdError::control_character);
      }
    }
    return Id{std::move(value)};
  }

  [[nodiscard]] auto value() const noexcept -> std::string_view { return m_value; }

  auto operator<=>(const Id&) const = default;

  static constexpr std::size_t max_size{128};

 private:
  explicit Id(std::string value) : m_value(std::move(value)) {}

  std::string m_value;
};

struct SessionIdTag;
struct RunIdTag;
struct EventIdTag;
struct MessageIdTag;
struct InferenceIdTag;
struct InvocationIdTag;
struct QuestionIdTag;
struct ArtifactIdTag;
struct ViewIdTag;
struct ModelIdTag;
struct SurfaceIdTag;
struct WorkspaceIdTag;
struct PersonaIdTag;
struct PermissionProfileIdTag;
struct ContextEntryIdTag;
struct ContextSourceIdTag;
struct RepositoryIdTag;
struct EvidenceIdTag;
struct ContextParcelIdTag;
struct ProjectInstructionIdTag;
struct KnowledgeRecordIdTag;
struct KnowledgeEntityIdTag;
struct VerificationEvidenceIdTag;
struct ReviewReceiptIdTag;
struct ReviewRequirementIdTag;
struct ReviewFindingIdTag;
struct ReviewOverrideIdTag;
struct PlanIdTag;
struct PlanRevisionIdTag;
struct PlanTaskIdTag;
struct ProjectBacklogItemIdTag;

using SessionId = Id<SessionIdTag>;
using RunId = Id<RunIdTag>;
using EventId = Id<EventIdTag>;
using MessageId = Id<MessageIdTag>;
using InferenceId = Id<InferenceIdTag>;
using InvocationId = Id<InvocationIdTag>;
using QuestionId = Id<QuestionIdTag>;
using ArtifactId = Id<ArtifactIdTag>;
using ViewId = Id<ViewIdTag>;
using ModelId = Id<ModelIdTag>;
using SurfaceId = Id<SurfaceIdTag>;
using WorkspaceId = Id<WorkspaceIdTag>;
using PersonaId = Id<PersonaIdTag>;
using PermissionProfileId = Id<PermissionProfileIdTag>;
using ContextEntryId = Id<ContextEntryIdTag>;
using ContextSourceId = Id<ContextSourceIdTag>;
using RepositoryId = Id<RepositoryIdTag>;
using EvidenceId = Id<EvidenceIdTag>;
using ContextParcelId = Id<ContextParcelIdTag>;
using ProjectInstructionId = Id<ProjectInstructionIdTag>;
using KnowledgeRecordId = Id<KnowledgeRecordIdTag>;
using KnowledgeEntityId = Id<KnowledgeEntityIdTag>;
using VerificationEvidenceId = Id<VerificationEvidenceIdTag>;
using ReviewReceiptId = Id<ReviewReceiptIdTag>;
using ReviewRequirementId = Id<ReviewRequirementIdTag>;
using ReviewFindingId = Id<ReviewFindingIdTag>;
using ReviewOverrideId = Id<ReviewOverrideIdTag>;
using PlanId = Id<PlanIdTag>;
using PlanRevisionId = Id<PlanRevisionIdTag>;
using PlanTaskId = Id<PlanTaskIdTag>;
using ProjectBacklogItemId = Id<ProjectBacklogItemIdTag>;

}  // namespace aiforge::domain
