#include <aiforge/adapters/git_repository_snapshot_source.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
#include <stop_token>
#include <string>
#include <string_view>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace {

using namespace aiforge;

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    static std::atomic<unsigned> sequence{};
    m_path = std::filesystem::temp_directory_path() /
             ("aiforge-repository-test-" + std::to_string(::getpid()) + "-" +
              std::to_string(sequence.fetch_add(1)));
    std::filesystem::create_directories(m_path);
  }

  ~TemporaryDirectory() {
    if (m_path.empty()) return;
    std::error_code error;
    std::filesystem::remove_all(m_path, error);
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  auto operator=(const TemporaryDirectory&) -> TemporaryDirectory& = delete;
  TemporaryDirectory(TemporaryDirectory&& other) noexcept
      : m_path(std::move(other.m_path)) {
    other.m_path.clear();
  }
  auto operator=(TemporaryDirectory&& other) noexcept
      -> TemporaryDirectory& {
    if (this == &other) return *this;
    std::error_code error;
    if (!m_path.empty()) std::filesystem::remove_all(m_path, error);
    m_path = std::move(other.m_path);
    other.m_path.clear();
    return *this;
  }

  [[nodiscard]] auto path() const -> const std::filesystem::path& {
    return m_path;
  }

 private:
  std::filesystem::path m_path;
};

auto write_file(const std::filesystem::path& path, const std::string_view text)
    -> void {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  REQUIRE(output);
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
  REQUIRE(output);
}

auto shell_quote(const std::filesystem::path& path) -> std::string {
  std::string result{"'"};
  for (const char value : path.string()) {
    if (value == '\'') result.append("'\\''");
    else result.push_back(value);
  }
  result.push_back('\'');
  return result;
}

auto git(const std::filesystem::path& root, const std::string_view arguments)
    -> void {
  const auto command = shell_quote(REPOSITORY_TEST_GIT) + " -C " +
                       shell_quote(root) + " " + std::string{arguments} +
                       " >/dev/null 2>&1";
  REQUIRE(std::system(command.c_str()) == 0);
}

auto initialized_repository() -> TemporaryDirectory {
  TemporaryDirectory directory;
  git(directory.path(), "init -q");
  git(directory.path(), "config user.email test@example.invalid");
  git(directory.path(), "config user.name Test");
  write_file(directory.path() / ".gitignore", "ignored.txt\n");
  write_file(directory.path() / "tracked.txt", "first\n");
  git(directory.path(), "add .gitignore tracked.txt");
  git(directory.path(), "commit -qm initial");
  return directory;
}

auto source() -> adapters::GitRepositorySnapshotSource {
  auto opened = adapters::GitRepositorySnapshotSource::open(REPOSITORY_TEST_GIT);
  REQUIRE(opened);
  return std::move(*opened);
}

auto write_executable(const std::filesystem::path& path,
                      const std::string_view script) -> void {
  write_file(path, script);
  std::filesystem::permissions(
      path, std::filesystem::perms::owner_read |
                std::filesystem::perms::owner_write |
                std::filesystem::perms::owner_exec);
}

}  // namespace

TEST_CASE("Git repository source validates executable and root failures") {
  auto relative = adapters::GitRepositorySnapshotSource::open("git");
  REQUIRE_FALSE(relative);
  REQUIRE(relative.error().code ==
          repository::RepositorySnapshotErrorCode::invalid_request);

  auto observer = source();
  auto missing = observer.observe({"/aiforge/definitely/missing", {}});
  REQUIRE_FALSE(missing);
  REQUIRE(missing.error().code ==
          repository::RepositorySnapshotErrorCode::not_found);

  TemporaryDirectory directory;
  write_file(directory.path() / "file", "text");
  auto file = observer.observe({(directory.path() / "file").string(), {}});
  REQUIRE_FALSE(file);
  REQUIRE(file.error().code ==
          repository::RepositorySnapshotErrorCode::not_directory);

  std::stop_source cancelled;
  cancelled.request_stop();
  auto stopped = observer.observe({directory.path().string(), {}},
                                  cancelled.get_token());
  REQUIRE_FALSE(stopped);
  REQUIRE(stopped.error().code ==
          repository::RepositorySnapshotErrorCode::cancelled);
}

