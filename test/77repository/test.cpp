#include <aiforge/repository/snapshot_source.hpp>
#include <aiforge/testing/scripted_repository_snapshot_source.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <stop_token>
#include <string>
#include <utility>

namespace {

using namespace aiforge;

template <typename Id> auto id(std::string value) -> Id {
  auto parsed = Id::from(std::move(value));
  REQUIRE(parsed);
  return std::move(*parsed);
}

auto digest(std::string value = "0123456789abcdef", std::uint64_t bytes = 4)
    -> domain::ContentDigest {
  return {"test-sha256", std::move(value), bytes};
}

auto snapshot(std::string fingerprint = "aaaaaaaaaaaaaaaa")
    -> domain::RepositorySnapshot {
  return {
      {id<domain::RepositoryId>("repository-1"), "/work/repository"},
      domain::VcsState{"git", "sha256", domain::VcsHeadKind::branch, "main",
                       "bbbbbbbbbbbbbbbb"},
      {},
      digest(std::move(fingerprint), 64),
      std::chrono::sys_time<std::chrono::milliseconds>{
          std::chrono::milliseconds{100}},
  };
}

} // namespace

TEST_CASE(
    "repository snapshot validates clean branch detached and non-VCS states") {
  auto clean = snapshot();
  REQUIRE(repository::validate_repository_snapshot(clean));

  clean.vcs = domain::VcsState{"git", "sha1", domain::VcsHeadKind::detached,
                               std::nullopt, "cccccccccccccccc"};
  REQUIRE(repository::validate_repository_snapshot(clean));

  clean.vcs = domain::VcsState{"git", "sha1", domain::VcsHeadKind::unborn,
                               "topic", std::nullopt};
  REQUIRE(repository::validate_repository_snapshot(clean));

  clean.vcs.reset();
  clean.changes.push_back(domain::RepositoryChange{
      "src/main.cpp", std::nullopt, domain::RepositoryEntryKind::regular_file,
      domain::RepositoryChangeKind::untracked,
      domain::RepositoryChangeStage::untracked, std::nullopt, digest()});
  REQUIRE(repository::validate_repository_snapshot(clean));
}

TEST_CASE("repository snapshot rejects malformed identities and entries") {
  auto value = snapshot();
  value.root.canonical_path = "relative/repository";
  REQUIRE_FALSE(repository::validate_repository_snapshot(value));

  value = snapshot();
  value.vcs->head_kind = domain::VcsHeadKind::detached;
  REQUIRE_FALSE(repository::validate_repository_snapshot(value));

  value = snapshot();
  value.fingerprint.algorithm = "bad algorithm";
  REQUIRE_FALSE(repository::validate_repository_snapshot(value));

  value = snapshot();
  value.changes = {
      {"b.cpp", std::nullopt, domain::RepositoryEntryKind::regular_file,
       domain::RepositoryChangeKind::modified,
       domain::RepositoryChangeStage::worktree, std::nullopt, digest()},
      {"a.cpp", std::nullopt, domain::RepositoryEntryKind::regular_file,
       domain::RepositoryChangeKind::modified,
       domain::RepositoryChangeStage::worktree, std::nullopt, digest()},
  };
  REQUIRE_FALSE(repository::validate_repository_snapshot(value));

  value = snapshot();
  value.changes = {
      {"../escape", std::nullopt, domain::RepositoryEntryKind::regular_file,
       domain::RepositoryChangeKind::modified,
       domain::RepositoryChangeStage::worktree, std::nullopt, digest()}};
  REQUIRE_FALSE(repository::validate_repository_snapshot(value));

  value = snapshot();
  value.changes = {
      {"old.cpp", std::nullopt, domain::RepositoryEntryKind::regular_file,
       domain::RepositoryChangeKind::renamed,
       domain::RepositoryChangeStage::index, std::nullopt, digest()}};
  REQUIRE_FALSE(repository::validate_repository_snapshot(value));

  value = snapshot();
  value.changes = {
      {"gone.cpp", std::nullopt, domain::RepositoryEntryKind::regular_file,
       domain::RepositoryChangeKind::deleted,
       domain::RepositoryChangeStage::worktree, std::nullopt, digest()}};
  REQUIRE_FALSE(repository::validate_repository_snapshot(value));

  value = snapshot();
  value.changes = {
      {"new.cpp", std::nullopt, domain::RepositoryEntryKind::regular_file,
       domain::RepositoryChangeKind::untracked,
       domain::RepositoryChangeStage::index, std::nullopt, digest()}};
  REQUIRE_FALSE(repository::validate_repository_snapshot(value));
}

