#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include <aiforge/credentials/credential.hpp>

namespace {

using namespace aiforge::credentials;

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    auto pattern =
        (std::filesystem::temp_directory_path() / "aiforge-credential-XXXXXX")
            .string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    const auto* created = ::mkdtemp(writable.data());
    REQUIRE(created != nullptr);
    m_path = created;
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(m_path, ignored);
  }

  [[nodiscard]] auto path() const -> const std::filesystem::path& {
    return m_path;
  }

 private:
  std::filesystem::path m_path;
};

class ScriptedStore final : public CredentialStore {
 public:
  auto load()
      -> std::expected<std::optional<Secret>, CredentialError> override {
    ++loads;
    if (error) return std::unexpected(*error);
    if (!value) return std::optional<Secret>{};
    auto secret = make_secret(*value);
    REQUIRE(secret);
    return std::optional<Secret>{std::move(*secret)};
  }

  auto store(const Secret&) -> std::expected<void, CredentialError> override {
    ++stores;
    if (error) return std::unexpected(*error);
    return {};
  }

  int loads{};
  int stores{};
  std::optional<std::string> value;
  std::optional<CredentialError> error;
};

[[nodiscard]] auto mode(const std::filesystem::path& path) -> mode_t {
  struct stat info{};
  REQUIRE(::stat(path.c_str(), &info) == 0);
  return info.st_mode & 0777;
}

} // namespace

TEST_CASE("credential path resolution follows XDG precedence",
          "[credentials][path][failure]") {
  auto path = resolve_credential_path({"/tmp/xdg", "/tmp/home"});
  REQUIRE(path == std::filesystem::path{"/tmp/xdg/aiforge/credentials"});

  path = resolve_credential_path({"relative", "/tmp/home"});
  REQUIRE(path ==
          std::filesystem::path{"/tmp/home/.config/aiforge/credentials"});

  auto missing = resolve_credential_path({std::nullopt, std::nullopt});
  REQUIRE_FALSE(missing);
  REQUIRE(missing.error().code == CredentialErrorCode::missing_home);

  auto relative = resolve_credential_path({std::nullopt, "relative"});
  REQUIRE_FALSE(relative);
  REQUIRE(relative.error().code == CredentialErrorCode::invalid_base_path);
}

TEST_CASE("credential values reject line and resource boundary violations",
          "[credentials][failure]") {
  REQUIRE_FALSE(make_secret(""));
  REQUIRE_FALSE(make_secret("contains space"));
  REQUIRE_FALSE(make_secret("contains\nline"));
  REQUIRE_FALSE(make_secret(std::string(maximum_credential_bytes + 1U, 'x')));

  auto maximum = make_secret(std::string(maximum_credential_bytes, 'x'));
  REQUIRE(maximum);
  REQUIRE(maximum->view().size() == maximum_credential_bytes);

  const std::string value{"move-only-secret"};
  auto original = make_secret(value);
  REQUIRE(original);
  auto moved = std::move(*original);
  REQUIRE(moved.view() == value);
  REQUIRE(original->view().empty());
}

TEST_CASE("explicit environment credentials are authoritative",
          "[credentials][resolution][failure]") {
  ScriptedStore store;
  store.value = "stored-secret";
  auto resolution =
      resolve_credential(std::string{"environment-secret"}, store);
  REQUIRE(resolution);
  REQUIRE(store.loads == 0);
  REQUIRE(resolution->credential);
  REQUIRE(resolution->credential->secret.view() == "environment-secret");
  REQUIRE(resolution->credential->source.kind ==
          aiforge::domain::CredentialSourceKind::environment);
  REQUIRE(resolution->credential->source.identity == "VENICE_API_KEY");

  auto invalid = resolve_credential(std::string{}, store);
  REQUIRE_FALSE(invalid);
  REQUIRE(invalid.error().code == CredentialErrorCode::invalid_value);
  REQUIRE(store.loads == 0);
}

TEST_CASE("stored credential failures degrade to bounded warnings",
          "[credentials][resolution][failure]") {
  ScriptedStore store;
  store.error = CredentialError{CredentialErrorCode::insecure_permissions,
                                {},
                                "credential permissions are unsafe"};
  auto resolution = resolve_credential(std::nullopt, store);
  REQUIRE(resolution);
  REQUIRE_FALSE(resolution->credential);
  REQUIRE(resolution->warnings ==
          std::vector<std::string>{"credential permissions are unsafe"});

  store.error.reset();
  store.value = "stored-secret";
  resolution = resolve_credential(std::nullopt, store);
  REQUIRE(resolution->credential);
  REQUIRE(resolution->credential->secret.view() == "stored-secret");
  REQUIRE(resolution->credential->source.kind ==
          aiforge::domain::CredentialSourceKind::configuration_file);
  REQUIRE(resolution->credential->source.identity == "aiforge/credentials");
}

