#include <aiforge/runtime/repository_context_controller.hpp>

#include <aiforge/detail/sha256.hpp>
#include <aiforge/runtime/context_builder.hpp>
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <functional>
#include <map>
#include <span>
#include <thread>

namespace {
using namespace aiforge;
using namespace std::chrono_literals;
template <class Id> auto id(std::string value) -> Id {
  return Id::from(std::move(value)).value();
}
auto digest(std::string_view text) -> domain::ContentDigest {
  detail::Sha256 hash;
  hash.update(std::as_bytes(std::span{text.data(), text.size()}));
  return {"sha256", hash.finish(), text.size()};
}
class Source final : public runtime::RepositoryContextSource {
 public:
  bool guaranteed{true};
  std::string binding{"root:123:456"};
  domain::RepositorySnapshot snapshot{
      {id<domain::RepositoryId>("repo"), "/repo"},
      domain::VcsState{"git", "sha1", domain::VcsHeadKind::branch, "main",
                       std::string(40, 'a')},
      {},
      digest("snapshot"),
      {}};
  std::map<std::string, std::string> files{{"src/main.cpp", "int main() {}\n"},
                                           {"src/other.cpp", "other source\n"}};
  std::vector<std::pair<std::string, std::string>> instructions{
      {"", "Root instructions\n"}, {"src", "Source instructions\n"}};
  std::size_t observes{}, discoveries{}, reads{};
  std::function<void()> on_observe;
  std::function<void(domain::ProjectInstructionDiscovery&)> on_discover;
  std::function<void(repository::ExactSourceReadResult&)> on_read;
  bool read_denied{};
  auto identity() const noexcept -> std::string_view override {
    return binding;
  }
  auto guarantees_pinned_read_only_sources() const noexcept -> bool override {
    return guaranteed;
  }
  auto observe(repository::RepositorySnapshotLimits, std::stop_token)
      -> std::expected<domain::RepositorySnapshot,
                       repository::RepositorySnapshotError> override {
    ++observes;
    if (on_observe) on_observe();
    return snapshot;
  }
  auto discover(repository::ProjectInstructionRequest request, std::stop_token)
      -> std::expected<domain::ProjectInstructionDiscovery,
                       repository::ProjectInstructionError> override {
    ++discoveries;
    domain::ProjectInstructionDiscovery result{
        domain::snapshot_identity(snapshot), request.target_subtree, {}};
    std::uint64_t order{};
    for (const auto& [subtree, text] : instructions) {
      if (!subtree.empty() && request.target_subtree != subtree &&
          !request.target_subtree.starts_with(subtree + "/"))
        continue;
      const auto specificity =
          subtree.empty()
              ? 0U
              : static_cast<unsigned>(std::ranges::count(subtree, '/') + 1);
      auto path = subtree.empty() ? "AGENTS.md" : subtree + "/AGENTS.md";
      result.documents.push_back(
          {id<domain::ProjectInstructionId>(
               "project:" + std::to_string(specificity) + ":" +
               digest(text).value),
           {domain::snapshot_identity(snapshot), path, digest(text), {}},
           subtree,
           text,
           specificity,
           ++order});
    }
    if (on_discover) on_discover(result);
    return result;
  }
  auto read(repository::ExactSourceReadRequest request, std::stop_token)
      -> std::expected<repository::ExactSourceReadResult,
                       repository::ExactSourceEditError> override {
    ++reads;
    if (read_denied || !files.contains(request.relative_path))
      return std::unexpected(repository::ExactSourceEditError{
          repository::ExactSourceEditErrorCode::unsupported_entry,
          "untracked or nonregular",
          {},
          {},
          false,
          false});
    const auto& text = files.at(request.relative_path);
    repository::ExactSourceReadResult result{
        {domain::snapshot_identity(snapshot),
         request.relative_path,
         digest(text),
         {}},
        text};
    if (on_read) on_read(result);
    return result;
  }
};
auto selection(const runtime::PreparedRepositoryContext& prepared,
               std::optional<std::uint64_t> evidence_limit = {})
    -> runtime::ContextSelectionResult {
  runtime::ContextSelectionRequest request;
  request.capacity = {100000, 100, 0};
  request.instructions = prepared.instructions;
  request.instructions.push_back(
      {id<domain::ContextEntryId>("runtime"),
       domain::InstructionLayer::application_runtime,
       domain::InstructionOperation::add,
       {},
       domain::Message{id<domain::MessageId>("runtime-message"),
                       domain::Role::system,
                       {domain::TextBlock{"Runtime"}},
                       {},
                       {}},
       {id<domain::ContextSourceId>("runtime-source"), {}, {}},
       0,
       1,
       7});
  if (!prepared.evidence.items.empty())
    request.parcels.push_back(prepared.evidence);
  request.budgets.repository_evidence_tokens = evidence_limit;
  auto result = runtime::ContextBuilder{}.select_and_build(std::move(request));
  INFO((result ? "selected" : result.error().message));
  REQUIRE(result);
  return std::move(*result);
}
auto admission(const runtime::PreparedRepositoryContext& prepared,
               std::optional<std::uint64_t> evidence_limit = {})
    -> domain::RepositoryContextAdmission {
  auto result = runtime::finalize_repository_context_admission(
      prepared, selection(prepared, evidence_limit));
  INFO((result ? "sealed" : result.error().message));
  REQUIRE(result);
  return std::move(*result);
}
} // namespace

