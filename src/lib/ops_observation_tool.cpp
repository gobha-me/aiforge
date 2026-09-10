#include <aiforge/runtime/ops_observation_tool.hpp>

#include <aiforge/detail/utf8_text.hpp>
#include <aiforge/runtime/tool_policy.hpp>
#include <algorithm>
#include <array>
#include <exception>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>
#include <string_view>
#include <utility>

namespace aiforge::runtime {
namespace {
using Json = nlohmann::json;
using Operation = domain::OpsObservationOperation;
using Code = ToolExecutionErrorCode;
constexpr std::size_t maximum_argument_bytes{16384};
constexpr std::size_t maximum_result_bytes{std::size_t{256} * 1024};
constexpr std::array<std::string_view, 8> operations{
    "linux_health",       "linux_services",       "linux_service_health",
    "linux_service_logs", "kubernetes_workloads", "kubernetes_pod_health",
    "kubernetes_events",  "kubernetes_pod_logs"};
class InvalidArguments final : public std::exception {};
auto require(bool condition) -> void {
  if (!condition) throw InvalidArguments{};
}
auto error(Code code, std::string message)
    -> std::unexpected<ToolExecutionError> {
  return std::unexpected(ToolExecutionError{code, std::move(message), false});
}
auto fields(const Json& value, std::initializer_list<std::string_view> names)
    -> void {
  require(value.is_object() && value.size() == names.size());
  for (auto name : names)
    require(value.contains(name));
}
auto text(const Json& value, std::size_t maximum) -> std::string {
  require(value.is_string());
  auto result = value.get<std::string>();
  require(!result.empty() && result.size() <= maximum &&
          detail::is_safe_utf8_text(result));
  return result;
}
template <class Id> auto identity(const Json& value) -> Id {
  auto parsed = Id::from(text(value, 128));
  require(parsed.has_value());
  return std::move(*parsed);
}
template <class Integer> auto number(const Json& value) -> Integer {
  require(value.is_number_unsigned());
  const auto result = value.get<std::uint64_t>();
  require(std::in_range<Integer>(result));
  return static_cast<Integer>(result);
}
auto parse_json(const domain::StructuredDataBlock& arguments) -> Json {
  require(arguments.media_type == "application/json" &&
          !arguments.data.empty() &&
          arguments.data.size() <= maximum_argument_bytes);
  std::size_t events{};
  std::vector<std::set<std::string>> keys;
  const auto callback = [&](int depth, Json::parse_event_t event,
                            Json& parsed) {
    require(depth <= 16 && ++events <= 4096);
    if (event == Json::parse_event_t::object_start) keys.emplace_back();
    if (event == Json::parse_event_t::key) {
      require(!keys.empty());
      require(keys.back().insert(parsed.get<std::string>()).second);
    }
    if (event == Json::parse_event_t::object_end) {
      require(!keys.empty());
      keys.pop_back();
    }
    return true;
  };
  return Json::parse(arguments.data, callback, true, false);
}
auto resource(const Json& value) -> domain::OpsResourceIdentity {
  require(value.is_object() && value.contains("kind"));
  const auto kind = text(value.at("kind"), 32);
  if (kind == "none") {
    fields(value, {"kind"});
    return {};
  }
  if (kind == "linux_service") {
    fields(value, {"kind", "unit_name", "invocation_id"});
    return domain::LinuxServiceIdentity{
        text(value.at("unit_name"), 255),
        value.at("invocation_id").is_null()
            ? std::nullopt
            : std::optional{
                  identity<domain::OpsResourceUid>(value.at("invocation_id"))}};
  }
  require(kind == "kubernetes_pod");
  fields(value, {"kind", "namespace_name", "name", "uid", "container"});
  domain::KubernetesPodIdentity result{
      text(value.at("namespace_name"), 63),
      text(value.at("name"), 253),
      identity<domain::OpsResourceUid>(value.at("uid")),
      {}};
  if (!value.at("container").is_null()) {
    const auto& container = value.at("container");
    fields(container, {"name", "runtime_identity"});
    result.container = domain::KubernetesContainerIdentity{
        text(container.at("name"), 63),
        text(container.at("runtime_identity"), 512)};
  }
  return result;
}
auto resource_json(const domain::OpsResourceIdentity& value) -> Json {
  if (const auto* service = std::get_if<domain::LinuxServiceIdentity>(&value)) {
    return {{"kind", "linux_service"},
            {"unit_name", service->unit_name},
            {"invocation_id",
             service->invocation_id
                 ? Json(std::string(service->invocation_id->value()))
                 : Json(nullptr)}};
  }
  if (const auto* pod = std::get_if<domain::KubernetesPodIdentity>(&value)) {
    Json container = nullptr;
    if (pod->container)
      container = {{"name", pod->container->name},
                   {"runtime_identity", pod->container->runtime_identity}};
    return {{"kind", "kubernetes_pod"},
            {"namespace_name", pod->namespace_name},
            {"name", pod->name},
            {"uid", std::string(pod->uid.value())},
            {"container", std::move(container)}};
  }
  require(std::holds_alternative<std::monostate>(value));
  return {{"kind", "none"}};
}
auto parse_limits(const Json& value) -> domain::OpsObservationLimits {
  fields(value,
         {"maximum_entries", "maximum_bytes", "timeout_ms", "maximum_log_lines",
          "maximum_log_bytes", "maximum_log_age_seconds"});
  return {
      number<std::size_t>(value.at("maximum_entries")),
      number<std::size_t>(value.at("maximum_bytes")),
      std::chrono::milliseconds{number<std::int64_t>(value.at("timeout_ms"))},
      number<std::size_t>(value.at("maximum_log_lines")),
      number<std::size_t>(value.at("maximum_log_bytes")),
      std::chrono::seconds{
          number<std::int64_t>(value.at("maximum_log_age_seconds"))}};
}
auto limits_json(const domain::OpsObservationLimits& value) -> Json {
  return {{"maximum_entries", value.maximum_entries},
          {"maximum_bytes", value.maximum_bytes},
          {"timeout_ms", value.timeout.count()},
          {"maximum_log_lines", value.maximum_log_lines},
          {"maximum_log_bytes", value.maximum_log_bytes},
          {"maximum_log_age_seconds", value.maximum_log_age.count()}};
}
auto parse_intent(const domain::StructuredDataBlock& arguments,
                  const domain::OpsObservationAuthoritySpec& spec)
    -> OpsObservationIntent {
  const auto value = parse_json(arguments);
  require(value.is_object());
  if (value.contains("limits"))
    fields(value, {"target_id", "selection_generation", "operation", "resource",
                   "limits"});
  else
    fields(value,
           {"target_id", "selection_generation", "operation", "resource"});
  const auto name = text(value.at("operation"), 64);
  const auto found = std::ranges::find(operations, name);
  require(found != operations.end());
  const auto operation = static_cast<Operation>(found - operations.begin());
  require(std::ranges::find(spec.operations, operation) !=
          spec.operations.end());
  return {identity<domain::OpsTargetId>(value.at("target_id")),
          number<std::uint64_t>(value.at("selection_generation")), operation,
          resource(value.at("resource")),
          value.contains("limits") ? parse_limits(value.at("limits"))
                                   : spec.limits};
}
auto intent_json(const OpsObservationIntent& value)
    -> domain::StructuredDataBlock {
  const auto index = static_cast<std::size_t>(value.operation);
  require(index < operations.size());
  Json encoded{{"target_id", std::string(value.target_id.value())},
               {"selection_generation", value.selection_generation},
               {"operation", operations[index]},
               {"resource", resource_json(value.resource)},
               {"limits", limits_json(value.limits)}};
  auto result = encoded.dump();
  require(result.size() <= maximum_argument_bytes);
  return {"application/json", std::move(result)};
}
auto effects(Operation operation) -> std::vector<domain::Effect> {
  if (operation == Operation::linux_health) return {domain::Effect::read};
  if (operation <= Operation::linux_service_logs)
    return {domain::Effect::read, domain::Effect::execute};
  return {domain::Effect::read, domain::Effect::execute,
          domain::Effect::network};
}
auto scopes(const domain::OpsTargetId& target,
            const std::vector<domain::Effect>& effects)
    -> std::vector<domain::CapabilityScope> {
  std::vector<domain::CapabilityScope> result;
  result.reserve(effects.size());
  for (auto effect : effects)
    result.push_back({effect, "ops.target", std::string(target.value())});
  return result;
}
auto normalized(const OpsObservationIntent& intent) -> ValidatedToolArguments {
  auto required_effects = effects(intent.operation);
  return {intent_json(intent), scopes(intent.target_id, required_effects),
          std::move(required_effects)};
}
auto valid_intent(const OpsObservationIntent& intent,
                  const domain::OpsObservationAuthoritySpec& spec) -> bool {
  if (intent.target_id != spec.target.target_id ||
      intent.selection_generation != spec.selection_generation ||
      std::ranges::find(spec.operations, intent.operation) ==
          spec.operations.end() ||
      !domain::validate_ops_observation_limits(intent.limits))
    return false;
  const auto& limit = intent.limits;
  if (limit.maximum_entries > spec.limits.maximum_entries ||
      limit.maximum_bytes > spec.limits.maximum_bytes ||
      limit.timeout > spec.limits.timeout ||
      limit.maximum_log_lines > spec.limits.maximum_log_lines ||
      limit.maximum_log_bytes > spec.limits.maximum_log_bytes ||
      limit.maximum_log_age > spec.limits.maximum_log_age)
    return false;
  const bool logs = intent.operation == Operation::linux_service_logs ||
                    intent.operation == Operation::kubernetes_pod_logs;
  // Check borrowed resource shape/size before copying any typed human input.
  if (!std::holds_alternative<std::monostate>(intent.resource) &&
      !domain::validate_ops_resource_identity(spec.target, intent.resource,
                                              logs))
    return false;
  if (logs &&
      (!spec.logs.enabled ||
       std::ranges::find(spec.logs.permitted_sources, intent.resource) ==
           spec.logs.permitted_sources.end()))
    return false;
  switch (intent.operation) {
    case Operation::linux_health:
    case Operation::linux_services:
    case Operation::kubernetes_workloads:
      return std::holds_alternative<std::monostate>(intent.resource);
    case Operation::linux_service_health:
    case Operation::linux_service_logs:
      return std::holds_alternative<domain::LinuxServiceIdentity>(
          intent.resource);
    case Operation::kubernetes_pod_health:
    case Operation::kubernetes_pod_logs:
      return std::holds_alternative<domain::KubernetesPodIdentity>(
          intent.resource);
    case Operation::kubernetes_events:
      return std::holds_alternative<std::monostate>(intent.resource) ||
             std::holds_alternative<domain::KubernetesPodIdentity>(
                 intent.resource);
  }
  return false;
}
auto broker_error(OpsBrokerFailure failure)
    -> std::unexpected<ToolExecutionError> {
  if (failure.code == OpsBrokerError::cancelled)
    return error(Code::cancelled, "observation cancelled");
  if (failure.code == OpsBrokerError::timed_out)
    return error(Code::timed_out, "observation deadline expired");
  if (failure.code == OpsBrokerError::invalid_request)
    return error(Code::invalid_arguments, "observation request is invalid");
  if (failure.code == OpsBrokerError::internal_failure)
    return error(Code::internal_failure, "observation failed internally");
  return error(Code::unavailable,
               "observation source or current authority is unavailable");
}
class ObservationStream final : public ToolExecutionStream {
 public:
  ObservationStream(std::shared_ptr<OpsObservationEndpoint> endpoint,
                    domain::InvocationId invocation,
                    domain::OpsObservationRequest request,
                    std::chrono::steady_clock::time_point deadline)
      : m_endpoint(std::move(endpoint)), m_invocation(std::move(invocation)),
        m_request(std::move(request)), m_deadline(deadline) {}
  auto next(std::stop_token stop)
      -> std::expected<std::optional<ToolExecutionEvent>,
                       ToolExecutionError> override {
    if (m_done) return std::optional<ToolExecutionEvent>{};
    m_done = true;
    auto receipt =
        m_endpoint->observe(m_invocation, m_request, m_deadline, stop);
    if (!receipt) return broker_error(receipt.error());
    return std::optional<ToolExecutionEvent>{
        OpsObservationReady{std::move(*receipt)}};
  }

