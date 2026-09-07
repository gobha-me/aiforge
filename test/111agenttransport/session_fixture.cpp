#include <aiforge/adapters/sqlite_session_store.hpp>
#include <aiforge/domain/events.hpp>
#include <chrono>
#include <iostream>
#include <string>
#include <vector>

namespace {
using namespace aiforge;
template <class Id> auto id(std::string value) -> Id {
  return Id::from(value).value();
}
auto events(const bool completed) -> std::vector<domain::RunEvent> {
  std::vector<domain::RunEvent> result;
  const auto add = [&](domain::RunEventPayload payload) {
    const auto sequence = result.size() + 1;
    result.push_back(
        {{id<domain::EventId>("event-" + std::to_string(sequence)),
          id<domain::RunId>("run"),
          sequence,
          1,
          domain::EventTimestamp{std::chrono::milliseconds{sequence}},
          {},
          {},
          {}},
         std::move(payload)});
  };
  add(domain::RunStarted{id<domain::SurfaceId>("fixture"),
                         id<domain::WorkspaceId>("chat"),
                         id<domain::PermissionProfileId>("observe"),
                         {}});
  if (completed) {
    for (int index = 0; index < 32; ++index)
      add(domain::UserContentAdded{
          {id<domain::MessageId>("message-" + std::to_string(index)),
           domain::Role::user,
           {domain::TextBlock{std::string(120U * 1024U, 'x')}},
           {}}});
    add(domain::RunCompleted{});
  }
  return result;
}
} // namespace

auto main(int argc, char** argv) -> int {
  if (argc != 2) return 2;
  auto store = aiforge::adapters::SqliteSessionStore::open(argv[1]);
  if (!store) {
    std::cerr << store.error().message;
    return 1;
  }
  for (const bool completed : {false, true}) {
    auto session =
        id<aiforge::domain::SessionId>(completed ? "ready" : "pending");
    auto created = (*store)->create_session({session, {}}, {});
    if (!created) {
      std::cerr << created.error().message;
      return 1;
    }
    auto appended = (*store)->append_events(session, events(completed), {});
    if (!appended) {
      std::cerr << appended.error().message;
      return 1;
    }
  }
  return 0;
}
