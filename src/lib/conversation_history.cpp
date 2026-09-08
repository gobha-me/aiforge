#include <aiforge/runtime/conversation_history.hpp>

#include <aiforge/runtime/tool_registry.hpp>

#include <algorithm>
#include <concepts>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>

namespace aiforge::runtime {
namespace {

using namespace domain;
using Code = ConversationHistoryErrorCode;
using Status = std::expected<void, ConversationHistoryError>;

auto failure(Code code, std::string message,
             std::optional<RunId> run_id = std::nullopt)
    -> std::unexpected<ConversationHistoryError> {
  return std::unexpected(
      ConversationHistoryError{code, std::move(message), std::move(run_id)});
}

struct ContentBudget {
  const ConversationHistoryLimits& limits;
  std::stop_token stop;
  std::size_t bytes{};
  std::size_t items{};
  std::uint64_t tokens{};

  auto add_tokens(std::uint64_t size) -> Status {
    if (size > std::numeric_limits<std::uint64_t>::max() - tokens)
      return failure(Code::token_overflow, "conversation estimate overflowed");
    tokens += size;
    return {};
  }

  auto add_bytes(std::size_t size) -> Status {
    if (stop.stop_requested())
      return failure(Code::cancelled, "history cancelled");
    if (size > limits.maximum_content_bytes - bytes)
      return failure(Code::resource_exhausted,
                     "conversation content byte limit exceeded");
    bytes += size;
    return add_tokens(size);
  }

