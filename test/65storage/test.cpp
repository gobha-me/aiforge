#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>

#include <aiforge/adapters/sqlite_session_store.hpp>
#include <aiforge/detail/sha256.hpp>
#include <aiforge/testing/scripted_session_store.hpp>
#include <algorithm>
#include <array>

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace aiforge;

template <typename IdType> auto make_id(const std::string& value) -> IdType {
  return IdType::from(value).value();
}

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    auto pattern =
        (std::filesystem::temp_directory_path() / "aiforge-storage-XXXXXX")
            .string();
    pattern.push_back('\0');
    const auto* created = ::mkdtemp(pattern.data());
    REQUIRE(created != nullptr);
    m_path = created;
  }
  TemporaryDirectory(const TemporaryDirectory&) = delete;
  auto operator=(const TemporaryDirectory&) -> TemporaryDirectory& = delete;
  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(m_path, error);
  }
  [[nodiscard]] auto path() const -> const std::filesystem::path& {
    return m_path;
  }

 private:
  std::filesystem::path m_path;
};

auto metadata(const std::uint64_t sequence, std::string event_id,
              const std::uint32_t schema_version = 1) -> domain::EventMetadata {
  return {make_id<domain::EventId>(std::move(event_id)),
          make_id<domain::RunId>("run"),
          sequence,
          schema_version,
          domain::EventTimestamp{std::chrono::milliseconds{1000 + sequence}},
          std::nullopt,
          std::nullopt,
          std::nullopt};
}

template <typename Payload>
auto event(const std::uint64_t sequence, Payload payload, std::string id = {})
    -> domain::RunEvent {
  if (id.empty()) id = "event-" + std::to_string(sequence);
  domain::RunEvent result{metadata(sequence, std::move(id)),
                          std::move(payload)};
  if (std::holds_alternative<domain::PlanRevisionProposed>(result.payload)) {
    result.metadata.schema_version = 2;
  }
  if (const auto* child = std::get_if<domain::ChildRunCreated>(&result.payload);
      child != nullptr && child->descriptor) {
    result.metadata.schema_version =
        child->descriptor->review_receipt_id ? 4 : 3;
  }
  return result;
}

auto started() -> domain::RunStarted {
  return {make_id<domain::SurfaceId>("surface"),
          make_id<domain::WorkspaceId>("chat"),
          make_id<domain::PermissionProfileId>("observe"), std::nullopt};
}

auto verification_payload(const domain::InvocationId& invocation,
                          const domain::ArtifactId& artifact)
    -> domain::VerificationEvidenceRecorded {
  return {domain::VerificationEvidence{
      make_id<domain::VerificationEvidenceId>("verification"),
      domain::VerificationKind::test,
      std::nullopt,
      domain::VerificationOutcome::passed,
      {make_id<domain::RepositoryId>("repository"),
       {"sha256", "aaaaaaaaaaaaaaaa", 0}},
      std::nullopt,
      domain::ContentDigest{"sha256", "bbbbbbbbbbbbbbbb", 12},
      {"ctest", "3.28", "read", invocation},
      std::chrono::sys_time<std::chrono::milliseconds>{
          std::chrono::milliseconds{1200}},
      "tests passed",
      {{domain::VerificationOutputStream::standard_output, "passed", 6, false,
        std::nullopt}},
      {{domain::VerificationDiagnosticSeverity::warning, "W1", "warning",
        std::nullopt}},
      {artifact}}};
}

auto review_draft() -> domain::ReviewReceiptDraft {
  const auto receipt = make_id<domain::ReviewReceiptId>("review-receipt");
  const auto requirement =
      make_id<domain::ReviewRequirementId>("review-requirement");
  const auto artifact = make_id<domain::ArtifactId>("review-artifact");
  return {receipt,
          {{make_id<domain::RepositoryId>("review-repository"),
            {"sha256", "aaaaaaaaaaaaaaaa", 0}},
           "0123456789abcdef"},
          {{requirement,
            domain::ReviewEvidenceKind::verification,
            "ctest",
            "3.28",
            make_id<domain::VerificationEvidenceId>("review-verification"),
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            {"sha256", "bbbbbbbbbbbbbbbb", 64},
            {{artifact, {"sha256", "cccccccccccccccc", 128}}}}},
          domain::ReviewParticipantProvenance{
              {"author", "Author"},
              make_id<domain::RunId>("author-run"),
              std::string{"fake-backend"},
              std::string{"1"},
              make_id<domain::ModelId>("author-model"),
              std::string{"2026-08-28"}}};
}

auto plan_revision() -> domain::PlanRevision {
  return {make_id<domain::PlanId>("plan"),
          make_id<domain::PlanRevisionId>("revision-1"),
          std::nullopt,
          "Implement the accepted work",
          domain::RepositorySnapshotIdentity{
              make_id<domain::RepositoryId>("repository"),
              {"sha256", "dddddddddddddddd", 0}},
          {{make_id<domain::PlanTaskId>("task"),
            std::nullopt,
            {},
            "Implement the contract",
            {"The contract replays deterministically"},
            {domain::Effect::write},
            {{domain::Effect::write, "repository_path", "include"}}}},
          {{make_id<domain::EvidenceId>("planning-evidence"),
            {"sha256", "eeeeeeeeeeeeeeee", 32}}}};
}

auto backlog_item() -> domain::ProjectBacklogItem {
  const auto revision = plan_revision();
  return {make_id<domain::ProjectBacklogItemId>("backlog-item"),
          make_id<domain::RepositoryId>("repository"),
          {make_id<domain::SessionId>("session"), revision.plan_id,
           revision.revision_id, revision.tasks.front().task_id},
          revision.tasks.front(),
          domain::ProjectBacklogDecisionSource::user};
}

auto memory_proposal(domain::MemoryOwner owner,
                     const std::string_view suffix = "global")
    -> domain::MemoryProposal {
  return {
      make_id<domain::MemoryProposalId>("memory-proposal:" +
                                        std::string{suffix}),
      make_id<domain::MemoryRecordId>("memory-record:" + std::string{suffix}),
      std::move(owner),
      domain::MemoryKind::workflow,
      "Run focused tests before broad validation",
      "This keeps failures local and actionable",
      "focused tests passed",
      {make_id<domain::SessionId>("memory-source-session"),
       make_id<domain::RunId>("memory-source-run"),
       make_id<domain::InvocationId>("memory-source-invocation"),
       {make_id<domain::EventId>("memory-source-event")}},
      {make_id<domain::ModelId>("memory-model"), "test-runtime", "1"},
      std::nullopt,
      {}};
}

auto memory_record(const domain::MemoryProposal& proposal)
    -> domain::MemoryRecord {
  return {proposal.record_id, proposal.proposal_id, proposal.owner,
          proposal.kind,      proposal.content,     proposal.rationale,
          proposal.source,    proposal.producer};
}

auto memory_selection() -> domain::MemorySelection {
  const auto record =
      memory_record(memory_proposal(domain::MemoryOwner::global()));
  const auto digest = domain::memory_record_digest(record);
  REQUIRE(digest);
  const auto evidence = domain::memory_evidence_input(record, 1);
  REQUIRE(evidence);
  const auto& text =
      std::get<domain::TextBlock>(evidence->message.content.front()).text;
  detail::Sha256 hash;
  hash.update(std::as_bytes(std::span{text.data(), text.size()}));
  domain::MemorySelection selected{
      1,
      {},
      {},
      1000,
      1000,
      {{make_id<domain::SessionId>("memory-journal"),
        record.record_id,
        make_id<domain::EventId>("accepted-memory"),
        record.owner,
        record.source,
        *digest,
        {"sha256", hash.finish(), text.size()},
        evidence->order,
        evidence->estimated_tokens}}};
  REQUIRE(domain::seal_memory_selection(selected));
  return selected;
}

auto child_run_descriptor() -> domain::ChildRunDescriptor {
  return {make_id<domain::RunId>("run"),
          plan_revision().plan_id,
          plan_revision().revision_id,
          plan_revision().tasks.front().task_id,
          {make_id<domain::ContextParcelId>("context-parcel"),
           *plan_revision().source_snapshot,
           {make_id<domain::EvidenceId>("context-evidence")},
           128,
           32},
          {2, 3, 256, 128, std::chrono::seconds{30}},
          {domain::Effect::write},
          {{domain::Effect::write, "filesystem.root", "/workspace"}},
          1,
          review_draft().receipt_id};
}

auto session_task_result() -> domain::SessionTaskResult {
  const auto descriptor = child_run_descriptor();
  return {descriptor.plan_id,
          descriptor.revision_id,
          descriptor.task_id,
          make_id<domain::RunId>("child"),
          domain::SessionTaskOutcome::completed,
          {1, 2, {24, 12, 0, 0}},
          {make_id<domain::EvidenceId>("result-evidence")},
          {make_id<domain::ArtifactId>("result-artifact")},
          std::nullopt};
}

auto run_provenance() -> domain::RunProvenance {
  domain::RunProvenance result{
      "0.10.0",
      "venice",
      std::string{"1.2.3"},
      make_id<domain::ModelId>("model"),
      domain::CredentialSourceReference{
          domain::CredentialSourceKind::environment, "VENICE_API_KEY"},
      {{"model",
        std::string{"venice-model"},
        true,
        domain::ProvenanceSource::environment,
        false,
        {{domain::ProvenanceSource::environment,
          domain::ProvenanceDisposition::selected, std::nullopt},
         {domain::ProvenanceSource::file,
          domain::ProvenanceDisposition::shadowed,
          domain::ProvenanceDiagnosticCode::duplicate_source_value}}},
       {"secret",
        std::nullopt,
        true,
        domain::ProvenanceSource::file,
        true,
        {{domain::ProvenanceSource::file,
          domain::ProvenanceDisposition::selected, std::nullopt}}}},
      {{"aiforge", "0.10.0"}, {"sqlite3", "3.45.1"}},
      {{"read",
        {domain::Effect::read},
        {{domain::Effect::read, "root", "/workspace"}},
        "sha256:" + std::string(64, 'a')}},
      {{"venice.chat.web-search", std::string{"auto"},
        domain::RequestOptionSource::configuration},
       {"venice.chat.include-system-prompt", std::nullopt,
        domain::RequestOptionSource::provider_default},
       {"venice.media.safe-mode", std::string{"off"},
        domain::RequestOptionSource::session_override}}};
  result.tool_profile = domain::ToolProfileProvenance{
      make_id<domain::ToolProfileId>("essentials"),
      make_id<domain::ToolProfileId>("model-safe"),
      make_id<domain::ToolProfileId>("persona-safe"),
      std::vector<std::string>{"read"}};
  result.tool_policy = domain::ToolPolicyProvenance{
      "aiforge.tool-launch-policy.v1",
      make_id<domain::PermissionProfileId>("tools-medium-auto-v1"),
      domain::ToolRestrictionLevel::medium,
      domain::ToolApprovalMode::automatic,
      {domain::Effect::read},
      {{domain::Effect::read, "filesystem.root", "/workspace"}},
      {"read"}};
  return result;
}

auto run_provenance_v2() -> domain::RunProvenance {
  auto result = run_provenance();
  result.tool_policy = domain::ToolPolicyProvenance{
      "aiforge.tool-launch-policy.v2",
      make_id<domain::PermissionProfileId>("interactive-tools-v2"),
      domain::ToolRestrictionLevel::medium,
      domain::ToolApprovalMode::automatic,
      {domain::Effect::read},
      {{domain::Effect::read, "filesystem.root", "/workspace"}},
      {}};
  result.tool_policy->achieved_restriction_level =
      domain::ToolRestrictionLevel::medium;
  result.tool_policy->mechanism_identity = "aiforge.linux-restriction-levels";
  result.tool_policy->mechanism_version = "0018";
  result.tool_policy->restriction_policy_identity = "test.process-policy.v1";
  result.tool_policy->matcher_policy_identity =
      "aiforge.exact-tool-allowlist.v1";
  return result;
}

auto pricing_observation() -> domain::PricingObservation {
  domain::TextPricing pricing;
  pricing.base.input =
      domain::PriceRate{domain::DecimalAmount::from("1.42").value(),
                        domain::DecimalAmount::from("2.5").value()};
  pricing.base.output = domain::PriceRate{
      domain::DecimalAmount::from("2.83").value(), std::nullopt};
  return domain::make_pricing_observation(
             make_id<domain::ModelId>("model"), "venice.models", std::nullopt,
             std::chrono::sys_time<std::chrono::milliseconds>{
                 std::chrono::milliseconds{900}},
             domain::PricingCatalogOrigin::fresh_cache, std::move(pricing))
      .value();
}

auto open_store(const std::filesystem::path& path,
                storage::SessionStoreLimits limits = {})
    -> std::unique_ptr<adapters::SqliteSessionStore> {
  auto store = adapters::SqliteSessionStore::open(path, limits);
  REQUIRE(store);
  return std::move(*store);
}

auto create(storage::SessionStore& store, const std::string& id,
            const std::int64_t milliseconds) -> domain::SessionId {
  auto session = make_id<domain::SessionId>(id);
  REQUIRE(store.create_session(
      {session,
       domain::EventTimestamp{std::chrono::milliseconds{milliseconds}}}));
  return session;
}

