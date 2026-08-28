#include <aiforge/adapters/git_exact_source_editor.hpp>
#include <aiforge/adapters/git_project_instruction_source.hpp>
#include <aiforge/adapters/git_repository_snapshot_source.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

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
  auto operator=(TemporaryDirectory&& other) noexcept -> TemporaryDirectory& {
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

auto read_file(const std::filesystem::path& path) -> std::string {
  std::ifstream input{path, std::ios::binary};
  REQUIRE(input);
  return {std::istreambuf_iterator<char>{input},
          std::istreambuf_iterator<char>{}};
}

auto shell_quote(const std::filesystem::path& path) -> std::string {
  std::string result{"'"};
  for (const char value : path.string()) {
    if (value == '\'')
      result.append("'\\''");
    else
      result.push_back(value);
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
  auto opened =
      adapters::GitRepositorySnapshotSource::open(REPOSITORY_TEST_GIT);
  REQUIRE(opened);
  return std::move(*opened);
}

auto write_executable(const std::filesystem::path& path,
                      const std::string_view script) -> void {
  write_file(path, script);
  std::filesystem::permissions(path, std::filesystem::perms::owner_read |
                                         std::filesystem::perms::owner_write |
                                         std::filesystem::perms::owner_exec);
}

} // namespace

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
  auto stopped =
      observer.observe({directory.path().string(), {}}, cancelled.get_token());
  REQUIRE_FALSE(stopped);
  REQUIRE(stopped.error().code ==
          repository::RepositorySnapshotErrorCode::cancelled);
}

TEST_CASE(
    "plain repository snapshots hash files and symlinks without following") {
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
  const auto file = std::ranges::find(first->changes, "a.txt",
                                      &domain::RepositoryChange::relative_path);
  REQUIRE(file != first->changes.end());
  REQUIRE(file->worktree_digest != link->worktree_digest);

  auto second = observer.observe({directory.path().string(), {}});
  REQUIRE(second);
  REQUIRE(domain::same_source_state(*first, *second));
  write_file(directory.path() / "a.txt", "changed");
  auto changed = observer.observe({directory.path().string(), {}});
  REQUIRE(changed);
  REQUIRE_FALSE(domain::same_source_state(*first, *changed));
}

TEST_CASE("project instructions are discovered from root to target subtree") {
  auto directory = initialized_repository();
  write_file(directory.path() / "AGENTS.md", "root rules\n");
  write_file(directory.path() / "src" / "AGENTS.md", "nested rules\n");
  write_file(directory.path() / "sibling" / "AGENTS.md", "sibling rules\n");
  std::filesystem::create_directories(directory.path() / "src" / "lib");

  auto observer = source();
  const auto baseline = observer.observe({directory.path().string(), {}});
  INFO((baseline ? std::string{} : baseline.error().message));
  REQUIRE(baseline);
  adapters::GitProjectInstructionSource instructions{observer};
  const auto result = instructions.discover({*baseline, "src/lib", {}});
  INFO((result ? std::string{} : result.error().message));
  REQUIRE(result);
  REQUIRE(result->source_snapshot == domain::snapshot_identity(*baseline));
  REQUIRE(result->target_subtree == "src/lib");
  REQUIRE(result->documents.size() == 2);
  REQUIRE(result->documents[0].source.relative_path == "AGENTS.md");
  REQUIRE(result->documents[0].applicable_subtree.empty());
  REQUIRE(result->documents[0].specificity == 0);
  REQUIRE(result->documents[0].discovery_order == 1);
  REQUIRE(result->documents[0].text == "root rules\n");
  REQUIRE(result->documents[1].source.relative_path == "src/AGENTS.md");
  REQUIRE(result->documents[1].applicable_subtree == "src");
  REQUIRE(result->documents[1].specificity == 1);
  REQUIRE(result->documents[1].discovery_order == 2);
  REQUIRE(result->documents[1].text == "nested rules\n");
  REQUIRE(result->documents[0].source.content_digest.byte_size == 11);
}

