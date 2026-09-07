#include <aiforge/surfaces/agent.hpp>

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <utility>

using namespace aiforge;

namespace {
template <class Id> auto id(const std::string& value) -> Id {
  return Id::from(value).value();
}

auto event(domain::RunEventPayload payload) -> domain::RunEvent {
  return {{id<domain::EventId>("event"),
           id<domain::RunId>("run"),
           7,
           1,
           {},
           {},
           {},
           id<domain::InvocationId>("envelope-invocation")},
          std::move(payload)};
}

auto encode(domain::RunEventPayload payload)
    -> std::expected<std::string, surfaces::AgentError> {
  return surfaces::agent_event_record(id<domain::SessionId>("session"),
                                      event(std::move(payload)));
}

auto require_payload(domain::RunEventPayload payload,
                     const std::string& expected) -> void {
  const auto record = encode(std::move(payload));
  REQUIRE(record);
  CHECK(*record ==
        "{\"event_id\":\"event\",\"invocation_id\":\"envelope-invocation\","
        "\"payload\":" +
            expected +
            ",\"run_id\":\"run\",\"sequence\":7,\"session_id\":\"session\","
            "\"type\":\"event\",\"version\":1}\n");
}

auto provenance() -> domain::RunProvenance {
  domain::RunProvenance result{
      "test", "backend", {}, id<domain::ModelId>("model"), {}, {}, {}, {}};
  result.tools = {
      {"run_process",
       {domain::Effect::execute},
       {{domain::Effect::execute, "process.executable", "/bin/test"}},
       "registration"}};
  result.tool_profile =
      domain::ToolProfileProvenance{id<domain::ToolProfileId>("dev"),
                                    id<domain::ToolProfileId>("process"),
                                    {},
                                    std::vector<std::string>{"run_process"}};
  result.tool_policy = domain::ToolPolicyProvenance{
      "policy-v2",
      id<domain::PermissionProfileId>("permission"),
      domain::ToolRestrictionLevel::none,
      domain::ToolApprovalMode::automatic,
      {domain::Effect::execute},
      {{domain::Effect::execute, "process.executable", "/bin/test"}},
      {},
      domain::ToolRestrictionLevel::none,
      {},
      "bounded-argv",
      "1",
      "restriction",
      "matcher"};
  return result;
}
} // namespace

TEST_CASE("agent records reject invalid and oversized projected fields",
          "[agent][records][failure]") {
  auto value = provenance();
  SECTION("invalid UTF-8 policy identity") {
    value.tool_policy->identity = std::string{"\xff"};
  }
  SECTION("oversized policy identity") {
    value.tool_policy->identity.assign(surfaces::agent_maximum_record_bytes,
                                       'x');
  }
  SECTION("too many selected tools") {
    value.tool_profile->desired_tool_names = std::vector<std::string>(257, "x");
  }
  SECTION("too many policy capability scopes") {
    value.tool_policy->capability_ceiling.resize(
        1025, {domain::Effect::read, "filesystem.root", "/repo"});
  }
  SECTION("invalid restriction enum") {
    value.tool_policy->restriction_level =
        static_cast<domain::ToolRestrictionLevel>(99);
  }
  SECTION("invalid approval enum") {
    value.tool_policy->approval_mode =
        static_cast<domain::ToolApprovalMode>(99);
  }
  SECTION("invalid unavailable reason") {
    value.tool_policy->restriction_unavailable_reason =
        static_cast<domain::ToolRestrictionUnavailableReason>(99);
  }
  const auto result = encode(domain::RunProvenanceRecorded{std::move(value)});
  REQUIRE_FALSE(result);
  CHECK(result.error().code == surfaces::AgentErrorCode::resource_exhausted);
}

