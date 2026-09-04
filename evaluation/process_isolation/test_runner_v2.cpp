#include "runner_v2.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <stop_token>
#include <string>
#include <string_view>

#include <sys/stat.h>
#include <unistd.h>

namespace isolation = aiforge::evaluation::process_isolation;
namespace v2 = aiforge::evaluation::process_isolation::v2;

namespace {

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "aiforge-v2-runner-test-XXXXXX")
                       .string();
    pattern.push_back('\0');
    const auto* created = ::mkdtemp(pattern.data());
    REQUIRE(created != nullptr);
    m_path = created;
    REQUIRE(::chmod(m_path.c_str(), S_IRWXU) == 0);
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(m_path, ignored);
  }
  TemporaryDirectory(const TemporaryDirectory&) = delete;
  auto operator=(const TemporaryDirectory&) -> TemporaryDirectory& = delete;
  [[nodiscard]] auto path() const -> const std::filesystem::path& {
    return m_path;
  }

 private:
  std::filesystem::path m_path;
};

[[nodiscard]] auto shell_options(const TemporaryDirectory& temporary,
                                 std::string script) -> v2::RunnerOptions {
  v2::RunnerOptions options;
  options.child_executable = "/bin/sh";
  options.child_argument_prefix = {"-c", std::move(script)};
  options.temporary_parent = temporary.path();
  options.child_timeout = std::chrono::seconds{3};
  return options;
}

auto require_closed(const v2::EvidenceReport& report,
                    const v2::ReasonCode reason) -> void {
  REQUIRE(report.probes.size() == v2::required_probe_ids().size());
  for (std::size_t index{}; index < report.probes.size(); ++index) {
    CHECK(report.probes[index].probe_id == v2::required_probe_ids()[index]);
    CHECK(report.probes[index].state == isolation::ProbeState::probe_error);
    CHECK(report.probes[index].reason == reason);
  }
}

auto require_non_cgroup_closed(const v2::EvidenceReport& report,
                               const v2::ReasonCode reason) -> void {
  REQUIRE(report.probes.size() == v2::required_probe_ids().size());
  for (const auto& record : report.probes) {
    const auto name = v2::probe_id_name(record.probe_id);
    const bool cgroup = name.starts_with("cgroup_") ||
                        record.probe_id == v2::ProbeId::combined_setup_order ||
                        record.probe_id == v2::ProbeId::partial_setup_cleanup;
    CHECK(record.state == (cgroup ? isolation::ProbeState::unavailable
                                  : isolation::ProbeState::probe_error));
    CHECK(record.reason ==
          (cgroup ? v2::ReasonCode::missing_delegation : reason));
  }
}

} // namespace

TEST_CASE("evidence v2 runner rejects unsafe bounds",
          "[process-isolation][evidence-v2][failure]") {
  v2::RunnerOptions options;
  options.child_executable = "relative";
  auto result = v2::run_evaluation(std::string(40, 'a'), options);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == v2::RunnerErrorCode::invalid_options);

  options.child_executable = "/bin/true";
  options.maximum_child_output_bytes = v2::maximum_child_record_bytes + 1;
  result = v2::run_evaluation(std::string(40, 'a'), options);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == v2::RunnerErrorCode::invalid_options);

  options.maximum_child_output_bytes = v2::maximum_child_record_bytes;
  options.child_timeout = std::chrono::milliseconds{60001};
  result = v2::run_evaluation(std::string(40, 'a'), options);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == v2::RunnerErrorCode::invalid_options);

  for (const auto& path : {std::filesystem::path{"relative"},
                           std::filesystem::path{"/tmp/../escape"},
                           std::filesystem::path{"/tmp/"}}) {
    options.child_timeout = std::chrono::seconds{3};
    options.delegated_cgroup_root = path;
    result = v2::run_evaluation(std::string(40, 'a'), options);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == v2::RunnerErrorCode::invalid_options);
  }
}

