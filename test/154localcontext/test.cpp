#include <aiforge/detail/sha256.hpp>
#include <aiforge/domain/local_context.hpp>
#include <catch2/catch_test_macros.hpp>
#include <format>
#include <limits>

namespace {
using namespace aiforge;
auto admission() -> domain::LocalContextAdmission {
  const std::string suffix(64, 'c');
  return {
      1,
      domain::SessionId::from("session").value(),
      3,
      {100, 10, 0},
      {{domain::EvidenceId::from("local-evidence-" + suffix).value(),
        domain::ContextEntryId::from("local-context-entry-" + suffix).value(),
        domain::MessageId::from("local-context-message-" + suffix).value(),
        domain::ContextSourceId::from("local-context-source-" + suffix).value(),
        {{1, std::string(64, 'b')},
         "notes.txt",
         {"sha256",
          "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824",
          5}},
        7,
        5,
        domain::LocalContextDecision::admitted}},
      {}};
}
auto context(const domain::LocalContextAdmission& value)
    -> domain::ConstructedContext {
  const auto& ref = value.evidence.front();
  return {{{ref.entry_id,
            domain::ContextEntryKind::evidence,
            {},
            {ref.message_id,
             domain::Role::evidence,
             {domain::TextBlock{"hello"}},
             {}},
            {ref.source_id,
             domain::local_context_source_location(ref.source).value(),
             "sha256:" + ref.source.content_digest.value},
            0,
            ref.order,
            ref.estimated_tokens}},
          {},
          value.capacity,
          5};
}
} // namespace

TEST_CASE("local admission rejects malformed references and capacity before "
          "sealing") {
  auto value = admission();
  SECTION("future version") {
    value.version = 2;
  }
  SECTION("zero revision") {
    value.selection_revision = 0;
  }
  SECTION("zero capacity") {
    value.capacity.context_window_tokens = 0;
  }
  SECTION("output overflow") {
    value.capacity.reserved_output_tokens =
        std::numeric_limits<std::uint64_t>::max();
  }
  SECTION("input overflow") {
    value.capacity.reserved_input_tokens =
        std::numeric_limits<std::uint64_t>::max();
  }
  SECTION("bad root") {
    value.evidence[0].source.root.version = 2;
  }
  SECTION("absolute path") {
    value.evidence[0].source.relative_path = "/notes.txt";
  }
  SECTION("empty source") {
    value.evidence[0].source.content_digest.byte_size = 0;
  }
  SECTION("oversized source") {
    value.evidence[0].source.content_digest.byte_size =
        std::numeric_limits<std::uint64_t>::max();
  }
  SECTION("false estimate") {
    ++value.evidence[0].estimated_tokens;
  }
  SECTION("zero order") {
    value.evidence[0].order = 0;
  }
  SECTION("unknown decision") {
    value.evidence[0].decision = static_cast<domain::LocalContextDecision>(99);
  }
  SECTION("wrong entry namespace") {
    value.evidence[0].entry_id =
        domain::ContextEntryId::from("repository-context-entry-x").value();
  }
  SECTION("duplicate source") {
    value.evidence.push_back(value.evidence[0]);
  }
  SECTION("too many files") {
    value.evidence.resize(65, value.evidence[0]);
  }
  REQUIRE_FALSE(domain::seal_local_context_admission(value));
}

TEST_CASE("local admission seal detects any changed reference or scope") {
  auto value = admission();
  REQUIRE(domain::seal_local_context_admission(value));
  SECTION("missing seal") {
    value.admission_digest.reset();
  }
  SECTION("wrong hash") {
    value.admission_digest->value[0] = 'z';
  }
  SECTION("wrong sealed length") {
    ++value.admission_digest->byte_size;
  }
  SECTION("wrong algorithm") {
    value.admission_digest->algorithm = "git-sha256";
  }
  SECTION("foreign session") {
    value.session_id = domain::SessionId::from("other").value();
  }
  SECTION("selection change") {
    ++value.selection_revision;
  }
  SECTION("source replaced") {
    value.evidence[0].source.content_digest.value[0] = 'a';
  }
  SECTION("decision changed") {
    value.evidence[0].decision = domain::LocalContextDecision::omitted_budget;
  }
  SECTION("order changed") {
    ++value.evidence[0].order;
  }
  REQUIRE_FALSE(domain::validate_local_context_admission(value));
}

