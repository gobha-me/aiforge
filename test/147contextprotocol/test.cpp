#include <aiforge/adapters/context_jsonl.hpp>
#include <aiforge/surfaces/agent.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

namespace {
using namespace aiforge;
using Json = nlohmann::json;
auto candidate() -> Json {
  return {{"summary_id", "summary"},
          {"revision", 1},
          {"candidate_digest",
           {{"algorithm", "sha256"},
            {"value", std::string(64, 'a')},
            {"byte_size", 123}}}};
}
} // namespace
TEST_CASE("Context JSONL rejects malformed envelopes and coercion",
          "[context][protocol][failure]") {
  for (
      const auto* input :
      {"",
       "[]",
       "null",
       "{}",
       "{",
       R"({"version":1,"id":1,"op":"inspect","unknown":true})",
       R"({"version":2,"id":1,"op":"inspect"})",
       R"({"version":1.0,"id":1,"op":"inspect"})",
       R"({"version":1,"id":0,"op":"inspect"})",
       R"({"version":1,"id":-1,"op":"inspect"})",
       R"({"version":1,"id":1.0,"op":"inspect"})",
       R"({"version":1,"id":"1","op":"inspect"})",
       R"({"version":1,"id":1,"id":2,"op":"inspect"})",
       R"({"version":1,"id":1,"op":"unknown"})",
       R"({"version":1,"id":1,"op":"inspect","limit":0})",
       R"({"version":1,"id":1,"op":"inspect","limit":17})",
       R"({"version":1,"id":1,"op":"inspect","offset":-1})",
       R"({"version":1,"id":1,"op":"summary.preview.page","handle":"h","limit":0})",
       R"({"version":1,"id":1,"op":"summary.preview.page","handle":"h","offset":-1})",
       R"({"version":1,"id":1,"op":"close","draft":"x"})"}) {
    INFO(input);
    auto parsed = adapters::parse_context_request(input);
    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().code == "invalid_request");
  }
  CHECK_FALSE(adapters::parse_context_request(
      std::string(surfaces::agent_maximum_input_bytes + 1, 'x')));
}
TEST_CASE("Context JSONL bounds selections and exact candidate references",
          "[context][protocol][failure]") {
  Json value{{"version", 1},
             {"id", 1},
             {"op", "summary.preview"},
             {"candidate", candidate()},
             {"replacements", Json::array()},
             {"draft", "preserved"}};
  SECTION("unknown candidate field") {
    value["candidate"]["extra"] = true;
  }
  SECTION("zero revision") {
    value["candidate"]["revision"] = 0;
  }
  SECTION("coerced revision") {
    value["candidate"]["revision"] = "1";
  }
  SECTION("uppercase digest") {
    value["candidate"]["candidate_digest"]["value"] = std::string(64, 'A');
  }
  SECTION("unbounded digest") {
    value["candidate"]["candidate_digest"]["byte_size"] =
        domain::summary_maximum_manifest_bytes + 1;
  }
  SECTION("zero digest size") {
    value["candidate"]["candidate_digest"]["byte_size"] = 0;
  }
  SECTION("too many replacements") {
    for (std::size_t i{}; i <= domain::summary_maximum_active; ++i)
      value["replacements"].push_back(candidate());
  }
  CHECK_FALSE(adapters::parse_context_request(value.dump()));
  CHECK_FALSE(adapters::parse_context_request(
      R"({"version":1,"id":1,"op":"summary.generate","expected_sequence":0,"runs":[]})"));
  CHECK_FALSE(adapters::parse_context_request(
      R"({"version":1,"id":1,"op":"policy","expected_revision":0,"mode":"full","pins":["same","same"]})"));
  CHECK_FALSE(adapters::parse_context_request(
      R"({"version":1,"id":1,"op":"policy","expected_revision":0,"mode":"automatic","pins":[]})"));
}
TEST_CASE("Context JSONL parses every explicit operation and emits stable "
          "bounded records",
          "[context][protocol]") {
  const auto ref = candidate();
  const std::vector<Json> requests{
      {{"op", "inspect"}},
      {{"op", "policy"},
       {"expected_revision", 0},
       {"mode", "rolling"},
       {"pins", Json::array({"run"})}},
      {{"op", "summary.generate"},
       {"expected_sequence", 7},
       {"runs", Json::array({"run"})}},
      {{"op", "summary.publish"}, {"summary_id", "summary"}},
      {{"op", "summary.edit"},
       {"expected_sequence", 7},
       {"parent", ref},
       {"text", "edited"}},
      {{"op", "summary.preview"},
       {"candidate", ref},
       {"replacements", Json::array()},
       {"draft", "draft"}},
      {{"op", "summary.preview.page"},
       {"handle", "instance-1"},
       {"offset", 16},
       {"limit", 8}},
      {{"op", "summary.apply"}, {"handle", "instance-1"}, {"draft", "draft"}},
      {{"op", "summary.disable"},
       {"expected_revision", 1},
       {"candidate", ref},
       {"activation_event_id", "event"}},
      {{"op", "close"}}};
  for (auto request : requests) {
    request["version"] = 1;
    request["id"] = 42;
    const auto parsed = adapters::parse_context_request(request.dump());
    INFO(request.dump());
    REQUIRE(parsed);
    CHECK(parsed->id == 42);
  }
  auto encoded = adapters::encode_context_reply(
      {1, 7, surfaces::ContextFailure{"stale", "review expired", false}});
  REQUIRE(encoded);
  CHECK(encoded->back() == '\n');
  const auto value = Json::parse(*encoded);
  CHECK(value["version"] == 1);
  CHECK(value["type"] == "error");
  CHECK(value["sequence"] == 7);
  CHECK_FALSE(adapters::encode_context_reply(
      {1, 7,
       surfaces::ContextFailure{
           "large", std::string(surfaces::agent_maximum_record_bytes, 'x'),
           false}}));
}

