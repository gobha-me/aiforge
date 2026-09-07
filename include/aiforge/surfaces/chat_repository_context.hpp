#pragma once

#include <aiforge/domain/events.hpp>
#include <aiforge/runtime/repository_context_controller.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace aiforge::surfaces {

enum class ChatRepositoryChangeKind {
  select_target,
  disable,
  add_evidence,
  remove_evidence,
  clear_evidence
};
struct ChatRepositoryChange {
  ChatRepositoryChangeKind kind{ChatRepositoryChangeKind::select_target};
  std::string subject;
  auto operator==(const ChatRepositoryChange&) const -> bool = default;
};

enum class ChatRepositoryWorkPurpose {
  selection,
  submit,
  continuation,
  recovery,
  answer,
  approval
};
struct ChatRepositoryWorkToken {
  domain::SessionId session_id;
  domain::ModelId model_id;
  std::uint64_t generation{};
  std::uint64_t selection_revision{};
  ChatRepositoryWorkPurpose purpose{ChatRepositoryWorkPurpose::selection};
  auto operator==(const ChatRepositoryWorkToken&) const -> bool = default;
};

// Captured entirely on the session owner thread. Workers inspect only this
// owned value and the separately owned read-only controller, never ChatSession.
struct ChatRepositoryWork {
  ChatRepositoryWorkToken token;
  std::variant<runtime::RepositoryContextRequest,
               domain::RepositoryContextAdmission>
      input;
};
struct ChatRepositoryWorkCompletion {
  ChatRepositoryWorkToken token;
  std::expected<runtime::PreparedRepositoryContext,
                domain::RepositoryContextError>
      result;
};
struct ChatRepositoryWorkOutcome {
  std::vector<domain::RunEvent> events;
  bool submitted{};
  bool selection_changed{};
};
struct ChatRepositoryContextState {
  bool available{};
  bool enabled{};
  bool busy{};
  std::uint64_t selection_revision{};
  std::string target_subtree;
  std::vector<std::string> evidence_paths;
  // Latest prepared/admitted references are bounded and contain no source text.
  std::optional<domain::RepositoryContextAdmission> admission;
  std::string message;
};

} // namespace aiforge::surfaces
