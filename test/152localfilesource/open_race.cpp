#include <catch2/catch_test_macros.hpp>

#include <aiforge/adapters/local_file_source.hpp>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

namespace {
std::atomic<bool> race_armed{};
std::atomic<bool> read_open_attempted{};
std::atomic<int> replacement_error{};

class OpenRaceDirectory final {
 public:
  OpenRaceDirectory() {
    auto pattern = std::to_array("/tmp/aiforge-local-open-race-XXXXXX");
    auto* created = ::mkdtemp(pattern.data());
    if (created != nullptr) path = created;
  }
  ~OpenRaceDirectory() {
    race_armed.store(false);
    std::error_code error;
    if (!path.empty()) std::filesystem::remove_all(path, error);
  }
  std::filesystem::path path;
};
} // namespace

extern "C" auto __real_openat(int parent, const char* name, int flags, ...)
    -> int;
extern "C" auto __wrap_openat(int parent, const char* name, int flags, ...)
    -> int {
  mode_t mode{};
  const bool creates =
      (flags & O_CREAT) != 0 || (flags & O_TMPFILE) == O_TMPFILE;
  if (creates) {
    va_list arguments;
    va_start(arguments, flags);
    mode = va_arg(arguments, mode_t);
    va_end(arguments);
  }
  if (race_armed.load() && name != nullptr &&
      std::strcmp(name, "race-target.txt") == 0 && race_armed.exchange(false)) {
    read_open_attempted.store((flags & O_PATH) == 0);
    if (::renameat(parent, "replacement.pipe", parent, "race-target.txt") != 0)
      replacement_error.store(errno);
  }
  return creates ? __real_openat(parent, name, flags, mode)
                 : __real_openat(parent, name, flags);
}

TEST_CASE("a raced special leaf is pinned and refused before a read open") {
  using namespace aiforge;
  OpenRaceDirectory temporary;
  REQUIRE_FALSE(temporary.path.empty());
  {
    std::ofstream source{temporary.path / "race-target.txt"};
    source << "original regular-file bytes";
    REQUIRE(source.good());
  }
  REQUIRE(::mkfifo((temporary.path / "replacement.pipe").c_str(), 0600) == 0);
  const auto session = domain::SessionId::from("local-open-race").value();
  auto granted = adapters::grant_local_folder(session, temporary.path, 1);
  REQUIRE(granted);
  auto lease = std::move(*granted);
  const runtime::LocalSourceRequestToken token{session, lease->root_identity(),
                                               1, 1, 1};
  read_open_attempted.store(false);
  replacement_error.store(0);
  race_armed.store(true);
  const auto result = lease->preview(
      {token, "race-target.txt", runtime::LocalPreviewMode::exact, {}});
  CHECK_FALSE(race_armed.load());
  CHECK(replacement_error.load() == 0);
  CHECK_FALSE(read_open_attempted.load());
  REQUIRE_FALSE(result);
  CHECK(
      (result.error().code == domain::LocalSourceErrorCode::concurrent_change ||
       result.error().code == domain::LocalSourceErrorCode::unsupported_entry));
  CHECK(std::filesystem::is_fifo(temporary.path / "race-target.txt"));
}