TEST_CASE("repository context rejects unsafe paths and unproven source "
          "coupling before reads",
          "[repository-context][failure]") {
  Source source;
  runtime::RepositoryContextController controller{source, source.snapshot.root};
  for (const auto& target :
       {"/tmp", "../src", "src/../x", "src//x", "src/", "src\\x", "src\nx"}) {
    CHECK_FALSE(controller.prepare({target, 1, {}}));
  }
  CHECK_FALSE(controller.prepare({"src", 0, {}}));
  CHECK_FALSE(controller.prepare({"src", 1, {"src/main.cpp", "src/main.cpp"}}));
  CHECK_FALSE(controller.prepare({"src", 1, {"../outside"}}));
  CHECK(source.observes == 0);
  source.guaranteed = false;
  CHECK_FALSE(controller.prepare({"src", 1, {"src/main.cpp"}}));
  CHECK(source.observes == 0);
  CHECK(source.discoveries == 0);
  CHECK(source.reads == 0);
}

TEST_CASE("repository context rejects invalid limits before source calls",
          "[repository-context][failure]") {
  Source source;
  runtime::RepositoryContextLimits limits;
  SECTION("zero files") {
    limits.maximum_evidence_files = 0;
  }
  SECTION("overflow timeout") {
    limits.timeout = std::chrono::milliseconds::max();
  }
  SECTION("zero instruction deadline") {
    limits.instructions.timeout = 0ms;
  }
  SECTION("invalid observer deadline") {
    limits.snapshot.command_timeout = std::chrono::milliseconds::max();
  }
  runtime::RepositoryContextController controller{source, source.snapshot.root,
                                                  limits};
  CHECK_FALSE(controller.prepare({"src", 1, {}}));
  CHECK(source.observes == 0);
}