  auto add_item(std::uint64_t envelope = 8) -> Status {
    if (stop.stop_requested())
      return failure(Code::cancelled, "history cancelled");
    if (items == limits.maximum_content_items)
      return failure(Code::resource_exhausted,
                     "conversation content item limit exceeded");
    ++items;
    return add_tokens(envelope);
  }
};

auto count_block(const ContentBlock& block, ContentBudget& budget) -> Status {
  auto valid = budget.add_item();
  if (!valid) return valid;
  return std::visit(
      [&budget](const auto& value) -> Status {
        using T = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::same_as<T, TextBlock>) {
          return budget.add_bytes(value.text.size());
        } else if constexpr (std::same_as<T, StructuredDataBlock>) {
          auto valid = budget.add_bytes(value.media_type.size());
          if (!valid) return valid;
          return budget.add_bytes(value.data.size());
        } else if constexpr (std::same_as<T, CitationBlock>) {
          auto valid = budget.add_bytes(value.uri.size());
          if (!valid || !value.title) return valid;
          return budget.add_bytes(value.title->size());
        } else if constexpr (std::same_as<T, ArtifactReferenceBlock>) {
          auto valid = budget.add_bytes(value.artifact_id.value().size());
          if (!valid || !value.label) return valid;
          return budget.add_bytes(value.label->size());
        } else {
          return failure(Code::unsupported_content,
                         "unsupported conversation content");
        }
      },
      block);
}

auto count_call(const InvocationId& invocation, const std::string& name,
                const StructuredDataBlock& arguments, ContentBudget& budget)
    -> Status {
  auto valid = budget.add_item(16);
  if (!valid) return valid;
  for (const auto size : {invocation.value().size(), name.size(),
                          arguments.media_type.size(), arguments.data.size()}) {
    valid = budget.add_bytes(size);
    if (!valid) return valid;
  }
  return {};
}

auto count_message(const Message& message, ContentBudget& budget) -> Status {
  auto valid = budget.add_tokens(16);
  if (!valid) return valid;
  if (message.invocation_id) {
    valid = budget.add_bytes(message.invocation_id->value().size());
    if (!valid) return valid;
  }
  for (const auto& block : message.content) {
    valid = count_block(block, budget);
    if (!valid) return valid;
  }
  for (const auto& call : message.tool_calls) {
    valid =
        count_call(call.invocation_id, call.tool_name, call.arguments, budget);
    if (!valid) return valid;
  }
  return {};
}

enum class Terminal { none, completed, failed };

struct RunIndex {
  RunId run_id;
  std::vector<const RunEvent*> events;
  bool excluded{};
};

auto source_events(const ConversationHistoryRequest& request)
    -> std::expected<std::span<const RunEvent>, ConversationHistoryError> {
  const auto& events = request.log.events();
  if (!request.source_snapshot_sequence) return std::span{events};
  const auto snapshot = *request.source_snapshot_sequence;
  if (snapshot == 0) return std::span<const RunEvent>{};
  if (snapshot > request.log.last_sequence())
    return failure(Code::invalid_snapshot,
                   "conversation snapshot is beyond the log");
  const auto found =
      std::ranges::lower_bound(events, snapshot, {}, [](const auto& event) {
        return event.metadata.sequence;
      });
  if (found == events.end() || found->metadata.sequence != snapshot)
    return failure(Code::invalid_snapshot,
                   "conversation snapshot event is missing");
  const auto count = static_cast<std::size_t>(found - events.begin()) + 1;
  return std::span{events.data(), count};
}

auto index_runs(const ConversationHistoryRequest& request,
                std::span<const RunEvent> events, std::stop_token stop)
    -> std::expected<std::vector<RunIndex>, ConversationHistoryError> {
  if (events.size() > request.limits.maximum_events ||
      request.excluded_run_ids.size() > request.limits.maximum_runs)
    return failure(Code::resource_exhausted,
                   "conversation event/exclusion limit exceeded");
  std::set<RunId> excluded;
  for (const auto& run_id : request.excluded_run_ids)
    if (!excluded.insert(run_id).second)
      return failure(Code::invalid_exclusion,
                     "duplicate excluded run identity");
  std::map<RunId, std::size_t> lookup;
  std::vector<RunIndex> result;
  for (const auto& event : events) {
    if (stop.stop_requested())
      return failure(Code::cancelled, "history cancelled");
    if (excluded.contains(event.metadata.run_id)) continue;
    auto found = lookup.find(event.metadata.run_id);
    if (found == lookup.end()) {
      if (result.size() == request.limits.maximum_runs)
        return failure(Code::resource_exhausted,
                       "conversation run limit exceeded");
      found = lookup.emplace(event.metadata.run_id, result.size()).first;
      result.push_back({event.metadata.run_id, {}, false});
    }
    auto& run = result[found->second];
    run.events.push_back(&event);
    run.excluded = run.excluded || event.metadata.parent_run_id.has_value() ||
                   std::holds_alternative<ChildRunCreated>(event.payload);
    if (const auto* started = std::get_if<RunStarted>(&event.payload))
      run.excluded =
          run.excluded || started->purpose != RunPurpose::conversation;
  }
  return result;
}

auto conversation_activity(const RunEventPayload& payload) -> bool {
  return std::holds_alternative<UserContentAdded>(payload) ||
         std::holds_alternative<AssistantContentStarted>(payload) ||
         std::holds_alternative<AssistantContentDeltaAdded>(payload) ||
         std::holds_alternative<AssistantContentFinished>(payload) ||
         std::holds_alternative<ToolProposed>(payload) ||
         std::holds_alternative<ToolResultRecorded>(payload) ||
         std::holds_alternative<ToolErrored>(payload);
}

struct RunSources {
  Terminal terminal{Terminal::none};
  const RunEvent* user{};
  bool started{};