TEST_CASE("project instruction discovery fails closed on paths and content") {
  auto directory = initialized_repository();
  std::filesystem::create_directories(directory.path() / "target");
  auto observer = source();
  auto baseline = observer.observe({directory.path().string(), {}});
  REQUIRE(baseline);
  adapters::GitProjectInstructionSource instructions{observer};

  auto invalid = instructions.discover({*baseline, "../escape", {}});
  REQUIRE_FALSE(invalid);
  REQUIRE(invalid.error().code ==
          repository::ProjectInstructionErrorCode::invalid_request);

  auto missing = instructions.discover({*baseline, "missing", {}});
  REQUIRE_FALSE(missing);
  REQUIRE(missing.error().code ==
          repository::ProjectInstructionErrorCode::not_found);

  std::filesystem::create_directory_symlink(directory.path().parent_path(),
                                            directory.path() / "escape");
  auto escaped = instructions.discover({*baseline, "escape", {}});
  REQUIRE_FALSE(escaped);
  REQUIRE(escaped.error().code ==
          repository::ProjectInstructionErrorCode::outside_repository);

  std::filesystem::remove(directory.path() / "escape");
  write_file(directory.path() / "real-agents", "rules\n");
  std::filesystem::create_symlink("real-agents",
                                  directory.path() / "AGENTS.md");
  baseline = observer.observe({directory.path().string(), {}});
  REQUIRE(baseline);
  auto symlink = instructions.discover({*baseline, "", {}});
  REQUIRE_FALSE(symlink);
  REQUIRE(symlink.error().code ==
          repository::ProjectInstructionErrorCode::unsupported_entry);

  std::filesystem::remove(directory.path() / "AGENTS.md");
  write_file(directory.path() / "AGENTS.md", std::string{"bad\0text", 8});
  baseline = observer.observe({directory.path().string(), {}});
  REQUIRE(baseline);
  auto malformed = instructions.discover({*baseline, "", {}});
  REQUIRE_FALSE(malformed);
  REQUIRE(malformed.error().code ==
          repository::ProjectInstructionErrorCode::malformed_text);

  write_file(directory.path() / "AGENTS.md", "12345");
  baseline = observer.observe({directory.path().string(), {}});
  REQUIRE(baseline);
  repository::ProjectInstructionLimits limits;
  limits.maximum_document_bytes = 4;
  limits.maximum_total_bytes = 4;
  auto oversized = instructions.discover({*baseline, "", limits});
  REQUIRE_FALSE(oversized);
  REQUIRE(oversized.error().code ==
          repository::ProjectInstructionErrorCode::resource_exhausted);
}

TEST_CASE(
    "project instruction discovery detects stale baselines and cancellation") {
  auto directory = initialized_repository();
  write_file(directory.path() / "AGENTS.md", "first\n");
  auto observer = source();
  const auto baseline = observer.observe({directory.path().string(), {}});
  REQUIRE(baseline);
  adapters::GitProjectInstructionSource instructions{observer};

  write_file(directory.path() / "AGENTS.md", "second\n");
  auto stale = instructions.discover({*baseline, "", {}});
  REQUIRE_FALSE(stale);
  REQUIRE(stale.error().code ==
          repository::ProjectInstructionErrorCode::stale_snapshot);

  const auto current = observer.observe({directory.path().string(), {}});
  REQUIRE(current);
  std::stop_source stopped;
  stopped.request_stop();
  auto cancelled =
      instructions.discover({*current, "", {}}, stopped.get_token());
  REQUIRE_FALSE(cancelled);
  REQUIRE(cancelled.error().code ==
          repository::ProjectInstructionErrorCode::cancelled);

  write_file(directory.path() / "AGENTS.md", "");
  const auto empty_baseline = observer.observe({directory.path().string(), {}});
  REQUIRE(empty_baseline);
  auto empty = instructions.discover({*empty_baseline, "", {}});
  REQUIRE(empty);
  REQUIRE(empty->documents.empty());
}

