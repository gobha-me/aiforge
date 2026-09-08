#include "fixture.hpp"
#include <aiforge/adapters/git_exact_source_editor.hpp>
#include <aiforge/adapters/git_repository_snapshot_source.hpp>
#include <aiforge/adapters/pinned_repository_root_authority.hpp>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
struct CloseTrap {
  dev_t device{};
  ino_t inode{};
  std::shared_ptr<folder_grant_test::Gate> gate =
      std::make_shared<folder_grant_test::Gate>();
  std::atomic<unsigned> entered{}, completed{};
  std::atomic<bool> on_owner{};
  std::thread::id owner = std::this_thread::get_id();
};
std::atomic<std::shared_ptr<CloseTrap>> close_trap;
struct ResetTrap {
  std::shared_ptr<CloseTrap> trap;
  ~ResetTrap() {
    close_trap.store({});
    trap->gate->release();
  }
};
} // namespace
extern "C" auto __real_close(int descriptor) -> int;
extern "C" auto __wrap_close(int descriptor) -> int {
  const auto trap = close_trap.load();
  struct stat identity{};
  if (!trap || ::fstat(descriptor, &identity) != 0 ||
      identity.st_dev != trap->device || identity.st_ino != trap->inode)
    return __real_close(descriptor);
  ++trap->entered;
  if (std::this_thread::get_id() == trap->owner) trap->on_owner = true;
  trap->gate->wait();
  const auto result = __real_close(descriptor);
  ++trap->completed;
  return result;
}
using namespace repository_work_test;
TEST_CASE("Repository work refuses borrowed sources and malformed bounded "
          "inputs before observation") {
  Fixture f;
  CHECK_FALSE(runtime::RepositoryContextController::create_owned(
      {}, f.source->snapshot.root));
  auto borrowed = std::make_shared<runtime::RepositoryContextController>(
      *f.source, f.source->snapshot.root);
  CHECK_FALSE(borrowed->owns_source());
  CHECK_FALSE(f.worker->submit(borrowed, request()));
  for (unsigned bad = 0; bad < 8; ++bad) {
    auto input = request();
    auto& operation =
        std::get<runtime::RepositoryContextRequest>(input.operation);
    switch (bad) {
      case 0: input.token.session_epoch = 0; break;
      case 1: input.token.request_id = 0; break;
      case 2: input.token.selection_revision = 0; break;
      case 3: ++operation.selection_revision; break;
      case 4: operation.target_subtree = "../outside"; break;
      case 5: operation.evidence_paths.push_back("file.txt"); break;
      case 6: operation.evidence_paths = {std::string(4097, 'x')}; break;
      case 7: operation.evidence_paths.resize(65, "file.txt"); break;
    }
    INFO(bad);
    CHECK_FALSE(f.worker->submit(f.controller, std::move(input)));
  }
  CHECK(f.source->observations == 0);
  f.source->guaranteed = false;
  CHECK_FALSE(runtime::RepositoryContextController::create_owned(
      f.source, f.source->snapshot.root));
  CHECK(f.source->observations == 0);
}
TEST_CASE("Repository recovery requires original seal and preserves omissions "
          "while allowing unrelated snapshot drift") {
  Fixture f;
  auto original = f.original();
  auto invalid = original;
  invalid.capacity.reserved_input_tokens = 1;
  CHECK_FALSE(f.worker->submit(
      f.controller, runtime::RepositoryContextWorkRequest{token(), invalid}));
  CHECK(f.source->observations == 0);
  f.source->snapshot.fingerprint = digest("unrelated snapshot change");
  REQUIRE(f.worker->submit(
      f.controller, runtime::RepositoryContextWorkRequest{token(), original}));
  auto completed = f.completed();
  INFO((completed.result ? "revalidated" : completed.result.error().message));
  REQUIRE(completed.result);
  CHECK(domain::repository_context_admission_successor(
      original, completed.result->admission));
  CHECK(completed.result->admission.evidence.back().decision ==
        domain::RepositoryContextDecision::omitted_budget);
  CHECK(completed.result->admission.capacity == original.capacity);
  CHECK(completed.result->admission.source_snapshot !=
        original.source_snapshot);
  f.source->changed = true;
  REQUIRE(f.worker->submit(
      f.controller, runtime::RepositoryContextWorkRequest{token(2), original}));
  auto stale = f.completed(token(2));
  CHECK_FALSE(stale.result);
}
TEST_CASE("Repository worker returns canonical failures and immutable prepared "
          "values") {
  Fixture f;
  SECTION("port error") {
    f.source->failed = true;
  }
  SECTION("exception") {
    f.source->throws = true;
  }
  SECTION("malformed snapshot") {
    f.source->malformed = true;
  }
  REQUIRE(f.worker->submit(f.controller, request()));
  auto completed = f.completed();
  REQUIRE_FALSE(completed.result);
  CHECK(completed.result.error().message.find("secret") == std::string::npos);
  CHECK(completed.result.error().message.find("/private") == std::string::npos);
}
namespace {
struct Repository {
  std::filesystem::path path;
  Repository() {
    static std::atomic<unsigned> next{};
    path = std::filesystem::temp_directory_path() /
           ("aiforge-owned-repository-" + std::to_string(::getpid()) + "-" +
            std::to_string(next++));
    std::filesystem::create_directory(path);
    git({"init", "-q"});
    git({"config", "user.email", "test@example.invalid"});
    git({"config", "user.name", "Test"});
    std::ofstream{path / "AGENTS.md"} << "Keep instructions.";
    std::ofstream{path / "file.txt"} << "hello";
    git({"add", "."});
    git({"commit", "-qm", "fixture"});
  }
  ~Repository() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
  auto git(std::vector<std::string> arguments) -> void {
    arguments.insert(arguments.begin(),
                     {REPOSITORY_TEST_GIT, "-C", path.string()});
    std::vector<char*> argv;
    for (auto& argument : arguments)
      argv.push_back(argument.data());
    argv.push_back(nullptr);
    const auto pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
      ::execv(REPOSITORY_TEST_GIT, argv.data());
      ::_exit(127);
    }
    int status{};
    REQUIRE(::waitpid(pid, &status, 0) == pid);
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 0);
  }
};
auto open_sources(const Repository& repository)
    -> std::expected<adapters::OwnedPinnedRepositorySources,
                     runtime::AutomaticApprovalMatcherError> {
  auto opened = adapters::GitRepositorySnapshotSource::open(
      REPOSITORY_TEST_GIT, adapters::GitCommandPolicy::isolated_read_only);
  REQUIRE(opened);
  return adapters::open_owned_pinned_repository_sources(repository.path,
                                                        std::move(*opened));
}
auto await_sources(const Repository& repository)
    -> adapters::OwnedPinnedRepositorySources {
  std::optional<adapters::OwnedPinnedRepositorySources> result;
  REQUIRE(until([&] {
    auto opened = open_sources(repository);
    if (opened) {
      result.emplace(std::move(*opened));
      return true;
    }
    REQUIRE(opened.error().message ==
            "Repository source retirement capacity remains occupied");
    return false;
  }));
  return std::move(*result);
}
} // namespace
TEST_CASE("Owned pinned repository aliases retain the complete adapter graph "
          "without cycles") {
  Repository repository;
  auto owned_sources = await_sources(repository);
  auto* sources = &owned_sources;
  std::weak_ptr<adapters::GitRepositorySnapshotSource> weak =
      sources->snapshots;
  SECTION("controller retains all sources") {
    auto owned = runtime::RepositoryContextController::create_owned(
        sources->context, sources->authority->baseline().root);
    REQUIRE(owned);
    *sources = {};
    CHECK_FALSE(weak.expired());
    auto prepared = (*owned)->prepare({"", 1, {"file.txt"}});
    INFO((prepared ? "prepared" : prepared.error().message));
    REQUIRE(prepared);
    CHECK(prepared->instructions.size() == 1);
    CHECK(prepared->admission.evidence.size() == 1);
    owned->reset();
  }
  SECTION("authority retains borrowed exact and snapshot adapters") {
    auto authority = sources->authority;
    *sources = {};
    auto read = authority->read_exact({authority->baseline(), "file.txt", {}});
    INFO((read ? "read" : read.error().message));
    REQUIRE(read);
    CHECK(read->content == "hello");
    authority.reset();
  }
  CHECK(weak.expired());
}
TEST_CASE("Repository preparation stays unsealed and completes exactly once") {
  Fixture f;
  REQUIRE(f.worker->submit(f.controller, request()));
  auto result = f.completed();
  REQUIRE(result.result);
  CHECK_FALSE(result.result->admission.admission_digest);
  CHECK(result.result->admission.capacity == domain::ContextCapacity{});
  CHECK(result.result->admission.evidence.size() == 2);
  CHECK_FALSE(f.worker->poll(token()));
}