TEST_CASE("agent records exclude config credentials and opaque provider data",
          "[agent][records][secrets]") {
  auto value = provenance();
  value.credential_source = domain::CredentialSourceReference{
      domain::CredentialSourceKind::environment, "CREDENTIAL_SENTINEL"};
  value.configuration = {{"CONFIG_KEY_SENTINEL",
                          "CONFIG_VALUE_SENTINEL",
                          true,
                          domain::ProvenanceSource::environment,
                          false,
                          {}}};
  value.effective_request_options = {
      {"OPTION_KEY_SENTINEL", "OPTION_VALUE_SENTINEL",
       domain::RequestOptionSource::configuration}};
  auto record = encode(domain::RunProvenanceRecorded{std::move(value)});
  REQUIRE(record);
  CHECK(record->find("SENTINEL") == std::string::npos);
  CHECK(record->find("credential") == std::string::npos);
  CHECK(record->find("configuration") == std::string::npos);
  require_payload(
      domain::ReasoningMetadataAdded{
          id<domain::InferenceId>("inference"),
          "RAW_REASONING_SENTINEL",
          {{"RAW_PROVIDER_KEY", "RAW_PROVIDER_VALUE_SENTINEL"}}},
      R"({"kind":"state"})");
  require_payload(
      domain::UnknownEvent{
          "OPAQUE_TYPE_SENTINEL",
          {"application/json", R"({"secret":"OPAQUE_PAYLOAD_SENTINEL"})"}},
      R"({"kind":"state"})");
  require_payload(
      domain::AssistantContentDeltaAdded{
          id<domain::MessageId>("message"),
          id<domain::InferenceId>("inference"),
          domain::UnknownContentBlock{"OPAQUE_CONTENT_SENTINEL"}},
      R"({"content":{"kind":"unsupported"},"inference_id":"inference","kind":"assistant_content","message_id":"message"})");
}

TEST_CASE("agent records preserve typed errors and exact correlation",
          "[agent][records][errors]") {
  const domain::DomainError failure{domain::ErrorCode::unavailable,
                                    "temporarily unavailable", true};
  require_payload(
      domain::ToolPolicyFailed{id<domain::InvocationId>("call"), failure},
      R"({"code":"unavailable","invocation_id":"call","kind":"tool_policy_error","message":"temporarily unavailable","retryable":true})");
  require_payload(
      domain::ToolErrored{id<domain::InvocationId>("call"), failure,
                          id<domain::MessageId>("result")},
      R"({"code":"unavailable","invocation_id":"call","kind":"tool_error","message":"temporarily unavailable","result_message_id":"result","retryable":true})");
  require_payload(
      domain::InferenceFailed{id<domain::InferenceId>("inference"), failure},
      R"({"code":"unavailable","inference_id":"inference","kind":"inference_error","message":"temporarily unavailable","retryable":true})");
  require_payload(
      domain::RunFailed{failure},
      R"({"code":"unavailable","kind":"run_error","message":"temporarily unavailable","retryable":true})");
  require_payload(
      domain::ToolApprovalRequested{
          id<domain::InvocationId>("call"),
          {{domain::Effect::read, "filesystem.root", "/repo"}},
          {}},
      R"({"interaction":"tool_approval","invocation_id":"call","kind":"interaction_required","requested_scopes":[{"effect":"read","kind":"filesystem.root","value":"/repo"}]})");
  require_payload(
      domain::RunAwaitingInput{id<domain::QuestionId>("question")},
      R"({"interaction":"question","kind":"interaction_required","question_id":"question"})");
  require_payload(
      domain::ToolResultRecorded{id<domain::InvocationId>("call"),
                                 {domain::TextBlock{"done"}},
                                 id<domain::MessageId>("result")},
      R"({"content":[{"kind":"text","text":"done"}],"invocation_id":"call","kind":"tool_result","result_message_id":"result"})");
}

TEST_CASE("agent records distinguish inference finish and cancellation",
          "[agent][records][inference]") {
  const std::array cases{
      std::pair{domain::FinishReason::stop, "stop"},
      std::pair{domain::FinishReason::length, "length"},
      std::pair{domain::FinishReason::tool_call, "tool_call"},
      std::pair{domain::FinishReason::content_filter, "content_filter"},
      std::pair{domain::FinishReason::other, "other"}};
  for (const auto& [reason, name] : cases) {
    require_payload(
        domain::InferenceFinished{id<domain::InferenceId>("inference"), reason},
        "{\"finish_reason\":\"" + std::string{name} +
            "\",\"inference_id\":\"inference\",\"kind\":\"inference_"
            "finished\"}");
  }
  require_payload(
      domain::InferenceCancelled{id<domain::InferenceId>("inference"), {}},
      R"({"inference_id":"inference","kind":"inference_cancelled","reason":null})");
  CHECK_FALSE(
      encode(domain::InferenceFinished{id<domain::InferenceId>("inference"),
                                       static_cast<domain::FinishReason>(99)}));
}

