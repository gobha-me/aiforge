#include "conversation_context.hpp"

#include <aiforge/domain/run_projection.hpp>
#include <concepts>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <type_traits>
#include <variant>

namespace aiforge::surfaces::detail {
namespace {

[[nodiscard]] auto content_bytes(
    const std::vector<domain::ContentBlock>& content)
    -> std::optional<std::uint64_t> {
  std::uint64_t total{};
  for (const auto& block : content) {
    const auto size = std::visit(
        [](const auto& value) -> std::optional<std::uint64_t> {
          using Value = std::remove_cvref_t<decltype(value)>;
          if constexpr (std::same_as<Value, domain::TextBlock>) {
            return value.text.size();
          } else if constexpr (std::same_as<Value,
                                            domain::StructuredDataBlock>) {
            return value.media_type.size() + value.data.size();
          } else if constexpr (std::same_as<Value, domain::CitationBlock>) {
            return value.uri.size() +
                   (value.title ? value.title->size() : std::size_t{});
          } else if constexpr (std::same_as<Value,
                                            domain::ArtifactReferenceBlock>) {
            return value.artifact_id.value().size() +
                   (value.label ? value.label->size() : std::size_t{});
          } else {
            return std::nullopt;
          }
        },
        block);
    if (!size || *size > std::numeric_limits<std::uint64_t>::max() - total) {
      return std::nullopt;
    }
    total += *size;
  }
  return total == 0 ? std::nullopt : std::optional{total};
}

template <typename IdType>
[[nodiscard]] auto make_context_id(const std::string_view prefix,
                                   const std::uint64_t suffix)
    -> std::optional<IdType> {
  auto id = IdType::from(std::string{prefix} + '-' + std::to_string(suffix));
  if (!id) return std::nullopt;
  return std::move(*id);
}

}  // namespace

auto replayed_conversation(const domain::SessionEventLog& log,
                           const std::uint64_t suffix)
    -> std::expected<std::vector<domain::ContextContentInput>, std::string> {
  std::map<domain::RunId, domain::RunProjection> projections;
  std::vector<domain::RunId> run_order;
  std::set<domain::RunId> seen;
  for (const auto& event : log.events()) {
    if (seen.insert(event.metadata.run_id).second) {
      run_order.push_back(event.metadata.run_id);
    }
    auto projection = projections.contains(event.metadata.run_id)
                          ? projections.at(event.metadata.run_id)
                          : domain::RunProjection{};
    if (!projection.apply(event)) {
      return std::unexpected(
          std::string{"session replay could not rebuild conversation"});
    }
    projections.insert_or_assign(event.metadata.run_id, std::move(projection));
  }

  std::vector<domain::ContextContentInput> result;
  std::uint64_t order{1};
  std::uint64_t index{};
  for (const auto& run_id : run_order) {
    const auto& projection = projections.at(run_id);
    for (const auto& message : projection.messages()) {
      if (!message.complete || (message.role != domain::Role::user &&
                                message.role != domain::Role::assistant)) {
        continue;
      }
      if (message.role == domain::Role::assistant &&
          projection.status() != domain::RunStatus::completed) {
        continue;
      }
      auto bytes = content_bytes(message.content);
      if (!bytes) continue;
      auto entry_id = make_context_id<domain::ContextEntryId>("history-entry",
                                                              suffix ^ ++index);
      auto source_id = make_context_id<domain::ContextSourceId>(
          "history-source", suffix ^ index);
      if (!entry_id || !source_id) {
        return std::unexpected(
            std::string{"replay context identity generation failed"});
      }
      result.push_back(
          {*entry_id,
           domain::ContextContentKind::conversation,
           {message.message_id, message.role, message.content, std::nullopt},
           {*source_id,
            "session:" + std::string{log.session_id().value()} +
                "/run:" + std::string{run_id.value()} +
                "/message:" + std::string{message.message_id.value()},
            std::nullopt},
           order++,
           *bytes});
    }
  }
  return result;
}

}  // namespace aiforge::surfaces::detail