TEST_CASE("credential file publication is restrictive and round trips",
          "[credentials][file]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "config" / "aiforge" / "credentials";
  FileCredentialStore store{path};

  auto missing = store.load();
  REQUIRE(missing);
  REQUIRE_FALSE(*missing);

  auto secret = make_secret("stored-secret");
  REQUIRE(secret);
  REQUIRE(store.store(*secret));
  REQUIRE(mode(path.parent_path()) == 0700);
  REQUIRE(mode(path) == 0600);
  REQUIRE(mode(path.parent_path() / "credentials.lock") == 0600);

  auto loaded = store.load();
  REQUIRE(loaded);
  REQUIRE(*loaded);
  REQUIRE((*loaded)->view() == "stored-secret");

  std::ifstream raw{path, std::ios::binary};
  REQUIRE(raw);
  const std::string contents{std::istreambuf_iterator<char>{raw},
                             std::istreambuf_iterator<char>{}};
  REQUIRE(contents == "stored-secret\n");
}

TEST_CASE("credential reads fail closed on insecure or unsafe paths",
          "[credentials][file][failure]") {
  TemporaryDirectory temporary;
  const auto directory = temporary.path() / "config" / "aiforge";
  REQUIRE(std::filesystem::create_directories(directory));
  REQUIRE(::chmod(directory.c_str(), 0700) == 0);
  const auto path = directory / "credentials";

  {
    std::ofstream output{path};
    output << "secret\n";
  }
  REQUIRE(::chmod(path.c_str(), 0644) == 0);
  FileCredentialStore store{path};
  auto loaded = store.load();
  REQUIRE_FALSE(loaded);
  REQUIRE(loaded.error().code == CredentialErrorCode::insecure_permissions);

  REQUIRE(::chmod(path.c_str(), 0600) == 0);
  REQUIRE(::chmod(directory.c_str(), 0755) == 0);
  loaded = store.load();
  REQUIRE_FALSE(loaded);
  REQUIRE(loaded.error().code == CredentialErrorCode::insecure_permissions);

  REQUIRE(::chmod(directory.c_str(), 0700) == 0);
  REQUIRE(std::filesystem::remove(path));
  std::filesystem::create_symlink("target", path);
  REQUIRE(std::filesystem::is_symlink(path));
  loaded = store.load();
  REQUIRE_FALSE(loaded);
  REQUIRE(loaded.error().code == CredentialErrorCode::path_escape);

  REQUIRE(std::filesystem::remove(path));
  REQUIRE(std::filesystem::create_directory(path));
  loaded = store.load();
  REQUIRE_FALSE(loaded);
  REQUIRE(loaded.error().code == CredentialErrorCode::not_regular);
}

TEST_CASE("credential files reject malformed and oversized records",
          "[credentials][file][failure]") {
  TemporaryDirectory temporary;
  const auto directory = temporary.path() / "config" / "aiforge";
  REQUIRE(std::filesystem::create_directories(directory));
  REQUIRE(::chmod(directory.c_str(), 0700) == 0);
  const auto path = directory / "credentials";
  FileCredentialStore store{path};

  {
    std::ofstream output{path, std::ios::binary};
    output << "first\nsecond\n";
  }
  REQUIRE(::chmod(path.c_str(), 0600) == 0);
  auto loaded = store.load();
  REQUIRE_FALSE(loaded);
  REQUIRE(loaded.error().code == CredentialErrorCode::invalid_value);

  {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output << std::string(maximum_credential_bytes + 2U, 'x');
  }
  REQUIRE(::chmod(path.c_str(), 0600) == 0);
  loaded = store.load();
  REQUIRE_FALSE(loaded);
  REQUIRE(loaded.error().code == CredentialErrorCode::too_large);
}

TEST_CASE("concurrent credential writers publish one complete value",
          "[credentials][file][concurrency]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "config" / "aiforge" / "credentials";
  FileCredentialStore store{path};
  auto first = make_secret(std::string(4096, 'a'));
  auto second = make_secret(std::string(4096, 'b'));
  REQUIRE(first);
  REQUIRE(second);
  std::atomic_bool first_ok{};
  std::atomic_bool second_ok{};

  std::jthread writer_one{
      [&] { first_ok.store(store.store(*first).has_value()); }};
  std::jthread writer_two{
      [&] { second_ok.store(store.store(*second).has_value()); }};
  writer_one.join();
  writer_two.join();
  REQUIRE(first_ok.load());
  REQUIRE(second_ok.load());

  auto loaded = store.load();
  REQUIRE(loaded);
  REQUIRE(*loaded);
  REQUIRE(((*loaded)->view() == std::string(4096, 'a') ||
           (*loaded)->view() == std::string(4096, 'b')));
  REQUIRE(mode(path) == 0600);
}

TEST_CASE("credential login refuses to replace an insecure target",
          "[credentials][file][failure]") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "config" / "aiforge" / "credentials";
  FileCredentialStore store{path};
  auto original = make_secret("original-secret");
  auto replacement = make_secret("replacement-secret");
  REQUIRE(original);
  REQUIRE(replacement);
  REQUIRE(store.store(*original));
  REQUIRE(::chmod(path.c_str(), 0644) == 0);

  auto changed = store.store(*replacement);
  REQUIRE_FALSE(changed);
  REQUIRE(changed.error().code == CredentialErrorCode::insecure_permissions);
  std::ifstream input{path};
  std::string contents;
  std::getline(input, contents);
  REQUIRE(contents == "original-secret");
}
