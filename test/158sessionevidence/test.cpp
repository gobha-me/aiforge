#include <aiforge/detail/sha256.hpp>
#include <aiforge/runtime/context_builder.hpp>
#include <aiforge/runtime/session_evidence.hpp>
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <span>

namespace {
using namespace aiforge;
template <class Id> auto id(const std::string& value) -> Id {
  return Id::from(value).value();
}
auto digest(std::string_view text) -> domain::ContentDigest {
  detail::Sha256 hash;
  hash.update(std::as_bytes(std::span{text.data(), text.size()}));
  return {"sha256", hash.finish(), text.size()};
}
auto content(std::string name, domain::Role role, std::string text,
             std::uint64_t order, std::uint64_t tokens)
    -> domain::ContextContentInput {
  return {id<domain::ContextEntryId>(name),
          role == domain::Role::evidence
              ? domain::ContextContentKind::evidence
              : domain::ContextContentKind::conversation,
          {id<domain::MessageId>(name + "-message"),
           role,
           {domain::TextBlock{std::move(text)}},
           {}},
          {id<domain::ContextSourceId>(name + "-source"), {}, {}},
          order,
          tokens};
}
auto base(std::uint64_t window = 100) -> runtime::PreparedSessionContext {
  domain::ContextBuildInput input{
      {window, 10, 3},
      {{id<domain::ContextEntryId>("runtime"),
        domain::InstructionLayer::application_runtime,
        domain::InstructionOperation::add,
        {},
        domain::Message{id<domain::MessageId>("runtime-message"),
                        domain::Role::system,
                        {domain::TextBlock{"Runtime"}},
                        {}},
        {id<domain::ContextSourceId>("runtime-source"), {}, {}},
        0,
        1,
        7}},
      {content("current", domain::Role::user, "Draft", 17, 5)}};
  domain::MemorySelection memory{1, {}, {}, 100, 100, {}, {}};
  REQUIRE(domain::seal_memory_selection(memory));
  domain::ConversationAdmission admission{
      .version = 2,
      .session_id = id<domain::SessionId>("session"),
      .model_id = id<domain::ModelId>("model"),
      .policy_event_id = {},
      .capacity = input.capacity,
      .mandatory_input_tokens = 12,
      .groups = {},
      .omitted_groups_digest = {},
      .admission_digest = {}};
  REQUIRE(domain::seal_conversation_admission(admission));
  return {input, memory, admission};
}
auto local(std::vector<std::string> texts = {"local"})
    -> runtime::PreparedLocalContext {
  runtime::PreparedLocalContext result{
      {}, {1, id<domain::SessionId>("session"), 3, {}, {}, {}}};
  std::uint64_t order{};
  for (const auto& text : texts) {
    const auto suffix = digest(std::to_string(++order)).value;
    domain::LocalContextEvidence ref{
        id<domain::EvidenceId>("local-evidence-" + suffix),
        id<domain::ContextEntryId>("local-context-entry-" + suffix),
        id<domain::MessageId>("local-context-message-" + suffix),
        id<domain::ContextSourceId>("local-context-source-" + suffix),
        {{1, std::string(64, 'b')},
         "file-" + std::to_string(order),
         digest(text)},
        order,
        text.size(),
        domain::LocalContextDecision::admitted};
    domain::ContextContentInput input{
        ref.entry_id,
        domain::ContextContentKind::evidence,
        {ref.message_id, domain::Role::evidence, {domain::TextBlock{text}}, {}},
        {ref.source_id,
         domain::local_context_source_location(ref.source).value(),
         "sha256:" + ref.source.content_digest.value},
        order,
        ref.estimated_tokens};
    result.candidates.push_back({input,
                                 runtime::ContextBudgetClass::attachment,
                                 runtime::ContextRepresentation::exact,
                                 domain::EvidenceFreshness::current,
                                 false,
                                 order,
                                 {}});
    result.admission.evidence.push_back(ref);
  }
  return result;
}
auto repo_prepared(std::vector<std::string> texts = {"repo"})
    -> runtime::PreparedRepositoryContext {
  const domain::RepositorySnapshotIdentity snapshot{
      id<domain::RepositoryId>("repo"), digest("snapshot")};
  runtime::PreparedRepositoryContext result{
      {{snapshot.repository_id, "/unused"}, {}, {}, snapshot.fingerprint, {}},
      {snapshot, {}, {}},
      {},
      {{id<domain::ContextParcelId>("parcel"),
        "Selected source",
        domain::TaskPhase::orientation,
        snapshot,
        {}},
       {}},
      {1, "root:1", snapshot, {}, 1, {}, {}, {}, {}}};
  std::uint64_t order{};
  for (const auto& text : texts) {
    const auto suffix = std::to_string(++order);
    domain::RepositoryContextEvidence ref{
        id<domain::EvidenceId>("repo-evidence-" + suffix),
        id<domain::ContextEntryId>("repository-context-entry-" + suffix),
        id<domain::MessageId>("repository-context-message-" + suffix),
        id<domain::ContextSourceId>("repository-context-source-" + suffix),
        {snapshot, "source-" + suffix, digest(text), {}},
        order,
        std::max(std::uint64_t{1}, static_cast<std::uint64_t>(text.size())),
        domain::RepositoryContextDecision::admitted,
        digest(text)};
    result.evidence.parcel.items.push_back(
        {ref.evidence_id,
         domain::ExactSourceEvidence{ref.source},
         domain::EvidenceFreshness::current,
         {domain::EvidenceDerivation::observed,
          "repository-context",
          "1",
          {},
          snapshot,
          {},
          {},
          {}},
         {domain::TextBlock{text}},
         text.size(),
         ref.estimated_tokens});
    result.evidence.items.push_back(
        {ref.evidence_id,
         ref.entry_id,
         ref.message_id,
         ref.source_id,
         runtime::ContextBudgetClass::repository_evidence,
         runtime::ContextRepresentation::exact,
         false,
         order,
         order,
         {}});
    result.admission.evidence.push_back(ref);
  }
  return result;
}
auto selected(const runtime::PreparedSessionContext& input,
              std::optional<runtime::PreparedRepositoryContext> repo = {},
              std::optional<runtime::PreparedLocalContext> files = {})
    -> runtime::SelectedSessionEvidence {
  auto result = runtime::select_session_evidence(input, repo, files);
  INFO((result ? "selected" : result.error().message));
  REQUIRE(result);
  return std::move(*result);
}
} // namespace