TEST_CASE("Context inspection exposes decisions and lineage within escaped "
          "output pages",
          "[context][protocol][bounds]") {
  const auto model = domain::ModelId::from("model").value();
  const auto session = domain::SessionId::from("session").value();
  const auto output = domain::EventId::from("output").value();
  const auto run = domain::RunId::from(std::string(128, '\\')).value();
  const domain::ContentDigest digest{"sha256", std::string(64, 'a'), 100};
  surfaces::ContextInspected inspected{
      {{}, model, {{32768, 256, 0}, {}, {}}, {}, {}}, {}, 0, 16};
  inspected.context.groups.push_back(
      {run, 2, 40, false,
       runtime::ConversationSelectionDecision::omitted_summary});
  for (unsigned i{}; i < 16; ++i) {
    const auto summary =
        domain::ConversationSummaryId::from("summary-" + std::to_string(i))
            .value();
    domain::ConversationSummaryIntent intent{
        1,       1,
        summary, {1, 1, session, 7, {}, digest},
        run,     domain::InferenceId::from("inference").value(),
        model,   domain::MessageId::from("message").value(),
        "test",  {32768, 256, 0},
        100,     65536,
        digest};
    for (unsigned group{}; group < 128; ++group)
      intent.sources.groups.push_back({run, output, 7, {}});
    inspected.catalog.snapshot.intents.push_back(intent);
    inspected.catalog.snapshot.candidates.push_back(
        {1,
         session,
         summary,
         1,
         digest,
         digest,
         output,
         8,
         output,
         9,
         domain::ConversationSummaryAuthor::model,
         {},
         std::string(domain::summary_maximum_text_bytes, '\x01'),
         digest});
  }
  domain::ConstructedContext constructed{{}, {}, {32768, 256, 0}, 100};
  for (unsigned index{}; index < 100; ++index)
    constructed.entries.push_back(
        {domain::ContextEntryId::from("entry-" + std::to_string(index)).value(),
         domain::ContextEntryKind::conversation,
         {},
         {domain::MessageId::from("message-" + std::to_string(index)).value(),
          domain::Role::assistant,
          {domain::TextBlock{"not exported"}},
          {}},
         {domain::ContextSourceId::from("source").value(), {}, {}},
         0,
         index + 1,
         1});
  inspected.context.next_context = std::move(constructed);
  const auto encoded = adapters::encode_context_reply({1, 9, inspected});
  REQUIRE(encoded);
  CHECK(encoded->size() < surfaces::agent_maximum_record_bytes);
  const auto page = Json::parse(*encoded);
  CHECK(page["groups"][0]["decision"] == "omitted_summary");
  CHECK(page["next_context"]["entries"].size() == 16);
  CHECK(page["next_context"]["entries_count"] == 100);
  CHECK(page["next_context"]["entries_next_offset"] == 16);
  CHECK(encoded->find("not exported") == std::string::npos);
  REQUIRE(page["summaries"].size() > 0);
  CHECK(page["summaries"].size() < 16);
  CHECK(page["summaries_next_offset"] == page["summaries"].size());
  const auto& row = page["summaries"][0];
  CHECK(row["source_digest"]["value"] == digest.value);
  CHECK(row["producer"]["covered_run_ids"].size() == 128);
  CHECK(row["producer"]["producing_run_id"] == run.value());
  CHECK(row["producer"]["model_id"] == "model");
  CHECK(row["producer"]["producing_inference_id"] == "inference");
  inspected.offset = page["summaries_next_offset"].get<std::size_t>();
  const auto next = adapters::encode_context_reply({2, 9, inspected});
  REQUIRE(next);
  CHECK(Json::parse(*next)["summaries"][0]["candidate"]["summary_id"] ==
        "summary-" + std::to_string(inspected.offset));
}
