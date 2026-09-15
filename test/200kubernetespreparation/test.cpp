#include <aiforge/adapters/kubernetes_ops_source_preparation.hpp>
#include <aiforge/runtime/local_source_worker.hpp>

#include "static_kubernetes_config_parser.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace {
using namespace aiforge;
using namespace std::chrono_literals;
using Error = runtime::OpsObservationSourceError;
namespace fs = std::filesystem;

struct ReadGateState {
  std::mutex mutex;
  std::condition_variable changed;
  dev_t device{};
  ino_t inode{};
  bool enabled{};
  bool entered{};
  bool released{};
};
ReadGateState read_gate;

template <class Predicate> auto until(Predicate predicate) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(2ms);
  }
  return predicate();
}

class ReadPause final {
 public:
  explicit ReadPause(const fs::path& path) {
    struct stat state{};
    REQUIRE(::stat(path.c_str(), &state) == 0);
    const std::lock_guard lock{read_gate.mutex};
    read_gate.device = state.st_dev;
    read_gate.inode = state.st_ino;
    read_gate.enabled = true;
    read_gate.entered = false;
    read_gate.released = false;
  }
  ~ReadPause() {
    {
      const std::lock_guard lock{read_gate.mutex};
      read_gate.enabled = false;
      read_gate.released = true;
    }
    read_gate.changed.notify_all();
  }
  [[nodiscard]] auto entered() -> bool {
    const std::lock_guard lock{read_gate.mutex};
    return read_gate.entered;
  }
  auto release() -> void {
    {
      const std::lock_guard lock{read_gate.mutex};
      read_gate.released = true;
    }
    read_gate.changed.notify_all();
  }
};

class TemporaryFiles final {
 public:
  TemporaryFiles() {
    auto pattern =
        (fs::temp_directory_path() / "aiforge-kube-preparation-XXXXXX")
            .string();
    std::vector<char> bytes(pattern.begin(), pattern.end());
    bytes.push_back('\0');
    const auto* made = ::mkdtemp(bytes.data());
    REQUIRE(made != nullptr);
    root = made;
    fs::permissions(root, fs::perms::owner_all);
  }
  ~TemporaryFiles() {
    std::error_code ignored;
    fs::remove_all(root, ignored);
  }
  [[nodiscard]] auto path(std::string_view name = "config") const -> fs::path {
    return root / name;
  }
  auto write(const fs::path& target, std::string_view bytes,
             fs::perms permissions = fs::perms::owner_read |
                                     fs::perms::owner_write) const -> void {
    std::ofstream output{target, std::ios::binary};
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    REQUIRE(output);
    fs::permissions(target, permissions);
  }
  fs::path root;
};

template <class T> auto id(std::string value) -> T {
  auto result = T::from(std::move(value));
  REQUIRE(result);
  return std::move(*result);
}

constexpr auto ca = "LS0tLS1CRUdJTiBDRVJUSUZJQ0FURS0tLS0tCkFRSUQKLS0tLS1FTkQgQ0"
                    "VSVElGSUNBVEUtLS0tLQo=";
constexpr auto cert =
    "LS0tLS1CRUdJTiBDRVJUSUZJQ0FURS0tLS0tCkJBVUcKLS0tLS1FTkQgQ0"
    "VSVElGSUNBVEUtLS0tLQo=";
constexpr auto key = "LS0tLS1CRUdJTiBQUklWQVRFIEtFWS0tLS0tCkJBVUcKLS0tLS1FTkQgU"
                     "FJJVkFURSBLRVktLS0tLQo=";