TEST_CASE(
    "session evidence rejects invalid base proof before optional selection") {
  auto input = base();
  SECTION("missing conversation seal") {
    input.conversation_admission.admission_digest.reset();
  }
  SECTION("changed capacity") {
    ++input.input.capacity.context_window_tokens;
  }
  SECTION("false mandatory total") {
    ++input.conversation_admission.mandatory_input_tokens;
    REQUIRE(domain::seal_conversation_admission(input.conversation_admission));
  }
  SECTION("changed memory seal") {
    input.memory_selection.admission_digest.reset();
  }
  SECTION("zero order") {
    input.input.content[0].order = 0;
  }
  SECTION("duplicate order") {
    input.input.content.push_back(
        content("extra", domain::Role::evidence, "x", 17, 1));
  }
  SECTION("duplicate current user") {
    input.input.content.push_back(
        content("extra", domain::Role::user, "x", 18, 1));
  }
  SECTION("token overflow") {
    input.input.content[0].estimated_tokens =
        std::numeric_limits<std::uint64_t>::max();
  }
  CHECK_FALSE(runtime::select_session_evidence(input));
}

TEST_CASE("session evidence rejects tampered optional candidates including "
          "omitted rows") {
  auto input = base(26);
  auto files = local({std::string(100, 'x')});
  SECTION("foreign session") {
    files.admission.session_id = id<domain::SessionId>("other");
  }
  SECTION("changed candidate bytes") {
    files.candidates[0].content.message.content = {
        domain::TextBlock{std::string(100, 'y')}};
  }
  SECTION("required flag") {
    files.candidates[0].required = true;
  }
  SECTION("changed candidate order") {
    ++files.candidates[0].content.order;
  }
  SECTION("changed rank") {
    ++files.candidates[0].relevance_rank;
  }
  SECTION("changed source") {
    files.admission.evidence[0].source.relative_path = "other";
  }
  SECTION("duplicate source") {
    files.candidates.push_back(files.candidates[0]);
    files.admission.evidence.push_back(files.admission.evidence[0]);
  }
  SECTION("oversized source") {
    files.admission.evidence[0].source.content_digest.byte_size =
        std::numeric_limits<std::uint64_t>::max();
  }
  SECTION("extra candidate") {
    files.candidates.push_back(files.candidates[0]);
  }
  CHECK_FALSE(runtime::select_session_evidence(input, {}, files));
}

