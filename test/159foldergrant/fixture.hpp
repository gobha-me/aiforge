#pragma once
#include <aiforge/runtime/local_source_grant.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace folder_grant_test {
using namespace aiforge;
using namespace std::chrono_literals;
inline auto request(std::uint64_t id = 1) -> runtime::LocalFolderGrantRequest {
  return {{domain::SessionId::from("session").value(), 1, id},
          "/private/folder",
          1,
          {}};
}
struct Gate {
  std::mutex mutex;
  std::condition_variable changed;
  bool entered{}, released{};
  auto wait() -> void {
    std::unique_lock lock{mutex};
    entered = true;
    changed.notify_all();
    changed.wait(lock, [&] { return released; });
  }
  auto await() -> bool {
    std::unique_lock lock{mutex};
    return changed.wait_for(lock, 3s, [&] { return entered; });
  }
  auto release() -> void {
    {
      const std::lock_guard lock{mutex};
      released = true;
    }
    changed.notify_all();
  }
};
struct Release {
  std::shared_ptr<Gate> gate;
  bool enabled{true};
  ~Release() {
    if (enabled) gate->release();
  }
};
template <typename Predicate> auto until(Predicate predicate) -> bool {
  const auto end = std::chrono::steady_clock::now() + 3s;
  do {
    if (predicate()) return true;
    std::this_thread::yield();
  } while (std::chrono::steady_clock::now() < end);
  return false;
}
struct Destruction {
  std::shared_ptr<Gate> gate;
  std::atomic<bool> done{};
  std::thread::id thread;
};
class Lease final : public runtime::LocalSourceLease {
 public:
  domain::SessionId session{request().token.session_id};
  domain::LocalRootIdentity root{1, std::string(64, 'a')};
  std::uint64_t generation{1};
  bool pinned{true};
  std::atomic<bool> revoked{};
  std::shared_ptr<Destruction> destruction;
  ~Lease() override {
    if (destruction) {
      destruction->thread = std::this_thread::get_id();
      if (destruction->gate) destruction->gate->wait();
      destruction->done = true;
    }
  }
  auto root_identity() const noexcept
      -> const domain::LocalRootIdentity& override {
    return root;
  }
  auto session_id() const noexcept -> const domain::SessionId& override {
    return session;
  }
  auto lease_generation() const noexcept -> std::uint64_t override {
    return generation;
  }
  auto guarantees_pinned_read_only_sources() const noexcept -> bool override {
    return pinned;
  }
  auto revoke() noexcept -> void override { revoked = true; }
  auto list(runtime::LocalListRequest value, std::stop_token)
      -> std::expected<runtime::LocalListResult,
                       domain::LocalSourceError> override {
    return runtime::LocalListResult{value.token,
                                    value.directory,
                                    {},
                                    0,
                                    runtime::LocalListingState::complete};
  }
  auto preview(runtime::LocalPreviewRequest, std::stop_token)
      -> std::expected<runtime::LocalPreviewResult,
                       domain::LocalSourceError> override {
    return std::unexpected(domain::LocalSourceError{
        domain::LocalSourceErrorCode::unavailable, "unused"});
  }
  auto revalidate(runtime::LocalRevalidateRequest, std::stop_token)
      -> std::expected<runtime::LocalReadResult,
                       domain::LocalSourceError> override {
    return std::unexpected(domain::LocalSourceError{
        domain::LocalSourceErrorCode::unavailable, "unused"});
  }
};
} // namespace folder_grant_test
