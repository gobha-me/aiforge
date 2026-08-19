#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <vector>

#include <aiforge/runtime/context_builder.hpp>
#include <aiforge/runtime/project_instructions.hpp>
#include <aiforge/testing/scripted_project_instruction_source.hpp>

namespace {

using namespace aiforge;
using namespace std::chrono_literals;

template <typename IdType>
auto id(std::string value) -> IdType {
  return IdType::from(std::move(value)).value();
}

auto digest(std::string value = "aaaaaaaaaaaaaaaa")
    -> domain::ContentDigest {
  return {"git-sha1", std::move(value), 4};
}

auto snapshot(std::string fingerprint = "bbbbbbbbbbbbbbbb")
    -> domain::RepositorySnapshot {
  return {{id<domain::RepositoryId>("repository"), "/work/repository"},
          std::nullopt,
          {},
          {"git-sha1", std::move(fingerprint), 64},
          std::chrono::sys_time<std::chrono::milliseconds>{100ms}};
}

auto document(std::string instruction_id, std::string path,
              std::string subtree, std::string text,
              const std::uint32_t specificity,
              const std::uint64_t order)
    -> domain::ProjectInstructionDocument {
  auto content_digest = digest();
  content_digest.byte_size = text.size();
  return {id<domain::ProjectInstructionId>(std::move(instruction_id)),
          {domain::snapshot_identity(snapshot()), std::move(path),
           std::move(content_digest), std::nullopt},
          std::move(subtree), std::move(text), specificity, order};
}

auto discovery() -> domain::ProjectInstructionDiscovery {
  return {domain::snapshot_identity(snapshot()),
          "src/lib",
          {document("root-instruction", "AGENTS.md", "", "root", 0, 1),
           document("src-instruction", "src/AGENTS.md", "src", "nested", 1,
                    2)}};
}

auto estimates() -> std::vector<runtime::ProjectInstructionTokenEstimate> {
  return {{id<domain::ProjectInstructionId>("root-instruction"), 3},
          {id<domain::ProjectInstructionId>("src-instruction"), 4}};
}

}  // namespace

TEST_CASE("scripted project instruction sources are bounded and cancellable",
          "[instructions][fake][failure]") {
  const repository::ProjectInstructionRequest request{snapshot(), "src/lib", {}};
  testing::ScriptedProjectInstructionSource source{{{request, discovery()}}};
  const auto result = source.discover(request);
  REQUIRE(result == discovery());
  REQUIRE(source.recorded_requests() ==
          std::vector<repository::ProjectInstructionRequest>{request});
  REQUIRE(source.remaining_exchanges() == 0);

  auto exhausted = source.discover(request);
  REQUIRE_FALSE(exhausted);
  REQUIRE(exhausted.error().code ==
          repository::ProjectInstructionErrorCode::internal_failure);

  std::stop_source cancelled;
  cancelled.request_stop();
  testing::ScriptedProjectInstructionSource stopped;
  auto cancellation = stopped.discover(request, cancelled.get_token());
  REQUIRE_FALSE(cancellation);
  REQUIRE(cancellation.error().code ==
          repository::ProjectInstructionErrorCode::cancelled);
  REQUIRE(stopped.recorded_requests().empty());
}

TEST_CASE("project instruction context handoff rejects stale and invalid input",
          "[instructions][context][failure]") {
  auto value = discovery();
  auto current = value.source_snapshot;
  current.fingerprint.value = "cccccccccccccccc";
  auto result = runtime::project_instruction_inputs(value, current, estimates());
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          runtime::ProjectInstructionContextErrorCode::stale_snapshot);

  auto duplicate = estimates();
  duplicate.push_back(duplicate.front());
  result = runtime::project_instruction_inputs(value, value.source_snapshot,
                                               duplicate);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          runtime::ProjectInstructionContextErrorCode::duplicate_estimate);

  auto missing = estimates();
  missing.pop_back();
  result = runtime::project_instruction_inputs(value, value.source_snapshot,
                                               missing);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          runtime::ProjectInstructionContextErrorCode::missing_estimate);

  auto zero = estimates();
  zero.front().estimated_tokens = 0;
  result = runtime::project_instruction_inputs(value, value.source_snapshot,
                                               zero);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          runtime::ProjectInstructionContextErrorCode::invalid_estimate);

  value.documents.back().source.relative_path = "other/AGENTS.md";
  result = runtime::project_instruction_inputs(value, value.source_snapshot,
                                               estimates());
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          runtime::ProjectInstructionContextErrorCode::invalid_discovery);

  value = discovery();
  value.documents.back().applicable_subtree = "sibling";
  value.documents.back().source.relative_path = "sibling/AGENTS.md";
  result = runtime::project_instruction_inputs(value, value.source_snapshot,
                                               estimates());
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          runtime::ProjectInstructionContextErrorCode::invalid_discovery);

  value = discovery();
  value.documents.back().text = std::string{"bad\0text", 8};
  value.documents.back().source.content_digest.byte_size = 8;
  result = runtime::project_instruction_inputs(value, value.source_snapshot,
                                               estimates());
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          runtime::ProjectInstructionContextErrorCode::invalid_discovery);
}

TEST_CASE("project instructions enter the accepted precedence path",
          "[instructions][context][smoke]") {
  const auto value = discovery();
  auto inputs = runtime::project_instruction_inputs(
      value, value.source_snapshot, estimates());
  REQUIRE(inputs);
  REQUIRE(inputs->size() == 2);
  REQUIRE((*inputs)[0].layer == domain::InstructionLayer::project);
  REQUIRE((*inputs)[0].specificity == 0);
  REQUIRE((*inputs)[1].specificity == 1);
  REQUIRE((*inputs)[1].provenance.source_location == "src/AGENTS.md");

  domain::ContextBuildInput build{{128, 16, 0}, {}, {}};
  build.instructions.push_back(domain::InstructionInput{
      id<domain::ContextEntryId>("runtime"),
      domain::InstructionLayer::application_runtime,
      domain::InstructionOperation::add,
      std::nullopt,
      domain::Message{id<domain::MessageId>("runtime-message"),
                      domain::Role::system,
                      {domain::TextBlock{"Runtime safety"}}, std::nullopt},
      {id<domain::ContextSourceId>("runtime-source"), std::nullopt,
       std::nullopt},
      0,
      10,
      5});
  build.instructions.insert(build.instructions.end(), inputs->begin(),
                            inputs->end());
  const auto context = runtime::ContextBuilder{}.build(std::move(build));
  REQUIRE(context);
  REQUIRE(context->entries.size() == 3);
  REQUIRE(context->entries[0].instruction_layer ==
          domain::InstructionLayer::application_runtime);
  REQUIRE(context->entries[1].message.content ==
          std::vector<domain::ContentBlock>{domain::TextBlock{"root"}});
  REQUIRE(context->entries[2].message.content ==
          std::vector<domain::ContentBlock>{domain::TextBlock{"nested"}});
}