  auto apply(const RunEvent& event) -> Status {
    if (conversation_activity(event.payload) &&
        (!started || terminal != Terminal::none))
      return failure(Code::invalid_history,
                     "conversation activity outside live run");
    if (std::holds_alternative<RunStarted>(event.payload)) {
      if (started) return failure(Code::invalid_history, "duplicate run start");
      started = true;
      return {};
    }
    if (std::holds_alternative<UserContentAdded>(event.payload)) {
      if (user != nullptr)
        return failure(Code::invalid_history,
                       "ambiguous conversation user input");
      user = &event;
      return {};
    }
    const bool completed = std::holds_alternative<RunCompleted>(event.payload);
    if (completed || std::holds_alternative<RunFailed>(event.payload) ||
        std::holds_alternative<RunCancelled>(event.payload)) {
      if (!started || terminal != Terminal::none)
        return failure(Code::invalid_history,
                       "ambiguous conversation run completion");
      terminal = completed ? Terminal::completed : Terminal::failed;
    }
    return {};
  }
};

auto run_sources(const RunIndex& run, std::stop_token stop)
    -> std::expected<RunSources, ConversationHistoryError> {
  RunSources result;
  for (const auto* event : run.events) {
    if (stop.stop_requested())
      return failure(Code::cancelled, "history cancelled");
    auto applied = result.apply(*event);
    if (!applied) return std::unexpected(applied.error());
  }
  return result;
}

auto count_blocks(const std::vector<ContentBlock>& content,
                  ContentBudget& budget) -> Status {
  for (const auto& block : content) {
    auto valid = count_block(block, budget);
    if (!valid) return valid;
  }
  return {};
}

auto count_metadata(const ArtifactMetadata& artifact, ContentBudget& budget)
    -> Status {
  auto valid = budget.add_bytes(artifact.media_type.size());
  if (!valid) return valid;
  return budget.add_bytes(artifact.digest.size());
}

auto count_result(const ToolResultRecorded& result, ContentBudget& budget)
    -> Status {
  auto valid = count_blocks(result.content, budget);
  if (!valid) return valid;
  const bool artifacts =
      std::ranges::any_of(result.content, [](const auto& block) {
        return std::holds_alternative<ArtifactReferenceBlock>(block);
      });
  // The shared renderer permits at most 32 KiB metadata per tool result.
  // Reserve its worst-case expansion before entering that renderer. This is
  // a resource bound only; final token estimates use the actual rendered text.
  if (artifacts) return budget.add_bytes(std::size_t{32} * 1024U);
  return {};
}

auto count_projection_event(const RunEvent& event, ContentBudget& budget)
    -> std::expected<bool, ConversationHistoryError> {
  Status valid;
  if (const auto* delta =
          std::get_if<AssistantContentDeltaAdded>(&event.payload)) {
    valid = count_block(delta->delta, budget);
  } else if (const auto* call = std::get_if<ToolProposed>(&event.payload)) {
    valid = count_call(call->invocation_id, call->tool_name, call->arguments,
                       budget);
  } else if (const auto* result =
                 std::get_if<ToolResultRecorded>(&event.payload)) {
    valid = count_result(*result, budget);
  } else if (const auto* failed = std::get_if<ToolErrored>(&event.payload)) {
    valid = budget.add_bytes(failed->error.message.size());
  } else if (const auto* created =
                 std::get_if<ArtifactCreated>(&event.payload)) {
    valid = count_metadata(created->artifact, budget);
  } else {
    return std::holds_alternative<AssistantContentStarted>(event.payload) ||
           std::holds_alternative<AssistantContentFinished>(event.payload);
  }
  if (!valid) return std::unexpected(valid.error());
  return true;
}

// Copy only the proposal fields consumed by the projector. Quotes, capability
// lists and other authority records are not conversation content.
auto projection_event(const RunEvent& event) -> RunEvent {
  if (const auto* call = std::get_if<ToolProposed>(&event.payload))
    return {event.metadata,
            ToolProposed{
                call->invocation_id, call->tool_name, call->arguments, {}}};
  return event;
}

struct MessageSource {
  Message message;
  const RunEvent* source;
  // Provider order is distinct from source completion order: tool validation
  // errors can precede the assistant's completion event.
  std::uint64_t assistant_start_sequence{};
  std::size_t exchange_order{};
};

auto has_payload(const ContentBlock& block) -> bool {
  return std::visit(
      [](const auto& value) {
        using T = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::same_as<T, TextBlock>) {
          return !value.text.empty();
        } else if constexpr (std::same_as<T, StructuredDataBlock>) {
          return !value.media_type.empty() || !value.data.empty();
        } else if constexpr (std::same_as<T, CitationBlock>) {
          return !value.uri.empty() || (value.title && !value.title->empty());
        } else {
          // Unsupported representations must reach explicit validation, never
          // be silently discarded as an empty answer.
          return true;
        }
      },
      block);
}

struct PlainMessages {
  std::optional<Message> active;
  std::optional<InferenceId> inference;
  bool has_tools{};
  std::uint64_t active_start_sequence{};
  std::map<MessageId, std::uint64_t> starts;
  std::map<MessageId, const RunEvent*> sources;
  std::vector<MessageSource> messages;
  std::size_t empty_messages{};