TEST_CASE("session evidence rejects repository tampering and missing mandatory "
          "instructions") {
  auto input = base(26);
  auto repo = repo_prepared({std::string(100, 'x')});
  SECTION("required") {
    repo.evidence.items[0].required = true;
  }
  SECTION("false source") {
    std::get<domain::ExactSourceEvidence>(
        repo.evidence.parcel.items[0].reference)
        .source.relative_path = "other";
  }
  SECTION("false text") {
    repo.evidence.parcel.items[0].content = {
        domain::TextBlock{std::string(100, 'y')}};
  }
  SECTION("wrong order") {
    ++repo.evidence.items[0].order;
  }
  SECTION("wrong estimate") {
    ++repo.evidence.parcel.items[0].estimated_tokens;
  }
  SECTION("missing parcel row") {
    repo.evidence.parcel.items.clear();
  }
  SECTION("unexpected instruction") {
    auto instruction = input.input.instructions[0];
    instruction.entry_id = id<domain::ContextEntryId>("missing-project");
    repo.instructions.push_back(instruction);
  }
  CHECK_FALSE(runtime::select_session_evidence(input, repo));
}

TEST_CASE("session evidence does not reselect sealed recovery preparations") {
  auto input = base();
  auto files = local();
  auto repo = repo_prepared();
  const auto result = selected(input, repo, files);
  SECTION("local") {
    files.admission = *result.local_admission;
  }
  SECTION("repository") {
    repo.admission = *result.repository_admission;
  }
  CHECK_FALSE(runtime::select_session_evidence(input, repo, files));
}

TEST_CASE(
    "session evidence checks order overflow and supports the final order") {
  auto input = base();
  input.input.content[0].order = std::numeric_limits<std::uint64_t>::max();
  CHECK_FALSE(runtime::select_session_evidence(input, {}, local()));
  CHECK(runtime::select_session_evidence(input));
  --input.input.content[0].order;
  const auto one = selected(input, {}, local());
  CHECK(one.local_admission->evidence[0].order ==
        std::numeric_limits<std::uint64_t>::max());
  CHECK_FALSE(
      runtime::select_session_evidence(input, repo_prepared(), local()));
}

TEST_CASE("session evidence cancellation and candidate limits are bounded") {
  auto input = base();
  std::stop_source stop;
  stop.request_stop();
  const auto cancelled =
      runtime::select_session_evidence(input, {}, {}, stop.get_token());
  REQUIRE_FALSE(cancelled);
  CHECK(cancelled.error().code == runtime::SessionEvidenceErrorCode::cancelled);
  auto files = local();
  files.candidates.resize(65, files.candidates.front());
  files.admission.evidence.resize(65, files.admission.evidence.front());
  CHECK_FALSE(runtime::select_session_evidence(input, {}, files));
}