auto all_payloads() -> std::vector<domain::RunEventPayload> {
  const auto inference = make_id<domain::InferenceId>("inference");
  const auto invocation = make_id<domain::InvocationId>("invocation");
  const auto parent_invocation =
      make_id<domain::InvocationId>("parent-invocation");
  const auto question = make_id<domain::QuestionId>("question");
  const auto artifact = make_id<domain::ArtifactId>("artifact");
  const auto view = make_id<domain::ViewId>("view");
  const auto message = make_id<domain::MessageId>("message");
  const domain::DomainError error{domain::ErrorCode::backend, "redacted", true};
  const domain::CapabilityScope scope{domain::Effect::read, "root",
                                      "/workspace"};
  const domain::QuestionDefinition definition{
      question,
      "Choose",
      domain::QuestionSelection::one,
      {{"yes", "Yes", std::string{"recommended"}}},
      true,
      1,
      1,
      domain::QuestionOtherInput{"Other", std::nullopt, 4096}};
  const domain::ArtifactMetadata artifact_metadata{
      artifact, "image/png", 3, "sha256:abc", std::nullopt, 1, 2, inference};
  auto usd = domain::MonetaryAmount::create(
                 "USD", domain::DecimalAmount::from("0").value())
                 .value();
  auto diem =
      domain::MonetaryAmount::create(
          "venice.diem", domain::DecimalAmount::from("0.0645375").value())
          .value();
  auto reported_cost =
      domain::ReportedCost::create({std::move(usd), std::move(diem)}).value();
  const auto video_operation =
      make_id<domain::VideoOperationId>("video-operation");
  const auto video_job = make_id<domain::VideoJobId>("video-job");
  const domain::VideoGenerationSpec video_spec{
      make_id<domain::ModelId>("video-model"), "video prompt",
      std::chrono::seconds{5}};
  const domain::ArtifactMetadata video_artifact{
      make_id<domain::ArtifactId>("video-artifact"),
      "video/mp4",
      42,
      "sha256:" + std::string(64, 'a'),
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt};
  const auto video_quote =
      domain::MonetaryAmount::create(
          "USD", domain::DecimalAmount::from("1.25").value())
          .value();
  const auto proposal = memory_proposal(domain::MemoryOwner::global());
  const auto record = memory_record(proposal);
  return {
      started(),
      domain::RunProvenanceRecorded{run_provenance()},
      domain::PersonaSelectionRecorded{
          {domain::PersonaSelectionAction::selected,
           domain::PersonaSelectionSource::command_line,
           domain::PersonaReference{
               make_id<domain::PersonaId>("persona:reviewer"),
               "reviewer",
               "personas/reviewer.md",
               {"sha256", std::string(64, 'a'), 7}},
           std::nullopt}},
      domain::SessionSpendCeilingSet{
          domain::SessionSpendCeiling::from("12.345678").value(),
          domain::SessionSpendCeilingSource::command_line},
      domain::RunAwaitingInput{question},
      domain::RunResumed{question},
      domain::RunCompletionRequested{},
      domain::RunCompleted{},
      domain::RunFailed{error},
      domain::RunCancelRequested{std::string{"requested"}},
      domain::RunCancelled{std::string{"cancelled"}},
      domain::UserContentAdded{domain::Message{
          message,
          domain::Role::user,
          {domain::TextBlock{"text"},
           domain::StructuredDataBlock{"application/example", "data"},
           domain::CitationBlock{"https://example.test", std::string{"title"}},
           domain::ArtifactReferenceBlock{artifact, std::string{"label"}},
           domain::UnknownContentBlock{"future.content"}},
          std::nullopt,
          {{invocation, "read", {"application/json", "{}"}}}}},
      domain::AssistantContentStarted{message, inference},
      domain::AssistantContentDeltaAdded{message, inference,
                                         domain::TextBlock{"delta"}},
      domain::AssistantContentFinished{message, inference},
      domain::InferenceStarted{inference, make_id<domain::ModelId>("model")},
      domain::InferencePricingObserved{inference, pricing_observation()},
      domain::ReasoningMetadataAdded{
          inference, std::string{"summary"}, {{"visibility", "summary"}}},
      domain::UsageRecorded{inference, domain::Usage{1, 2, 3, 4}},
      domain::InferenceCostRecorded{inference, std::move(reported_cost)},
      domain::InferenceFinished{inference, domain::FinishReason::tool_call},
      domain::InferenceFailed{inference, error},
      domain::InferenceCancelled{inference, std::string{"cancelled"}},
      domain::ToolProposed{invocation,
                           "read",
                           {"application/json", "{}"},
                           {domain::Effect::read, domain::Effect::network},
                           parent_invocation,
                           true,
                           {scope},
                           {scope},
                           message},
      domain::ToolPolicyDecided{invocation,
                                domain::PolicyDecision::allow,
                                {scope},
                                std::string{"allowed"},
                                domain::PolicyDecisionSource::saved_grant},
      domain::ToolApprovalRequested{
          invocation, {scope}, std::string{"runtime-owned reason"}},
      domain::ToolApprovalDecided{invocation,
                                  domain::ApprovalDecision::approved,
                                  {scope},
                                  domain::ApprovalGrantLifetime::saved},
      domain::ToolPolicyFailed{invocation, error},
      domain::ToolSpendReserved{domain::ToolSpendReservation{
          invocation,
          domain::MonetaryAmount::create(
              "USD", domain::DecimalAmount::from("0.2").value())
              .value(),
          domain::ToolSpendEstimateBasis::catalog_estimate,
          {"sha256", std::string(64, 'f'), 42},
          domain::EventTimestamp::max()}},
      domain::ToolStarted{invocation},
      domain::ToolProgressed{invocation, {domain::TextBlock{"working"}}},
      domain::ToolSpendReleased{invocation},
      domain::ToolSpendFinalized{domain::ToolSpendFinalization{
          invocation,
          domain::MonetaryAmount::create(
              "USD", domain::DecimalAmount::from("0.1").value())
              .value(),
          domain::ToolSpendFinalizationBasis::provider_reported,
          domain::ContentDigest{"sha256", std::string(64, 'e'), 21}}},
      domain::ToolSpendReconciliationRequired{
          invocation,
          domain::ToolSpendReconciliationReason::provider_cost_unavailable},
      domain::ToolResultRecorded{
          invocation, {domain::TextBlock{"done"}}, message},
      domain::ToolErrored{invocation, error, message},
      domain::QuestionRequested{definition},
      domain::QuestionAnswered{
          domain::QuestionAnswer{question, {"yes"}, std::string{"other"}}},
      domain::QuestionCancelled{question, std::string{"cancelled"}},
      domain::ArtifactCreated{artifact_metadata},
      domain::ArtifactReferenced{artifact, message},
      domain::ArtifactDisplayed{artifact, view, "right-pane"},
      domain::ArtifactRemovedFromView{artifact, view},
      verification_payload(invocation, artifact),
      domain::ReviewReceiptDrafted{review_draft()},
      domain::ReviewRequested{review_draft().receipt_id,
                              {"reviewer", "Reviewer"}},
      domain::ReviewFindingOpened{
          review_draft().receipt_id,
          {make_id<domain::ReviewFindingId>("review-finding"),
           "finding",
           make_id<domain::VerificationEvidenceId>("review-verification"),
           {make_id<domain::ArtifactId>("review-artifact")},
           domain::ReviewFindingSeverity::high,
           std::nullopt,
           {make_id<domain::VerificationEvidenceId>("review-verification")}}},
      domain::ReviewFindingResolved{
          review_draft().receipt_id,
          make_id<domain::ReviewFindingId>("review-finding"),
          {"reviewer", "Reviewer"},
          std::string{"resolved"}},
      domain::ReviewVerdictRecorded{
          review_draft().receipt_id,
          domain::ReviewVerdict::approved,
          {"reviewer", "Reviewer"},
          domain::ReviewParticipantProvenance{
              {"reviewer", "Reviewer"},
              make_id<domain::RunId>("reviewer-run"),
              std::string{"fake-backend"},
              std::string{"1"},
              make_id<domain::ModelId>("review-model"),
              std::string{"2026-08-28"}}},
      domain::ReviewVerdictRevoked{
          review_draft().receipt_id,
          make_id<domain::EventId>("review-verdict-event"),
          {"reviewer", "Reviewer"},
          "revoked"},
      domain::ReviewOverrideRecorded{domain::ReviewOverride{
          make_id<domain::ReviewOverrideId>("review-override"),
          review_draft().receipt_id,
          review_draft().candidate,
          {"maintainer", "Maintainer"},
          "explicit override"}},
      domain::ReviewOverrideRevoked{
          review_draft().receipt_id,
          make_id<domain::ReviewOverrideId>("review-override"),
          {"maintainer", "Maintainer"},
          "override revoked"},
      domain::PlanRevisionProposed{plan_revision()},
      domain::PlanRevisionDecisionRecorded{domain::PlanRevisionDecision{
          plan_revision().plan_id, plan_revision().revision_id,
          domain::PlanDecision::approved, domain::PlanDecisionSource::user,
          std::string{"approved after review"}}},
      domain::PlanRevisionInvalidated{domain::PlanRevisionInvalidation{
          plan_revision().plan_id,
          plan_revision().revision_id,
          {domain::PlanInvalidationTrigger::evidence_changed}}},
      domain::SessionTasksMaterialized{plan_revision().plan_id,
                                       plan_revision().revision_id},
      domain::ChildRunCreated{make_id<domain::RunId>("child"),
                              child_run_descriptor()},
      domain::SessionTaskResultRecorded{session_task_result()},
      domain::InterRunMessageSent{make_id<domain::RunId>("target"),
                                  {domain::TextBlock{"message"}}},
      domain::ProjectBacklogItemPromoted{backlog_item()},
      domain::ProjectBacklogItemStatusChanged{
          {backlog_item().item_id, backlog_item().repository_id,
           domain::ProjectBacklogItemStatus::resolved,
           domain::ProjectBacklogDecisionSource::policy,
           std::string{"completed by another session"},
           make_id<domain::EventId>("promotion-event")}},
      domain::MemoryProposed{proposal},
      domain::MemoryPolicyDecided{
          {proposal.proposal_id, domain::MemoryPolicyAction::stage,
           domain::MemoryDecisionSource::policy, "review required",
           make_id<domain::EventId>("memory-proposed-event")}},
      domain::MemoryAccepted{
          {record, domain::MemoryDecisionSource::user,
           make_id<domain::EventId>("memory-proposed-event")}},
      domain::MemoryEditedAndAccepted{
          {{record, domain::MemoryDecisionSource::user,
            make_id<domain::EventId>("memory-proposed-event")},
           make_id<domain::MemoryRecordId>("memory-replaced-record"),
           make_id<domain::EventId>("memory-record-event")}},
      domain::MemoryRejected{
          {proposal.proposal_id, domain::MemoryDecisionSource::user,
           "not durable", make_id<domain::EventId>("memory-proposed-event")}},
      domain::MemorySuperseded{
          {record.record_id,
           make_id<domain::MemoryRecordId>("memory-replacement-record"),
           domain::MemoryDecisionSource::user,
           make_id<domain::EventId>("memory-record-event")}},
      domain::MemoryExpired{{record.record_id,
                             domain::MemoryDecisionSource::user, "expired",
                             make_id<domain::EventId>("memory-record-event")}},
      domain::VideoGenerationRequested{video_operation, video_spec,
                                       video_artifact.artifact_id},
      domain::VideoQuoteObserved{video_operation, video_quote},
      domain::VideoJobQueued{video_operation, video_job},
      domain::VideoJobStatusObserved{video_operation, video_job, 1,
                                     domain::VideoJobState::completed},
      domain::VideoArtifactPublished{video_operation, video_job,
                                     video_artifact},
      domain::VideoCleanupPending{video_operation, video_job, 1},
      domain::VideoCleanupCompleted{video_operation, video_job, 1},
      domain::VideoCleanupFailed{video_operation, video_job, 2, error},
      domain::VideoTranscriptionRequested{
          video_operation, make_id<domain::ModelId>("transcription-model")},
      domain::VideoTranscriptionObserved{video_operation, "transcript",
                                         std::string{"en"}},
      domain::UnknownEvent{"future.event",
                           {"application/json", "{\"nested\":{\"value\":1}}"}},
  };
}

TEST_CASE("SQLite discovers repository backlog facts across source sessions",
          "[storage][sqlite][plan][tasks]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "aiforge" / "sessions.sqlite3";
  auto store = open_store(path);
  const auto session = create(*store, "session", 100);
  auto item = backlog_item();
  item.origin.session_id = session;
  REQUIRE(store->append_events(
      session,
      std::array{
          event(1, domain::ProjectBacklogItemPromoted{item}, "promotion-event"),
          event(2,
                domain::ProjectBacklogItemStatusChanged{
                    {item.item_id, item.repository_id,
                     domain::ProjectBacklogItemStatus::resolved,
                     domain::ProjectBacklogDecisionSource::user,
                     std::string{"implemented"},
                     make_id<domain::EventId>("promotion-event")}},
                "status-event")}));

  const auto other_session = create(*store, "other-session", 200);
  auto other = item;
  other.item_id = make_id<domain::ProjectBacklogItemId>("other-item");
  other.repository_id = make_id<domain::RepositoryId>("other-repository");
  other.origin.session_id = other_session;
  REQUIRE(store->append_events(
      other_session,
      std::array{event(1, domain::ProjectBacklogItemPromoted{other},
                       "other-promotion")}));

  const auto histories = store->replay_project_backlog(item.repository_id, 10);
  REQUIRE(histories);
  REQUIRE(histories->size() == 1);
  REQUIRE(histories->front().session_id == session);
  const auto projection =
      domain::ProjectBacklogProjection::rebuild(item.repository_id, *histories);
  REQUIRE(projection);
  REQUIRE(projection->items().size() == 1);
  REQUIRE(projection->items().front().status ==
          domain::ProjectBacklogItemStatus::resolved);
  REQUIRE(projection->items().front().status_reason == "implemented");

  const auto invalid_limit =
      store->replay_project_backlog(item.repository_id, 0);
  REQUIRE_FALSE(invalid_limit);
  REQUIRE(invalid_limit.error().code ==
          storage::SessionStoreErrorCode::invalid_argument);
}