  auto finish(const AssistantContentFinished& finished, const RunEvent& event)
      -> Status {
    if (!active || !inference || active->message_id != finished.message_id ||
        *inference != finished.inference_id ||
        !sources.emplace(finished.message_id, &event).second)
      return failure(Code::invalid_history, "ambiguous assistant completion");
    if (!has_tools) {
      if (std::ranges::any_of(active->content, has_payload)) {
        messages.push_back(
            {std::move(*active), &event, active_start_sequence, 0});
      } else {
        // Legacy replay preserved the user input when the provider completed
        // without an answer. Keep validating this completion's source identity.
        ++empty_messages;
      }
    }
    active.reset();
    inference.reset();
    return {};
  }

  auto result_source(const std::optional<MessageId>& message_id,
                     const RunEvent& event) -> Status {
    if (!message_id || !sources.emplace(*message_id, &event).second)
      return failure(Code::invalid_history, "ambiguous tool result source");
    return {};
  }

  auto apply(const RunEvent& event) -> Status {
    if (const auto* started =
            std::get_if<AssistantContentStarted>(&event.payload)) {
      if (active)
        return failure(Code::invalid_history, "overlapping assistant messages");
      active = Message{started->message_id, Role::assistant, {}, std::nullopt};
      inference = started->inference_id;
      active_start_sequence = event.metadata.sequence;
      if (!starts.emplace(started->message_id, active_start_sequence).second)
        return failure(Code::invalid_history, "duplicate assistant identity");
      has_tools = false;
    } else if (const auto* delta =
                   std::get_if<AssistantContentDeltaAdded>(&event.payload)) {
      if (!active || active->message_id != delta->message_id ||
          inference != delta->inference_id)
        return failure(Code::invalid_history,
                       "assistant delta has no matching message");
      active->content.push_back(delta->delta);
    } else if (const auto* finished =
                   std::get_if<AssistantContentFinished>(&event.payload)) {
      return finish(*finished, event);
    } else if (std::holds_alternative<ToolProposed>(event.payload)) {
      if (!active)
        return failure(Code::invalid_history,
                       "tool call has no assistant message");
      has_tools = true;
    } else if (const auto* result =
                   std::get_if<ToolResultRecorded>(&event.payload)) {
      return result_source(result->result_message_id, event);
    } else if (const auto* failed = std::get_if<ToolErrored>(&event.payload)) {
      return result_source(failed->result_message_id, event);
    }
    return {};
  }
};

auto reserve_projection_work(std::size_t events, std::size_t messages,
                             std::size_t& remaining) -> Status {
  if (messages != 0 && events > remaining / messages)
    return failure(Code::resource_exhausted,
                   "conversation projection work limit exceeded");
  remaining -= events * messages;
  return {};
}

auto preflight_projection(const RunIndex& run, ContentBudget& budget,
                          std::size_t available_sources)
    -> std::expected<std::vector<const RunEvent*>, ConversationHistoryError> {
  std::vector<const RunEvent*> relevant;
  std::size_t source_count{};
  for (const auto* event : run.events) {
    if (budget.stop.stop_requested())
      return failure(Code::cancelled, "history cancelled");
    auto included = count_projection_event(*event, budget);
    if (!included) return std::unexpected(included.error());
    if (*included) relevant.push_back(event);
    if (std::holds_alternative<AssistantContentFinished>(event->payload) ||
        std::holds_alternative<ToolResultRecorded>(event->payload) ||
        std::holds_alternative<ToolErrored>(event->payload)) {
      if (source_count == available_sources)
        return failure(Code::resource_exhausted,
                       "conversation source reference limit exceeded");
      ++source_count;
    }
  }
  return relevant;
}

