#include "../131summarykernel/fixture.hpp"
#include <aiforge/testing/scripted_backend.hpp>

namespace {
using namespace summary_kernel_test;
enum class Output {
  structured,
  image,
  artifact_reference,
  citation,
  citation_event,
  unknown,
  text
};
auto output(Output kind, const backend::BackendRequest& request)
    -> backend::BackendEvent {
  if (kind == Output::image)
    return backend::ArtifactProduced{{id<domain::ArtifactId>("generated"),
                                      "image/png",
                                      100,
                                      "sha256:" + std::string(64, 'a'),
                                      {},
                                      10,
                                      10,
                                      request.inference_id},
                                     "Generated image"};
  if (kind == Output::citation_event)
    return backend::CitationObserved{{"https://example.test/source", "source"}};
  domain::ContentBlock block = domain::TextBlock{"Valid summary text."};
  if (kind == Output::structured)
    block = domain::StructuredDataBlock{"application/json", "{\"value\":1}"};
  if (kind == Output::artifact_reference)
    block = domain::ArtifactReferenceBlock{id<domain::ArtifactId>("external"),
                                           "image"};
  if (kind == Output::citation)
    block = domain::CitationBlock{"https://example.test/source", "source"};
  if (kind == Output::unknown)
    block = domain::UnknownContentBlock{"future-image"};
  return backend::ContentDelta{request.assistant_message_id, std::move(block)};
}
auto script(Output kind, const backend::BackendRequest& request)
    -> testing::StreamScript {
  const auto amount = domain::MonetaryAmount::create(
                          "USD", domain::DecimalAmount::from("0.01").value())
                          .value();
  const auto cost = domain::ReportedCost::create({amount}).value();
  return {{testing::ScriptedStep{backend::ResponseStarted{"summary"}},
           testing::ScriptedStep{backend::UsageObserved{{2, 1, 0, 1}}},
           testing::ScriptedStep{backend::CostObserved{cost}},
           testing::ScriptedStep{
               backend::ReasoningDelta{"Accounting remains visible", {}}},
           testing::ScriptedStep{output(kind, request)},
           testing::ScriptedStep{
               backend::ResponseFinished{domain::FinishReason::stop}},
           testing::EndOfStream{}}};
}
template <typename T>
auto count(const domain::SessionEventLog& log) -> std::size_t {
  return static_cast<std::size_t>(
      std::ranges::count_if(log.events(), [](const auto& event) {
        return std::holds_alternative<T>(event.payload);
      }));
}
auto drain(runtime::RunKernel& kernel) -> void {
  for (unsigned i = 0; i < 1000 && kernel.active_inference_id(); ++i) {
    const auto result = kernel.drain();
    if (!result)
      CHECK(result.error().code ==
            runtime::RunKernelErrorCode::protocol_failure);
    if (kernel.active_inference_id())
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  REQUIRE_FALSE(kernel.active_inference_id());
}
} // namespace

TEST_CASE(
    "summary output rejects non-text and artifacts before event retention",
    "[summaryoutput][failure]") {
  auto kind = Output::structured;
  SECTION("structured data") {
    kind = Output::structured;
  }
  SECTION("produced image") {
    kind = Output::image;
  }
  SECTION("artifact reference") {
    kind = Output::artifact_reference;
  }
  SECTION("citation content") {
    kind = Output::citation;
  }
  SECTION("citation observation") {
    kind = Output::citation_event;
  }
  SECTION("unknown content") {
    kind = Output::unknown;
  }
  Fixture fixture;
  const auto start = fixture.request();
  fixture.kernel.reset();
  testing::ScriptedBackend backend{
      {{start.request, script(kind, start.request)}}};
  auto opened =
      runtime::RunKernel::open_durable({id<domain::SessionId>("session"),
                                        runtime::DurableSessionMode::resume,
                                        {}},
                                       fixture.store, backend);
  REQUIRE(opened);
  auto& kernel = **opened;
  REQUIRE(kernel.start(start));
  drain(kernel);
  const auto& log = kernel.event_log();
  CHECK(count<domain::RunFailed>(log) == 1);
  CHECK(count<domain::ArtifactCreated>(log) == 0);
  CHECK(count<domain::ArtifactReferenced>(log) == 0);
  CHECK(count<domain::AssistantContentDeltaAdded>(log) == 0);
  CHECK(count<domain::ConversationSummaryCandidatePublished>(log) == 0);
  CHECK(count<domain::ToolStarted>(log) == 0);
  CHECK(count<domain::UsageRecorded>(log) == 1);
  CHECK(count<domain::InferenceCostRecorded>(log) == 1);
  CHECK(count<domain::ReasoningMetadataAdded>(log) == 1);
  const auto before = log.events();
  CHECK_FALSE(kernel.publish_conversation_summary(
      {id<domain::RunId>("publish"), attributes(domain::RunPurpose::control),
       start.summary_intent->summary_id}));
  CHECK(log.events() == before);
}

TEST_CASE("summary textual output retains accounting and remains publishable",
          "[summaryoutput][success]") {
  Fixture fixture;
  const auto start = fixture.request();
  fixture.kernel.reset();
  testing::ScriptedBackend backend{
      {{start.request, script(Output::text, start.request)}}};
  auto opened =
      runtime::RunKernel::open_durable({id<domain::SessionId>("session"),
                                        runtime::DurableSessionMode::resume,
                                        {}},
                                       fixture.store, backend);
  REQUIRE(opened);
  auto& kernel = **opened;
  REQUIRE(kernel.start(start));
  drain(kernel);
  CHECK(count<domain::RunFailed>(kernel.event_log()) == 0);
  CHECK(count<domain::UsageRecorded>(kernel.event_log()) == 1);
  CHECK(count<domain::InferenceCostRecorded>(kernel.event_log()) == 1);
  CHECK(count<domain::ReasoningMetadataAdded>(kernel.event_log()) == 1);
  CHECK(count<domain::ArtifactCreated>(kernel.event_log()) == 0);
  const auto candidate = kernel.publish_conversation_summary(
      {id<domain::RunId>("publish"), attributes(domain::RunPurpose::control),
       start.summary_intent->summary_id});
  INFO((candidate ? "published" : candidate.error().message));
  REQUIRE(candidate);
  CHECK(candidate->text == "Valid summary text.");
}