TEST_CASE("Last production repository aliases retire physical roots off owner "
          "and hold graph capacity through blocked cleanup") {
  Repository repository;
  auto first = await_sources(repository);
  auto second = await_sources(repository);
  struct stat identity{};
  REQUIRE(::stat(repository.path.c_str(), &identity) == 0);
  auto trap = std::make_shared<CloseTrap>();
  trap->device = identity.st_dev;
  trap->inode = identity.st_ino;
  ResetTrap reset{trap};
  close_trap.store(trap);
  // Releasing all aliases must only signal the already-running reclaimer.
  // The physical close barrier cannot hold up the caller releasing aliases.
  returns_before_release(
      [&] {
        trap->owner = std::this_thread::get_id();
        first = {};
        second = {};
      },
      trap->gate);
  REQUIRE(until([&] { return trap->entered.load() >= 2; }));
  CHECK_FALSE(trap->on_owner.load());
  CHECK(trap->completed.load() == 0);
  auto busy = open_sources(repository);
  REQUIRE_FALSE(busy);
  CHECK(busy.error().code ==
        runtime::AutomaticApprovalMatcherErrorCode::path_unavailable);
  CHECK(busy.error().message ==
        "Repository source retirement capacity remains occupied");
  trap->gate->release();
  REQUIRE(until([&] { return trap->completed.load() >= 2; }));
  close_trap.store({});
  // Cleanup completion returns capacity without a join or another cleanup job.
  auto replacement = await_sources(repository);
  REQUIRE(replacement.context);
}