TEST_CASE(
    "repository context rechecks ignored instruction bytes and membership "
    "after evidence reads",
    "[repository-context][failure]") {
  Source source;
  const auto fingerprint = source.snapshot.fingerprint;
  SECTION("changed ignored instruction bytes") {
    source.on_read = [&](auto&) {
      source.instructions[1].second = "Changed ignored instructions\n";
    };
  }
  SECTION("removed ignored instruction") {
    source.on_read = [&](auto&) { source.instructions.pop_back(); };
  }
  SECTION("new ignored applicable instruction") {
    source.on_read = [&](auto&) {
      source.instructions.emplace_back("src/nested",
                                       "New ignored instructions\n");
    };
  }
  SECTION("cancellation during final chain check") {
    std::stop_source stop;
    source.on_discover = [&](auto&) {
      if (source.discoveries == 2) stop.request_stop();
    };
    runtime::RepositoryContextController controller{source,
                                                    source.snapshot.root};
    auto result = controller.prepare({"src/nested", 1, {"src/main.cpp"}},
                                     stop.get_token());
    REQUIRE_FALSE(result);
    CHECK(result.error().code == domain::RepositoryContextErrorCode::cancelled);
    CHECK(source.observes == 1);
    return;
  }
  runtime::RepositoryContextController controller{source, source.snapshot.root};
  auto result = controller.prepare({"src/nested", 1, {"src/main.cpp"}});
  REQUIRE_FALSE(result);
  CHECK(result.error().code ==
        domain::RepositoryContextErrorCode::stale_source);
  CHECK(source.snapshot.fingerprint == fingerprint);
  CHECK(source.reads == 1);
  CHECK(source.discoveries == 2);
}

TEST_CASE("repository context rejects malformed instruction authority",
          "[repository-context][failure]") {
  Source source;
  runtime::RepositoryContextController controller{source, source.snapshot.root};
  SECTION("wrong target") {
    source.on_discover = [](auto& result) { result.target_subtree = "other"; };
  }
  SECTION("source digest does not describe bytes") {
    source.on_discover = [](auto& result) {
      result.documents[0].source.content_digest = digest("wrong same length\n");
    };
  }
  SECTION("sibling instruction") {
    source.on_discover = [](auto& result) {
      result.documents[0].applicable_subtree = "other";
    };
  }
  SECTION("duplicate membership") {
    source.on_discover = [](auto& result) {
      result.documents.push_back(result.documents[0]);
    };
  }
  SECTION("swapped root to target ordering") {
    source.on_discover = [](auto& result) {
      std::ranges::reverse(result.documents);
    };
  }
  SECTION("control bytes") {
    source.instructions[0].second = "\x01";
  }
  SECTION("malformed UTF8") {
    source.instructions[0].second = "\xFF";
  }
  SECTION("stale source fingerprint") {
    source.on_discover = [](auto& result) {
      result.source_snapshot.fingerprint = digest("old");
    };
  }
  CHECK_FALSE(controller.prepare({"src", 1, {"src/main.cpp"}}));
  CHECK(source.reads == 0);
}

TEST_CASE("repository context rejects nonexact or unsafe evidence",
          "[repository-context][failure]") {
  Source source;
  runtime::RepositoryContextController controller{source, source.snapshot.root};
  SECTION("untracked symlink or hardlink denied by pinned source") {
    source.read_denied = true;
  }
  SECTION("wrong path") {
    source.on_read = [](auto& result) {
      result.source.relative_path = "src/other.cpp";
    };
  }
  SECTION("wrong snapshot") {
    source.on_read = [](auto& result) {
      result.source.snapshot.fingerprint = digest("stale");
    };
  }
  SECTION("ranged source is not whole file") {
    source.on_read = [](auto& result) {
      result.source.range = domain::SourceByteRange{0, 1};
    };
  }
  SECTION("wrong source digest") {
    source.on_read = [](auto& result) {
      result.source.content_digest.value[0] =
          result.source.content_digest.value[0] == 'f' ? 'a' : 'f';
    };
  }
  SECTION("empty file cannot be inline parcel evidence") {
    source.files["src/main.cpp"].clear();
  }
  SECTION("malformed UTF8") {
    source.files["src/main.cpp"] = "\xFF";
  }
  SECTION("terminal control") {
    source.files["src/main.cpp"] = "\x1B[2J";
  }
  SECTION("root replacement during read") {
    source.on_read = [&](auto&) { source.binding = "root:other"; };
  }
  SECTION("repository drift during read") {
    source.on_read = [&](auto&) {
      source.snapshot.fingerprint = digest("changed");
    };
  }
  CHECK_FALSE(controller.prepare({"src", 1, {"src/main.cpp"}}));
}