 private:
  std::shared_ptr<OpsObservationEndpoint> m_endpoint;
  domain::InvocationId m_invocation;
  domain::OpsObservationRequest m_request;
  std::chrono::steady_clock::time_point m_deadline;
  bool m_done{};
};

auto object_schema(Json properties) -> Json {
  Json required = Json::array();
  for (const auto& [key, unused] : properties.items()) {
    static_cast<void>(unused);
    required.push_back(key);
  }
  return {{"type", "object"},
          {"additionalProperties", false},
          {"required", std::move(required)},
          {"properties", std::move(properties)}};
}
auto string_schema(std::size_t maximum) -> Json {
  return {{"type", "string"}, {"minLength", 1}, {"maxLength", maximum}};
}
auto nullable(Json schema) -> Json {
  return {{"anyOf", Json::array({std::move(schema), Json{{"type", "null"}}})}};
}
auto resource_schema() -> Json {
  auto none = object_schema({{"kind", {{"const", "none"}}}});
  auto service =
      object_schema({{"kind", {{"const", "linux_service"}}},
                     {"unit_name", string_schema(255)},
                     {"invocation_id", nullable(string_schema(128))}});
  auto container = object_schema(
      {{"name", string_schema(63)}, {"runtime_identity", string_schema(512)}});
  auto pod = object_schema({{"kind", {{"const", "kubernetes_pod"}}},
                            {"namespace_name", string_schema(63)},
                            {"name", string_schema(253)},
                            {"uid", string_schema(128)},
                            {"container", nullable(std::move(container))}});
  return {{"oneOf",
           Json::array({std::move(none), std::move(service), std::move(pod)})}};
}
auto declaration(const domain::OpsObservationAuthoritySpec& spec)
    -> backend::ToolDeclaration {
  Json permitted = Json::array();
  std::vector<domain::Effect> declared_effects;
  for (const auto operation : spec.operations) {
    if (!spec.logs.enabled && (operation == Operation::linux_service_logs ||
                               operation == Operation::kubernetes_pod_logs))
      continue;
    const auto index = static_cast<std::size_t>(operation);
    require(index < operations.size());
    permitted.push_back(operations[index]);
    for (auto effect : effects(operation))
      if (std::ranges::find(declared_effects, effect) == declared_effects.end())
        declared_effects.push_back(effect);
  }
  require(!permitted.empty());
  auto schema = object_schema(
      {{"target_id", {{"const", std::string(spec.target.target_id.value())}}},
       {"selection_generation", {{"const", spec.selection_generation}}},
       {"operation", {{"type", "string"}, {"enum", std::move(permitted)}}},
       {"resource", resource_schema()}});
  Json limit_properties = Json::object();
  const auto ceilings = limits_json(spec.limits);
  for (const auto& [name, ceiling] : ceilings.items())
    limit_properties[name] = {
        {"type", "integer"}, {"minimum", 1}, {"maximum", ceiling}};
  schema["properties"]["limits"] = object_schema(std::move(limit_properties));
  auto declared_scopes = scopes(spec.target.target_id, declared_effects);
  return {"observe_target",
          "Collect one bounded observation from the selected target. Use "
          "resource kind none for health, "
          "service/workload lists or unfiltered Kubernetes events; exact "
          "service or pod identity for "
          "inspection and logs. Logs require enabled selected-source consent "
          "and actual service "
          "invocation or pod/container runtime identity. Optional limits may "
          "only narrow the displayed "
          "ceilings. Target changes invalidate this registration; no refresh "
          "occurs during replay.",
          {"application/schema+json", schema.dump()},
          std::move(declared_effects),
          std::move(declared_scopes)};
}
} // namespace

OpsObservationTool::OpsObservationTool(
    domain::OpsObservationAuthority authority,
    std::shared_ptr<OpsObservationEndpoint> endpoint)
    : m_authority(std::move(authority)), m_endpoint(std::move(endpoint)) {
}

auto OpsObservationTool::validate(const domain::StructuredDataBlock& arguments)
    const -> std::expected<ValidatedToolArguments, ToolExecutionError> {
  try {
    // Validation alone cannot manufacture invocation-bound proof or dispatch.
    const auto intent = parse_intent(arguments, m_authority.specification());
    require(valid_intent(intent, m_authority.specification()));
    return normalized(intent);
  } catch (...) {
    return error(Code::invalid_arguments, "observation arguments are invalid");
  }
}
auto OpsObservationTool::prepare(const domain::InvocationId& invocation,
                                 const domain::StructuredDataBlock& arguments)
    const -> std::expected<ValidatedToolArguments, ToolExecutionError> {
  try {
    return prepare(invocation,
                   parse_intent(arguments, m_authority.specification()));
  } catch (...) {
    return error(Code::invalid_arguments, "observation arguments are invalid");
  }
}
auto OpsObservationTool::prepare(const domain::InvocationId& invocation,
                                 const OpsObservationIntent& intent) const
    -> std::expected<ValidatedToolArguments, ToolExecutionError> {
  try {
    const auto& spec = m_authority.specification();
    if (!valid_intent(intent, spec))
      return error(Code::invalid_arguments,
                   "observation intent exceeds its frozen target authority");
    const auto request_id =
        domain::OpsRequestId::from(std::string(invocation.value()));
    if (!request_id || intent.target_id != spec.target.target_id)
      return error(Code::invalid_arguments, "observation identity is invalid");
    domain::OpsObservationRequest request{spec.owner_id,
                                          spec.session_id,
                                          *request_id,
                                          spec.target,
                                          intent.selection_generation,
                                          intent.operation,
                                          intent.resource,
                                          spec.logs.revision,
                                          intent.limits};
    if (!m_authority.validate(request))
      return error(Code::invalid_arguments,
                   "observation exceeds its frozen target authority");
    auto result = normalized(intent);
    result.observation_request = std::move(request);
    return result;
  } catch (...) {
    return error(Code::invalid_arguments, "observation arguments are invalid");
  }
}
auto OpsObservationTool::check_current(
    const OpsObservationBroker& broker, const domain::InvocationId& invocation,
    const ValidatedToolArguments& arguments) const
    -> std::expected<void, ToolExecutionError> {
  try {
    const auto prepared = prepare(invocation, arguments.value);
    if (!prepared || *prepared != arguments || !arguments.observation_request)
      return error(Code::invalid_arguments,
                   "observation invocation proof is invalid");
    if (!broker.preflight(*m_endpoint, *arguments.observation_request))
      return error(Code::unavailable,
                   "observation target or session is no longer current");
    return {};
  } catch (...) {
    return error(Code::internal_failure,
                 "observation preflight failed internally");
  }
}

auto OpsObservationTool::start(ToolInvocation invocation, std::stop_token stop)
    -> std::expected<std::unique_ptr<ToolExecutionStream>, ToolExecutionError> {
  try {
    if (stop.stop_requested())
      return error(Code::cancelled, "observation cancelled");
    const auto prepared =
        prepare(invocation.invocation_id, invocation.arguments.value);
    if (!prepared || *prepared != invocation.arguments ||
        invocation.parent_invocation_id ||
        invocation.tool_name != "observe_target" ||
        invocation.arguments.spend_quote ||
        invocation.limits.timeout <= std::chrono::milliseconds::zero() ||
        invocation.limits.output_bytes == 0 ||
        invocation.limits.output_bytes > maximum_result_bytes)
      return error(Code::invalid_arguments,
                   "observation invocation proof is invalid");
    const auto& required = prepared->required_scopes;
    if (invocation.granted_scopes.size() != required.size() ||
        !std::ranges::all_of(required, [&](const auto& scope) {
          return std::ranges::find(invocation.granted_scopes, scope) !=
                 invocation.granted_scopes.end();
        }))
      return error(Code::invalid_arguments,
                   "observation invocation scopes are invalid");
    const auto& request = prepared->observation_request;
    if (!request)
      return error(Code::invalid_arguments,
                   "observation invocation proof is missing");
    const auto timeout =
        std::min(invocation.limits.timeout, request->limits.timeout);
    return std::unique_ptr<ToolExecutionStream>{
        new ObservationStream{m_endpoint, invocation.invocation_id, *request,
                              std::chrono::steady_clock::now() + timeout}};
  } catch (...) {
    return error(Code::internal_failure,
                 "observation execution failed internally");
  }
}

auto register_ops_observation_tool(
    ToolRegistry& registry, domain::OpsObservationAuthority authority,
    std::shared_ptr<OpsObservationEndpoint> endpoint)
    -> std::expected<void, ToolRegistryError> {
  try {
    if (!endpoint)
      return std::unexpected(
          ToolRegistryError{ToolRegistryErrorCode::invalid_declaration,
                            "observation endpoint is unavailable"});
    auto tool = declaration(authority.specification());
    const auto timeout = authority.specification().limits.timeout;
    return registry.register_tool(
        std::move(tool),
        std::shared_ptr<ToolExecutor>{
            new OpsObservationTool{std::move(authority), std::move(endpoint)}},
        {maximum_result_bytes, 1, timeout},
        ToolExecutorContract{"aiforge.ops-observation", "1"});
  } catch (...) {
    return std::unexpected(
        ToolRegistryError{ToolRegistryErrorCode::invalid_declaration,
                          "observation registration is invalid"});
  }
}
} // namespace aiforge::runtime