auto execute_sql(const std::filesystem::path& path, const std::string& sql)
    -> void {
  sqlite3* database{};
  REQUIRE(sqlite3_open(path.c_str(), &database) == SQLITE_OK);
  char* message{};
  const auto result =
      sqlite3_exec(database, sql.c_str(), nullptr, nullptr, &message);
  if (message != nullptr) sqlite3_free(message);
  REQUIRE(result == SQLITE_OK);
  REQUIRE(sqlite3_close(database) == SQLITE_OK);
}

auto execute_sql_result(const std::filesystem::path& path,
                        const std::string& sql) -> int {
  sqlite3* database{};
  REQUIRE(sqlite3_open(path.c_str(), &database) == SQLITE_OK);
  char* message{};
  const auto result =
      sqlite3_exec(database, sql.c_str(), nullptr, nullptr, &message);
  if (message != nullptr) sqlite3_free(message);
  REQUIRE(sqlite3_close(database) == SQLITE_OK);
  return result;
}

auto replace_with_version_three_store(const std::filesystem::path& path,
                                      const std::string_view session_rows)
    -> void {
  std::string sql{
      "PRAGMA foreign_keys=OFF;"
      "DROP TABLE events;"
      "DROP TABLE sessions;"
      "CREATE TABLE sessions("
      "session_id TEXT PRIMARY KEY NOT NULL,"
      "created_at_ms INTEGER NOT NULL,"
      "storage_format_version INTEGER NOT NULL "
      "CHECK(storage_format_version=1),"
      "session_kind TEXT NOT NULL CHECK(session_kind IN ('user','memory')) "
      "DEFAULT 'user',"
      "journal_key TEXT,"
      "CHECK((session_kind='user' AND journal_key IS NULL) OR "
      "(session_kind='memory' AND journal_key IS NOT NULL))) STRICT;"
      "CREATE TABLE events("
      "session_id TEXT NOT NULL REFERENCES sessions(session_id),"
      "sequence INTEGER NOT NULL CHECK(sequence>0),"
      "event_id TEXT NOT NULL,run_id TEXT NOT NULL,"
      "schema_version INTEGER NOT NULL CHECK(schema_version>0),"
      "timestamp_ms INTEGER NOT NULL,caused_by_event_id TEXT,"
      "parent_run_id TEXT,invocation_id TEXT,payload_type TEXT NOT NULL,"
      "payload_json TEXT NOT NULL CHECK(json_valid(payload_json)),"
      "PRIMARY KEY(session_id,sequence),UNIQUE(session_id,event_id)) STRICT;"
      "CREATE INDEX events_session_timestamp "
      "ON events(session_id,timestamp_ms);"
      "CREATE UNIQUE INDEX sessions_memory_journal_key "
      "ON sessions(journal_key) WHERE session_kind='memory';"};
  sql += session_rows;
  sql += "PRAGMA user_version=3;PRAGMA foreign_keys=ON;";
  execute_sql(path, sql);
}

auto file_content(const std::filesystem::path& path) -> std::string {
  std::ifstream input{path, std::ios::binary};
  REQUIRE(input);
  return {std::istreambuf_iterator<char>{input},
          std::istreambuf_iterator<char>{}};
}

auto directory_entries(const std::filesystem::path& path)
    -> std::vector<std::string> {
  std::vector<std::string> entries;
  for (const auto& entry : std::filesystem::directory_iterator{path}) {
    entries.push_back(entry.path().filename().string());
  }
  std::ranges::sort(entries);
  return entries;
}

} // namespace

TEST_CASE("session-store path resolution follows XDG state semantics",
          "[storage][path][failure]") {
  auto path = adapters::resolve_session_store_path(
      {std::filesystem::path{"/tmp/state"},
       std::filesystem::path{"/home/user"}});
  REQUIRE(path == "/tmp/state/aiforge/sessions.sqlite3");

  path = adapters::resolve_session_store_path(
      {std::filesystem::path{"relative"}, std::filesystem::path{"/home/user"}});
  REQUIRE(path == "/home/user/.local/state/aiforge/sessions.sqlite3");

  path = adapters::resolve_session_store_path({std::nullopt, std::nullopt});
  REQUIRE_FALSE(path);
  REQUIRE(path.error().code ==
          storage::SessionStoreErrorCode::invalid_argument);
}

TEST_CASE("SQLite store creates restrictive state and rejects unsafe paths",
          "[storage][sqlite][path][failure]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "state" / "aiforge" / "sessions.sqlite3";
  auto store = open_store(path);
  struct stat directory_info{};
  struct stat file_info{};
  REQUIRE(::stat(path.parent_path().c_str(), &directory_info) == 0);
  REQUIRE(::stat(path.c_str(), &file_info) == 0);
  REQUIRE((directory_info.st_mode & 0777) == 0700);
  REQUIRE((file_info.st_mode & 0777) == 0600);
  store.reset();

  REQUIRE(::chmod(path.c_str(), 0644) == 0);
  auto insecure = adapters::SqliteSessionStore::open(path);
  REQUIRE_FALSE(insecure);
  REQUIRE(insecure.error().code ==
          storage::SessionStoreErrorCode::permission_denied);

  REQUIRE(::chmod(path.c_str(), 0600) == 0);
  const auto target = temporary.path() / "target.sqlite3";
  std::ofstream{target} << "target";
  const auto linked = temporary.path() / "state" / "aiforge" / "linked.sqlite3";
  std::filesystem::create_symlink(target, linked);
  auto symlink = adapters::SqliteSessionStore::open(linked);
  REQUIRE_FALSE(symlink);
  REQUIRE(symlink.error().code ==
          storage::SessionStoreErrorCode::permission_denied);
}

TEST_CASE("read-only SQLite replay neither creates nor changes state files",
          "[storage][sqlite][replay][read-only]") {
  TemporaryDirectory temporary;
  const auto missing_path = temporary.path() / "missing" / "sessions.sqlite3";
  auto missing =
      adapters::SqliteSessionStore::open_existing_read_only(missing_path);
  REQUIRE_FALSE(missing);
  CHECK_FALSE(std::filesystem::exists(missing_path.parent_path()));

  const auto path = temporary.path() / "aiforge" / "sessions.sqlite3";
  auto writable = open_store(path);
  const auto session = create(*writable, "read-only-session", 100);
  const std::array events{event(1, started(), "read-only-event")};
  REQUIRE(writable->append_events(session, events));
  writable.reset();

  const auto before_content = file_content(path);
  const auto before_entries = directory_entries(path.parent_path());
  auto read_only = adapters::SqliteSessionStore::open_existing_read_only(path);
  REQUIRE(read_only);
  const auto replayed = (*read_only)->replay_events(session);
  REQUIRE(replayed);
  CHECK(std::ranges::equal(*replayed, events));
  read_only->reset();
  CHECK(file_content(path) == before_content);
  CHECK(directory_entries(path.parent_path()) == before_entries);
}

TEST_CASE("SQLite creates a session and its initial events atomically",
          "[storage][sqlite][create][crash][failure]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "aiforge" / "sessions.sqlite3";
  auto store = open_store(path);
  const auto session = make_id<domain::SessionId>("atomic-session");
  const std::array events{event(1, started(), "initial-event")};
  auto invalid_events = events;
  invalid_events.front().metadata.sequence = 2;
  const auto invalid = store->create_session_with_events(
      {session, domain::EventTimestamp{std::chrono::milliseconds{100}}},
      invalid_events);
  REQUIRE_FALSE(invalid);
  CHECK(invalid.error().code ==
        storage::SessionStoreErrorCode::invalid_argument);
  REQUIRE_FALSE(store->open_session(session));

  execute_sql(
      path, "CREATE TRIGGER reject_initial_event BEFORE INSERT ON events "
            "BEGIN SELECT RAISE(FAIL,'injected initial-event failure'); END;");

  const auto failed = store->create_session_with_events(
      {session, domain::EventTimestamp{std::chrono::milliseconds{100}}},
      events);

  REQUIRE_FALSE(failed);
  const auto absent = store->open_session(session);
  REQUIRE_FALSE(absent);
  CHECK(absent.error().code == storage::SessionStoreErrorCode::not_found);

  execute_sql(path, "DROP TRIGGER reject_initial_event;");
  REQUIRE(store->create_session_with_events(
      {session, domain::EventTimestamp{std::chrono::milliseconds{100}}},
      events));
  const auto replayed = store->replay_events(session);
  REQUIRE(replayed);
  CHECK(std::ranges::equal(*replayed, events));
}

TEST_CASE("SQLite storage version one migrates backlog indexes transactionally",
          "[storage][sqlite][migration][plan][tasks]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "aiforge" / "sessions.sqlite3";
  auto store = open_store(path);
  store.reset();
  execute_sql(
      path,
      "PRAGMA foreign_keys=OFF;"
      "DROP TABLE events;"
      "DROP TABLE sessions;"
      "CREATE TABLE sessions("
      "session_id TEXT PRIMARY KEY NOT NULL,"
      "created_at_ms INTEGER NOT NULL,"
      "storage_format_version INTEGER NOT NULL "
      "CHECK(storage_format_version=1)) STRICT;"
      "CREATE TABLE events("
      "session_id TEXT NOT NULL REFERENCES sessions(session_id),"
      "sequence INTEGER NOT NULL CHECK(sequence>0),"
      "event_id TEXT NOT NULL,run_id TEXT NOT NULL,"
      "schema_version INTEGER NOT NULL CHECK(schema_version>0),"
      "timestamp_ms INTEGER NOT NULL,caused_by_event_id TEXT,"
      "parent_run_id TEXT,invocation_id TEXT,payload_type TEXT NOT NULL,"
      "payload_json TEXT NOT NULL CHECK(json_valid(payload_json)),"
      "PRIMARY KEY(session_id,sequence),UNIQUE(session_id,event_id)) STRICT;"
      "CREATE INDEX events_session_timestamp "
      "ON events(session_id,timestamp_ms);"
      "PRAGMA user_version=1;"
      "PRAGMA foreign_keys=ON;");

  store = open_store(path);
  const auto repository = make_id<domain::RepositoryId>("repository");
  const auto histories = store->replay_project_backlog(repository, 10);
  REQUIRE(histories);
  REQUIRE(histories->empty());
  const auto persona_journal = store->open_or_create_memory_journal(
      {make_id<domain::SessionId>("migrated-persona-journal"),
       domain::MemoryOwner::persona(
           make_id<domain::PersonaId>("persona:migrated")),
       domain::EventTimestamp{std::chrono::milliseconds{200}}});
  REQUIRE(persona_journal);
}

TEST_CASE("SQLite storage version three migrates exact memory owners",
          "[storage][sqlite][migration][memory][persona]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "aiforge" / "sessions.sqlite3";
  auto store = open_store(path);
  store.reset();
  replace_with_version_three_store(
      path, "INSERT INTO sessions VALUES"
            "('preserved-user',99,1,'user',NULL),"
            "('legacy-global',100,1,'memory','global'),"
            "('legacy-project',101,1,'memory','project:repository');"
            "INSERT INTO events VALUES("
            "'preserved-user',1,'migrated-event','run',1,1001,NULL,NULL,NULL,"
            "'run.started',"
            "'{\"permission_profile_id\":\"observe\",\"persona_id\":null,"
            "\"surface_id\":\"surface\",\"workspace_id\":\"chat\"}');");

  store = open_store(path);
  const auto events =
      store->replay_events(make_id<domain::SessionId>("preserved-user"));
  REQUIRE(events);
  REQUIRE(*events ==
          std::vector<domain::RunEvent>{event(1, started(), "migrated-event")});
  const auto global = store->open_or_create_memory_journal(
      {make_id<domain::SessionId>("replacement-global"),
       domain::MemoryOwner::global(),
       domain::EventTimestamp{std::chrono::milliseconds{200}}});
  REQUIRE(global);
  REQUIRE(global->session_id == make_id<domain::SessionId>("legacy-global"));
  const auto project = store->open_or_create_memory_journal(
      {make_id<domain::SessionId>("replacement-project"),
       domain::MemoryOwner::repository(
           make_id<domain::RepositoryId>("repository")),
       domain::EventTimestamp{std::chrono::milliseconds{200}}});
  REQUIRE(project);
  REQUIRE(project->session_id == make_id<domain::SessionId>("legacy-project"));
  const auto persona = store->open_or_create_memory_journal(
      {make_id<domain::SessionId>("new-persona"),
       domain::MemoryOwner::persona(
           make_id<domain::PersonaId>("persona:reviewer")),
       domain::EventTimestamp{std::chrono::milliseconds{200}}});
  REQUIRE(persona);
  REQUIRE(persona->session_id == make_id<domain::SessionId>("new-persona"));

  CHECK(execute_sql_result(
            path,
            "INSERT INTO sessions VALUES"
            "('ownerless-memory',200,1,'memory','ownerless',NULL,NULL)") ==
        SQLITE_CONSTRAINT);
  CHECK(execute_sql_result(path,
                           "INSERT INTO sessions VALUES"
                           "('owned-user',200,1,'user',NULL,'global',NULL)") ==
        SQLITE_CONSTRAINT);
  CHECK(execute_sql_result(
            path, "INSERT INTO sessions VALUES"
                  "('invalid-kind',200,1,'memory','invalid','invalid',NULL)") ==
        SQLITE_CONSTRAINT);
  CHECK(execute_sql_result(
            path, "UPDATE sessions SET journal_owner_kind='persona',"
                  "journal_owner_id=NULL WHERE session_id='legacy-global'") ==
        SQLITE_CONSTRAINT);
  CHECK(execute_sql_result(path, "UPDATE sessions SET journal_key='owned' "
                                 "WHERE session_id='preserved-user'") ==
        SQLITE_CONSTRAINT);

  const auto global_after = store->open_or_create_memory_journal(
      {make_id<domain::SessionId>("unused-global"),
       domain::MemoryOwner::global(),
       domain::EventTimestamp{std::chrono::milliseconds{300}}});
  REQUIRE(global_after);
  CHECK(global_after->session_id ==
        make_id<domain::SessionId>("legacy-global"));
  const auto events_after =
      store->replay_events(make_id<domain::SessionId>("preserved-user"));
  REQUIRE(events_after);
  CHECK(*events_after == *events);
}

