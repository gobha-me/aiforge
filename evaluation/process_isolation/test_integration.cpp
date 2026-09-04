#include "probes.hpp"
#include "runner.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace isolation = aiforge::evaluation::process_isolation;

namespace {

[[nodiscard]] auto executable_directory() -> std::filesystem::path {
  std::string path(4096, '\0');
  const auto count = ::readlink("/proc/self/exe", path.data(), path.size());
  REQUIRE(count > 0);
  REQUIRE(static_cast<std::size_t>(count) < path.size());
  path.resize(static_cast<std::size_t>(count));
  return std::filesystem::path{path}.parent_path();
}

class IntegrationTemporaryDirectory {
 public:
  IntegrationTemporaryDirectory() {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "aiforge-integration-test-XXXXXX")
                       .string();
    pattern.push_back('\0');
    const auto* created = ::mkdtemp(pattern.data());
    REQUIRE(created != nullptr);
    m_path = created;
    REQUIRE(::chmod(m_path.c_str(), S_IRWXU) == 0);
  }
  ~IntegrationTemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(m_path, ignored);
  }
  IntegrationTemporaryDirectory(const IntegrationTemporaryDirectory&) = delete;
  auto operator=(const IntegrationTemporaryDirectory&)
      -> IntegrationTemporaryDirectory& = delete;
  [[nodiscard]] auto path() const -> const std::filesystem::path& {
    return m_path;
  }

 private:
  std::filesystem::path m_path;
};

} // namespace

TEST_CASE("Linux probe harness emits complete deterministic bounded evidence") {
  IntegrationTemporaryDirectory temporary;
  REQUIRE(::setenv("AIFORGE_PROCESS_ISOLATION_LEAK_TEST", "must-not-leak", 1) ==
          0);
  const auto inherited_descriptor = ::open("/dev/null", O_RDONLY);
  REQUIRE(inherited_descriptor >= 0);
  isolation::RunnerOptions options;
  options.child_executable =
      executable_directory() / "aiforge_process_isolation_probe";
  options.temporary_parent = temporary.path();

  const auto report = isolation::run_evaluation(std::string(40, 'a'), options);
  CHECK(::close(inherited_descriptor) == 0);
  CHECK(::unsetenv("AIFORGE_PROCESS_ISOLATION_LEAK_TEST") == 0);
  REQUIRE(report);
  REQUIRE(isolation::validate_report(*report));
  REQUIRE(report->probes.size() == isolation::required_probe_ids().size());
  for (std::size_t index{}; index < report->probes.size(); ++index) {
    CHECK(report->probes[index].probe_id ==
          isolation::required_probe_ids()[index]);
    CHECK(report->probes[index].state != isolation::ProbeState::probe_error);
  }
  const auto inherited = std::ranges::find_if(
      report->probes, [](const isolation::ProbeRecord& record) {
        return record.probe_id == isolation::ProbeId::inherited_descriptors;
      });
  REQUIRE(inherited != report->probes.end());
  CHECK(inherited->state == isolation::ProbeState::enforced);
  CHECK(inherited->reason == isolation::ReasonCode::none);
  CHECK(report->platform == "linux");
  CHECK(report->kernel.size() <= isolation::maximum_platform_metadata_bytes);
  CHECK(report->architecture.size() <=
        isolation::maximum_platform_metadata_bytes);
  CHECK(report->kernel.find('/') == std::string::npos);
  CHECK(report->architecture.find('/') == std::string::npos);
  CHECK(std::filesystem::is_empty(temporary.path()));

  const auto first = isolation::serialize_report(*report);
  const auto second = isolation::serialize_report(*report);
  REQUIRE(first);
  REQUIRE(second);
  CHECK(*first == *second);
  CHECK(first->size() <= isolation::maximum_report_bytes);
  REQUIRE(isolation::parse_report(*first));
  CHECK(*isolation::parse_report(*first) == *report);
  REQUIRE(isolation::evidence_run_succeeded(*report));
  CHECK(*isolation::evidence_run_succeeded(*report));
}

TEST_CASE("callable probe failures stay unavailable rather than enforced") {
  const auto failed = isolation::test_support::callable_assertion_failure();
  CHECK(failed.probe_id == isolation::ProbeId::no_new_privileges);
  CHECK(failed.state == isolation::ProbeState::unavailable);
  CHECK(failed.reason == isolation::ReasonCode::enforcement_failed);

  const auto denied = isolation::test_support::runtime_permission_denial();
  CHECK(denied.probe_id == isolation::ProbeId::user_namespace);
#if defined(__x86_64__) || defined(__aarch64__)
  CHECK(denied.state == isolation::ProbeState::unavailable);
  CHECK(denied.reason == isolation::ReasonCode::permission_denied);
#else
  CHECK(denied.state == isolation::ProbeState::unavailable);
  CHECK(denied.reason == isolation::ReasonCode::unsupported_architecture);
#endif
}

TEST_CASE("a valid all-unavailable report is a completed evaluation") {
  isolation::EvidenceReport report{
      std::string(40, 'a'), "linux", "6.0.0", "x86_64", {}};
  for (const auto probe_id : isolation::required_probe_ids()) {
    report.probes.push_back({probe_id, isolation::ProbeState::unavailable,
                             isolation::ReasonCode::permission_denied});
  }
  REQUIRE(isolation::validate_report(report));
  const auto succeeded = isolation::evidence_run_succeeded(report);
  REQUIRE(succeeded);
  CHECK(*succeeded);
}