TEST_CASE("one session evidence pass shares optional budget and preserves gap "
          "orders") {
  const auto input = base(37); // 13 reserved + 12 required leaves 12 optional.
  const auto repo = repo_prepared({std::string(100, 'x'), "repo"});
  const auto files = local({"local", "oversize"});
  const auto result = selected(input, repo, files);
  REQUIRE(result.repository_admission);
  REQUIRE(result.local_admission);
  CHECK(result.repository_admission->evidence[0].decision ==
        domain::RepositoryContextDecision::omitted_budget);
  CHECK(result.repository_admission->evidence[1].decision ==
        domain::RepositoryContextDecision::admitted);
  CHECK(result.local_admission->evidence[0].decision ==
        domain::LocalContextDecision::admitted);
  CHECK(result.local_admission->evidence[1].decision ==
        domain::LocalContextDecision::omitted_budget);
  CHECK(result.repository_admission->evidence[0].order == 18);
  CHECK(result.repository_admission->evidence[1].order == 19);
  CHECK(result.local_admission->evidence[0].order == 20);
  REQUIRE(result.session.input.content.size() == 3);
  CHECK(result.session.input.content.front() == input.input.content.front());
  CHECK(result.session.input.content[1].order == 19);
  CHECK(result.session.input.content[2].order == 20);
  CHECK(result.session.input.instructions == input.input.instructions);
  CHECK(result.session.memory_selection == input.memory_selection);
  CHECK(result.session.conversation_admission.mandatory_input_tokens == 21);
  CHECK(result.context.estimated_input_tokens == 24);
  CHECK(result.usage.repository_evidence_tokens == 4);
  CHECK(result.usage.attachment_tokens == 5);
  CHECK(runtime::ContextBuilder{}.build(result.session.input).value() ==
        result.context);
  CHECK(domain::validate_conversation_admission(
      result.session.conversation_admission));
  CHECK(repo.admission.evidence[0].order == 1);
  CHECK(files.admission.evidence[0].order == 1);
}

TEST_CASE("absent and explicit empty session evidence remain distinct") {
  const auto input = base();
  const auto absent = selected(input);
  CHECK(absent.session == input);
  CHECK_FALSE(absent.repository_admission);
  CHECK_FALSE(absent.local_admission);
  const auto empty = selected(input, repo_prepared({}), local({}));
  CHECK(empty.context == absent.context);
  REQUIRE(empty.repository_admission);
  REQUIRE(empty.local_admission);
  CHECK(empty.repository_admission->evidence.empty());
  CHECK(empty.local_admission->evidence.empty());
  CHECK(domain::validate_local_context_admission(*empty.local_admission));
}

namespace {
auto mandatory_sources() -> runtime::PreparedSessionContext {
  auto input = base(200);
  auto history = content("history", domain::Role::user, "Original", 7, 8);
  auto summary = content("summary", domain::Role::evidence, "Summary", 4, 7);
  auto memory =
      content("memory-entry-record", domain::Role::evidence, "Memory", 2, 6);
  memory.message.message_id = id<domain::MessageId>("memory-message-record");
  memory.provenance = {id<domain::ContextSourceId>("memory-source-record"),
                       "memory:record;session:origin",
                       {}};
  input.memory_selection.entries.push_back(
      {id<domain::SessionId>("journal"),
       id<domain::MemoryRecordId>("record"),
       id<domain::EventId>("accepted"),
       domain::MemoryOwner::global(),
       {id<domain::SessionId>("origin"),
        id<domain::RunId>("origin-run"),
        id<domain::InvocationId>("origin-call"),
        {id<domain::EventId>("origin-event")}},
       digest("record proof"),
       digest("Memory"),
       2,
       6});
  REQUIRE(domain::seal_memory_selection(input.memory_selection));
  auto& admission = input.conversation_admission;
  admission.source_snapshot_sequence = 20;
  admission.mode = domain::ConversationMode::rolling;
  admission.policy_event_id = id<domain::EventId>("policy");
  admission.policy_revision = 1;
  admission.mandatory_input_tokens += 13;
  admission.groups.push_back(
      {id<domain::RunId>("old"),
       {{id<domain::EventId>("old-completed"), 2, history.entry_id,
         history.message.message_id, history.provenance, history.order,
         history.estimated_tokens, history.kind,
         domain::normalized_conversation_message_digest(history.message)
             .value()}},
       true});
  admission.summaries.push_back(
      {{id<domain::ConversationSummaryId>("summary"), 1, digest("candidate")},
       id<domain::EventId>("activation"),
       9,
       1,
       summary.entry_id,
       summary.message.message_id,
       summary.provenance,
       summary.order,
       summary.estimated_tokens,
       domain::normalized_conversation_message_digest(summary.message)
           .value()});
  REQUIRE(domain::seal_conversation_admission(admission));
  input.input.content.insert(input.input.content.begin(),
                             {memory, summary, history});
  return input;
}
} // namespace

