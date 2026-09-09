#include "fixture.hpp"

#include <algorithm>

#include <aiforge/runtime/run_kernel.hpp>
#include <aiforge/testing/scripted_backend.hpp>
#include <aiforge/testing/scripted_tool_executor.hpp>

namespace {
auto backend_request(const runtime::ToolRegistrySnapshot& registry)
    -> backend::BackendRequest {
  domain::ConstructedContext context{
      {domain::ContextEntry{
          id<domain::ContextEntryId>("context"),
          domain::ContextEntryKind::instruction,
          domain::InstructionLayer::application_runtime,
          domain::Message{id<domain::MessageId>("runtime"),
                          domain::Role::system,
                          {domain::TextBlock{"runtime contract"}},
                          {}},
          {id<domain::ContextSourceId>("source"), {}, {}},
          0,
          1,
          2}},
      {},
      {4096, 512, 0},
      2};
  return {id<domain::InferenceId>("inference"),
          id<domain::MessageId>("assistant"),
          id<domain::ModelId>("fake"),
          std::move(context),
          registry.declarations(),
          {}};
}
auto start(backend::BackendRequest request) -> runtime::RunStart {
  return {id<domain::RunId>("run"),
          {id<domain::SurfaceId>("test"),
           id<domain::WorkspaceId>("chat"),
           id<domain::PermissionProfileId>("observe"),
           {}},
          {id<domain::MessageId>("user"),
           domain::Role::user,
           {domain::TextBlock{"inspect"}},
           {}},
          std::move(request)};
}
auto proposal(std::string name, std::string arguments)
    -> testing::StreamScript {
  return {{{backend::BackendEvent{backend::ResponseStarted{"response"}}},
           {backend::BackendEvent{backend::ToolCallDelta{
               invocation_id(), std::move(name), std::move(arguments)}}},
           {backend::BackendEvent{
               backend::ResponseFinished{domain::FinishReason::tool_call}}},
           testing::EndOfStream{}}};
}
auto policy(std::vector<domain::CapabilityScope> scopes)
    -> std::shared_ptr<runtime::ToolPolicy> {
  return std::make_shared<runtime::CapabilityPolicy>(
      runtime::PermissionProfile{id<domain::PermissionProfileId>("observe"),
                                 {domain::Effect::read},
                                 std::move(scopes),
                                 {},
                                 {}});
}
template <class Payload> auto seen(const runtime::RunKernel& kernel) -> bool {
  return std::ranges::any_of(
      kernel.event_log().events(), [](const auto& event) {
        return std::holds_alternative<Payload>(event.payload);
      });
}
template <class Payload> auto drain_until(runtime::RunKernel& kernel) -> void {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{5};
  bool valid = true;
  while (!seen<Payload>(kernel) &&
         std::chrono::steady_clock::now() < deadline) {
    valid = kernel.drain().has_value();
    if (!valid) break;
    std::this_thread::yield();
  }
  REQUIRE(valid);
  REQUIRE(seen<Payload>(kernel));
}
auto obtain_receipt(Fixture& fixture) -> runtime::OpsObservationReceipt {
  const auto invocation = fixture.invocation();
  auto future = std::async(std::launch::async, [&] {
    return fixture.endpoint->observe(
        invocation.invocation_id, *invocation.arguments.observation_request,
        std::chrono::steady_clock::now() + std::chrono::seconds{5});
  });
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{3};
  bool serviced = true;
  while (future.wait_for(std::chrono::milliseconds{0}) !=
             std::future_status::ready &&
         std::chrono::steady_clock::now() < deadline) {
    serviced = fixture.broker->service().has_value();
    if (!serviced) break;
    std::this_thread::yield();
  }
  REQUIRE(serviced);
  REQUIRE(future.wait_for(std::chrono::milliseconds{0}) ==
          std::future_status::ready);
  auto receipt = future.get();
  REQUIRE(receipt);
  return *receipt;
}
} // namespace

TEST_CASE(
    "native Ops kernel dispatch stays unavailable until durable integration") {
  Fixture fixture;
  auto request = backend_request(fixture.registry);
  testing::ScriptedBackend backend{
      {{request, proposal("observe_target", input().dump())}}};
  runtime::RunKernel kernel{
      spec().session_id,
      backend,
      nullptr,
      {},
      {},
      fixture.registry,
      policy({{domain::Effect::read, "ops.target", "target"}})};
  REQUIRE(kernel.start(start(request)));
  drain_until<domain::ToolErrored>(kernel);
  CHECK(backend.recorded_requests().size() == 1);
  CHECK_FALSE(seen<domain::ToolStarted>(kernel));
  CHECK_FALSE(seen<domain::ToolResultRecorded>(kernel));
  CHECK(fixture.source->calls == 0);
}

TEST_CASE(
    "ordinary executor receipts cannot publish evidence through the kernel") {
  Fixture fixture;
  const auto receipt = obtain_receipt(fixture);
  const domain::CapabilityScope scope{domain::Effect::read, "filesystem.root",
                                      "/fixture"};
  const runtime::ToolExecutionLimits limits{4096, 1, std::chrono::seconds{2}};
  const runtime::ToolInvocation invocation{
      invocation_id(),
      {},
      "ordinary",
      runtime::ValidatedToolArguments{{"application/json", "{}"}},
      {scope},
      limits};
  auto executor = std::make_shared<testing::ScriptedToolExecutor>(
      std::vector<testing::ScriptedToolExchange>{
          {invocation, testing::ToolStreamScript{
                           {runtime::ToolExecutionEvent{
                                runtime::OpsObservationReady{receipt}},
                            testing::ToolEndOfStream{}}}}});
  runtime::ToolRegistry registrations;
  REQUIRE(registrations.register_tool(
      {"ordinary",
       "ordinary read fixture",
       {"application/schema+json", R"({"type":"object"})"},
       {domain::Effect::read},
       {scope}},
      executor, limits));
  const auto registry = registrations.snapshot().value();
  const auto request = backend_request(registry);
  testing::ScriptedBackend backend{{{request, proposal("ordinary", "{}")}}};
  runtime::RunKernel kernel{spec().session_id, backend,        nullptr, {}, {},
                            registry,          policy({scope})};
  REQUIRE(kernel.start(start(request)));
  drain_until<domain::RunFailed>(kernel);
  CHECK(executor->recorded_invocations().size() == 1);
  CHECK(backend.recorded_requests().size() == 1);
  CHECK_FALSE(seen<domain::ToolResultRecorded>(kernel));
  CHECK_FALSE(seen<domain::ArtifactCreated>(kernel));
  CHECK(fixture.source->calls == 1);
}
