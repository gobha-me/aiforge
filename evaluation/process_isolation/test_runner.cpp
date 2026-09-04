#include "runner.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>

#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace isolation = aiforge::evaluation::process_isolation;

namespace {

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    auto pattern =
        (std::filesystem::temp_directory_path() / "aiforge-runner-test-XXXXXX")
            .string();
    pattern.push_back('\0');
    const auto* created = ::mkdtemp(pattern.data());
    REQUIRE(created != nullptr);
    m_path = created;
    REQUIRE(::chmod(m_path.c_str(), S_IRWXU) == 0);
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  auto operator=(const TemporaryDirectory&) -> TemporaryDirectory& = delete;
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

class EscapedProcessGuard {
 public:
  explicit EscapedProcessGuard(std::filesystem::path marker)
      : m_marker{std::move(marker)} {}
  EscapedProcessGuard(const EscapedProcessGuard&) = delete;
  auto operator=(const EscapedProcessGuard&) -> EscapedProcessGuard& = delete;
  ~EscapedProcessGuard() {
    if (!m_active) return;
    std::ifstream input{m_marker};
    pid_t process{};
    if (!(input >> process) || process <= 0) return;
    static_cast<void>(::kill(process, SIGKILL));
    while (::waitpid(process, nullptr, 0) < 0 && errno == EINTR) {
    }
  }
  auto disarm() noexcept -> void { m_active = false; }

 private:
  std::filesystem::path m_marker;
  bool m_active{true};
};

[[nodiscard]] auto shell_options(const TemporaryDirectory& temporary,
                                 std::string script)
    -> isolation::RunnerOptions {
  isolation::RunnerOptions options;
  options.child_executable = "/bin/sh";
  options.child_argument_prefix = {"-c", std::move(script)};
  options.temporary_parent = temporary.path();
  options.child_timeout = std::chrono::seconds{5};
  options.cpu_limit_probe_timeout = options.child_timeout;
  return options;
}

auto require_closed(const isolation::EvidenceReport& report,
                    const isolation::ReasonCode reason) -> void {
  REQUIRE(report.probes.size() == isolation::required_probe_ids().size());
  for (std::size_t index{}; index < report.probes.size(); ++index) {
    CHECK(report.probes[index].probe_id ==
          isolation::required_probe_ids()[index]);
    CHECK(report.probes[index].state == isolation::ProbeState::probe_error);
    CHECK(report.probes[index].reason == reason);
  }
}

} // namespace

TEST_CASE("runner rejects unsafe launch options") {
  isolation::RunnerOptions options;
  options.child_executable = "relative";
  auto result = isolation::run_evaluation(std::string(40, 'a'), options);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == isolation::RunnerErrorCode::invalid_options);

  options.child_executable = "/bin/true";
  result = isolation::run_evaluation("not-a-source-sha", options);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == isolation::RunnerErrorCode::invalid_options);

  options.child_argument_prefix = {std::string{"embedded\0nul", 12}};
  result = isolation::run_evaluation(std::string(40, 'a'), options);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == isolation::RunnerErrorCode::invalid_options);

  options.child_argument_prefix.clear();
  options.cpu_limit_probe_timeout = std::chrono::milliseconds::zero();
  result = isolation::run_evaluation(std::string(40, 'a'), options);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == isolation::RunnerErrorCode::invalid_options);
}

TEST_CASE(
    "runner maps malformed and partial protocol closed and cleans state") {
  TemporaryDirectory temporary;
  for (const std::string_view script :
       {"printf x", "printf '{\\\"probe_id\\\"'"}) {
    auto options = shell_options(temporary, std::string{script});
    const auto result =
        isolation::run_evaluation(std::string(40, 'a'), options);
    REQUIRE(result);
    require_closed(*result, isolation::ReasonCode::malformed_protocol);
    CHECK(std::filesystem::is_empty(temporary.path()));
  }
}