TEST_CASE("delegated cgroup bootstrap failures are phase-specific and closed",
          "[process-isolation][evidence-v2][failure]") {
  using Phase = v2::test_support::BootstrapFailurePhase;
  for (const auto phase :
       {Phase::pin_root, Phase::verify_ownership, Phase::create_supervisor,
        Phase::move_to_supervisor, Phase::verify_supervisor,
        Phase::verify_root_empty, Phase::enable_controllers,
        Phase::verify_enabled_controllers}) {
    const auto outcome =
        v2::test_support::bootstrap_failure_outcome(phase, true);
    CHECK(outcome.state == isolation::ProbeState::unavailable);
    CHECK(outcome.reason == v2::ReasonCode::missing_delegation);
  }
  const auto missing_controller = v2::test_support::bootstrap_failure_outcome(
      Phase::verify_controllers, true);
  CHECK(missing_controller.probe_id ==
        v2::ProbeId::cgroup_required_controllers);
  CHECK(missing_controller.state == isolation::ProbeState::unavailable);
  CHECK(missing_controller.reason == v2::ReasonCode::missing_controller);

  for (const auto phase : {Phase::disable_controllers, Phase::move_to_root,
                           Phase::await_empty, Phase::remove_supervisor}) {
    const auto outcome =
        v2::test_support::bootstrap_failure_outcome(phase, true);
    CHECK(outcome.state == isolation::ProbeState::probe_error);
    CHECK(outcome.reason == v2::ReasonCode::cleanup_failed);
  }
  for (const auto phase :
       {Phase::pin_root, Phase::verify_ownership, Phase::verify_controllers,
        Phase::create_supervisor, Phase::move_to_supervisor,
        Phase::verify_supervisor, Phase::verify_root_empty,
        Phase::enable_controllers, Phase::verify_enabled_controllers}) {
    const auto rollback_failed =
        v2::test_support::bootstrap_failure_outcome(phase, false);
    CHECK(rollback_failed.state == isolation::ProbeState::probe_error);
    CHECK(rollback_failed.reason == v2::ReasonCode::cleanup_failed);
  }
}

TEST_CASE("evidence v2 runner maps hostile child outcomes closed and cleans",
          "[process-isolation][evidence-v2][failure]") {
  TemporaryDirectory temporary;
  struct Case {
    std::string_view script;
    v2::ReasonCode reason;
  };
  for (const auto& test_case : {
           Case{"printf x", v2::ReasonCode::malformed_protocol},
           Case{"kill -TERM $$", v2::ReasonCode::signaled},
           Case{"exit 9", v2::ReasonCode::nonzero_exit},
           Case{"while :; do printf 0123456789; done",
                v2::ReasonCode::output_limit},
       }) {
    auto options = shell_options(temporary, std::string{test_case.script});
    if (test_case.reason == v2::ReasonCode::output_limit)
      options.maximum_child_output_bytes = 32;
    const auto result = v2::run_evaluation(std::string(40, 'a'), options);
    REQUIRE(result);
    require_non_cgroup_closed(*result, test_case.reason);
    CHECK(std::filesystem::is_empty(temporary.path()));
  }
}

TEST_CASE("evidence v2 runner bounds timeout and cancellation",
          "[process-isolation][evidence-v2][failure]") {
  TemporaryDirectory temporary;
  auto options = shell_options(temporary, "exec /bin/sleep 10");
  options.child_timeout = std::chrono::milliseconds{30};
  const auto timed_out = v2::run_evaluation(std::string(40, 'a'), options);
  REQUIRE(timed_out);
  require_non_cgroup_closed(*timed_out, v2::ReasonCode::timeout);

  std::stop_source cancelled;
  cancelled.request_stop();
  options = shell_options(temporary, "exit 70");
  const auto stopped =
      v2::run_evaluation(std::string(40, 'a'), options, cancelled.get_token());
  REQUIRE(stopped);
  require_closed(*stopped, v2::ReasonCode::cancelled);
  CHECK(std::filesystem::is_empty(temporary.path()));
}

TEST_CASE("evidence v2 runner accepts only the matching typed row",
          "[process-isolation][evidence-v2][smoke]") {
  TemporaryDirectory temporary;
  auto options = shell_options(
      temporary, "printf '{\"probe_id\":\"%s\",\"reason\":\"none\","
                 "\"schema_version\":2,\"state\":\"enforced\"}' \"$0\"");
  const auto result = v2::run_evaluation(std::string(40, 'a'), options);
  REQUIRE(result);
  REQUIRE(result->probes.size() == v2::required_probe_ids().size());
  for (const auto& record : result->probes) {
    const auto name = v2::probe_id_name(record.probe_id);
    const bool cgroup = name.starts_with("cgroup_") ||
                        record.probe_id == v2::ProbeId::combined_setup_order ||
                        record.probe_id == v2::ProbeId::partial_setup_cleanup;
    CHECK(record.state == (cgroup ? isolation::ProbeState::unavailable
                                  : isolation::ProbeState::enforced));
    CHECK(record.reason ==
          (cgroup ? v2::ReasonCode::missing_delegation : v2::ReasonCode::none));
  }
  CHECK(std::filesystem::is_empty(temporary.path()));
}