TEST_CASE(
    "repository context source calls honor cancellation and total deadline",
    "[repository-context][failure]") {
  Source source;
  std::stop_source stop;
  runtime::RepositoryContextLimits limits;
  SECTION("already cancelled") {
    stop.request_stop();
  }
  SECTION("cancel during observation") {
    source.on_observe = [&] { stop.request_stop(); };
  }
  SECTION("observation returns after deadline") {
    limits.timeout = 1ms;
    source.on_observe = [] { std::this_thread::sleep_for(5ms); };
  }
  runtime::RepositoryContextController controller{source, source.snapshot.root,
                                                  limits};
  const auto result = controller.prepare({"src", 1, {}}, stop.get_token());
  REQUIRE_FALSE(result);
  CHECK((result.error().code == domain::RepositoryContextErrorCode::cancelled ||
         result.error().code == domain::RepositoryContextErrorCode::timed_out));
  CHECK(source.discoveries == 0);
  CHECK(source.reads == 0);
}

TEST_CASE("repository context enforces file count and byte boundaries",
          "[repository-context][failure]") {
  Source source;
  runtime::RepositoryContextLimits limits;
  SECTION("count") {
    limits.maximum_evidence_files = 1;
  }
  SECTION("one file") {
    limits.maximum_evidence_file_bytes = 1;
  }
  SECTION("total bytes") {
    limits.maximum_evidence_total_bytes =
        source.files.at("src/main.cpp").size();
  }
  SECTION("instruction bytes") {
    limits.instructions.maximum_document_bytes = 1;
  }
  runtime::RepositoryContextController controller{source, source.snapshot.root,
                                                  limits};
  CHECK_FALSE(
      controller.prepare({"src", 1, {"src/main.cpp", "src/other.cpp"}}));
}

TEST_CASE(
    "repository admission rejects context substitution and orphan authority",
    "[repository-context][failure]") {
  Source source;
  runtime::RepositoryContextController controller{source, source.snapshot.root};
  const auto prepared = controller.prepare({"src", 7, {"src/main.cpp"}});
  REQUIRE(prepared);
  auto selected = selection(*prepared);
  const auto sealed = admission(*prepared);
  REQUIRE(domain::repository_context_admission_matches_context(
      sealed, selected.context));
  auto project =
      std::ranges::find_if(selected.context.entries, [](const auto& entry) {
        return entry.instruction_layer == domain::InstructionLayer::project;
      });
  REQUIRE(project != selected.context.entries.end());
  SECTION("changed bytes") {
    std::get<domain::TextBlock>(project->message.content[0]).text += "changed";
  }
  SECTION("changed role") {
    project->message.role = domain::Role::evidence;
  }
  SECTION("missing instruction") {
    selected.context.entries.erase(project);
  }
  SECTION("extra instruction") {
    selected.context.entries.push_back(*project);
  }
  SECTION("changed estimate") {
    ++project->estimated_tokens;
  }
  SECTION("changed source location") {
    project->provenance.source_location = "other/AGENTS.md";
  }
  SECTION("changed specificity") {
    ++project->specificity;
  }
  SECTION("capacity mismatch") {
    ++selected.context.capacity.context_window_tokens;
  }
  CHECK_FALSE(domain::repository_context_admission_matches_context(
      sealed, selected.context));
  CHECK_FALSE(domain::repository_context_admission_matches_context(
      std::optional<domain::RepositoryContextAdmission>{}, selected.context));
}