auto document(bool certificate = false, std::string_view user_extra = {})
    -> std::string {
  auto auth = certificate
                  ? "    client-certificate-data: " + std::string{cert} +
                        "\n    client-key-data: " + std::string{key} + "\n"
                  : "    token: synthetic-static-token\n";
  auth += user_extra;
  return "apiVersion: v1\nkind: Config\nclusters:\n- name: selected-cluster\n  "
         "cluster:\n    server: https://fixture.invalid:6443\n    "
         "certificate-authority-data: " +
         std::string{ca} + "\nusers:\n- name: selected-user\n  user:\n" + auth +
         "contexts:\n- name: selected-context\n  context:\n    cluster: "
         "selected-cluster\n    user: selected-user\n    namespace: "
         "document-default\n"
         "current-context: selected-context\n";
}

auto factory(const fs::path& path, std::string context = "selected-context",
             std::string name_space = "configured") {
  return adapters::KubernetesOpsSourcePreparationFactory::create(
      id<domain::OpsTargetId>("cluster"),
      id<domain::OpsConfigurationRevision>("revision"),
      config::StaticKubernetesTargetConfig{path.string(), std::move(context),
                                           std::move(name_space)});
}
auto request(const runtime::OpsSourcePreparationFactory& factory,
             std::uint64_t sequence = 1,
             std::chrono::milliseconds lifetime = 5s)
    -> runtime::OpsSourcePreparationRequest {
  return {{id<domain::SessionId>("session"), 1, sequence,
           factory.preparation_identity()},
          std::chrono::steady_clock::now() + lifetime};
}
struct WorkerFixture {
  std::shared_ptr<runtime::LocalSourceWorker> worker;
  WorkerFixture() {
    auto created = runtime::LocalSourceWorker::create(1);
    REQUIRE(created);
    worker = std::move(*created);
  }
  auto complete(const runtime::OpsSourcePreparationToken& token)
      -> runtime::OpsSourcePreparationCompletion {
    REQUIRE(until([&] { return worker->ready_results() == 1; }));
    auto result = worker->poll(token);
    REQUIRE(result);
    REQUIRE(*result);
    return std::move(**result);
  }
};
} // namespace

extern "C" auto __real_read(int descriptor, void* output, std::size_t size)
    -> ssize_t;
extern "C" auto __wrap_read(int descriptor, void* output, std::size_t size)
    -> ssize_t {
  struct stat state{};
  if (::fstat(descriptor, &state) == 0) {
    std::unique_lock lock{read_gate.mutex};
    if (read_gate.enabled && state.st_dev == read_gate.device &&
        state.st_ino == read_gate.inode) {
      read_gate.entered = true;
      read_gate.changed.notify_all();
      read_gate.changed.wait(lock, [] { return read_gate.released; });
    }
  }
  return __real_read(descriptor, output, size);
}

TEST_CASE("Kubernetes preparation metadata rejects unsafe selections without "
          "opening sources") {
  TemporaryFiles files;
  auto path = files.path();
  SECTION("relative") {
    path = "relative/config";
  }
  SECTION("dot component") {
    path = files.root / ".." / "config";
  }
  SECTION("trailing slash") {
    path = path.string() + "/";
  }
  SECTION("unsafe path") {
    path = files.root / "bad\nname";
  }
  SECTION("long path") {
    path = "/" + std::string(4096, 'x');
  }
  SECTION("unsafe context") {
    CHECK_FALSE(factory(path, "bad\ncontext"));
    return;
  }
  SECTION("invalid namespace") {
    CHECK_FALSE(factory(path, "selected-context", "Bad_namespace"));
    return;
  }
  CHECK_FALSE(factory(path));
}

TEST_CASE("Kubernetes preparation checks cancellation and deadline before IO") {
  TemporaryFiles files;
  auto made = factory(files.path("missing"));
  REQUIRE(made);
  auto work = request(**made);
  SECTION("cancelled") {
    std::stop_source stop;
    REQUIRE(stop.request_stop());
    auto result = (*made)->prepare(work, stop.get_token());
    REQUIRE_FALSE(result);
    CHECK(result.error() == Error::cancelled);
  }
  SECTION("expired") {
    work.deadline = std::chrono::steady_clock::time_point::min();
    auto result = (*made)->prepare(work);
    REQUIRE_FALSE(result);
    CHECK(result.error() == Error::timed_out);
  }
}