TEST_CASE("SQLite v3 memory migration rolls back malformed legacy owners",
          "[storage][sqlite][migration][memory][failure]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "aiforge" / "sessions.sqlite3";
  auto store = open_store(path);
  store.reset();
  replace_with_version_three_store(
      path, "INSERT INTO sessions VALUES"
            "('preserved-user',99,1,'user',NULL),"
            "('malformed-owner',100,1,'memory','project:');");

  const auto failed = adapters::SqliteSessionStore::open(path);
  REQUIRE_FALSE(failed);
  REQUIRE(failed.error().code == storage::SessionStoreErrorCode::corrupt);

  sqlite3* database{};
  REQUIRE(sqlite3_open(path.c_str(), &database) == SQLITE_OK);
  sqlite3_stmt* statement{};
  REQUIRE(sqlite3_prepare_v2(database, "PRAGMA user_version", -1, &statement,
                             nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_step(statement) == SQLITE_ROW);
  CHECK(sqlite3_column_int(statement, 0) == 3);
  REQUIRE(sqlite3_finalize(statement) == SQLITE_OK);
  REQUIRE(sqlite3_prepare_v2(database,
                             "SELECT count(*) FROM sessions WHERE "
                             "session_id='preserved-user'",
                             -1, &statement, nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_step(statement) == SQLITE_ROW);
  CHECK(sqlite3_column_int(statement, 0) == 1);
  REQUIRE(sqlite3_finalize(statement) == SQLITE_OK);
  REQUIRE(sqlite3_prepare_v2(database,
                             "SELECT count(*) FROM sessions WHERE "
                             "session_id='malformed-owner' AND "
                             "journal_key='project:'",
                             -1, &statement, nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_step(statement) == SQLITE_ROW);
  CHECK(sqlite3_column_int(statement, 0) == 1);
  REQUIRE(sqlite3_finalize(statement) == SQLITE_OK);
  REQUIRE(sqlite3_close(database) == SQLITE_OK);
}

TEST_CASE("SQLite v3 memory migration rejects duplicate exact owners",
          "[storage][sqlite][migration][memory][failure]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "aiforge" / "sessions.sqlite3";
  auto store = open_store(path);
  store.reset();
  replace_with_version_three_store(
      path, "DROP INDEX sessions_memory_journal_key;"
            "INSERT INTO sessions VALUES"
            "('duplicate-one',100,1,'memory','project:repository'),"
            "('duplicate-two',101,1,'memory','project:repository');");

  const auto failed = adapters::SqliteSessionStore::open(path);
  REQUIRE_FALSE(failed);
  REQUIRE(failed.error().code == storage::SessionStoreErrorCode::corrupt);

  sqlite3* database{};
  REQUIRE(sqlite3_open(path.c_str(), &database) == SQLITE_OK);
  sqlite3_stmt* statement{};
  REQUIRE(sqlite3_prepare_v2(database, "PRAGMA user_version", -1, &statement,
                             nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_step(statement) == SQLITE_ROW);
  CHECK(sqlite3_column_int(statement, 0) == 3);
  REQUIRE(sqlite3_finalize(statement) == SQLITE_OK);
  REQUIRE(sqlite3_prepare_v2(database, "SELECT count(*) FROM sessions", -1,
                             &statement, nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_step(statement) == SQLITE_ROW);
  CHECK(sqlite3_column_int(statement, 0) == 2);
  REQUIRE(sqlite3_finalize(statement) == SQLITE_OK);
  REQUIRE(sqlite3_close(database) == SQLITE_OK);
}

TEST_CASE("SQLite memory journals are isolated by exact owner",
          "[storage][sqlite][memory][persona][failure]") {
  TemporaryDirectory temporary;
  auto store = open_store(temporary.path() / "aiforge" / "sessions.sqlite3");
  const auto created = domain::EventTimestamp{std::chrono::milliseconds{100}};
  const auto global = store->open_or_create_memory_journal(
      {make_id<domain::SessionId>("global-journal"),
       domain::MemoryOwner::global(), created});
  const auto repository = store->open_or_create_memory_journal(
      {make_id<domain::SessionId>("repository-journal"),
       domain::MemoryOwner::repository(
           make_id<domain::RepositoryId>("shared-name")),
       created});
  const auto persona = store->open_or_create_memory_journal(
      {make_id<domain::SessionId>("persona-journal"),
       domain::MemoryOwner::persona(make_id<domain::PersonaId>("shared-name")),
       created});
  REQUIRE(global);
  REQUIRE(repository);
  REQUIRE(persona);
  CHECK(global->session_id == make_id<domain::SessionId>("global-journal"));
  CHECK(repository->session_id ==
        make_id<domain::SessionId>("repository-journal"));
  CHECK(persona->session_id == make_id<domain::SessionId>("persona-journal"));

  const auto reopened = store->open_or_create_memory_journal(
      {make_id<domain::SessionId>("unused-candidate"),
       domain::MemoryOwner::persona(make_id<domain::PersonaId>("shared-name")),
       created});
  REQUIRE(reopened);
  CHECK(reopened->session_id == persona->session_id);

  const domain::MemoryOwner invalid{
      domain::MemoryOwnerKind::persona,
      make_id<domain::RepositoryId>("wrong-id-kind"), std::nullopt};
  const auto rejected = store->open_or_create_memory_journal(
      {make_id<domain::SessionId>("invalid-owner"), invalid, created});
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().code ==
        storage::SessionStoreErrorCode::invalid_argument);
}

TEST_CASE("all typed payloads and opaque future payloads round trip",
          "[storage][sqlite][codec]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "aiforge" / "sessions.sqlite3";
  auto store = open_store(path);
  const auto session = create(*store, "session", 100);
  auto payloads = all_payloads();
  std::vector<domain::RunEvent> events;
  for (std::size_t index = 0; index < payloads.size(); ++index) {
    events.push_back(event(index + 1, std::move(payloads[index])));
  }
  REQUIRE(store->append_events(session, events));
  const auto replayed = store->replay_events(session);
  const auto replay_error = replayed ? std::string{} : replayed.error().message;
  INFO(replay_error);
  REQUIRE(replayed);
  REQUIRE(*replayed == events);

  const auto info = store->open_session(session);
  REQUIRE(info);
  REQUIRE(info->last_sequence == events.size());
  REQUIRE(info->last_activity_at == events.back().metadata.timestamp);
  REQUIRE(info->run_count == 1);
  const auto listed = store->list_sessions(10);
  REQUIRE(listed);
  REQUIRE(*listed == std::vector<storage::SessionInfo>{*info});

  auto malformed_result = session_task_result();
  malformed_result.error =
      domain::DomainError{domain::ErrorCode::backend, "invalid", false};
  auto malformed = event(events.size() + 1,
                         domain::SessionTaskResultRecorded{malformed_result},
                         "malformed-task-result");
  const auto malformed_append =
      store->append_events(session, std::array{malformed});
  REQUIRE_FALSE(malformed_append);
  REQUIRE(malformed_append.error().code ==
          storage::SessionStoreErrorCode::invalid_argument);

  const auto future_session = create(*store, "future-session", 200);
  auto future =
      event(1,
            domain::UnknownEvent{
                "run.started", {"application/json", "{\"future_field\":true}"}},
            "future-event");
  future.metadata.schema_version = 3;
  const auto future_append =
      store->append_events(future_session, std::array{future});
  INFO((future_append ? "future schema appended"
                      : future_append.error().message));
  REQUIRE(future_append);
  const auto future_replay = store->replay_events(future_session);
  REQUIRE(future_replay);
  REQUIRE(*future_replay == std::vector<domain::RunEvent>{future});

  auto noncanonical =
      event(2,
            domain::UnknownEvent{"future.noncanonical",
                                 {"application/json", "{ \"value\": 1 }"}},
            "noncanonical");
  const auto rejected =
      store->append_events(future_session, std::array{noncanonical});
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().code ==
          storage::SessionStoreErrorCode::invalid_argument);
}

TEST_CASE("schema-v1 memory codecs retain global and project owners",
          "[storage][sqlite][codec][memory][compatibility]") {
  TemporaryDirectory temporary;
  auto store = open_store(temporary.path() / "aiforge" / "sessions.sqlite3");
  const auto session = create(*store, "legacy-memory-owners", 100);
  const auto global = memory_proposal(domain::MemoryOwner::global(), "legacy");
  const auto project =
      memory_proposal(domain::MemoryOwner::repository(
                          make_id<domain::RepositoryId>("legacy-repository")),
                      "legacy-project");
  const std::vector events{
      event(1, domain::MemoryProposed{global}, "legacy-global-memory"),
      event(2,
            domain::MemoryAccepted{
                {memory_record(project), domain::MemoryDecisionSource::user,
                 make_id<domain::EventId>("legacy-project-proposal")}},
            "legacy-project-memory")};

  REQUIRE(store->append_events(session, events));
  const auto replayed = store->replay_events(session);
  REQUIRE(replayed);
  CHECK(*replayed == events);
}

TEST_CASE("schema-v2 persona memory codecs are exact and fail closed",
          "[storage][sqlite][codec][memory][persona][failure]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "aiforge" / "sessions.sqlite3";
  auto store = open_store(path);
  const auto session = create(*store, "persona-memory", 100);
  const auto proposal =
      memory_proposal(domain::MemoryOwner::persona(
                          make_id<domain::PersonaId>("persona:reviewer")),
                      "persona");
  auto proposed =
      event(1, domain::MemoryProposed{proposal}, "persona-memory-proposed");
  proposed.metadata.schema_version = 2;
  auto accepted =
      event(2,
            domain::MemoryAccepted{{memory_record(proposal),
                                    domain::MemoryDecisionSource::user,
                                    proposed.metadata.event_id}},
            "persona-memory-accepted");
  accepted.metadata.schema_version = 2;
  const std::vector events{proposed, accepted};
  REQUIRE(store->append_events(session, events));
  const auto replayed = store->replay_events(session);
  REQUIRE(replayed);
  CHECK(*replayed == events);

  const auto legacy_session = create(*store, "invalid-legacy-persona", 200);
  proposed.metadata.sequence = 1;
  proposed.metadata.schema_version = 1;
  proposed.metadata.event_id =
      make_id<domain::EventId>("legacy-persona-memory");
  const auto rejected =
      store->append_events(legacy_session, std::array{proposed});
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().code ==
        storage::SessionStoreErrorCode::invalid_argument);

  store.reset();
  execute_sql(path, "UPDATE events SET payload_json=json_set(payload_json,"
                    "'$.proposal.owner.repository_id','forged-repository') "
                    "WHERE event_id='persona-memory-proposed'");
  store = open_store(path);
  const auto corrupt = store->replay_events(session);
  REQUIRE_FALSE(corrupt);
  CHECK(corrupt.error().code == storage::SessionStoreErrorCode::corrupt);
}

TEST_CASE("legacy review payload shapes remain canonical",
          "[storage][sqlite][codec][review][compatibility]") {
  TemporaryDirectory temporary;
  auto store = open_store(temporary.path() / "aiforge" / "sessions.sqlite3");
  const auto session = create(*store, "legacy-review", 100);
  auto draft = review_draft();
  draft.author.reset();
  const domain::ReviewFinding finding{
      make_id<domain::ReviewFindingId>("legacy-finding"),
      "legacy finding",
      make_id<domain::VerificationEvidenceId>("review-verification"),
      {make_id<domain::ArtifactId>("review-artifact")},
      domain::ReviewFindingSeverity::medium,
      std::nullopt,
      {}};
  std::vector<domain::RunEvent> events{
      event(1, domain::ReviewReceiptDrafted{draft}),
      event(2, domain::ReviewRequested{draft.receipt_id,
                                       {"reviewer", "Reviewer"}}),
      event(3, domain::ReviewFindingOpened{draft.receipt_id, finding}),
      event(4, domain::ReviewVerdictRecorded{
                   draft.receipt_id,
                   domain::ReviewVerdict::changes_requested,
                   {"reviewer", "Reviewer"},
                   std::nullopt})};
  REQUIRE(store->append_events(session, events));
  const auto replayed = store->replay_events(session);
  REQUIRE(replayed);
  REQUIRE(*replayed == events);
}

TEST_CASE("automatic approval evidence is schema-v2 bounded and strict",
          "[storage][sqlite][codec][tool-policy][failure]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "aiforge" / "sessions.sqlite3";
  auto store = open_store(path);
  const auto session = create(*store, "automatic-policy", 100);
  const auto invocation = make_id<domain::InvocationId>("automatic-call");
  auto decided = event(
      1,
      domain::ToolPolicyDecided{
          invocation,
          domain::PolicyDecision::allow,
          {{domain::Effect::read, "filesystem.root", "/repo"}},
          std::string{"allowed by a bounded automatic approval rule"},
          domain::PolicyDecisionSource::automatic_matcher,
          domain::AutomaticApprovalEvidence{
              "aiforge.auto-policy.v1.sha256:" + std::string(64, 'a'),
              "aiforge.auto-rule.exact.v1.sha256:" + std::string(64, 'b')}},
      "automatic-policy-decision");
  decided.metadata.schema_version = 2;
  decided.metadata.invocation_id = invocation;
  REQUIRE(store->append_events(session, std::array{decided}));
  const auto replayed = store->replay_events(session);
  REQUIRE(replayed);
  REQUIRE(*replayed == std::vector<domain::RunEvent>{decided});
  store.reset();

  SECTION("evidence is missing") {
    execute_sql(path, "UPDATE events SET payload_json=json_remove(payload_json,"
                      "'$.automatic_approval') WHERE event_id="
                      "'automatic-policy-decision'");
  }
  SECTION("evidence contains a raw identity") {
    execute_sql(path, "UPDATE events SET payload_json=json_set(payload_json,"
                      "'$.automatic_approval.rule_identity','/secret/rule') "
                      "WHERE event_id='automatic-policy-decision'");
  }
  SECTION("policy identity is secret-like alphanumeric text") {
    execute_sql(path, "UPDATE events SET payload_json=json_set(payload_json,"
                      "'$.automatic_approval.policy_identity',"
                      "'SecretLikePolicyToken123') WHERE event_id="
                      "'automatic-policy-decision'");
  }
  SECTION("rule identity is secret-like alphanumeric text") {
    execute_sql(path, "UPDATE events SET payload_json=json_set(payload_json,"
                      "'$.automatic_approval.rule_identity',"
                      "'SecretLikeRuleToken456') WHERE event_id="
                      "'automatic-policy-decision'");
  }
  SECTION("digest uses uppercase hexadecimal") {
    execute_sql(path,
                "UPDATE events SET payload_json=json_set(payload_json,"
                "'$.automatic_approval.rule_identity',"
                "'aiforge.auto-rule.exact.v1.sha256:BBBBBBBBBBBBBBBBBBBBBBBB"
                "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB') WHERE event_id="
                "'automatic-policy-decision'");
  }
  SECTION("decision source contradicts evidence") {
    execute_sql(path, "UPDATE events SET payload_json=json_set(payload_json,"
                      "'$.source','permission_profile') WHERE event_id="
                      "'automatic-policy-decision'");
  }
  SECTION("evidence has an unknown field") {
    execute_sql(path,
                "UPDATE events SET payload_json=json_set(payload_json,"
                "'$.automatic_approval.raw_rule','secret') WHERE event_id="
                "'automatic-policy-decision'");
  }
  store = open_store(path);
  const auto corrupted = store->replay_events(session);
  REQUIRE_FALSE(corrupted);
  REQUIRE(corrupted.error().code == storage::SessionStoreErrorCode::corrupt);
  store.reset();

  TemporaryDirectory legacy_temporary;
  store = open_store(legacy_temporary.path() / "aiforge" / "sessions.sqlite3");
  const auto legacy_session = create(*store, "legacy-automatic-policy", 100);
  decided.metadata.schema_version = 1;
  const auto legacy_append =
      store->append_events(legacy_session, std::array{decided});
  REQUIRE_FALSE(legacy_append);
  REQUIRE(legacy_append.error().code ==
          storage::SessionStoreErrorCode::invalid_argument);
}

TEST_CASE("malformed persisted reported cost fails replay explicitly",
          "[storage][sqlite][cost][corrupt][failure]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "aiforge" / "sessions.sqlite3";
  auto store = open_store(path);
  const auto session = create(*store, "cost-session", 100);
  auto amount = domain::MonetaryAmount::create(
                    "USD", domain::DecimalAmount::from("1").value())
                    .value();
  auto cost = domain::ReportedCost::create({std::move(amount)}).value();
  REQUIRE(store->append_events(
      session, std::array{event(1,
                                domain::InferenceCostRecorded{
                                    make_id<domain::InferenceId>("inference"),
                                    std::move(cost)},
                                "cost-event")}));
  store.reset();

  execute_sql(path,
              "UPDATE events SET payload_json='{"
              "\"inference_id\":\"inference\","
              "\"cost\":{\"amounts\":[{\"unit\":\"USD\",\"amount\":\"-1\"}]}}' "
              "WHERE event_id='cost-event'");
  store = open_store(path);
  const auto replayed = store->replay_events(session);
  REQUIRE_FALSE(replayed);
  REQUIRE(replayed.error().code == storage::SessionStoreErrorCode::corrupt);
}

TEST_CASE("video codecs reject invalid writes without poisoning replay",
          "[storage][sqlite][video][failure]") {
  TemporaryDirectory temporary;
  auto store = open_store(temporary.path() / "aiforge" / "sessions.sqlite3");
  const auto session = create(*store, "invalid-video-write", 100);
  const auto operation = make_id<domain::VideoOperationId>("operation");
  const auto job = make_id<domain::VideoJobId>("job");
  auto invalid = event(
      1,
      domain::VideoGenerationRequested{operation,
                                       {make_id<domain::ModelId>("video-model"),
                                        "prompt", std::chrono::seconds::zero()},
                                       make_id<domain::ArtifactId>("artifact")},
      "invalid-video");

  SECTION("generation spec") {
  }
  SECTION("poll number") {
    invalid.payload = domain::VideoJobStatusObserved{
        operation, job, 0, domain::VideoJobState::queued};
  }
  SECTION("cleanup pending attempt") {
    invalid.payload = domain::VideoCleanupPending{operation, job, 0};
  }
  SECTION("cleanup completed attempt") {
    invalid.payload = domain::VideoCleanupCompleted{operation, job, 0};
  }
  SECTION("cleanup failed attempt") {
    invalid.payload = domain::VideoCleanupFailed{
        operation, job, 0, {domain::ErrorCode::backend, "redacted", true}};
  }
  SECTION("transcription") {
    invalid.payload = domain::VideoTranscriptionObserved{operation, "", {}};
  }

  const auto rejected = store->append_events(session, std::array{invalid});
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().code ==
          storage::SessionStoreErrorCode::invalid_argument);
  const auto replayed = store->replay_events(session);
  REQUIRE(replayed);
  REQUIRE(replayed->empty());
}

TEST_CASE("malformed persisted video payload fails replay explicitly",
          "[storage][sqlite][video][corrupt][failure]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "aiforge" / "sessions.sqlite3";
  auto store = open_store(path);
  const auto session = create(*store, "video-session", 100);
  const auto operation = make_id<domain::VideoOperationId>("operation");
  const domain::VideoGenerationSpec spec{
      make_id<domain::ModelId>("video-model"), "prompt",
      std::chrono::seconds{5}};
  REQUIRE(store->append_events(
      session, std::array{event(1,
                                domain::VideoGenerationRequested{
                                    operation, spec,
                                    make_id<domain::ArtifactId>("artifact")},
                                "video-event")}));
  store.reset();

  execute_sql(path,
              "UPDATE events SET payload_json=json_set(payload_json, "
              "'$.spec.duration_seconds',0) WHERE event_id='video-event'");
  store = open_store(path);
  const auto replayed = store->replay_events(session);
  REQUIRE_FALSE(replayed);
  REQUIRE(replayed.error().code == storage::SessionStoreErrorCode::corrupt);
}

TEST_CASE("malformed persisted plan graph fails replay explicitly",
          "[storage][sqlite][plan][corrupt][failure]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "aiforge" / "sessions.sqlite3";
  auto store = open_store(path);
  const auto session = create(*store, "plan-session", 100);
  REQUIRE(store->append_events(
      session,
      std::array{event(1, domain::PlanRevisionProposed{plan_revision()},
                       "plan-event")}));
  store.reset();

  execute_sql(
      path, "UPDATE events SET payload_json=json_set(payload_json, "
            "'$.revision.tasks[0].dependency_task_ids',json('[\"missing\"]')) "
            "WHERE event_id='plan-event'");
  store = open_store(path);
  const auto replayed = store->replay_events(session);
  REQUIRE_FALSE(replayed);
  REQUIRE(replayed.error().code == storage::SessionStoreErrorCode::corrupt);
}

TEST_CASE("persisted plan revisions without evidence decode compatibly",
          "[storage][sqlite][plan][compatibility]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "aiforge" / "sessions.sqlite3";
  auto store = open_store(path);
  const auto session = create(*store, "legacy-plan-session", 100);
  auto legacy_revision = plan_revision();
  legacy_revision.evidence.clear();
  auto legacy_event =
      event(1, domain::PlanRevisionProposed{std::move(legacy_revision)},
            "legacy-plan-event");
  legacy_event.metadata.schema_version = 1;
  REQUIRE(store->append_events(session, std::array{legacy_event}));
  store.reset();

  store = open_store(path);
  const auto replayed = store->replay_events(session);
  REQUIRE(replayed);
  const auto& proposed =
      std::get<domain::PlanRevisionProposed>(replayed->front().payload);
  REQUIRE(proposed.revision.evidence.empty());
}

TEST_CASE("schema-v2 child dispatches decode first attempts compatibly",
          "[storage][sqlite][child-run][compatibility]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "aiforge" / "sessions.sqlite3";
  auto store = open_store(path);
  const auto session = create(*store, "legacy-child-session", 100);
  auto legacy = event(1,
                      domain::ChildRunCreated{make_id<domain::RunId>("child"),
                                              child_run_descriptor()},
                      "legacy-child-event");
  legacy.metadata.schema_version = 2;
  REQUIRE(store->append_events(session, std::array{legacy}));
  const auto replayed = store->replay_events(session);
  REQUIRE(replayed);
  REQUIRE(*replayed == std::vector<domain::RunEvent>{legacy});

  auto retry_descriptor = child_run_descriptor();
  retry_descriptor.attempt = 2;
  auto retry =
      event(2,
            domain::ChildRunCreated{make_id<domain::RunId>("retry-child"),
                                    std::move(retry_descriptor)},
            "legacy-retry-event");
  retry.metadata.schema_version = 2;
  const auto rejected = store->append_events(session, std::array{retry});
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().code ==
          storage::SessionStoreErrorCode::invalid_argument);

  store.reset();
  execute_sql(path, "UPDATE events SET payload_json=json_set(payload_json, "
                    "'$.descriptor.attempt',2) "
                    "WHERE event_id='legacy-child-event'");
  store = open_store(path);
  auto corrupt = store->replay_events(session);
  REQUIRE_FALSE(corrupt);
  REQUIRE(corrupt.error().code == storage::SessionStoreErrorCode::corrupt);

  store.reset();
  execute_sql(path,
              "UPDATE events SET schema_version=3, "
              "payload_json=json_remove(payload_json,'$.descriptor.attempt') "
              "WHERE event_id='legacy-child-event'");
  store = open_store(path);
  corrupt = store->replay_events(session);
  REQUIRE_FALSE(corrupt);
  REQUIRE(corrupt.error().code == storage::SessionStoreErrorCode::corrupt);
}