TEST_CASE("repository admission seals omitted references and rejects missing "
          "decisions",
          "[repository-context][failure]") {
  Source source;
  runtime::RepositoryContextController controller{source, source.snapshot.root};
  const auto prepared = controller.prepare({"src", 7, {"src/main.cpp"}});
  REQUIRE(prepared);
  auto selected = selection(*prepared, 0);
  const auto sealed = admission(*prepared, 0);
  REQUIRE(sealed.evidence.size() == 1);
  CHECK(sealed.evidence[0].decision ==
        domain::RepositoryContextDecision::omitted_class_budget);
  CHECK(domain::repository_context_admission_matches_context(sealed,
                                                             selected.context));
  auto tampered = sealed;
  tampered.evidence.clear();
  CHECK_FALSE(domain::validate_repository_context_admission(tampered));
  REQUIRE_FALSE(selected.decisions.empty());
  selected.decisions.clear();
  CHECK_FALSE(
      runtime::finalize_repository_context_admission(*prepared, selected));
}

TEST_CASE("repository revalidation pins instruction membership and all "
          "selected bytes",
          "[repository-context][failure]") {
  Source source;
  runtime::RepositoryContextController controller{source, source.snapshot.root};
  const auto prepared = controller.prepare({"src", 7, {"src/main.cpp"}});
  REQUIRE(prepared);
  const auto sealed = admission(*prepared, 0);
  source.snapshot.fingerprint = digest("new snapshot");
  SECTION("omitted selected evidence changed") {
    source.files["src/main.cpp"] += "changed";
  }
  SECTION("root instructions changed") {
    source.instructions[0].second += "changed";
  }
  SECTION("instruction removed") {
    source.instructions.erase(source.instructions.begin());
  }
  SECTION("new applicable instruction") {
    source.instructions.push_back({"src/nested", "new instructions"});
    auto changed = sealed;
    changed.target_subtree = "src/nested";
    REQUIRE(domain::seal_repository_context_admission(changed));
    CHECK_FALSE(controller.revalidate(changed));
    return;
  }
  SECTION("physical root replaced") {
    source.binding = "root:123:999";
  }
  SECTION("different repository") {
    source.snapshot.root.repository_id = id<domain::RepositoryId>("foreign");
  }
  CHECK_FALSE(controller.revalidate(sealed));
}

TEST_CASE(
    "repository context admits unrelated drift only after exact revalidation",
    "[repository-context]") {
  Source source;
  runtime::RepositoryContextController controller{source, source.snapshot.root};
  const auto prepared = controller.prepare({"src", 7, {"src/main.cpp"}});
  REQUIRE(prepared);
  auto rebased = *prepared;
  rebased.admission.evidence[0].order = 20;
  rebased.evidence.items[0].order = 20;
  const auto sealed = admission(rebased, 0);
  source.files["src/other.cpp"] += "unrelated change";
  source.snapshot.fingerprint = digest("new snapshot");
  const auto restored = controller.revalidate(sealed);
  INFO((restored ? "revalidated" : restored.error().message));
  REQUIRE(restored);
  CHECK(domain::repository_context_admission_successor(sealed,
                                                       restored->admission));
  CHECK(restored->admission.source_snapshot != sealed.source_snapshot);
  CHECK(restored->admission.evidence[0].text_digest ==
        sealed.evidence[0].text_digest);
  CHECK(restored->admission.evidence[0].entry_id ==
        sealed.evidence[0].entry_id);
  CHECK(restored->admission.evidence[0].decision ==
        sealed.evidence[0].decision);
  runtime::RepositoryContextController reopened{source, source.snapshot.root};
  CHECK(reopened.revalidate(sealed));
  CHECK(source.reads == 3);
}

TEST_CASE("repository context supports explicit empty instruction membership",
          "[repository-context]") {
  Source source;
  source.instructions.clear();
  runtime::RepositoryContextController controller{source, source.snapshot.root};
  const auto prepared = controller.prepare({".", 1, {}});
  REQUIRE(prepared);
  CHECK(prepared->discovery.target_subtree.empty());
  CHECK(prepared->instructions.empty());
  const auto sealed = admission(*prepared);
  CHECK(domain::validate_repository_context_admission(sealed));
  CHECK(controller.revalidate(sealed));
  auto context = selection(*prepared).context;
  CHECK(domain::repository_context_admission_matches_context(
      std::optional<domain::RepositoryContextAdmission>{}, context));
}