TEST_CASE("Kubernetes preparation fails closed on unusable exact sources") {
  TemporaryFiles files;
  auto path = files.path();
  Error expected{Error::invalid_result};
  SECTION("missing") {
    expected = Error::unavailable;
  }
  SECTION("leaf symlink") {
    files.write(files.path("other"), document());
    fs::create_symlink(files.path("other"), path);
    expected = Error::source_changed;
  }
  SECTION("ancestor symlink") {
    const auto real = files.path("real");
    REQUIRE(fs::create_directory(real));
    files.write(real / "config", document());
    const auto alias = files.path("alias");
    fs::create_directory_symlink(real, alias);
    path = alias / "config";
    expected = Error::source_changed;
  }
  SECTION("directory") {
    REQUIRE(fs::create_directory(path));
  }
  SECTION("fifo") {
    REQUIRE(::mkfifo(path.c_str(), 0600) == 0);
  }
  SECTION("insecure") {
    files.write(path, document(),
                fs::perms::owner_read | fs::perms::owner_write |
                    fs::perms::group_read);
    expected = Error::permission_denied;
  }
  SECTION("not owner-readable") {
    files.write(path, document(), fs::perms::owner_write);
    expected = Error::permission_denied;
  }
  SECTION("oversize") {
    files.write(
        path,
        std::string(adapters::static_kubernetes_detail::input_limit + 1, 'x'));
    expected = Error::resource_exhausted;
  }
  SECTION("unsupported top-level shape") {
    files.write(path, "not: [closed");
    expected = Error::unsupported;
  }
  SECTION("missing selected context") {
    files.write(path, document());
    auto made = factory(path, "absent-context");
    REQUIRE(made);
    WorkerFixture fixture;
    const auto work = request(**made);
    REQUIRE(fixture.worker->submit(*made, work));
    auto done = fixture.complete(work.token);
    REQUIRE_FALSE(done.result);
    CHECK(done.result.error() == Error::invalid_result);
    return;
  }
  auto made = factory(path);
  REQUIRE(made);
  WorkerFixture fixture;
  const auto work = request(**made);
  REQUIRE(fixture.worker->submit(*made, work));
  auto done = fixture.complete(work.token);
  REQUIRE_FALSE(done.result);
  CHECK(done.result.error() == expected);
}

TEST_CASE("Kubernetes preparation never uses ambient config or auth helpers") {
  TemporaryFiles files;
  const auto ambient = files.path("ambient");
  files.write(ambient, document());
  const auto* existing = std::getenv("KUBECONFIG");
  const std::optional<std::string> original =
      existing == nullptr ? std::nullopt : std::optional<std::string>{existing};
  REQUIRE(::setenv("KUBECONFIG", ambient.c_str(), 1) == 0);
  struct Restore final {
    std::optional<std::string> original;
    ~Restore() {
      if (original)
        static_cast<void>(::setenv("KUBECONFIG", original->c_str(), 1));
      else
        static_cast<void>(::unsetenv("KUBECONFIG"));
    }
  } restore{original};

  SECTION("missing configured source") {
    auto made = factory(files.path("missing"));
    REQUIRE(made);
    WorkerFixture fixture;
    const auto work = request(**made);
    REQUIRE(fixture.worker->submit(*made, work));
    auto done = fixture.complete(work.token);
    REQUIRE_FALSE(done.result);
    CHECK(done.result.error() == Error::unavailable);
  }
  SECTION("unsupported helper") {
    const auto marker = files.path("helper-was-run");
    const auto configured = files.path();
    files.write(configured, document(false, "    exec:\n      command: " +
                                                marker.string() + "\n"));
    auto made = factory(configured);
    REQUIRE(made);
    WorkerFixture fixture;
    const auto work = request(**made);
    REQUIRE(fixture.worker->submit(*made, work));
    auto done = fixture.complete(work.token);
    REQUIRE_FALSE(done.result);
    CHECK(done.result.error() == Error::unsupported);
    CHECK_FALSE(fs::exists(marker));
  }
}