TEST_CASE("plain repository snapshots hash files and symlinks without following") {
  TemporaryDirectory directory;
  write_file(directory.path() / "a.txt", "alpha");
  write_file(directory.path() / "sub" / "b.txt", "beta");
  std::filesystem::create_symlink("a.txt", directory.path() / "link");

  auto observer = source();
  auto first = observer.observe({directory.path().string(), {}});
  INFO((first ? std::string{} : first.error().message));
  REQUIRE(first);
  REQUIRE_FALSE(first->vcs);
  REQUIRE(first->changes.size() == 3);
  REQUIRE(std::ranges::all_of(first->changes, [](const auto& change) {
    return change.change_kind == domain::RepositoryChangeKind::untracked &&
           change.worktree_digest.has_value();
  }));
  const auto link = std::ranges::find(first->changes, "link",
                                      &domain::RepositoryChange::relative_path);
  REQUIRE(link != first->changes.end());
  REQUIRE(link->entry_kind == domain::RepositoryEntryKind::symbolic_link);
  REQUIRE(link->worktree_digest->byte_size == 5);

  auto second = observer.observe({directory.path().string(), {}});
  REQUIRE(second);
  REQUIRE(domain::same_source_state(*first, *second));
  write_file(directory.path() / "a.txt", "changed");
  auto changed = observer.observe({directory.path().string(), {}});
  REQUIRE(changed);
  REQUIRE_FALSE(domain::same_source_state(*first, *changed));
}

TEST_CASE("Git snapshots resolve aliases and describe branch dirty and detached state") {
  auto directory = initialized_repository();
  std::filesystem::create_directories(directory.path() / "nested");
  write_file(directory.path() / "ignored.txt", "ignored");

  auto observer = source();
  auto clean = observer.observe({(directory.path() / "nested").string(), {}});
  REQUIRE(clean);
  REQUIRE(clean->vcs);
  REQUIRE(clean->vcs->head_kind == domain::VcsHeadKind::branch);
  REQUIRE(clean->vcs->branch);
  REQUIRE(clean->changes.empty());

  TemporaryDirectory aliases;
  const auto alias = aliases.path() / "repository";
  std::filesystem::create_directory_symlink(directory.path(), alias);
  auto through_alias = observer.observe({alias.string(), {}});
  REQUIRE(through_alias);
  REQUIRE(through_alias->root == clean->root);
  REQUIRE(domain::same_source_state(*clean, *through_alias));

  write_file(directory.path() / "tracked.txt", "modified\n");
  write_file(directory.path() / "untracked name.txt", "new\n");
  auto dirty = observer.observe({directory.path().string(), {}});
  REQUIRE(dirty);
  REQUIRE(dirty->changes.size() == 2);
  REQUIRE_FALSE(domain::same_source_state(*clean, *dirty));

  git(directory.path(), "checkout -q --detach");
  auto detached = observer.observe({directory.path().string(), {}});
  REQUIRE(detached);
  REQUIRE(detached->vcs->head_kind == domain::VcsHeadKind::detached);
  REQUIRE_FALSE(detached->vcs->branch);
  REQUIRE(detached->vcs->revision);
}

TEST_CASE("Git snapshots distinguish unborn and nested repository roots") {
  TemporaryDirectory unborn_directory;
  git(unborn_directory.path(), "init -q");
  auto observer = source();
  auto unborn = observer.observe({unborn_directory.path().string(), {}});
  REQUIRE(unborn);
  REQUIRE(unborn->vcs);
  REQUIRE(unborn->vcs->head_kind == domain::VcsHeadKind::unborn);
  REQUIRE(unborn->vcs->branch);
  REQUIRE_FALSE(unborn->vcs->revision);

  auto outer = initialized_repository();
  const auto nested = outer.path() / "nested";
  std::filesystem::create_directories(nested);
  git(nested, "init -q");
  git(nested, "config user.email test@example.invalid");
  git(nested, "config user.name Test");
  write_file(nested / "nested.txt", "nested\n");
  git(nested, "add nested.txt");
  git(nested, "commit -qm nested");

  auto outer_snapshot = observer.observe({outer.path().string(), {}});
  auto nested_snapshot = observer.observe({nested.string(), {}});
  INFO((outer_snapshot ? std::string{} : outer_snapshot.error().message));
  INFO((nested_snapshot ? std::string{} : nested_snapshot.error().message));
  REQUIRE(outer_snapshot);
  REQUIRE(nested_snapshot);
  REQUIRE(outer_snapshot->root.repository_id !=
          nested_snapshot->root.repository_id);
  REQUIRE(nested_snapshot->root.canonical_path ==
          std::filesystem::canonical(nested).generic_string());
}