auto merge_tool_messages(PlainMessages& plain, std::vector<Message> tools,
                         const RunId& run_id) -> Status {
  std::uint64_t assistant_start{};
  std::size_t exchange_order{};
  for (auto& message : tools) {
    const auto found = plain.sources.find(message.message_id);
    if (found == plain.sources.end())
      return failure(Code::invalid_history, "tool message source is missing",
                     run_id);
    if (message.role == Role::assistant) {
      assistant_start = plain.starts.at(message.message_id);
      exchange_order = 0;
    }
    plain.messages.push_back(
        {std::move(message), found->second, assistant_start, exchange_order++});
  }
  if (plain.messages.size() + plain.empty_messages != plain.sources.size())
    return failure(Code::invalid_history,
                   "completed run contains an incomplete tool group", run_id);
  std::ranges::sort(plain.messages, {}, [](const auto& message) {
    return std::pair{message.assistant_start_sequence, message.exchange_order};
  });
  return {};
}

auto completed_messages(const RunIndex& run, ContentBudget& budget,
                        std::size_t& projection_work,
                        std::size_t available_sources)
    -> std::expected<std::vector<MessageSource>, ConversationHistoryError> {
  auto relevant = preflight_projection(run, budget, available_sources);
  if (!relevant) return std::unexpected(relevant.error());
  // All copied payloads have passed the shared aggregate byte/item bounds.
  PlainMessages plain;
  std::vector<RunEvent> projected_events;
  for (const auto* event : *relevant) {
    if (budget.stop.stop_requested())
      return failure(Code::cancelled, "history cancelled");
    const auto valid = plain.apply(*event);
    if (!valid) return std::unexpected(valid.error());
    projected_events.push_back(projection_event(*event));
  }
  if (plain.active)
    return failure(Code::invalid_history,
                   "completed run contains an unfinished message", run.run_id);
  auto work = reserve_projection_work(projected_events.size(),
                                      plain.sources.size(), projection_work);
  if (!work) return std::unexpected(work.error());
  if (budget.stop.stop_requested())
    return failure(Code::cancelled, "history cancelled");
  auto tools = tool_continuation_messages(projected_events);
  if (!tools)
    return failure(Code::invalid_history,
                   "completed tool history cannot be reconstructed",
                   run.run_id);
  auto merged = merge_tool_messages(plain, std::move(*tools), run.run_id);
  if (!merged) return std::unexpected(merged.error());
  return std::move(plain.messages);
}

auto history_entry(const MessageSource& source, const SessionId& session_id,
                   std::uint64_t order, ContentBudget& budget)
    -> std::expected<ConversationHistoryEntry, ConversationHistoryError> {
  const auto before = budget.tokens;
  if (std::ranges::any_of(source.message.content, [](const auto& block) {
        return std::holds_alternative<ArtifactReferenceBlock>(block);
      }))
    return failure(
        Code::unsupported_content,
        "conversation artifact requires a supported metadata projection");
  const auto counted = count_message(source.message, budget);
  if (!counted) return std::unexpected(counted.error());
  const auto& metadata = source.source->metadata;
  const auto suffix = std::to_string(metadata.sequence);
  auto entry_id = ContextEntryId::from("history-entry-" + suffix);
  auto source_id = ContextSourceId::from("history-source-" + suffix);
  if (!entry_id || !source_id)
    return failure(Code::invalid_history,
                   "history identity cannot be constructed");
  return ConversationHistoryEntry{
      {*entry_id,
       source.message.role == Role::tool ? ContextContentKind::tool_result
                                         : ContextContentKind::conversation,
       source.message,
       {*source_id,
        "session:" + std::string{session_id.value()} +
            "/run:" + std::string{metadata.run_id.value()} +
            "/event:" + std::string{metadata.event_id.value()},
        std::nullopt},
       order,
       budget.tokens - before},
      metadata.event_id,
      metadata.sequence};
}