TEST_CASE("Kubernetes preparation detects configured path replacement") {
  TemporaryFiles files;
  auto configured = files.path();
  SECTION("leaf") {
  }
  SECTION("ancestor") {
    const auto directory = files.path("selected");
    REQUIRE(fs::create_directory(directory));
    configured = directory / "config";
  }
  files.write(configured, document());
  auto made = factory(configured);
  REQUIRE(made);
  WorkerFixture fixture;
  const auto work = request(**made);
  ReadPause pause{configured};
  REQUIRE(fixture.worker->submit(*made, work));
  REQUIRE(until([&] { return pause.entered(); }));
  if (configured.parent_path() == files.root) {
    const auto replacement = files.path("replacement");
    files.write(replacement, document(false, "    tokenFile: ignored\n"));
    fs::rename(replacement, configured);
  } else {
    const auto selected = configured.parent_path();
    fs::rename(selected, files.path("retired"));
    REQUIRE(fs::create_directory(selected));
    files.write(configured, document(false, "    tokenFile: ignored\n"));
  }
  pause.release();
  auto done = fixture.complete(work.token);
  REQUIRE_FALSE(done.result);
  CHECK(done.result.error() == Error::source_changed);
}

TEST_CASE("Kubernetes preparation cancellation retains its physical slot") {
  TemporaryFiles files;
  const auto configured = files.path();
  files.write(configured, document());
  auto made = factory(configured);
  REQUIRE(made);
  WorkerFixture fixture;
  const auto work = request(**made);
  ReadPause pause{configured};
  REQUIRE(fixture.worker->submit(*made, work));
  REQUIRE(until([&] { return pause.entered(); }));
  REQUIRE(fixture.worker->cancel(work.token));
  CHECK(fixture.worker->occupied_slots() == 1);
  CHECK_FALSE(fixture.worker->poll(work.token));
  pause.release();
  REQUIRE(until([&] { return fixture.worker->occupied_slots() == 0; }));
}

TEST_CASE("Kubernetes preparation preserves the original deadline") {
  TemporaryFiles files;
  const auto configured = files.path();
  files.write(configured, document());
  auto made = factory(configured);
  REQUIRE(made);
  WorkerFixture fixture;
  const auto work = request(**made, 1, 100ms);
  ReadPause pause{configured};
  REQUIRE(fixture.worker->submit(*made, work));
  REQUIRE(until([&] { return pause.entered(); }));
  std::this_thread::sleep_until(work.deadline + 1ms);
  CHECK(fixture.worker->occupied_slots() == 1);
  pause.release();
  auto done = fixture.complete(work.token);
  REQUIRE_FALSE(done.result);
  CHECK(done.result.error() == Error::timed_out);
}

TEST_CASE("Kubernetes preparation owns static token and certificate bindings") {
  TemporaryFiles files;
  const bool certificate = GENERATE(false, true);
  const auto configured = files.path();
  files.write(configured, document(certificate));
  auto made = factory(configured);
  REQUIRE(made);
  WorkerFixture fixture;
  const auto work = request(**made);
  REQUIRE(fixture.worker->submit(*made, work));
  auto done = fixture.complete(work.token);
  REQUIRE(done.result);
  const auto& binding = done.result->source->target_binding();
  CHECK(binding.target_id == work.token.selection.target_id);
  CHECK(binding.configuration_revision ==
        work.token.selection.configuration_revision);
  const auto* identity =
      std::get_if<domain::KubernetesOpsIdentity>(&binding.identity);
  REQUIRE(identity != nullptr);
  CHECK(identity->context_name == "selected-context");
  CHECK(identity->namespace_name == "configured");
  CHECK(identity->endpoint ==
        domain::OpsHttpsEndpoint{"fixture.invalid", 6443});
  CHECK(identity->trust_identity.starts_with("sha256:"));
}