TEST_CASE("local admission matches evidence exactly in both directions") {
  auto value = admission();
  REQUIRE(domain::seal_local_context_admission(value));
  auto actual = context(value);
  REQUIRE(domain::local_context_admission_matches_context(value, actual));
  SECTION("missing admitted entry") {
    actual.entries.clear();
  }
  SECTION("duplicate entry") {
    actual.entries.push_back(actual.entries[0]);
  }
  SECTION("changed text") {
    actual.entries[0].message.content = {domain::TextBlock{"jello"}};
  }
  SECTION("changed estimate") {
    ++actual.entries[0].estimated_tokens;
  }
  SECTION("changed order") {
    ++actual.entries[0].order;
  }
  SECTION("changed provenance") {
    actual.entries[0].provenance.source_location = "other";
  }
  SECTION("instruction authority") {
    actual.entries[0].kind = domain::ContextEntryKind::instruction;
    actual.entries[0].instruction_layer = domain::InstructionLayer::project;
  }
  SECTION("wrong role") {
    actual.entries[0].message.role = domain::Role::system;
  }
  SECTION("wrong capacity") {
    ++actual.capacity.context_window_tokens;
  }
  SECTION("unadmitted namespace") {
    actual.entries[0].entry_id =
        domain::ContextEntryId::from("local-context-unknown").value();
  }
  REQUIRE_FALSE(domain::local_context_admission_matches_context(value, actual));
}

TEST_CASE("omissions empty proofs and exact successors preserve semantics") {
  auto value = admission();
  value.evidence[0].decision =
      domain::LocalContextDecision::omitted_class_budget;
  REQUIRE(domain::seal_local_context_admission(value));
  auto actual = context(value);
  CHECK_FALSE(domain::local_context_admission_matches_context(value, actual));
  CHECK_FALSE(domain::local_context_admission_matches_context(
      std::optional<domain::LocalContextAdmission>{}, actual));
  actual.entries.clear();
  REQUIRE(domain::local_context_admission_matches_context(value, actual));
  CHECK(domain::local_context_admission_matches_context(
      std::optional<domain::LocalContextAdmission>{}, actual));
  REQUIRE(domain::local_context_admission_successor(value, value));
  auto changed = value;
  ++changed.selection_revision;
  REQUIRE(domain::seal_local_context_admission(changed));
  CHECK_FALSE(domain::local_context_admission_successor(value, changed));
  changed = value;
  changed.evidence.clear();
  REQUIRE(domain::seal_local_context_admission(changed));
  REQUIRE(domain::validate_local_context_admission(changed));
  CHECK_FALSE(domain::local_context_admission_successor(value, changed));
}

TEST_CASE("local v1 seal has an independent canonical golden") {
  auto value = admission();
  REQUIRE(domain::seal_local_context_admission(value));
  REQUIRE(value.admission_digest);
  CHECK(value.admission_digest->byte_size == 571);
  CHECK(value.admission_digest->value ==
        "4ea0820d561e0635e36227c182699fe832f1c495d151f9e6cc38f5e215d3029b");
  const auto actual = context(value);
  value.evidence[0].source.root.binding[0] = 'a';
  REQUIRE(domain::seal_local_context_admission(value));
  CHECK_FALSE(domain::local_context_admission_matches_context(value, actual));
}

TEST_CASE(
    "tagged local provenance cannot evade admission by replacing identifiers") {
  auto actual = context(admission());
  actual.entries[0].entry_id =
      domain::ContextEntryId::from("other-entry").value();
  actual.entries[0].message.message_id =
      domain::MessageId::from("other-message").value();
  actual.entries[0].provenance.source_id =
      domain::ContextSourceId::from("other-source").value();
  CHECK_FALSE(domain::local_context_admission_matches_context(
      std::optional<domain::LocalContextAdmission>{}, actual));
}
TEST_CASE(
    "local admission bounds distinct files roots and total selected bytes") {
  auto value = admission();
  const auto original = value.evidence[0];
  value.evidence.clear();
  for (std::uint64_t index = 0; index < 64; ++index) {
    auto ref = original;
    const auto suffix = std::format("{:064x}", index);
    ref.evidence_id =
        domain::EvidenceId::from("local-evidence-" + suffix).value();
    ref.entry_id =
        domain::ContextEntryId::from("local-context-entry-" + suffix).value();
    ref.message_id =
        domain::MessageId::from("local-context-message-" + suffix).value();
    ref.source_id =
        domain::ContextSourceId::from("local-context-source-" + suffix).value();
    ref.source.relative_path = std::to_string(index);
    ref.order = index + 1;
    value.evidence.push_back(ref);
  }
  REQUIRE(domain::seal_local_context_admission(value));
  SECTION("root limit") {
    value.evidence.erase(value.evidence.begin() + 17, value.evidence.end());
    for (std::size_t index = 0; index < value.evidence.size(); ++index)
      value.evidence[index].source.root.binding = std::format("{:064x}", index);
  }
  SECTION("aggregate byte bound") {
    value.evidence.erase(value.evidence.begin() + 9, value.evidence.end());
    for (auto& ref : value.evidence) {
      ref.source.content_digest.byte_size = std::uint64_t{256} * 1024;
      ref.estimated_tokens = ref.source.content_digest.byte_size;
    }
  }
  REQUIRE_FALSE(domain::seal_local_context_admission(value));
}