TEST_CASE("runner maps timeout signal nonzero and excessive output closed") {
  TemporaryDirectory temporary;
  struct Case {
    std::string script;
    isolation::ReasonCode expected;
  };
  const Case cases[]{
      {"exec /bin/sleep 10", isolation::ReasonCode::timeout},
      {"kill -TERM $$", isolation::ReasonCode::signaled},
      {"exit 9", isolation::ReasonCode::nonzero_exit},
      {"while :; do printf 0123456789; done",
       isolation::ReasonCode::output_limit},
  };
  for (const auto& test_case : cases) {
    auto options = shell_options(temporary, test_case.script);
    if (test_case.expected == isolation::ReasonCode::timeout)
      options.child_timeout = std::chrono::milliseconds{50};
    if (test_case.expected == isolation::ReasonCode::timeout)
      options.cpu_limit_probe_timeout = options.child_timeout;
    if (test_case.expected == isolation::ReasonCode::output_limit)
      options.maximum_child_output_bytes = 32;
    const auto result =
        isolation::run_evaluation(std::string(40, 'a'), options);
    REQUIRE(result);
    require_closed(*result, test_case.expected);
    CHECK(std::filesystem::is_empty(temporary.path()));
  }
}

TEST_CASE("runner rejects a child record for the wrong probe") {
  TemporaryDirectory temporary;
  const auto document = isolation::serialize_child_record(
      {isolation::ProbeId::no_new_privileges,
       isolation::ProbeState::unavailable,
       isolation::ReasonCode::permission_denied});
  REQUIRE(document);
  auto script = std::string{"printf '%s' '"} + *document + "'";
  auto options = shell_options(temporary, std::move(script));
  const auto result = isolation::run_evaluation(std::string(40, 'a'), options);
  REQUIRE(result);
  CHECK(result->probes.front().state == isolation::ProbeState::unavailable);
  CHECK(result->probes.front().reason ==
        isolation::ReasonCode::permission_denied);
  for (std::size_t index = 1; index < result->probes.size(); ++index) {
    CHECK(result->probes[index].state == isolation::ProbeState::probe_error);
    CHECK(result->probes[index].reason ==
          isolation::ReasonCode::malformed_protocol);
  }
}

TEST_CASE(
    "runner kills and reaps a session descendant that escapes its group") {
  TemporaryDirectory temporary;
  const auto marker = temporary.path() / "escaped-pid";
  EscapedProcessGuard emergency_cleanup{marker};
  const auto script =
      std::string{"if [ \"$0\" = no_new_privileges ]; then "} +
      "setsid /bin/sh -c 'echo $$ > \"$1\"; while :; do sleep 10; done' "
      "daemon '" +
      marker.string() +
      "' </dev/null >/dev/null 2>&1 & "
      "while [ ! -s '" +
      marker.string() +
      "' ]; do :; done; fi; "
      "printf '{\"probe_id\":\"%s\",\"reason\":\"none\","
      "\"schema_version\":1,\"state\":\"enforced\"}' \"$0\"";
  auto options = shell_options(temporary, script);
  const auto result = isolation::run_evaluation(std::string(40, 'a'), options);
  REQUIRE(result);
  REQUIRE(result->probes.front().state == isolation::ProbeState::probe_error);
  CHECK(result->probes.front().reason == isolation::ReasonCode::cleanup_failed);

  std::ifstream pid_file{marker};
  pid_t escaped{};
  REQUIRE(pid_file >> escaped);
  REQUIRE(escaped > 0);
  errno = 0;
  CHECK(::kill(escaped, 0) == -1);
  CHECK(errno == ESRCH);
  errno = 0;
  CHECK(::waitpid(escaped, nullptr, WNOHANG) == -1);
  CHECK(errno == ECHILD);
  emergency_cleanup.disarm();
  REQUIRE(std::filesystem::remove(marker));
  CHECK(std::filesystem::is_empty(temporary.path()));
}