auto append_group(const RunIndex& run, const RunSources& sources,
                  const ConversationHistoryRequest& request,
                  ContentBudget& input, ContentBudget& output,
                  std::size_t& projection_work, std::uint64_t& order)
    -> std::expected<ConversationHistoryGroup, ConversationHistoryError> {
  const auto& user = std::get<UserContentAdded>(sources.user->payload).message;
  if (order == request.limits.maximum_source_references)
    return failure(Code::resource_exhausted,
                   "conversation source reference limit exceeded");
  const auto bounded = count_message(user, input);
  if (!bounded) return std::unexpected(bounded.error());
  std::vector<MessageSource> messages{{user, sources.user}};
  if (sources.terminal == Terminal::completed) {
    auto completed = completed_messages(
        run, input, projection_work,
        request.limits.maximum_source_references - order - 1);
    if (!completed) return std::unexpected(completed.error());
    for (auto& message : *completed)
      messages.push_back(std::move(message));
  }
  if (messages.size() > request.limits.maximum_source_references - order)
    return failure(Code::resource_exhausted,
                   "conversation source reference limit exceeded");
  ConversationHistoryGroup group{run.run_id, {}};
  for (const auto& message : messages) {
    auto entry =
        history_entry(message, request.log.session_id(), ++order, output);
    if (!entry) return std::unexpected(entry.error());
    group.entries.push_back(std::move(*entry));
  }
  return group;
}

auto validate_groups(std::vector<ConversationHistoryGroup> groups,
                     const ConversationHistoryLimits& limits,
                     std::stop_token stop)
    -> std::expected<std::vector<ConversationHistoryGroup>,
                     ConversationHistoryError> {
  ConversationSelectionRequest selected;
  std::ranges::sort(groups, {}, [](const auto& group) {
    return group.entries.front().event_sequence;
  });
  std::uint64_t order{};
  for (auto& group : groups)
    for (auto& entry : group.entries)
      entry.content.order = ++order;
  selected.capacity.context_window_tokens =
      std::numeric_limits<std::uint64_t>::max();
  selected.groups = std::move(groups);
  selected.limits = {limits.maximum_runs,
                     limits.maximum_source_references,
                     limits.maximum_source_references,
                     0,
                     limits.maximum_content_items,
                     std::numeric_limits<std::size_t>::max()};
  // Source locations are generated bounded IDs; content bytes were already
  // checked before projection and after metadata rendering.
  auto result = select_conversation(selected, stop);
  if (!result) {
    if (result.error().code == ConversationSelectionErrorCode::cancelled)
      return failure(Code::cancelled, "history cancelled");
    return failure(Code::invalid_history,
                   "reconstructed conversation group is invalid",
                   result.error().run_id);
  }
  return std::move(result->selected_groups);
}

auto active_tool_run(const SessionEventLog& log, const RunId& run_id,
                     const ConversationHistoryLimits& limits,
                     std::stop_token stop)
    -> std::expected<RunIndex, ConversationHistoryError> {
  if (stop.stop_requested())
    return failure(Code::cancelled, "history cancelled");
  if (log.events().size() > limits.maximum_events || limits.maximum_runs == 0)
    return failure(Code::resource_exhausted,
                   "active tool history scan limit exceeded");
  RunIndex run{run_id, {}, false};
  for (const auto& event : log.events()) {
    if (stop.stop_requested())
      return failure(Code::cancelled, "history cancelled");
    if (event.metadata.run_id == run_id) run.events.push_back(&event);
  }
  auto sources = run_sources(run, stop);
  if (!sources) return std::unexpected(sources.error());
  if (!sources->started || sources->user == nullptr ||
      sources->terminal != Terminal::none)
    return failure(Code::invalid_history,
                   "tool continuation requires a live source run", run_id);
  return run;
}

auto active_projection_events(const RunIndex& run, ContentBudget& input)
    -> std::expected<std::vector<RunEvent>, ConversationHistoryError> {
  auto relevant =
      preflight_projection(run, input, input.limits.maximum_source_references);
  if (!relevant) return std::unexpected(relevant.error());
  const auto source_count = static_cast<std::size_t>(
      std::ranges::count_if(*relevant, [](const RunEvent* event) {
        return std::holds_alternative<AssistantContentFinished>(
                   event->payload) ||
               std::holds_alternative<ToolResultRecorded>(event->payload) ||
               std::holds_alternative<ToolErrored>(event->payload);
      }));
  auto remaining_work = input.limits.maximum_projection_work;
  auto work =
      reserve_projection_work(relevant->size(), source_count, remaining_work);
  if (!work) return std::unexpected(work.error());
  std::vector<RunEvent> projected;
  projected.reserve(relevant->size());
  for (const auto* event : *relevant) {
    if (input.stop.stop_requested())
      return failure(Code::cancelled, "history cancelled");
    projected.push_back(projection_event(*event));
  }
  return projected;
}