TEST_CASE("repository snapshot enforces entry path and byte limits") {
  auto value = snapshot();
  value.changes = {{"source.cpp", std::nullopt,
                    domain::RepositoryEntryKind::regular_file,
                    domain::RepositoryChangeKind::modified,
                    domain::RepositoryChangeStage::worktree, std::nullopt,
                    digest("abcd", 9)}};

  repository::RepositorySnapshotLimits limits;
  limits.maximum_entries = 0;
  REQUIRE(
      repository::validate_repository_snapshot(value, limits).error().code ==
      repository::RepositorySnapshotErrorCode::invalid_request);

  limits = {};
  limits.maximum_entries = 1;
  limits.maximum_path_bytes = 4;
  REQUIRE(
      repository::validate_repository_snapshot(value, limits).error().code ==
      repository::RepositorySnapshotErrorCode::invalid_request);

  limits = {};
  limits.maximum_file_bytes = 8;
  REQUIRE(
      repository::validate_repository_snapshot(value, limits).error().code ==
      repository::RepositorySnapshotErrorCode::resource_exhausted);

  limits = {};
  limits.maximum_total_bytes = 8;
  REQUIRE(
      repository::validate_repository_snapshot(value, limits).error().code ==
      repository::RepositorySnapshotErrorCode::invalid_request);
}

TEST_CASE("same source state uses repository identity and fingerprint only") {
  auto left = snapshot();
  auto right = left;
  right.observed_at += std::chrono::hours{1};
  REQUIRE(domain::same_source_state(left, right));

  right.fingerprint.value = "cccccccccccccccc";
  REQUIRE_FALSE(domain::same_source_state(left, right));

  right = left;
  right.root.repository_id = id<domain::RepositoryId>("repository-2");
  REQUIRE_FALSE(domain::same_source_state(left, right));
}

TEST_CASE(
    "scripted repository source records outcomes errors and cancellation") {
  repository::RepositorySnapshotRequest request{"/work/repository", {}};
  const auto expected = snapshot();
  testing::ScriptedRepositorySnapshotSource source{{
      {request, expected},
      {request,
       repository::RepositorySnapshotError{
           repository::RepositorySnapshotErrorCode::unstable,
           "repository changed", true}},
  }};

  auto first = source.observe(request);
  REQUIRE(first == expected);
  REQUIRE(source.recorded_requests() ==
          std::vector<repository::RepositorySnapshotRequest>{request});

  auto second = source.observe(request);
  REQUIRE_FALSE(second);
  REQUIRE(second.error().code ==
          repository::RepositorySnapshotErrorCode::unstable);
  REQUIRE(second.error().retryable);
  REQUIRE(source.remaining_exchanges() == 0);

  auto exhausted = source.observe(request);
  REQUIRE_FALSE(exhausted);
  REQUIRE(exhausted.error().code ==
          repository::RepositorySnapshotErrorCode::internal_failure);

  std::stop_source cancelled;
  cancelled.request_stop();
  testing::ScriptedRepositorySnapshotSource cancelled_source;
  auto stopped = cancelled_source.observe(request, cancelled.get_token());
  REQUIRE_FALSE(stopped);
  REQUIRE(stopped.error().code ==
          repository::RepositorySnapshotErrorCode::cancelled);
  REQUIRE(cancelled_source.recorded_requests().empty());
}