TEST_CASE(
    "project instruction discovery rejects repository changes during reads") {
  TemporaryDirectory directory;
  const auto fake_git = directory.path() / "git";
  const auto counter = directory.path() / "status-count";
  const auto canonical = std::filesystem::canonical(directory.path()).string();
  write_executable(
      fake_git,
      "#!/bin/sh\n"
      "case \"$*\" in\n"
      "  *--show-toplevel*) printf '%s\\n' '" +
          canonical +
          "' ;;\n"
          "  *--show-object-format*) printf 'sha1\\n' ;;\n"
          "  *status*) n=0; [ -f '" +
          counter.string() + "' ] && n=$(cat '" + counter.string() +
          "'); n=$((n + 1)); printf '%s' \"$n\" > '" + counter.string() +
          "'; if [ \"$n\" -le 4 ]; then "
          "oid=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa; "
          "else oid=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb; fi; "
          "printf '# branch.oid %s\\0# branch.head main\\0' \"$oid\" ;;\n"
          "  *hash-object*) cat >/dev/null; printf "
          "'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\\n' ;;\n"
          "  *) exit 1 ;;\n"
          "esac\n");
  auto opened = adapters::GitRepositorySnapshotSource::open(fake_git.string());
  REQUIRE(opened);
  const auto baseline = opened->observe({directory.path().string(), {}});
  REQUIRE(baseline);
  adapters::GitProjectInstructionSource instructions{*opened};
  const auto result = instructions.discover({*baseline, "", {}});
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::ProjectInstructionErrorCode::unstable);
}