TEST_CASE("session evidence refuses changes to already selected history memory "
          "and summary") {
  auto input = mandatory_sources();
  SECTION("history content changed") {
    input.input.content[2].message.content = {domain::TextBlock{"Changed"}};
  }
  SECTION("history estimate changed") {
    ++input.input.content[2].estimated_tokens;
  }
  SECTION("history order changed") {
    ++input.input.content[2].order;
  }
  SECTION("summary content changed") {
    input.input.content[1].message.content = {domain::TextBlock{"Changed"}};
  }
  SECTION("summary provenance changed") {
    input.input.content[1].provenance.digest = "changed";
  }
  SECTION("memory content changed") {
    input.input.content[0].message.content = {domain::TextBlock{"Changed"}};
  }
  SECTION("missing history") {
    input.input.content.erase(input.input.content.begin() + 2);
  }
  CHECK_FALSE(
      runtime::select_session_evidence(input, repo_prepared(), local()));
}

TEST_CASE("session evidence preserves mandatory history memory and summary "
          "without double accounting") {
  const auto input = mandatory_sources();
  const auto result = selected(input, repo_prepared(), local());
  CHECK(result.session.memory_selection == input.memory_selection);
  CHECK(result.session.conversation_admission.groups ==
        input.conversation_admission.groups);
  CHECK(result.session.conversation_admission.summaries ==
        input.conversation_admission.summaries);
  CHECK(result.session.conversation_admission.mandatory_input_tokens == 34);
  CHECK(result.context.estimated_input_tokens == 45);
  CHECK(result.usage.memory_tokens == 6);
  CHECK(result.usage.summary_tokens == 7);
  CHECK(result.usage.conversation_tokens == 13);
  CHECK(result.usage.repository_evidence_tokens == 4);
  CHECK(result.usage.attachment_tokens == 5);
  REQUIRE(result.session.input.content.size() == 6);
  CHECK(std::equal(input.input.content.begin(), input.input.content.end(),
                   result.session.input.content.begin()));
  CHECK(domain::memory_selection_matches_context(input.memory_selection,
                                                 result.context));
  CHECK(runtime::ContextBuilder{}.build(result.session.input).value() ==
        result.context);
}

TEST_CASE(
    "session evidence rejects local namespaced input without an admission") {
  auto input = base();
  auto files = local();
  input.input.content.push_back(files.candidates[0].content);
  input.conversation_admission.mandatory_input_tokens += 5;
  REQUIRE(domain::seal_conversation_admission(input.conversation_admission));
  CHECK_FALSE(runtime::select_session_evidence(input));
}

TEST_CASE(
    "session evidence preflights exact local aggregate and message bounds") {
  const auto input = base();
  auto files = local();
  SECTION("aggregate selected text") {
    files = local(std::vector<std::string>(9, std::string(256 * 1024, 'x')));
    const auto result = runtime::select_session_evidence(input, {}, files);
    REQUIRE_FALSE(result);
    CHECK(result.error().code ==
          runtime::SessionEvidenceErrorCode::resource_exhausted);
  }
  SECTION("oversized untrusted content") {
    files.candidates[0].content.message.content = {
        domain::TextBlock{std::string(256 * 1024 + 1, 'x')}};
    CHECK_FALSE(runtime::select_session_evidence(input, {}, files));
  }
  SECTION("oversized untrusted provenance") {
    files.candidates[0].content.provenance.source_location =
        std::string(8192, 'x');
    CHECK_FALSE(runtime::select_session_evidence(input, {}, files));
  }
}

TEST_CASE("required stdin evidence retains the attachment usage class") {
  auto input = base();
  input.input.content.push_back(
      content("stdin", domain::Role::evidence, "Piped input", 25, 11));
  input.conversation_admission.mandatory_input_tokens += 11;
  REQUIRE(domain::seal_conversation_admission(input.conversation_admission));
  const auto result = selected(input);
  CHECK(result.session == input);
  CHECK(result.usage.attachment_tokens == 11);
  CHECK(result.usage.conversation_tokens == 5);
}