TEST_CASE("agent artifact records retain tool and inference producers",
          "[agent][records][artifact]") {
  domain::ArtifactMetadata artifact{id<domain::ArtifactId>("artifact"),
                                    "image/png",
                                    42,
                                    "digest",
                                    {},
                                    4,
                                    3,
                                    {}};
  SECTION("tool producer") {
    artifact.producing_invocation_id = id<domain::InvocationId>("producer");
    require_payload(
        domain::ArtifactCreated{artifact},
        R"({"artifact_id":"artifact","bytes":42,"digest":"digest","height":3,"kind":"artifact","media_type":"image/png","producing_inference_id":null,"producing_invocation_id":"producer","width":4})");
  }
  SECTION("inference producer") {
    artifact.producing_inference_id = id<domain::InferenceId>("producer");
    require_payload(
        domain::ArtifactCreated{artifact},
        R"({"artifact_id":"artifact","bytes":42,"digest":"digest","height":3,"kind":"artifact","media_type":"image/png","producing_inference_id":"producer","producing_invocation_id":null,"width":4})");
  }
}

TEST_CASE("agent declarations and provenance expose exact safe policy fields",
          "[agent][records][policy]") {
  const auto accepted = surfaces::agent_accepted_record(
      id<domain::SessionId>("session"), id<domain::RunId>("run"),
      {{"read_repository_file",
        "Read",
        {"application/schema+json", "{}"},
        {domain::Effect::read},
        {{domain::Effect::read, "filesystem.root", "/repo"}}}});
  REQUIRE(accepted);
  CHECK(
      *accepted ==
      R"({"run_id":"run","session_id":"session","tools":[{"declared_effects":["read"],"input_schema":"{}","name":"read_repository_file","scopes":[{"effect":"read","kind":"filesystem.root","value":"/repo"}]}],"type":"accepted","version":1})"
      "\n");
  require_payload(
      domain::RunProvenanceRecorded{provenance()},
      R"({"backend":"backend","kind":"provenance","model_id":"model","tool_policy":{"achieved_restriction_level":"none","approval_mode":"automatic","automatically_eligible_tools":[],"capability_ceiling":[{"effect":"execute","kind":"process.executable","value":"/bin/test"}],"effect_ceiling":["execute"],"identity":"policy-v2","matcher_policy_identity":"matcher","mechanism_identity":"bounded-argv","mechanism_version":"1","permission_profile_id":"permission","restriction_level":"none","restriction_policy_identity":"restriction","restriction_unavailable_reason":null},"tool_profile":{"desired_tool_names":["run_process"],"model_maximum_profile_id":"process","persona_maximum_profile_id":null,"selected_profile_id":"dev"},"tools":[{"declared_effects":["execute"],"name":"run_process","registration_digest":"registration","scopes":[{"effect":"execute","kind":"process.executable","value":"/bin/test"}]}]})");
  require_payload(
      domain::ToolPolicyDecided{
          id<domain::InvocationId>("call"),
          domain::PolicyDecision::allow,
          {{domain::Effect::read, "filesystem.root", "/repo"}},
          {},
          domain::PolicyDecisionSource::automatic_matcher,
          domain::AutomaticApprovalEvidence{"matcher", "rule"}},
      R"({"decision":"allow","invocation_id":"call","kind":"tool_policy","matcher_policy_id":"matcher","rule_id":"rule","scopes":[{"effect":"read","kind":"filesystem.root","value":"/repo"}],"source":"automatic_matcher"})");
}

TEST_CASE("agent usage keeps reported values and JSON framing",
          "[agent][records][usage]") {
  require_payload(
      domain::UsageRecorded{id<domain::InferenceId>("inference"),
                            {17, 9, 3, 2}},
      R"({"cached_input_tokens":3,"inference_id":"inference","input_tokens":17,"kind":"usage","output_tokens":9,"reasoning_tokens":2})");
  const auto amount = domain::DecimalAmount::from("0.000123");
  REQUIRE(amount);
  const auto money = domain::MonetaryAmount::create("USD", *amount);
  REQUIRE(money);
  const auto reported = domain::ReportedCost::create({*money});
  REQUIRE(reported);
  require_payload(
      domain::InferenceCostRecorded{id<domain::InferenceId>("inference"),
                                    *reported},
      R"({"amounts":[{"amount":"0.000123","unit":"USD"}],"inference_id":"inference","kind":"cost"})");
  const auto escaped = encode(
      domain::RunFailed{{domain::ErrorCode::backend, "quote\"\nnext", false}});
  REQUIRE(escaped);
  CHECK(escaped->ends_with("\n"));
  CHECK(std::ranges::count(*escaped, '\n') == 1);
  CHECK(escaped->find("quote\\\"\\nnext") != std::string::npos);
}