TEST_CASE("Git snapshots resolve aliases and describe branch dirty and "
          "detached state") {
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

TEST_CASE(
    "Git snapshots represent staged rename and enforce observation budgets") {
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

TEST_CASE(
    "Git repository source bounds malformed oversized and slow commands") {
  TemporaryDirectory directory;
  const auto malformed_git = directory.path() / "malformed-git";
  const auto canonical = std::filesystem::canonical(directory.path()).string();
  write_executable(malformed_git,
                   "#!/bin/sh\n"
                   "case \"$*\" in\n"
                   "  *--show-toplevel*) printf '%s\\n' '" +
                       canonical +
                       "' ;;\n"
                       "  *--show-object-format*) printf 'sha1\\n' ;;\n"
                       "  *status*) printf 'broken\\0' ;;\n"
                       "  *) exit 1 ;;\n"
                       "esac\n");
  auto malformed =
      adapters::GitRepositorySnapshotSource::open(malformed_git.string());
  REQUIRE(malformed);
  auto malformed_result = malformed->observe({directory.path().string(), {}});
  REQUIRE_FALSE(malformed_result);
  REQUIRE(malformed_result.error().code ==
          repository::RepositorySnapshotErrorCode::vcs_failure);

  const auto noisy_git = directory.path() / "noisy-git";
  write_executable(noisy_git, "#!/bin/sh\nprintf '0123456789abcdef'\n");
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

  auto cancellable =
      adapters::GitRepositorySnapshotSource::open(slow_git.string());
  REQUIRE(cancellable);
  std::stop_source stop;
  std::jthread canceller{[&] {
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    stop.request_stop();
  }};
  auto cancelled =
      cancellable->observe({directory.path().string(), {}}, stop.get_token());
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
      "  *--show-toplevel*) printf '%s\\n' '" +
          canonical +
          "' ;;\n"
          "  *--show-object-format*) printf 'sha1\\n' ;;\n"
          "  *status*) printf '# branch.oid "
          "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\\0# branch.head main\\0'; "
          "if [ -e '" +
          counter.string() + "' ]; then printf '? second\\0'; else : > '" +
          counter.string() +
          "'; printf '? first\\0'; fi ;;\n"
          "  *hash-object*) cat >/dev/null; printf "
          "'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\\n' ;;\n"
          "  *) exit 1 ;;\n"
          "esac\n");
  auto changing =
      adapters::GitRepositorySnapshotSource::open(changing_git.string());
  REQUIRE(changing);
  auto unstable = changing->observe({directory.path().string(), {}});
  REQUIRE_FALSE(unstable);
  REQUIRE(unstable.error().code ==
          repository::RepositorySnapshotErrorCode::unstable);
  REQUIRE(unstable.error().retryable);
}

TEST_CASE(
    "exact-source reads fail closed on missing unsupported and aliased paths",
    "[repository][edit][failure]") {
  auto directory = initialized_repository();
  TemporaryDirectory outside;
  write_file(outside.path() / "file.cpp", "outside\n");
  std::filesystem::create_directories(directory.path() / "folder");
  write_file(directory.path() / "linked-target", "linked\n");
  std::filesystem::create_symlink("linked-target",
                                  directory.path() / "symbolic");
  write_file(directory.path() / "aliased", "alias\n");
  std::filesystem::create_hard_link(directory.path() / "aliased",
                                    directory.path() / "hard-link");
  std::filesystem::create_directory_symlink(outside.path(),
                                            directory.path() / "escape");

  auto observer = source();
  const auto baseline = observer.observe({directory.path().string(), {}});
  INFO((baseline ? std::string{} : baseline.error().message));
  REQUIRE(baseline);
  adapters::GitExactSourceEditor editor{observer};

  const auto missing = editor.read({*baseline, "missing.cpp", {}});
  REQUIRE_FALSE(missing);
  REQUIRE(missing.error().code ==
          repository::ExactSourceEditErrorCode::not_found);
  const auto directory_result = editor.read({*baseline, "folder", {}});
  REQUIRE_FALSE(directory_result);
  REQUIRE(directory_result.error().code ==
          repository::ExactSourceEditErrorCode::unsupported_entry);
  const auto symlink = editor.read({*baseline, "symbolic", {}});
  REQUIRE_FALSE(symlink);
  REQUIRE(symlink.error().code ==
          repository::ExactSourceEditErrorCode::outside_repository);
  const auto escaped = editor.read({*baseline, "escape/file.cpp", {}});
  REQUIRE_FALSE(escaped);
  REQUIRE(escaped.error().code ==
          repository::ExactSourceEditErrorCode::outside_repository);
  const auto hard_link = editor.read({*baseline, "aliased", {}});
  REQUIRE_FALSE(hard_link);
  REQUIRE(hard_link.error().code ==
          repository::ExactSourceEditErrorCode::outside_repository);
  const auto traversal = editor.read({*baseline, "../tracked.txt", {}});
  REQUIRE_FALSE(traversal);
  REQUIRE(traversal.error().code ==
          repository::ExactSourceEditErrorCode::invalid_request);
}

TEST_CASE(
    "exact-source edits reject stale repository and preserve user content",
    "[repository][edit][failure]") {
  auto directory = initialized_repository();
  auto observer = source();
  const auto baseline = observer.observe({directory.path().string(), {}});
  REQUIRE(baseline);
  adapters::GitExactSourceEditor editor{observer};
  const auto exact = editor.read({*baseline, "tracked.txt", {}});
  REQUIRE(exact);

  write_file(directory.path() / "tracked.txt", "user edit\n");
  const repository::ExactSourceEditRequest request{
      *baseline, exact->source, {0, 5}, "agent", {}};
  const auto stale = editor.apply(request);
  REQUIRE_FALSE(stale);
  REQUIRE(stale.error().code ==
          repository::ExactSourceEditErrorCode::stale_snapshot);
  REQUIRE_FALSE(stale.error().may_have_applied);
  REQUIRE(read_file(directory.path() / "tracked.txt") == "user edit\n");

  auto second_directory = initialized_repository();
  auto second_observer = source();
  const auto second_baseline =
      second_observer.observe({second_directory.path().string(), {}});
  REQUIRE(second_baseline);
  adapters::GitExactSourceEditor second_editor{second_observer};
  const auto second_exact =
      second_editor.read({*second_baseline, "tracked.txt", {}});
  REQUIRE(second_exact);
  write_file(second_directory.path() / "unrelated.txt", "new\n");
  const auto unrelated = second_editor.apply(
      {*second_baseline, second_exact->source, {0, 5}, "agent", {}});
  REQUIRE_FALSE(unrelated);
  REQUIRE(unrelated.error().code ==
          repository::ExactSourceEditErrorCode::stale_snapshot);
  REQUIRE(read_file(second_directory.path() / "tracked.txt") == "first\n");
}

TEST_CASE(
    "exact-source edits reject deleted renamed branch and digest conflicts",
    "[repository][edit][failure]") {
  auto deleted_directory = initialized_repository();
  auto deleted_observer = source();
  const auto deleted_baseline =
      deleted_observer.observe({deleted_directory.path().string(), {}});
  REQUIRE(deleted_baseline);
  adapters::GitExactSourceEditor deleted_editor{deleted_observer};
  const auto deleted_source =
      deleted_editor.read({*deleted_baseline, "tracked.txt", {}});
  REQUIRE(deleted_source);
  std::filesystem::remove(deleted_directory.path() / "tracked.txt");
  const auto deleted = deleted_editor.apply(
      {*deleted_baseline, deleted_source->source, {0, 5}, "agent", {}});
  REQUIRE_FALSE(deleted);
  REQUIRE(deleted.error().code ==
          repository::ExactSourceEditErrorCode::stale_snapshot);
  REQUIRE_FALSE(
      std::filesystem::exists(deleted_directory.path() / "tracked.txt"));

  auto renamed_directory = initialized_repository();
  auto renamed_observer = source();
  const auto renamed_baseline =
      renamed_observer.observe({renamed_directory.path().string(), {}});
  REQUIRE(renamed_baseline);
  adapters::GitExactSourceEditor renamed_editor{renamed_observer};
  const auto renamed_source =
      renamed_editor.read({*renamed_baseline, "tracked.txt", {}});
  REQUIRE(renamed_source);
  std::filesystem::rename(renamed_directory.path() / "tracked.txt",
                          renamed_directory.path() / "moved.txt");
  const auto renamed = renamed_editor.apply(
      {*renamed_baseline, renamed_source->source, {0, 5}, "agent", {}});
  REQUIRE_FALSE(renamed);
  REQUIRE(renamed.error().code ==
          repository::ExactSourceEditErrorCode::stale_snapshot);
  REQUIRE(read_file(renamed_directory.path() / "moved.txt") == "first\n");

  auto branch_directory = initialized_repository();
  auto branch_observer = source();
  const auto branch_baseline =
      branch_observer.observe({branch_directory.path().string(), {}});
  REQUIRE(branch_baseline);
  adapters::GitExactSourceEditor branch_editor{branch_observer};
  const auto branch_source =
      branch_editor.read({*branch_baseline, "tracked.txt", {}});
  REQUIRE(branch_source);
  git(branch_directory.path(), "switch -qc other");
  const auto changed_branch = branch_editor.apply(
      {*branch_baseline, branch_source->source, {0, 5}, "agent", {}});
  REQUIRE_FALSE(changed_branch);
  REQUIRE(changed_branch.error().code ==
          repository::ExactSourceEditErrorCode::stale_snapshot);
  REQUIRE(read_file(branch_directory.path() / "tracked.txt") == "first\n");

  auto mismatch_directory = initialized_repository();
  auto mismatch_observer = source();
  const auto mismatch_baseline =
      mismatch_observer.observe({mismatch_directory.path().string(), {}});
  REQUIRE(mismatch_baseline);
  adapters::GitExactSourceEditor mismatch_editor{mismatch_observer};
  const auto mismatch_source =
      mismatch_editor.read({*mismatch_baseline, "tracked.txt", {}});
  REQUIRE(mismatch_source);
  auto invented = mismatch_source->source;
  invented.content_digest.value = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  const auto mismatch = mismatch_editor.apply(
      {*mismatch_baseline, invented, {0, 5}, "agent", {}});
  REQUIRE_FALSE(mismatch);
  REQUIRE(mismatch.error().code ==
          repository::ExactSourceEditErrorCode::source_mismatch);
  REQUIRE(mismatch.error().observed_source == mismatch_source->source);
  REQUIRE(read_file(mismatch_directory.path() / "tracked.txt") == "first\n");
}

TEST_CASE(
    "exact-source edits serialize shared baselines and preserve permissions",
    "[repository][edit][concurrency]") {
  auto directory = initialized_repository();
  const auto target = directory.path() / "tracked.txt";
  std::filesystem::permissions(target, std::filesystem::perms::owner_read |
                                           std::filesystem::perms::owner_write |
                                           std::filesystem::perms::group_read);
  auto first_observer = source();
  auto second_observer = source();
  const auto baseline = first_observer.observe({directory.path().string(), {}});
  REQUIRE(baseline);
  adapters::GitExactSourceEditor first_editor{first_observer};
  adapters::GitExactSourceEditor second_editor{second_observer};
  const auto exact = first_editor.read({*baseline, "tracked.txt", {}});
  REQUIRE(exact);

  const repository::ExactSourceEditRequest request{
      *baseline, exact->source, {0, 5}, "second", {}};
  const auto applied = first_editor.apply(request);
  INFO((applied ? std::string{} : applied.error().message));
  REQUIRE(applied);
  REQUIRE(read_file(target) == "second\n");
  REQUIRE(applied->previous_source == exact->source);
  REQUIRE(applied->replaced_range == domain::SourceByteRange{0, 5});
  REQUIRE(applied->resulting_range == domain::SourceByteRange{0, 6});
  REQUIRE(applied->resulting_source.content_digest.byte_size == 7);
  const auto permissions = std::filesystem::status(target).permissions();
  REQUIRE((permissions & std::filesystem::perms::owner_write) !=
          std::filesystem::perms::none);
  REQUIRE((permissions & std::filesystem::perms::group_read) !=
          std::filesystem::perms::none);

  const auto conflicted = second_editor.apply(request);
  REQUIRE_FALSE(conflicted);
  REQUIRE(conflicted.error().code ==
          repository::ExactSourceEditErrorCode::stale_snapshot);
  REQUIRE(read_file(target) == "second\n");
}

TEST_CASE("exact-source reads accept captured dirty state and edits support "
          "insertion",
          "[repository][edit][smoke]") {
  auto directory = initialized_repository();
  write_file(directory.path() / "tracked.txt", "dirty\n");
  auto observer = source();
  const auto baseline = observer.observe({directory.path().string(), {}});
  REQUIRE(baseline);
  REQUIRE_FALSE(baseline->changes.empty());
  adapters::GitExactSourceEditor editor{observer};
  const auto exact = editor.read({*baseline, "tracked.txt", {}});
  INFO((exact ? std::string{} : exact.error().message));
  REQUIRE(exact);
  REQUIRE(exact->content == "dirty\n");
  REQUIRE(exact->source.content_digest.byte_size == 6);

  const repository::ExactSourceEditRequest insert{
      *baseline, exact->source, {6, 6}, "tail\n", {}};
  const auto applied = editor.apply(insert);
  INFO((applied ? std::string{} : applied.error().message));
  REQUIRE(applied);
  REQUIRE(read_file(directory.path() / "tracked.txt") == "dirty\ntail\n");
  REQUIRE(applied->resulting_source.content_digest.byte_size == 11);
  REQUIRE(std::ranges::none_of(
      std::filesystem::directory_iterator{directory.path() / ".git"},
      [](const auto& entry) {
        return entry.path().filename().string().starts_with(".aiforge-edit-");
      }));

  std::stop_source cancelled;
  cancelled.request_stop();
  const auto stopped =
      editor.read({*baseline, "tracked.txt", {}}, cancelled.get_token());
  REQUIRE_FALSE(stopped);
  REQUIRE(stopped.error().code ==
          repository::ExactSourceEditErrorCode::cancelled);
}
