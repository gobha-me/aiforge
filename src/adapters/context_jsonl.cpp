#include <aiforge/adapters/context_jsonl.hpp>
#include <aiforge/surfaces/agent.hpp>
#include <algorithm>
#include <exception>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>
#include <utility>

namespace aiforge::adapters {
namespace {
using Json = nlohmann::json;
using namespace surfaces;
using namespace domain;
class Invalid final : public std::exception {};
template <class... T> struct Overloaded : T... {
  using T::operator()...;
};
auto exact(const Json& value, std::initializer_list<std::string_view> required,
           std::initializer_list<std::string_view> optional = {}) -> void {
  if (!value.is_object()) throw Invalid{};
  for (const auto key : required)
    if (!value.contains(key)) throw Invalid{};
  for (auto it = value.begin(); it != value.end(); ++it)
    if (std::ranges::find(required, it.key()) == required.end() &&
        std::ranges::find(optional, it.key()) == optional.end())
      throw Invalid{};
}
auto number(const Json& value) -> std::uint64_t {
  if (!value.is_number_unsigned()) throw Invalid{};
  return value.get<std::uint64_t>();
}
auto text(const Json& value, std::size_t maximum) -> std::string {
  if (!value.is_string() ||
      value.get_ref<const std::string&>().size() > maximum)
    throw Invalid{};
  return value.get<std::string>();
}
template <class Id> auto identifier(const Json& value) -> Id {
  auto parsed = Id::from(text(value, 128));
  if (!parsed) throw Invalid{};
  return std::move(*parsed);
}
auto digest(const Json& value) -> ContentDigest {
  exact(value, {"algorithm", "value", "byte_size"});
  auto result =
      ContentDigest{text(value.at("algorithm"), 8), text(value.at("value"), 64),
                    number(value.at("byte_size"))};
  if (result.algorithm != "sha256" || result.value.size() != 64 ||
      result.byte_size == 0 ||
      result.byte_size > summary_maximum_manifest_bytes ||
      !std::ranges::all_of(result.value, [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
      }))
    throw Invalid{};
  return result;
}
auto version(const Json& value) -> ConversationSummaryVersion {
  exact(value, {"summary_id", "revision", "candidate_digest"});
  auto revision = number(value.at("revision"));
  if (revision == 0) throw Invalid{};
  return {identifier<ConversationSummaryId>(value.at("summary_id")), revision,
          digest(value.at("candidate_digest"))};
}
auto runs(const Json& value, std::size_t maximum) -> std::vector<RunId> {
  if (!value.is_array() || value.size() > maximum) throw Invalid{};
  std::vector<RunId> result;
  std::set<RunId> unique;
  for (const auto& entry : value) {
    auto id = identifier<RunId>(entry);
    if (!unique.insert(id).second) throw Invalid{};
    result.push_back(std::move(id));
  }
  return result;
}
auto versions(const Json& value) -> std::vector<ConversationSummaryVersion> {
  if (!value.is_array() || value.size() > summary_maximum_active)
    throw Invalid{};
  std::vector<ConversationSummaryVersion> result;
  for (const auto& entry : value)
    result.push_back(version(entry));
  return result;
}
struct RequestedPage {
  std::size_t offset;
  std::size_t limit;
};
auto requested_page(const Json& value, std::size_t default_limit)
    -> RequestedPage {
  const auto offset = value.contains("offset") ? number(value.at("offset")) : 0;
  const auto limit =
      value.contains("limit") ? number(value.at("limit")) : default_limit;
  if (offset > std::numeric_limits<std::size_t>::max() || limit == 0 ||
      limit > context_maximum_page_size)
    throw Invalid{};
  return {static_cast<std::size_t>(offset), static_cast<std::size_t>(limit)};
}
auto inspect_request(const Json& value) -> ContextOperation {
  exact(value, {"version", "id", "op"}, {"draft", "offset", "limit"});
  const auto page = requested_page(value, 8);
  return ContextInspect{value.contains("draft")
                            ? text(value.at("draft"), agent_maximum_input_bytes)
                            : "",
                        page.offset, page.limit};
}
auto generate_request(const Json& value) -> ContextOperation {
  exact(value, {"version", "id", "op", "expected_sequence", "runs"},
        {"maximum_output_bytes"});
  const auto maximum = value.contains("maximum_output_bytes")
                           ? number(value.at("maximum_output_bytes"))
                           : summary_maximum_text_bytes;
  if (maximum == 0 || maximum > summary_maximum_text_bytes) throw Invalid{};
  auto selected = runs(value.at("runs"), summary_maximum_groups);
  if (selected.empty()) throw Invalid{};
  return ChatSummaryGenerate{number(value.at("expected_sequence")),
                             std::move(selected),
                             static_cast<std::size_t>(maximum)};
}
auto request_operation(const Json& value, const std::string& op)
    -> ContextOperation {
  if (op == "inspect") return inspect_request(value);
  if (op == "policy") {
    exact(value, {"version", "id", "op", "expected_revision", "mode", "pins"});
    const auto mode = text(value.at("mode"), 7);
    if (mode != "full" && mode != "rolling") throw Invalid{};
    return ContextPolicy{number(value.at("expected_revision")),
                         mode == "full" ? ConversationMode::full
                                        : ConversationMode::rolling,
                         runs(value.at("pins"), conversation_maximum_pins)};
  }
  if (op == "summary.generate") return generate_request(value);
  if (op == "summary.publish") {
    exact(value, {"version", "id", "op", "summary_id"});
    return ContextPublish{
        identifier<ConversationSummaryId>(value.at("summary_id"))};
  }
  if (op == "summary.edit") {
    exact(value,
          {"version", "id", "op", "expected_sequence", "parent", "text"});
    return ContextEdit{number(value.at("expected_sequence")),
                       version(value.at("parent")),
                       text(value.at("text"), summary_maximum_text_bytes)};
  }
  if (op == "summary.preview") {
    exact(value, {"version", "id", "op", "candidate", "replacements", "draft"});
    return ContextPreview{version(value.at("candidate")),
                          versions(value.at("replacements")),
                          text(value.at("draft"), agent_maximum_input_bytes)};
  }
  if (op == "summary.preview.page") {
    exact(value, {"version", "id", "op", "handle"}, {"offset", "limit"});
    const auto page = requested_page(value, context_maximum_page_size);
    return ContextPreviewPage{text(value.at("handle"), 128), page.offset,
                              page.limit};
  }
  if (op == "summary.apply") {
    exact(value, {"version", "id", "op", "handle", "draft"});
    return ContextApply{text(value.at("handle"), 128),
                        text(value.at("draft"), agent_maximum_input_bytes)};
  }
  if (op == "summary.disable") {
    exact(value, {"version", "id", "op", "expected_revision", "candidate",
                  "activation_event_id"});
    return ContextDisable{number(value.at("expected_revision")),
                          version(value.at("candidate")),
                          identifier<EventId>(value.at("activation_event_id"))};
  }
  if (op == "close") {
    exact(value, {"version", "id", "op"});
    return ContextClose{};
  }
  throw Invalid{};
}
auto digest_json(const ContentDigest& value) -> Json {
  return {{"algorithm", value.algorithm},
          {"value", value.value},
          {"byte_size", value.byte_size}};
}
auto version_json(const ConversationSummaryVersion& value) -> Json {
  return {{"summary_id", value.summary_id.value()},
          {"revision", value.revision},
          {"candidate_digest", digest_json(value.candidate_digest)}};
}
auto activation_json(const ConversationSummaryActivation& value) -> Json {
  auto covered = Json::array();
  for (const auto& run : value.covered_run_ids)
    covered.push_back(run.value());
  return {{"candidate", version_json(value.candidate)},
          {"activation_event_id", value.activation_event_id.value()},
          {"activation_sequence", value.activation_sequence},
          {"source_anchor_sequence", value.source_anchor_sequence},
          {"source_digest", digest_json(value.source_digest)},
          {"covered_run_ids", std::move(covered)}};
}
auto candidate_json(const ConversationSummaryCandidate& value) -> Json {
  if (!value.candidate_digest) throw Invalid{};
  Json result{{"candidate", version_json({value.summary_id, value.revision,
                                          *value.candidate_digest})},
              {"text", value.text},
              {"source_digest", digest_json(value.source_digest)},
              {"intent_digest", digest_json(value.intent_digest)},
              {"output_event_id", value.output_event_id.value()},
              {"output_sequence", value.output_sequence},
              {"created_event_id", value.created_event_id.value()},
              {"created_sequence", value.created_sequence},
              {"author", value.author == ConversationSummaryAuthor::model
                             ? "model"
                             : "user_edit"}};
  if (value.edited_from)
    result["edited_from"] = version_json(*value.edited_from);
  return result;
}
auto intent_json(const ConversationSummaryIntent& intent) -> Json {
  auto covered = Json::array();
  for (const auto& group : intent.sources.groups)
    covered.push_back(group.run_id.value());
  return {{"producing_run_id", intent.producing_run_id.value()},
          {"producing_inference_id", intent.producing_inference_id.value()},
          {"model_id", intent.model_id.value()},
          {"output_message_id", intent.output_message_id.value()},
          {"source_snapshot_sequence", intent.sources.snapshot_sequence},
          {"covered_run_ids", std::move(covered)}};
}
auto inspected_candidate(const ConversationSummaryCandidate& candidate,
                         const std::vector<ConversationSummaryIntent>& intents)
    -> Json {
  auto result = candidate_json(candidate);
  const auto found = std::ranges::find(intents, candidate.summary_id,
                                       &ConversationSummaryIntent::summary_id);
  if (found == intents.end()) throw Invalid{};
  result["producer"] = intent_json(*found);
  return result;
}
auto capacity_json(const ContextCapacity& value) -> Json {
  return {{"context_window_tokens", value.context_window_tokens},
          {"reserved_input_tokens", value.reserved_input_tokens},
          {"reserved_output_tokens", value.reserved_output_tokens}};
}
auto kind_name(ContextEntryKind kind) -> std::string_view {
  switch (kind) {
    case ContextEntryKind::instruction: return "instruction";
    case ContextEntryKind::conversation: return "conversation";
    case ContextEntryKind::evidence: return "evidence";
    case ContextEntryKind::tool_result: return "tool_result";
  }
  throw Invalid{};
}
auto status_name(RunStatus status) -> std::string_view {
  switch (status) {
    case RunStatus::completed: return "completed";
    case RunStatus::failed: return "failed";
    case RunStatus::cancelled: return "cancelled";
    default: throw Invalid{};
  }
}
struct Page {
  Json items{Json::array()};
  std::size_t next{};
  std::size_t total{};
};
template <class Render>
auto page(std::size_t total, std::size_t offset, std::size_t limit,
          std::size_t budget, Render render) -> Page {
  Page result{Json::array(), std::min(offset, total), total};
  const auto end = result.next + std::min(limit, total - result.next);
  while (result.next < end) {
    auto row = render(result.next);
    const auto bytes = row.dump().size() + 1;
    if (bytes > budget) {
      if (result.items.empty()) throw Invalid{};
      break;
    }
    budget -= bytes;
    result.items.push_back(std::move(row));
    ++result.next;
  }
  return result;
}
auto add_page(Json& output, std::string_view name, Page value) -> void {
  const std::string key{name};
  output[key] = std::move(value.items);
  output[key + "_count"] = value.total;
  output[key + "_next_offset"] =
      value.next < value.total ? Json(value.next) : Json(nullptr);
}
auto context_json(const ConstructedContext& value, std::size_t offset = 0,
                  std::size_t limit = context_maximum_page_size) -> Json {
  auto entries =
      page(value.entries.size(), offset, limit, std::size_t{32} * 1024,
           [&](std::size_t index) -> Json {
             const auto& entry = value.entries[index];
             return {{"entry_id", entry.entry_id.value()},
                     {"message_id", entry.message.message_id.value()},
                     {"order", entry.order},
                     {"estimated_tokens", entry.estimated_tokens},
                     {"kind", kind_name(entry.kind)}};
           });
  Json result{{"capacity", capacity_json(value.capacity)},
              {"estimated_input_tokens", value.estimated_input_tokens},
              {"offset", offset},
              {"limit", limit}};
  add_page(result, "entries", std::move(entries));
  return result;
}
auto decision_name(runtime::ConversationSelectionDecision decision)
    -> std::string_view {
  using enum runtime::ConversationSelectionDecision;
  switch (decision) {
    case admitted_full: return "admitted_full";
    case admitted_pin: return "admitted_pin";
    case admitted_recent: return "admitted_recent";
    case omitted_capacity: return "omitted_capacity";
    case omitted_older: return "omitted_older";
    case omitted_summary: return "omitted_summary";
  }
  throw Invalid{};
}
auto inspection_group(const ChatConversationGroupInspection& group) -> Json {
  return {{"run_id", group.run_id.value()},
          {"entries", group.entry_count},
          {"estimated_tokens", group.estimated_tokens},
          {"pinned", group.pinned},
          {"decision", group.decision ? Json(decision_name(*group.decision))
                                      : Json(nullptr)}};
}
auto inspection_summary(const ChatSummaryCatalog& catalog, std::size_t index)
    -> Json {
  if (index < catalog.snapshot.candidates.size())
    return inspected_candidate(catalog.snapshot.candidates[index],
                               catalog.snapshot.intents);
  index -= catalog.snapshot.candidates.size();
  if (index < catalog.unpublished.size()) {
    const auto& draft = catalog.unpublished[index];
    return {{"summary_id", draft.intent.summary_id.value()},
            {"unpublished", true},
            {"text", draft.text},
            {"producer", intent_json(draft.intent)}};
  }
  const auto& failure =
      catalog.unpublishable[index - catalog.unpublished.size()];
  return {{"summary_id", failure.summary_id.value()},
          {"unpublishable", true},
          {"message", failure.message}};
}
auto inspect_json(const ContextInspected& value) -> Json {
  const auto& pins = value.context.policy.policy.pinned_run_ids;
  const auto& active = value.catalog.snapshot.active;
  const auto total = value.catalog.snapshot.candidates.size() +
                     value.catalog.unpublished.size() +
                     value.catalog.unpublishable.size();
  Json result{
      {"type", "inspection"},
      {"model", value.context.model_id.value()},
      {"policy_revision", value.context.policy.policy.revision},
      {"mode", value.context.policy.policy.mode == ConversationMode::full
                   ? "full"
                   : "rolling"},
      {"capacity", capacity_json(value.context.mandatory.capacity)},
      {"offset", value.offset},
      {"limit", value.limit}};
  add_page(result, "pins",
           page(pins.size(), value.offset, value.limit, std::size_t{32} * 1024,
                [&](std::size_t i) -> Json { return pins[i].value(); }));
  add_page(result, "groups",
           page(value.context.groups.size(), value.offset, value.limit,
                std::size_t{32} * 1024, [&](std::size_t i) {
                  return inspection_group(value.context.groups[i]);
                }));
  add_page(result, "summaries",
           page(total, value.offset, value.limit, std::size_t{1024} * 1024,
                [&](std::size_t i) {
                  return inspection_summary(value.catalog, i);
                }));
  add_page(result, "active",
           page(active.size(), value.offset, value.limit,
                std::size_t{512} * 1024,
                [&](std::size_t i) { return activation_json(active[i]); }));
  if (value.context.next_context)
    result["next_context"] =
        context_json(*value.context.next_context, value.offset, value.limit);
  if (value.context.preparation_error)
    result["preparation_error"] = value.context.preparation_error->message;
  return result;
}
auto payload_json(const ContextPayload& payload) -> Json {
  return std::visit(
      Overloaded{
          [](const ContextOk& v) -> Json {
            return {{"type", v.closed ? "closed" : "ok"}};
          },
          [](const ContextInspected& v) { return inspect_json(v); },
          [](const ContextGenerated& v) -> Json {
            return {{"type", "generation.started"},
                    {"summary_id", v.summary_id.value()},
                    {"run_id", v.run_id.value()}};
          },
          [](const ConversationSummaryCandidate& v) {
            auto j = candidate_json(v);
            j["type"] = "candidate";
            return j;
          },
          [](const ContextPreviewed& v) -> Json {
            return {{"type", "preview"},
                    {"handle", v.handle},
                    {"activation", activation_json(v.activation)},
                    {"context", context_json(v.context, v.offset, v.limit)}};
          },
          [](const ConversationSummaryActivation& v) {
            auto j = activation_json(v);
            j["type"] = "activation";
            return j;
          },
          [](const ContextGenerationFinished& v) -> Json {
            return {{"type", "generation.finished"},
                    {"summary_id", v.summary_id.value()},
                    {"run_id", v.run_id.value()},
                    {"status", status_name(v.status)}};
          },
          [](const ContextFailure& v) -> Json {
            return {{"type", "error"},
                    {"code", v.code},
                    {"message", v.message},
                    {"effect_may_have_applied", v.effect_may_have_applied}};
          }},
      payload);
}
} // namespace
auto parse_context_request(std::string_view line)
    -> std::expected<ContextRequest, ContextFailure> {
  try {
    if (line.empty() || line.size() > agent_maximum_input_bytes)
      throw Invalid{};
    std::vector<std::set<std::string>> keys;
    std::size_t nodes{};
    auto callback = [&](int depth, Json::parse_event_t event, Json& value) {
      if (depth > 12 || ++nodes > 16384) throw Invalid{};
      if (event == Json::parse_event_t::object_start) keys.emplace_back();
      if (event == Json::parse_event_t::key &&
          (keys.empty() ||
           !keys.back().insert(value.get<std::string>()).second))
        throw Invalid{};
      if (event == Json::parse_event_t::object_end) keys.pop_back();
      return true;
    };
    const auto value = Json::parse(line, callback);
    if (!value.is_object() || number(value.at("version")) != 1) throw Invalid{};
    const auto id = number(value.at("id"));
    if (id == 0) throw Invalid{};
    return ContextRequest{id,
                          request_operation(value, text(value.at("op"), 32))};
  } catch (...) {
    return std::unexpected(
        ContextFailure{"invalid_request",
                       "invalid or oversized context protocol request", false});
  }
}
auto encode_context_reply(const ContextReply& reply)
    -> std::expected<std::string, ContextFailure> {
  try {
    auto value = payload_json(reply.payload);
    value["version"] = 1;
    value["id"] = reply.id;
    value["sequence"] = reply.sequence;
    auto output = value.dump() + '\n';
    if (output.size() > agent_maximum_record_bytes) throw Invalid{};
    return output;
  } catch (...) {
    return std::unexpected(ContextFailure{
        "output_failed", "context reply exceeds its output contract", false});
  }
}
} // namespace aiforge::adapters