TEST_CASE("forged persisted pricing observation fails replay explicitly",
          "[storage][sqlite][pricing][corrupt][failure]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "aiforge" / "sessions.sqlite3";
  auto store = open_store(path);
  const auto session = create(*store, "pricing-session", 100);
  REQUIRE(store->append_events(
      session, std::array{event(1,
                                domain::InferencePricingObserved{
                                    make_id<domain::InferenceId>("inference"),
                                    pricing_observation()},
                                "pricing-event")}));
  store.reset();

  execute_sql(path, "UPDATE events SET payload_json=json_set(payload_json, "
                    "'$.observation.rate_card_digest.value','forged') "
                    "WHERE event_id='pricing-event'");
  store = open_store(path);
  const auto replayed = store->replay_events(session);
  REQUIRE_FALSE(replayed);
  REQUIRE(replayed.error().code == storage::SessionStoreErrorCode::corrupt);
}

TEST_CASE("paid tool spend codecs reject invalid writes and stored damage",
          "[storage][sqlite][spend][corrupt][failure]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "aiforge" / "sessions.sqlite3";
  auto store = open_store(path);
  const auto session = create(*store, "spend-session", 100);
  const auto invocation = make_id<domain::InvocationId>("paid-call");
  auto reserved =
      event(1,
            domain::ToolSpendReserved{domain::ToolSpendReservation{
                invocation,
                domain::MonetaryAmount::create(
                    "USD", domain::DecimalAmount::from("0.2").value())
                    .value(),
                domain::ToolSpendEstimateBasis::catalog_estimate,
                {"sha256", std::string(64, 'a'), 42},
                domain::EventTimestamp{std::chrono::milliseconds{10'000}}}},
            "spend-reserved");
  reserved.metadata.invocation_id = invocation;
  REQUIRE(store->append_events(session, std::array{reserved}));

  auto invalid = reserved;
  invalid.metadata.sequence = 2;
  invalid.metadata.event_id = make_id<domain::EventId>("invalid-spend");
  std::get<domain::ToolSpendReserved>(invalid.payload).reservation.maximum =
      domain::MonetaryAmount::create("EUR",
                                     domain::DecimalAmount::from("0.2").value())
          .value();
  auto rejected = store->append_events(session, std::array{invalid});
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().code ==
          storage::SessionStoreErrorCode::invalid_argument);

  store.reset();
  execute_sql(path, "UPDATE events SET payload_json=json_set(payload_json, "
                    "'$.reservation.evidence_digest.value','bad') "
                    "WHERE event_id='spend-reserved'");
  store = open_store(path);
  const auto replayed = store->replay_events(session);
  REQUIRE_FALSE(replayed);
  REQUIRE(replayed.error().code == storage::SessionStoreErrorCode::corrupt);
}

TEST_CASE("schema-v2 tool proposals persist normalized arguments with optional "
          "spend quotes",
          "[storage][sqlite][spend][failure]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "aiforge" / "sessions.sqlite3";
  auto store = open_store(path);
  const auto session = create(*store, "spend-proposal-session", 100);
  const auto invocation = make_id<domain::InvocationId>("paid-call");
  const auto quote = domain::ToolSpendQuote{
      domain::MonetaryAmount::create("USD",
                                     domain::DecimalAmount::from("0.2").value())
          .value(),
      domain::ToolSpendEstimateBasis::catalog_estimate,
      {"sha256", std::string(64, 'a'), 42},
      domain::EventTimestamp{std::chrono::milliseconds{10'000}}};
  auto proposal = event(1,
                        domain::ToolProposed{invocation,
                                             "paid",
                                             {"application/json", "{}"},
                                             {domain::Effect::spend},
                                             std::nullopt,
                                             true,
                                             {},
                                             {},
                                             std::nullopt,
                                             quote,
                                             domain::StructuredDataBlock{
                                                 "application/json", "{}"}},
                        "paid-proposed");
  proposal.metadata.schema_version = 2;
  proposal.metadata.invocation_id = invocation;

  auto missing_quote = proposal;
  std::get<domain::ToolProposed>(missing_quote.payload).spend_quote.reset();
  std::get<domain::ToolProposed>(missing_quote.payload).tool_name = "lookup";
  std::get<domain::ToolProposed>(missing_quote.payload).declared_effects = {
      domain::Effect::read};
  REQUIRE(store->append_events(session, std::array{missing_quote}));
  auto replayed = store->replay_events(session);
  REQUIRE(replayed);
  REQUIRE(replayed->size() == 1);
  const auto& normalized =
      std::get<domain::ToolProposed>(replayed->front().payload);
  CHECK_FALSE(normalized.spend_quote);
  CHECK(normalized.validated_arguments ==
        domain::StructuredDataBlock{"application/json", "{}"});

  auto missing_arguments = proposal;
  missing_arguments.metadata.sequence = 2;
  missing_arguments.metadata.event_id =
      make_id<domain::EventId>("missing-arguments");
  std::get<domain::ToolProposed>(missing_arguments.payload)
      .validated_arguments.reset();
  auto rejected = store->append_events(session, std::array{missing_arguments});
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().code ==
        storage::SessionStoreErrorCode::invalid_argument);
  CHECK(store->replay_events(session)->size() == 1);

  auto unsupported = proposal;
  unsupported.metadata.schema_version = 3;
  unsupported.metadata.sequence = 2;
  unsupported.metadata.event_id = make_id<domain::EventId>("unsupported");
  rejected = store->append_events(session, std::array{unsupported});
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().code ==
        storage::SessionStoreErrorCode::unsupported_version);
  CHECK(store->replay_events(session)->size() == 1);

  store.reset();
  execute_sql(path,
              "UPDATE events SET payload_json=json_set(payload_json,"
              "'$.validated_arguments',NULL) WHERE event_id='paid-proposed'");
  store = open_store(path);
  replayed = store->replay_events(session);
  REQUIRE_FALSE(replayed);
  CHECK(replayed.error().code == storage::SessionStoreErrorCode::corrupt);
}

TEST_CASE("session discovery derives distinct run counts without schema state",
          "[storage][sqlite][catalog]") {
  TemporaryDirectory temporary;
  auto store = open_store(temporary.path() / "aiforge" / "sessions.sqlite3");
  const auto session = create(*store, "session", 100);

  const auto empty = store->open_session(session);
  REQUIRE(empty);
  REQUIRE(empty->run_count == 0);

  auto first = event(
      1, domain::UnknownEvent{"future.first", {"application/json", "{}"}});
  auto second = event(
      2, domain::UnknownEvent{"future.second", {"application/json", "{}"}});
  second.metadata.run_id = make_id<domain::RunId>("second-run");
  auto repeated = event(
      3, domain::UnknownEvent{"future.third", {"application/json", "{}"}});
  repeated.metadata.run_id = second.metadata.run_id;
  REQUIRE(store->append_events(session, std::array{first, second, repeated}));

  const auto info = store->open_session(session);
  REQUIRE(info);
  REQUIRE(info->last_sequence == 3);
  REQUIRE(info->run_count == 2);
  const auto listed = store->list_sessions(1);
  REQUIRE(listed);
  REQUIRE(listed->size() == 1);
  REQUIRE(listed->front() == *info);
}

TEST_CASE("append validation and constraints leave prior history readable",
          "[storage][sqlite][transaction][failure]") {
  TemporaryDirectory temporary;
  storage::SessionStoreLimits limits;
  limits.maximum_payload_bytes = 512;
  auto store =
      open_store(temporary.path() / "aiforge" / "sessions.sqlite3", limits);
  const auto session = create(*store, "session", 100);
  const auto first = event(1, started(), "one");
  REQUIRE(store->append_events(session, std::array{first}));

  const std::array duplicate_batch{
      event(2, domain::RunCompletionRequested{}, "two"),
      event(3, domain::RunCompleted{}, "one")};
  auto appended = store->append_events(session, duplicate_batch);
  REQUIRE_FALSE(appended);
  REQUIRE(appended.error().code == storage::SessionStoreErrorCode::conflict);
  const auto after_duplicate = store->replay_events(session);
  const auto duplicate_replay_error =
      after_duplicate ? std::string{} : after_duplicate.error().message;
  INFO(duplicate_replay_error);
  REQUIRE(after_duplicate);
  REQUIRE(after_duplicate->size() == 1);

  const std::array regressing{
      event(4, domain::RunCompletionRequested{}, "four"),
      event(3, domain::RunCompleted{}, "three")};
  appended = store->append_events(session, regressing);
  REQUIRE_FALSE(appended);
  REQUIRE(appended.error().code ==
          storage::SessionStoreErrorCode::invalid_argument);

  const auto oversized = event(2,
                               domain::UserContentAdded{domain::Message{
                                   make_id<domain::MessageId>("large"),
                                   domain::Role::user,
                                   {domain::TextBlock{std::string(1024, 'x')}},
                                   std::nullopt}},
                               "large-event");
  appended = store->append_events(session, std::array{oversized});
  REQUIRE_FALSE(appended);
  REQUIRE(appended.error().code ==
          storage::SessionStoreErrorCode::resource_exhausted);

  auto invalid_verification = verification_payload(
      make_id<domain::InvocationId>("verification-invocation"),
      make_id<domain::ArtifactId>("verification-artifact"));
  invalid_verification.evidence.summary.clear();
  appended = store->append_events(
      session, std::array{event(2, std::move(invalid_verification),
                                "invalid-verification")});
  REQUIRE_FALSE(appended);
  REQUIRE(appended.error().code ==
          storage::SessionStoreErrorCode::invalid_argument);

  auto invalid_review = review_draft();
  invalid_review.candidate.revision.clear();
  appended = store->append_events(
      session, std::array{event(2, domain::ReviewReceiptDrafted{invalid_review},
                                "invalid-review")});
  REQUIRE_FALSE(appended);
  REQUIRE(appended.error().code ==
          storage::SessionStoreErrorCode::invalid_argument);

  appended = store->append_events(
      session,
      std::array{event(2,
                       domain::ReviewRequested{review_draft().receipt_id,
                                               {"forged\nactor", "Reviewer"}},
                       "invalid-review-actor")});
  REQUIRE_FALSE(appended);
  REQUIRE(appended.error().code ==
          storage::SessionStoreErrorCode::invalid_argument);

  std::stop_source cancellation;
  cancellation.request_stop();
  appended = store->append_events(
      session,
      std::array{event(2, domain::RunCompletionRequested{}, "cancelled")},
      cancellation.get_token());
  REQUIRE_FALSE(appended);
  REQUIRE(appended.error().code == storage::SessionStoreErrorCode::cancelled);
  REQUIRE(store->replay_events(session)->size() == 1);
}

TEST_CASE("missing sessions malformed JSON and newer stores fail explicitly",
          "[storage][sqlite][corrupt][failure]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "aiforge" / "sessions.sqlite3";
  auto store = open_store(path);
  const auto missing = make_id<domain::SessionId>("missing");
  REQUIRE_FALSE(store->open_session(missing));
  REQUIRE(store->open_session(missing).error().code ==
          storage::SessionStoreErrorCode::not_found);
  REQUIRE_FALSE(store->replay_events(missing));

  const auto session = create(*store, "session", 100);
  REQUIRE(
      store->append_events(session, std::array{event(1, started(), "one")}));
  store.reset();
  execute_sql(path, "UPDATE events SET payload_json='{\"x\":1,\"x\":2}' "
                    "WHERE event_id='one'");
  store = open_store(path);
  const auto corrupt = store->replay_events(session);
  REQUIRE_FALSE(corrupt);
  REQUIRE(corrupt.error().code == storage::SessionStoreErrorCode::corrupt);
  store.reset();

  execute_sql(path, "PRAGMA user_version=999");
  const auto newer = adapters::SqliteSessionStore::open(path);
  REQUIRE_FALSE(newer);
  REQUIRE(newer.error().code ==
          storage::SessionStoreErrorCode::unsupported_version);
}

TEST_CASE("run provenance never persists a secret and rejects stored damage",
          "[storage][sqlite][codec][provenance][failure]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "aiforge" / "sessions.sqlite3";
  auto store = open_store(path);
  const auto session = create(*store, "session", 100);
  REQUIRE(
      store->append_events(session, std::array{event(1, started(), "one")}));

  auto sensitive = run_provenance();
  sensitive.configuration.front().sensitive = true;
  auto appended = store->append_events(
      session,
      std::array{event(2, domain::RunProvenanceRecorded{sensitive}, "secret")});
  REQUIRE_FALSE(appended);
  REQUIRE(appended.error().code ==
          storage::SessionStoreErrorCode::invalid_argument);

  auto duplicated = run_provenance();
  duplicated.configuration.push_back(duplicated.configuration.front());
  appended = store->append_events(
      session, std::array{event(2, domain::RunProvenanceRecorded{duplicated},
                                "duplicate")});
  REQUIRE_FALSE(appended);
  REQUIRE(appended.error().code ==
          storage::SessionStoreErrorCode::invalid_argument);

  auto secret_shaped = run_provenance();
  secret_shaped.credential_source->identity = "sk-live-abcdef==";
  appended = store->append_events(
      session, std::array{event(2, domain::RunProvenanceRecorded{secret_shaped},
                                "credential")});
  REQUIRE_FALSE(appended);
  REQUIRE(appended.error().code ==
          storage::SessionStoreErrorCode::invalid_argument);

  storage::SessionStoreLimits small;
  small.maximum_payload_bytes = 64;
  auto tight_store = open_store(path, small);
  appended = tight_store->append_events(
      session,
      std::array{event(2, domain::RunProvenanceRecorded{run_provenance()},
                       "oversized")});
  REQUIRE_FALSE(appended);
  REQUIRE(appended.error().code ==
          storage::SessionStoreErrorCode::resource_exhausted);
  tight_store.reset();

  // Nothing above reached storage.
  REQUIRE(store->replay_events(session)->size() == 1);
  REQUIRE(store->append_events(
      session,
      std::array{event(2, domain::RunProvenanceRecorded{run_provenance()},
                       "provenance")}));
  store.reset();

  execute_sql(path, "UPDATE events SET payload_json="
                    "json_remove(payload_json,'$.provenance.backend_id') "
                    "WHERE event_id='provenance'");
  store = open_store(path);
  auto damaged = store->replay_events(session);
  REQUIRE_FALSE(damaged);
  REQUIRE(damaged.error().code == storage::SessionStoreErrorCode::corrupt);
  store.reset();

  // Restore the removed field so this case fails only on the unknown enum name.
  execute_sql(path, "UPDATE events SET payload_json="
                    "json_set(json_set(payload_json,'$.provenance.backend_id',"
                    "'venice'),'$.provenance.configuration[0].source',"
                    "'future_source') WHERE event_id='provenance'");
  store = open_store(path);
  damaged = store->replay_events(session);
  REQUIRE_FALSE(damaged);
  REQUIRE(damaged.error().code == storage::SessionStoreErrorCode::corrupt);
}

TEST_CASE("effective request option provenance round-trips strictly and reads "
          "legacy records",
          "[storage][sqlite][codec][provenance][request-options]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "aiforge" / "sessions.sqlite3";
  auto store = open_store(path);
  const auto session = create(*store, "session", 100);
  REQUIRE(store->append_events(
      session,
      std::array{event(1, started(), "start"),
                 event(2, domain::RunProvenanceRecorded{run_provenance()},
                       "options")}));

  const auto replayed = store->replay_events(session);
  REQUIRE(replayed);
  const auto* recorded =
      std::get_if<domain::RunProvenanceRecorded>(&replayed->at(1).payload);
  REQUIRE(recorded != nullptr);
  REQUIRE(recorded->provenance.effective_request_options ==
          run_provenance().effective_request_options);
  store.reset();

  // Records written before this tail field existed remain valid and project an
  // empty effective request-option snapshot.
  execute_sql(
      path,
      "UPDATE events SET payload_json=json_remove(payload_json,"
      "'$.provenance.effective_request_options') WHERE event_id='options'");
  store = open_store(path);
  const auto legacy = store->replay_events(session);
  REQUIRE(legacy);
  const auto* legacy_recorded =
      std::get_if<domain::RunProvenanceRecorded>(&legacy->at(1).payload);
  REQUIRE(legacy_recorded != nullptr);
  REQUIRE(legacy_recorded->provenance.effective_request_options.empty());
  store.reset();

  execute_sql(
      path,
      "UPDATE events SET payload_json=json_set(payload_json,"
      "'$.provenance.effective_request_options',"
      "json_array(json_object('key','venice.chat.web-search','value','on',"
      "'source','future_source'))) WHERE event_id='options'");
  store = open_store(path);
  const auto unknown_source = store->replay_events(session);
  REQUIRE_FALSE(unknown_source);
  REQUIRE(unknown_source.error().code ==
          storage::SessionStoreErrorCode::corrupt);
  store.reset();

  execute_sql(path, "UPDATE events SET payload_json=json_set(payload_json,"
                    "'$.provenance.effective_request_options',json_object()) "
                    "WHERE event_id='options'");
  store = open_store(path);
  const auto wrong_shape = store->replay_events(session);
  REQUIRE_FALSE(wrong_shape);
  REQUIRE(wrong_shape.error().code == storage::SessionStoreErrorCode::corrupt);
}

TEST_CASE("user-global instruction provenance round-trips and reads legacy "
          "records",
          "[storage][sqlite][codec][provenance][instructions][failure]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "aiforge" / "sessions.sqlite3";
  auto store = open_store(path);
  const auto session = create(*store, "session", 100);
  auto provenance = run_provenance();
  provenance.user_global_instruction = domain::UserGlobalInstructionReference{
      make_id<domain::ContextSourceId>(
          std::string{domain::user_global_instruction_source_identity}),
      std::string{domain::user_global_instruction_source_location},
      {"sha256", std::string(64, 'a'), 6}};
  REQUIRE(store->append_events(
      session, std::array{event(1, started(), "start"),
                          event(2, domain::RunProvenanceRecorded{provenance},
                                "instructions")}));

  auto replayed = store->replay_events(session);
  REQUIRE(replayed);
  const auto* recorded =
      std::get_if<domain::RunProvenanceRecorded>(&replayed->at(1).payload);
  REQUIRE(recorded != nullptr);
  REQUIRE(recorded->provenance.user_global_instruction ==
          provenance.user_global_instruction);
  store.reset();

  execute_sql(path, "UPDATE events SET payload_json=json_remove(payload_json,"
                    "'$.provenance.user_global_instruction') WHERE "
                    "event_id='instructions'");
  store = open_store(path);
  replayed = store->replay_events(session);
  REQUIRE(replayed);
  recorded =
      std::get_if<domain::RunProvenanceRecorded>(&replayed->at(1).payload);
  REQUIRE(recorded != nullptr);
  REQUIRE_FALSE(recorded->provenance.user_global_instruction);
  store.reset();

  execute_sql(
      path,
      "UPDATE events SET payload_json=json_set(payload_json,"
      "'$.provenance.user_global_instruction',json_object('source_id',"
      "'wrong','source_location','instructions/global.md','content_digest',"
      "json_object('algorithm','sha256','value','" +
          std::string(64, 'a') +
          "','byte_size',6))) WHERE event_id='instructions'");
  store = open_store(path);
  replayed = store->replay_events(session);
  REQUIRE_FALSE(replayed);
  REQUIRE(replayed.error().code == storage::SessionStoreErrorCode::corrupt);
}

TEST_CASE("tool profile provenance round-trips strictly and reads legacy "
          "records",
          "[storage][sqlite][codec][provenance][tool-profile]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "aiforge" / "sessions.sqlite3";
  auto store = open_store(path);
  const auto session = create(*store, "session", 100);
  REQUIRE(store->append_events(
      session,
      std::array{event(1, started(), "start"),
                 event(2, domain::RunProvenanceRecorded{run_provenance()},
                       "profile")}));

  const auto replayed = store->replay_events(session);
  REQUIRE(replayed);
  const auto* recorded =
      std::get_if<domain::RunProvenanceRecorded>(&replayed->at(1).payload);
  REQUIRE(recorded != nullptr);
  REQUIRE(recorded->provenance.tool_profile == run_provenance().tool_profile);
  store.reset();

  execute_sql(path, "UPDATE events SET payload_json=json_set(payload_json,"
                    "'$.provenance.tool_profile.desired_tool_names',"
                    "json_array()) WHERE event_id='profile'");
  store = open_store(path);
  const auto contradictory = store->replay_events(session);
  REQUIRE_FALSE(contradictory);
  REQUIRE(contradictory.error().code ==
          storage::SessionStoreErrorCode::corrupt);
  store.reset();
  execute_sql(path, "UPDATE events SET payload_json=json_set(payload_json,"
                    "'$.provenance.tool_profile.desired_tool_names',"
                    "json_array('read')) WHERE event_id='profile'");

  execute_sql(path, "UPDATE events SET payload_json=json_set(payload_json,"
                    "'$.provenance.tool_profile.desired_tool_names',"
                    "json_array('read*')) WHERE event_id='profile'");
  store = open_store(path);
  const auto wildcard = store->replay_events(session);
  REQUIRE_FALSE(wildcard);
  REQUIRE(wildcard.error().code == storage::SessionStoreErrorCode::corrupt);
  store.reset();
  execute_sql(path, "UPDATE events SET payload_json=json_set(payload_json,"
                    "'$.provenance.tool_profile.desired_tool_names',"
                    "json_array('read')) WHERE event_id='profile'");

  execute_sql(path, "UPDATE events SET payload_json=json_remove(payload_json,"
                    "'$.provenance.tool_profile.desired_tool_names') "
                    "WHERE event_id='profile'");
  store = open_store(path);
  const auto pre_narrowing = store->replay_events(session);
  REQUIRE(pre_narrowing);
  const auto* pre_narrowing_recorded =
      std::get_if<domain::RunProvenanceRecorded>(&pre_narrowing->at(1).payload);
  REQUIRE(pre_narrowing_recorded != nullptr);
  REQUIRE(pre_narrowing_recorded->provenance.tool_profile);
  REQUIRE_FALSE(
      pre_narrowing_recorded->provenance.tool_profile->desired_tool_names);
  store.reset();

  execute_sql(path, "UPDATE events SET payload_json=json_remove(payload_json,"
                    "'$.provenance.tool_profile') WHERE event_id='profile'");
  store = open_store(path);
  const auto legacy = store->replay_events(session);
  REQUIRE(legacy);
  const auto* legacy_recorded =
      std::get_if<domain::RunProvenanceRecorded>(&legacy->at(1).payload);
  REQUIRE(legacy_recorded != nullptr);
  REQUIRE_FALSE(legacy_recorded->provenance.tool_profile);
  store.reset();

  execute_sql(path, "UPDATE events SET payload_json=json_set(payload_json,"
                    "'$.provenance.tool_profile',json_array()) "
                    "WHERE event_id='profile'");
  store = open_store(path);
  auto malformed = store->replay_events(session);
  REQUIRE_FALSE(malformed);
  REQUIRE(malformed.error().code == storage::SessionStoreErrorCode::corrupt);
  store.reset();

  execute_sql(path,
              "UPDATE events SET payload_json=json_set(payload_json,"
              "'$.provenance.tool_profile',"
              "json_object('model_maximum_profile_id',null,"
              "'persona_maximum_profile_id',null)) WHERE event_id='profile'");
  store = open_store(path);
  malformed = store->replay_events(session);
  REQUIRE_FALSE(malformed);
  REQUIRE(malformed.error().code == storage::SessionStoreErrorCode::corrupt);
  store.reset();

  execute_sql(path,
              "UPDATE events SET payload_json=json_set(payload_json,"
              "'$.provenance.tool_profile',"
              "json_object('selected_profile_id','essentials',"
              "'model_maximum_profile_id',42,"
              "'persona_maximum_profile_id',null)) WHERE event_id='profile'");
  store = open_store(path);
  malformed = store->replay_events(session);
  REQUIRE_FALSE(malformed);
  REQUIRE(malformed.error().code == storage::SessionStoreErrorCode::corrupt);
  store.reset();

  execute_sql(path,
              "UPDATE events SET payload_json=json_set(payload_json,"
              "'$.provenance.tool_profile',"
              "json_object('selected_profile_id','bad profile',"
              "'model_maximum_profile_id',null,"
              "'persona_maximum_profile_id',null)) WHERE event_id='profile'");
  store = open_store(path);
  malformed = store->replay_events(session);
  REQUIRE_FALSE(malformed);
  REQUIRE(malformed.error().code == storage::SessionStoreErrorCode::corrupt);
}

TEST_CASE("tool registration digests round-trip and legacy absence is readable",
          "[storage][sqlite][codec][provenance][tools][failure]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "aiforge" / "sessions.sqlite3";
  auto store = open_store(path);
  const auto session = create(*store, "session", 100);
  REQUIRE(store->append_events(
      session,
      std::array{event(1, started(), "start"),
                 event(2, domain::RunProvenanceRecorded{run_provenance()},
                       "provenance")}));

  auto replayed = store->replay_events(session);
  REQUIRE(replayed);
  const auto* recorded =
      std::get_if<domain::RunProvenanceRecorded>(&replayed->at(1).payload);
  REQUIRE(recorded != nullptr);
  REQUIRE(recorded->provenance.tools.front().registration_digest ==
          "sha256:" + std::string(64, 'a'));
  store.reset();

  execute_sql(path, "UPDATE events SET payload_json=json_remove(payload_json,"
                    "'$.provenance.tools[0].registration_digest') "
                    "WHERE event_id='provenance'");
  store = open_store(path);
  replayed = store->replay_events(session);
  REQUIRE(replayed);
  recorded =
      std::get_if<domain::RunProvenanceRecorded>(&replayed->at(1).payload);
  REQUIRE(recorded != nullptr);
  REQUIRE_FALSE(recorded->provenance.tools.front().registration_digest);
  store.reset();

  execute_sql(path, "UPDATE events SET payload_json=json_set(payload_json,"
                    "'$.provenance.tools[0].registration_digest','sha256:BAD') "
                    "WHERE event_id='provenance'");
  store = open_store(path);
  replayed = store->replay_events(session);
  REQUIRE_FALSE(replayed);
  REQUIRE(replayed.error().code == storage::SessionStoreErrorCode::corrupt);
}

TEST_CASE(
    "tool policy provenance round-trips strictly and reads legacy records",
    "[storage][sqlite][codec][provenance][tool-policy][failure]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "aiforge" / "sessions.sqlite3";
  auto store = open_store(path);
  const auto session = create(*store, "session", 100);
  REQUIRE(store->append_events(
      session,
      std::array{event(1, started(), "start"),
                 event(2, domain::RunProvenanceRecorded{run_provenance()},
                       "policy")}));

  auto replayed = store->replay_events(session);
  REQUIRE(replayed);
  const auto* recorded =
      std::get_if<domain::RunProvenanceRecorded>(&replayed->at(1).payload);
  REQUIRE(recorded != nullptr);
  REQUIRE(recorded->provenance.tool_policy == run_provenance().tool_policy);
  store.reset();

  execute_sql(path, "UPDATE events SET payload_json=json_remove(payload_json,"
                    "'$.provenance.tool_policy') WHERE event_id='policy'");
  store = open_store(path);
  replayed = store->replay_events(session);
  REQUIRE(replayed);
  recorded =
      std::get_if<domain::RunProvenanceRecorded>(&replayed->at(1).payload);
  REQUIRE(recorded != nullptr);
  REQUIRE_FALSE(recorded->provenance.tool_policy);
  store.reset();

  const auto set_policy = [&](const std::string_view policy) {
    execute_sql(path, "UPDATE events SET payload_json=json_set(payload_json,"
                      "'$.provenance.tool_policy',json('" +
                          std::string{policy} + "')) WHERE event_id='policy'");
  };
  set_policy(
      R"({"identity":"aiforge.tool-launch-policy.v1","permission_profile_id":"tools-medium-auto-v1","restriction_level":"future","approval_mode":"automatic","effect_ceiling":["read"],"capability_ceiling":[{"effect":"read","kind":"filesystem.root","value":"/workspace"}],"automatically_eligible_tools":["read"]})");
  store = open_store(path);
  REQUIRE_FALSE(store->replay_events(session));
  store.reset();

  set_policy(
      R"({"identity":"aiforge.tool-launch-policy.v1","permission_profile_id":"tools-medium-auto-v1","restriction_level":"medium","approval_mode":"automatic","effect_ceiling":["read"],"capability_ceiling":[{"effect":"read","kind":"filesystem.root","value":"/workspace","extra":true}],"automatically_eligible_tools":["read"]})");
  store = open_store(path);
  REQUIRE_FALSE(store->replay_events(session));
  store.reset();

  set_policy(
      R"({"identity":"aiforge.tool-launch-policy.v1","permission_profile_id":"tools-medium-auto-v1","restriction_level":"medium","approval_mode":"automatic","effect_ceiling":["read"],"capability_ceiling":[{"effect":"read","kind":"filesystem.root","value":"/workspace"}],"automatically_eligible_tools":"read"})");
  store = open_store(path);
  const auto wrong_shape = store->replay_events(session);
  REQUIRE_FALSE(wrong_shape);
  REQUIRE(wrong_shape.error().code == storage::SessionStoreErrorCode::corrupt);
}

TEST_CASE("v2 launch provenance rejects corrupted or secret-bearing state",
          "[storage][sqlite][codec][provenance][tool-policy][failure]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "aiforge" / "sessions.sqlite3";
  auto store = open_store(path);
  const auto session = create(*store, "session", 100);
  REQUIRE(store->append_events(
      session,
      std::array{event(1, started(), "start"),
                 event(2, domain::RunProvenanceRecorded{run_provenance_v2()},
                       "policy")}));

  auto replayed = store->replay_events(session);
  REQUIRE(replayed);
  const auto* recorded =
      std::get_if<domain::RunProvenanceRecorded>(&replayed->at(1).payload);
  REQUIRE(recorded != nullptr);
  REQUIRE(recorded->provenance == run_provenance_v2());
  store.reset();

  SECTION("selected and achieved restriction disagree") {
    execute_sql(path, "UPDATE events SET payload_json=json_set(payload_json,"
                      "'$.provenance.tool_policy.achieved_restriction_level',"
                      "'low') WHERE event_id='policy'");
  }
  SECTION("achieved and unavailable states are both present") {
    execute_sql(path,
                "UPDATE events SET payload_json=json_set(payload_json,"
                "'$.provenance.tool_policy.restriction_unavailable_reason',"
                "'mechanism_absent') WHERE event_id='policy'");
  }
  SECTION("automatic mode loses matcher identity") {
    execute_sql(path, "UPDATE events SET payload_json=json_set(payload_json,"
                      "'$.provenance.tool_policy.matcher_policy_identity',"
                      "NULL) WHERE event_id='policy'");
  }
  SECTION("achieved restriction loses its policy identity") {
    execute_sql(path, "UPDATE events SET payload_json=json_set(payload_json,"
                      "'$.provenance.tool_policy.restriction_policy_identity',"
                      "NULL) WHERE event_id='policy'");
  }
  SECTION("raw restriction path is forbidden") {
    execute_sql(path, "UPDATE events SET payload_json=json_set(payload_json,"
                      "'$.provenance.tool_policy.restriction_policy_identity',"
                      "'/sys/fs/cgroup/task') WHERE event_id='policy'");
  }
  SECTION("unknown unavailable reason") {
    execute_sql(path, "UPDATE events SET payload_json=json_set(payload_json,"
                      "'$.provenance.tool_policy.achieved_restriction_level',"
                      "NULL,'$.provenance.tool_policy."
                      "restriction_unavailable_reason','host_text') "
                      "WHERE event_id='policy'");
  }
  SECTION("delegation path is forbidden") {
    execute_sql(path, "UPDATE events SET payload_json=json_set(payload_json,"
                      "'$.provenance.tool_policy.delegation_path','/secret') "
                      "WHERE event_id='policy'");
  }
  SECTION("raw matcher configuration is forbidden") {
    execute_sql(path, "UPDATE events SET payload_json=json_set(payload_json,"
                      "'$.provenance.tool_policy.automatically_eligible_tools',"
                      "json('[\"read\"]')) WHERE event_id='policy'");
  }

  store = open_store(path);
  replayed = store->replay_events(session);
  REQUIRE_FALSE(replayed);
  REQUIRE(replayed.error().code == storage::SessionStoreErrorCode::corrupt);
}

TEST_CASE("bounded SQLite writer contention is retryable",
          "[storage][sqlite][concurrency][failure]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "aiforge" / "sessions.sqlite3";
  storage::SessionStoreLimits limits;
  limits.busy_timeout = std::chrono::milliseconds{10};
  auto store = open_store(path, limits);
  const auto session = create(*store, "session", 100);

  sqlite3* competing{};
  REQUIRE(sqlite3_open(path.c_str(), &competing) == SQLITE_OK);
  REQUIRE(sqlite3_exec(competing, "BEGIN IMMEDIATE", nullptr, nullptr,
                       nullptr) == SQLITE_OK);
  const auto result =
      store->append_events(session, std::array{event(1, started(), "one")});
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == storage::SessionStoreErrorCode::contention);
  REQUIRE(result.error().retryable);
  REQUIRE(sqlite3_exec(competing, "ROLLBACK", nullptr, nullptr, nullptr) ==
          SQLITE_OK);
  REQUIRE(sqlite3_close(competing) == SQLITE_OK);
  REQUIRE(store->replay_events(session)->empty());
}

TEST_CASE("two SQLite writers cannot commit the same paid reservation slot",
          "[storage][sqlite][spend][concurrency][failure]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "aiforge" / "sessions.sqlite3";
  auto first = open_store(path);
  const auto session = create(*first, "spend-race", 100);
  auto second = open_store(path);
  const auto invocation = make_id<domain::InvocationId>("paid-call");
  const auto quote = domain::ToolSpendQuote{
      domain::MonetaryAmount::create("USD",
                                     domain::DecimalAmount::from("0.5").value())
          .value(),
      domain::ToolSpendEstimateBasis::policy_upper_bound,
      {"sha256", std::string(64, 'a'), 32},
      domain::EventTimestamp{std::chrono::milliseconds{10'000}}};
  auto proposed = event(1,
                        domain::ToolProposed{invocation,
                                             "paid",
                                             {"application/json", "{}"},
                                             {domain::Effect::spend},
                                             std::nullopt,
                                             true,
                                             {},
                                             {},
                                             std::nullopt,
                                             quote,
                                             domain::StructuredDataBlock{
                                                 "application/json", "{}"}},
                        "paid-proposed");
  proposed.metadata.schema_version = 2;
  proposed.metadata.invocation_id = invocation;
  REQUIRE(first->append_events(session, std::array{proposed}));

  const auto reservation =
      domain::ToolSpendReservation{invocation, quote.maximum, quote.basis,
                                   quote.evidence_digest, quote.valid_until};
  auto admitted = event(2, domain::ToolSpendReserved{reservation}, "admitted");
  admitted.metadata.invocation_id = invocation;
  auto stale = event(2, domain::ToolSpendReserved{reservation}, "stale");
  stale.metadata.invocation_id = invocation;
  REQUIRE(first->append_events(session, std::array{admitted}));
  const auto rejected = second->append_events(session, std::array{stale});
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().code == storage::SessionStoreErrorCode::conflict);
  const auto replayed = second->replay_events(session);
  REQUIRE(replayed);
  CHECK(replayed->size() == 2);
  CHECK(replayed->back() == admitted);
}

TEST_CASE(
    "closing an interrupted SQLite transaction preserves committed history",
    "[storage][sqlite][crash][failure]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "aiforge" / "sessions.sqlite3";
  auto store = open_store(path);
  const auto session = create(*store, "session", 100);
  const auto first = event(1, started(), "one");
  REQUIRE(store->append_events(session, std::array{first}));
  store.reset();

  execute_sql(
      path,
      "BEGIN IMMEDIATE;"
      "INSERT INTO events(session_id,sequence,event_id,run_id,schema_version,"
      "timestamp_ms,payload_type,payload_json) VALUES("
      "'session',2,'interrupted','run',1,2000,'run.completed','{}')");

  store = open_store(path);
  const auto replay = store->replay_events(session);
  REQUIRE(replay);
  REQUIRE(*replay == std::vector<domain::RunEvent>{first});
}

TEST_CASE(
    "scripted session store records exact calls and deterministic failures",
    "[storage][fake][failure]") {
  const auto session = make_id<domain::SessionId>("session");
  const storage::SessionInfo info{
      session, domain::EventTimestamp{std::chrono::milliseconds{1}},
      domain::EventTimestamp{std::chrono::milliseconds{2}}, 3};
  testing::ScriptedSessionStore fake{{
      {testing::OpenSessionCall{session}, info},
      {testing::ReplayEventsCall{session},
       storage::SessionStoreError{storage::SessionStoreErrorCode::io_failure,
                                  "injected failure", true}},
  }};
  REQUIRE(fake.open_session(session) == info);
  const auto replay = fake.replay_events(session);
  REQUIRE_FALSE(replay);
  REQUIRE(replay.error().code == storage::SessionStoreErrorCode::io_failure);
  REQUIRE(fake.recorded_calls().size() == 2);
  REQUIRE(fake.remaining_exchanges() == 0);
}

TEST_CASE("run started preserves absent empty and populated memory snapshots",
          "[storage][sqlite][codec][memory][compatibility]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "aiforge" / "sessions.sqlite3";
  auto store = open_store(path);
  const auto session = create(*store, "memory-snapshots", 100);
  auto empty = started();
  empty.memory_selection = domain::MemorySelection{};
  REQUIRE(domain::seal_memory_selection(*empty.memory_selection));
  auto selected = started();
  selected.memory_selection = memory_selection();
  std::vector<domain::RunEvent> events{event(1, started(), "legacy-start"),
                                       event(2, empty, "empty-start"),
                                       event(3, selected, "selected-start")};
  events[1].metadata.schema_version = 2;
  events[2].metadata.schema_version = 2;
  REQUIRE(store->append_events(session, events));
  store.reset();
  store = open_store(path);
  const auto replayed = store->replay_events(session);
  REQUIRE(replayed);
  CHECK(*replayed == events);
  CHECK_FALSE(
      std::get<domain::RunStarted>((*replayed)[0].payload).memory_selection);
  REQUIRE(
      std::get<domain::RunStarted>((*replayed)[1].payload).memory_selection);
  CHECK(std::get<domain::RunStarted>((*replayed)[1].payload)
            .memory_selection->entries.empty());
  CHECK(std::get<domain::RunStarted>((*replayed)[2].payload).memory_selection ==
        selected.memory_selection);
}

TEST_CASE("memory snapshot presence must match the run started schema",
          "[storage][sqlite][codec][memory][failure]") {
  TemporaryDirectory temporary;
  auto store = open_store(temporary.path() / "aiforge" / "sessions.sqlite3");
  const auto session = create(*store, "invalid-memory-schema", 100);
  auto run = started();
  std::uint32_t schema = 1;
  SECTION("legacy schema cannot carry selection") {
    run.memory_selection = memory_selection();
  }
  SECTION("schema two requires explicit snapshot") {
    schema = 2;
  }
  auto invalid = event(1, run);
  invalid.metadata.schema_version = schema;
  const auto appended = store->append_events(session, std::array{invalid});
  REQUIRE_FALSE(appended);
  CHECK(appended.error().code ==
        storage::SessionStoreErrorCode::invalid_argument);
  REQUIRE(store->replay_events(session));
  CHECK(store->replay_events(session)->empty());
}

TEST_CASE(
    "corrupt memory selection snapshots never decode as a fresh selection",
    "[storage][sqlite][codec][memory][failure]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "aiforge" / "sessions.sqlite3";
  auto store = open_store(path);
  const auto session = create(*store, "corrupt-memory-selection", 100);
  auto run = started();
  run.memory_selection = memory_selection();
  auto persisted = event(1, run, "memory-start");
  persisted.metadata.schema_version = 2;
  REQUIRE(store->append_events(session, std::array{persisted}));
  store.reset();
  std::string expression;
  SECTION("missing snapshot") {
    expression = "json_remove(payload_json,'$.memory_selection')";
  }
  SECTION("null snapshot") {
    expression = "json_set(payload_json,'$.memory_selection',NULL)";
  }
  SECTION("missing entries") {
    expression = "json_remove(payload_json,'$.memory_selection.entries')";
  }
  SECTION("unsupported selection version") {
    expression = "json_set(payload_json,'$.memory_selection.version',2)";
  }
  SECTION("missing maximum budget") {
    expression =
        "json_remove(payload_json,'$.memory_selection.maximum_tokens')";
  }
  SECTION("admitted memory exceeds available budget") {
    expression =
        "json_set(payload_json,'$.memory_selection.available_tokens',0)";
  }
  SECTION("structurally valid changed budget") {
    expression =
        "json_set(payload_json,'$.memory_selection.maximum_tokens',999)";
  }
  SECTION("structurally valid changed order") {
    expression =
        "json_set(payload_json,'$.memory_selection.entries[0].order',2)";
  }
  SECTION("duplicate entry") {
    expression = "json_insert(payload_json,'$.memory_selection.entries[#]',"
                 "json_extract(payload_json,'$.memory_selection.entries[0]'))";
  }
  SECTION("wrong owner shape") {
    expression = "json_set(payload_json,'$.memory_selection.entries[0].owner."
                 "repository_id','foreign-repository')";
  }
  SECTION("unknown owner") {
    expression = "json_set(payload_json,'$.memory_selection.entries[0].owner."
                 "kind','unknown')";
  }
  SECTION("empty journal identity") {
    expression = "json_set(payload_json,'$.memory_selection.entries[0].journal_"
                 "session_id','')";
  }
  SECTION("missing accepted event identity") {
    expression = "json_remove(payload_json,'$.memory_selection.entries[0]."
                 "record_event_id')";
  }
  SECTION("missing source identity") {
    expression = "json_remove(payload_json,'$.memory_selection.entries[0]."
                 "source.session_id')";
  }
  SECTION("empty source references") {
    expression = "json_set(payload_json,'$.memory_selection.entries[0].source."
                 "event_ids',json('[]'))";
  }
  SECTION("too many source references") {
    std::string source_ids{"["};
    for (int index = 0; index < 257; ++index) {
      if (index != 0) source_ids += ',';
      source_ids += "\"source-" + std::to_string(index) + "\"";
    }
    source_ids += ']';
    expression = "json_set(payload_json,'$.memory_selection.entries[0].source."
                 "event_ids',json('" +
                 source_ids + "'))";
  }
  SECTION("duplicate source references") {
    expression = "json_insert(payload_json,'$.memory_selection.entries[0]."
                 "source.event_ids[#]',json_extract(payload_json,'$.memory_"
                 "selection.entries[0].source.event_ids[0]'))";
  }
  SECTION("malformed record digest") {
    expression = "json_set(payload_json,'$.memory_selection.entries[0].record_"
                 "digest.value','not-a-digest')";
  }
  SECTION("missing admission digest") {
    expression =
        "json_remove(payload_json,'$.memory_selection.admission_digest')";
  }
  SECTION("unknown extra field") {
    expression = "json_set(payload_json,'$.memory_selection.raw_text','must-"
                 "not-be-used')";
  }
  SECTION("downgraded schema cannot discard the snapshot") {
    execute_sql(
        path,
        "UPDATE events SET schema_version=1 WHERE event_id='memory-start'");
  }
  if (!expression.empty())
    execute_sql(path, "UPDATE events SET payload_json=" + expression +
                          " WHERE event_id='memory-start'");
  store = open_store(path);
  const auto replayed = store->replay_events(session);
  REQUIRE_FALSE(replayed);
  CHECK(replayed.error().code == storage::SessionStoreErrorCode::corrupt);
  CHECK(replayed.error().message.find("must-not-be-used") == std::string::npos);
}