auto active_tool_messages(const RunIndex& run,
                          const ConversationHistoryLimits& limits,
                          std::stop_token stop)
    -> std::expected<std::vector<Message>, ConversationHistoryError> {
  ContentBudget input{limits, stop};
  auto projected = active_projection_events(run, input);
  if (!projected) return std::unexpected(projected.error());
  if (stop.stop_requested())
    return failure(Code::cancelled, "history cancelled");
  auto messages = tool_continuation_messages(*projected);
  if (!messages)
    return failure(Code::invalid_history,
                   "active tool history cannot be reconstructed", run.run_id);
  if (messages->size() > limits.maximum_source_references)
    return failure(Code::resource_exhausted,
                   "active tool history source limit exceeded");
  ContentBudget output{limits, stop};
  for (const auto& message : *messages) {
    if (std::ranges::any_of(message.content, [](const auto& block) {
          return std::holds_alternative<ArtifactReferenceBlock>(block);
        }))
      return failure(Code::unsupported_content,
                     "active tool artifact requires metadata projection");
    auto valid = count_message(message, output);
    if (!valid) return std::unexpected(valid.error());
  }
  return std::move(*messages);
}

} // namespace

auto estimate_conversation_message(const Message& message,
                                   std::uint32_t version,
                                   const ConversationHistoryLimits& limits,
                                   std::stop_token stop)
    -> std::expected<std::uint64_t, ConversationHistoryError> {
  try {
    if (stop.stop_requested())
      return failure(Code::cancelled, "history cancelled");
    if (version != conversation_estimator_version)
      return failure(Code::unsupported_estimator,
                     "unsupported conversation estimator version");
    ContentBudget budget{limits, stop};
    auto counted = count_message(message, budget);
    if (!counted) return std::unexpected(counted.error());
    return budget.tokens;
  } catch (...) {
    return failure(Code::internal_failure, "conversation estimation failed");
  }
}

auto reconstruct_conversation_history(const ConversationHistoryRequest& request,
                                      std::stop_token stop)
    -> std::expected<std::vector<ConversationHistoryGroup>,
                     ConversationHistoryError> {
  try {
    if (stop.stop_requested())
      return failure(Code::cancelled, "history cancelled");
    if (request.estimator_version != conversation_estimator_version)
      return failure(Code::unsupported_estimator,
                     "unsupported conversation estimator version");
    auto events = source_events(request);
    if (!events) return std::unexpected(events.error());
    auto runs = index_runs(request, *events, stop);
    if (!runs) return std::unexpected(runs.error());
    ContentBudget input{request.limits, stop};
    ContentBudget output{request.limits, stop};
    auto projection_work = request.limits.maximum_projection_work;
    std::uint64_t order{};
    std::vector<ConversationHistoryGroup> groups;
    for (const auto& run : *runs) {
      if (stop.stop_requested())
        return failure(Code::cancelled, "history cancelled");
      if (run.excluded) continue;
      const auto sources = run_sources(run, stop);
      if (!sources) return std::unexpected(sources.error());
      if (sources->terminal == Terminal::none || sources->user == nullptr)
        continue;
      auto group = append_group(run, *sources, request, input, output,
                                projection_work, order);
      if (!group) return std::unexpected(group.error());
      groups.push_back(std::move(*group));
    }
    return validate_groups(std::move(groups), request.limits, stop);
  } catch (...) {
    return failure(Code::internal_failure,
                   "conversation history reconstruction failed");
  }
}

auto reconstruct_active_tool_continuation(
    const SessionEventLog& log, const RunId& run_id,
    const ConversationHistoryLimits& limits, std::stop_token stop)
    -> std::expected<std::vector<Message>, ConversationHistoryError> {
  try {
    auto run = active_tool_run(log, run_id, limits, stop);
    if (!run) return std::unexpected(run.error());
    return active_tool_messages(*run, limits, stop);
  } catch (...) {
    return failure(Code::internal_failure,
                   "active tool history reconstruction failed");
  }
}

} // namespace aiforge::runtime
