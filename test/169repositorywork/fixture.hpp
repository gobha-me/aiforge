#pragma once
#include "../159foldergrant/fixture.hpp"
#include <aiforge/detail/sha256.hpp>
#include <aiforge/runtime/local_source_worker.hpp>
#include <catch2/catch_test_macros.hpp>
#include <stdexcept>

namespace repository_work_test {
using namespace aiforge;
using folder_grant_test::Destruction;
using folder_grant_test::Gate;
using folder_grant_test::Release;
using folder_grant_test::until;
using Code = runtime::LocalSourceWorkerErrorCode;
template <typename Id> auto id(std::string value) -> Id {
  return Id::from(std::move(value)).value();
}
inline auto digest(std::string_view text) -> domain::ContentDigest {
  detail::Sha256 hash;
  hash.update(std::as_bytes(std::span{text.data(), text.size()}));
  return {"sha256", hash.finish(), text.size()};
}
class Source final : public runtime::RepositoryContextSource {
 public:
  bool guaranteed{true}, failed{}, throws{}, changed{}, malformed{};
  std::atomic<unsigned> observations{};
  std::shared_ptr<Gate> block, callback;
  std::shared_ptr<Destruction> destruction;
  domain::RepositorySnapshot snapshot{
      {id<domain::RepositoryId>("repo"), "/repo"},
      domain::VcsState{"git", "sha1", domain::VcsHeadKind::branch, "main",
                       std::string(40, 'a')},
      {},
      digest("snapshot"),
      {}};
  ~Source() override {
    if (destruction) {
      destruction->thread = std::this_thread::get_id();
      if (destruction->gate) destruction->gate->wait();
      destruction->done = true;
    }
  }
  auto identity() const noexcept -> std::string_view override {
    return "repository-root-binding";
  }
  auto guarantees_pinned_read_only_sources() const noexcept -> bool override {
    return guaranteed;
  }
  auto observe(repository::RepositorySnapshotLimits, std::stop_token stop)
      -> std::expected<domain::RepositorySnapshot,
                       repository::RepositorySnapshotError> override {
    ++observations;
    std::optional<std::stop_callback<std::function<void()>>> stopped;
    if (callback) stopped.emplace(stop, [gate = callback] { gate->wait(); });
    if (block) block->wait();
    if (throws) throw std::runtime_error("secret /private/repository");
    if (failed)
      return std::unexpected(repository::RepositorySnapshotError{
          repository::RepositorySnapshotErrorCode::not_found,
          "secret /private/repository"});
    auto result = snapshot;
    if (malformed) result.fingerprint = {};
    return result;
  }
  auto discover(repository::ProjectInstructionRequest request, std::stop_token)
      -> std::expected<domain::ProjectInstructionDiscovery,
                       repository::ProjectInstructionError> override {
    const auto source = domain::snapshot_identity(request.baseline);
    const std::string text{"Keep instructions."};
    return domain::ProjectInstructionDiscovery{
        source,
        request.target_subtree,
        {{id<domain::ProjectInstructionId>("instruction"),
          {source, "AGENTS.md", digest(text), {}},
          {},
          text,
          0,
          1}}};
  }
  auto read(repository::ExactSourceReadRequest request, std::stop_token)
      -> std::expected<repository::ExactSourceReadResult,
                       repository::ExactSourceEditError> override {
    const std::string text = changed ? "world" : "hello";
    return repository::ExactSourceReadResult{
        {domain::snapshot_identity(request.baseline),
         request.relative_path,
         digest(text),
         {}},
        text};
  }
};
inline auto token(std::uint64_t request_id = 1)
    -> runtime::RepositoryContextWorkToken {
  return {id<domain::SessionId>("session"), 1, request_id, 3};
}
inline auto request(std::uint64_t request_id = 1)
    -> runtime::RepositoryContextWorkRequest {
  return {token(request_id),
          runtime::RepositoryContextRequest{"", 3, {"file.txt", "other.txt"}}};
}
struct Fixture {
  std::shared_ptr<Source> source{std::make_shared<Source>()};
  std::shared_ptr<runtime::RepositoryContextController> controller;
  std::unique_ptr<runtime::LocalSourceWorker> worker;
  explicit Fixture(std::size_t capacity = 1) {
    auto owned = runtime::RepositoryContextController::create_owned(
        source, source->snapshot.root);
    REQUIRE(owned);
    controller = std::move(*owned);
    auto created = runtime::LocalSourceWorker::create(capacity);
    REQUIRE(created);
    worker = std::move(*created);
  }
  auto completed(runtime::RepositoryContextWorkToken work = token())
      -> runtime::RepositoryContextWorkCompletion {
    REQUIRE(until([&] { return worker->ready_results() == 1; }));
    auto result = worker->poll(work);
    REQUIRE(result);
    REQUIRE(*result);
    return std::move(**result);
  }
  auto original() -> domain::RepositoryContextAdmission {
    auto prepared = controller->prepare(
        std::get<runtime::RepositoryContextRequest>(request().operation));
    REQUIRE(prepared);
    auto admission = prepared->admission;
    admission.capacity = {10000, 100, 0};
    admission.evidence.back().decision =
        domain::RepositoryContextDecision::omitted_budget;
    REQUIRE(domain::seal_repository_context_admission(admission));
    source->observations = 0;
    return admission;
  }
};
template <typename Action>
auto returns_before_release(Action action, const std::shared_ptr<Gate>& gate)
    -> void {
  std::atomic<bool> returned{};
  std::jthread owner{[&] {
    action();
    returned = true;
  }};
  Release release{gate};
  REQUIRE(until([&] { return returned.load(); }));
  owner.join();
  release.enabled = false;
}
} // namespace repository_work_test