TEST_CASE("Git snapshots represent staged rename and enforce observation budgets") {
  auto directory = initialized_repository();
  git(directory.path(), "mv tracked.txt renamed.txt");

  auto observer = source();
  auto renamed = observer.observe({directory.path().string(), {}});
  REQUIRE(renamed);
  REQUIRE(renamed->changes.size() == 1);
  REQUIRE(renamed->changes.front().change_kind ==
          domain::RepositoryChangeKind::renamed);
  REQUIRE(renamed->changes.front().previous_path == "tracked.txt");
  REQUIRE(renamed->changes.front().relative_path == "renamed.txt");

  repository::RepositorySnapshotLimits limits;
  limits.maximum_entries = 1;
  write_file(directory.path() / "one", "1");
  write_file(directory.path() / "two", "2");
  auto too_many = observer.observe({directory.path().string(), limits});
  REQUIRE_FALSE(too_many);
  REQUIRE(too_many.error().code ==
          repository::RepositorySnapshotErrorCode::resource_exhausted);

  limits = {};
  limits.maximum_file_bytes = 1;
  auto too_large = observer.observe({directory.path().string(), limits});
  REQUIRE_FALSE(too_large);
  REQUIRE(too_large.error().code ==
          repository::RepositorySnapshotErrorCode::resource_exhausted);
}

TEST_CASE("Git repository source bounds malformed oversized and slow commands") {
  TemporaryDirectory directory;
  const auto malformed_git = directory.path() / "malformed-git";
  const auto canonical = std::filesystem::canonical(directory.path()).string();
  write_executable(
      malformed_git,
      "#!/bin/sh\n"
      "case \"$*\" in\n"
      "  *--show-toplevel*) printf '%s\\n' '" + canonical + "' ;;\n"
      "  *--show-object-format*) printf 'sha1\\n' ;;\n"
      "  *status*) printf 'broken\\0' ;;\n"
      "  *) exit 1 ;;\n"
      "esac\n");
  auto malformed = adapters::GitRepositorySnapshotSource::open(
      malformed_git.string());
  REQUIRE(malformed);
  auto malformed_result =
      malformed->observe({directory.path().string(), {}});
  REQUIRE_FALSE(malformed_result);
  REQUIRE(malformed_result.error().code ==
          repository::RepositorySnapshotErrorCode::vcs_failure);

  const auto noisy_git = directory.path() / "noisy-git";
  write_executable(noisy_git,
                   "#!/bin/sh\nprintf '0123456789abcdef'\n");
  auto noisy = adapters::GitRepositorySnapshotSource::open(noisy_git.string());
  REQUIRE(noisy);
  repository::RepositorySnapshotLimits tiny_output;
  tiny_output.maximum_command_output_bytes = 8;
  auto oversized = noisy->observe({directory.path().string(), tiny_output});
  REQUIRE_FALSE(oversized);
  REQUIRE(oversized.error().code ==
          repository::RepositorySnapshotErrorCode::resource_exhausted);

  const auto slow_git = directory.path() / "slow-git";
  write_executable(slow_git, "#!/bin/sh\nsleep 2\n");
  auto slow = adapters::GitRepositorySnapshotSource::open(slow_git.string());
  REQUIRE(slow);
  repository::RepositorySnapshotLimits short_timeout;
  short_timeout.command_timeout = std::chrono::milliseconds{20};
  auto timed_out = slow->observe({directory.path().string(), short_timeout});
  REQUIRE_FALSE(timed_out);
  REQUIRE(timed_out.error().code ==
          repository::RepositorySnapshotErrorCode::timed_out);
  REQUIRE(timed_out.error().retryable);

  auto cancellable = adapters::GitRepositorySnapshotSource::open(slow_git.string());
  REQUIRE(cancellable);
  std::stop_source stop;
  std::jthread canceller{[&] {
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    stop.request_stop();
  }};
  auto cancelled = cancellable->observe({directory.path().string(), {}},
                                        stop.get_token());
  REQUIRE_FALSE(cancelled);
  REQUIRE(cancelled.error().code ==
          repository::RepositorySnapshotErrorCode::cancelled);

  write_file(directory.path() / "first", "first");
  write_file(directory.path() / "second", "second");
  const auto changing_git = directory.path() / "changing-git";
  const auto counter = directory.path() / "counter";
  write_executable(
      changing_git,
      "#!/bin/sh\n"
      "case \"$*\" in\n"
      "  *--show-toplevel*) printf '%s\\n' '" + canonical + "' ;;\n"
      "  *--show-object-format*) printf 'sha1\\n' ;;\n"
      "  *status*) printf '# branch.oid aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\\0# branch.head main\\0'; "
          "if [ -e '" + counter.string() +
          "' ]; then printf '? second\\0'; else : > '" + counter.string() +
          "'; printf '? first\\0'; fi ;;\n"
      "  *hash-object*) cat >/dev/null; printf 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\\n' ;;\n"
      "  *) exit 1 ;;\n"
      "esac\n");
  auto changing = adapters::GitRepositorySnapshotSource::open(
      changing_git.string());
  REQUIRE(changing);
  auto unstable = changing->observe({directory.path().string(), {}});
  REQUIRE_FALSE(unstable);
  REQUIRE(unstable.error().code ==
          repository::RepositorySnapshotErrorCode::unstable);
  REQUIRE(unstable.error().retryable);
}
