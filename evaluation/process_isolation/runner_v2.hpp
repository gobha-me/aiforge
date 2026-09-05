#pragma once

#include "evidence_v2.hpp"

#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace aiforge::evaluation::process_isolation::v2 {

#if defined(AIFORGE_PROCESS_ISOLATION_TEST_SUPPORT)
enum class EarlyRunnerFailure {
  none,
  descendant_scan,
  subreaper_setup,
};
#endif

struct RunnerOptions {
  std::filesystem::path child_executable;
  std::vector<std::string> child_argument_prefix;
  std::filesystem::path temporary_parent;
  std::filesystem::path delegated_cgroup_root;
  std::chrono::milliseconds child_timeout{std::chrono::seconds{5}};
  std::size_t maximum_child_output_bytes{maximum_child_record_bytes};
#if defined(AIFORGE_PROCESS_ISOLATION_TEST_SUPPORT)
  EarlyRunnerFailure early_failure{EarlyRunnerFailure::none};
  bool force_temporary_root_cleanup_failure{};
#endif
};

enum class RunnerErrorCode {
  invalid_options,
  platform_metadata,
  internal_error,
};

struct RunnerError {
  RunnerErrorCode code{RunnerErrorCode::internal_error};
  std::string message;
  auto operator==(const RunnerError&) const -> bool = default;
};

[[nodiscard]] auto run_evaluation(std::string source_sha,
                                  const RunnerOptions& options,
                                  std::stop_token stop_token = {})
    -> std::expected<EvidenceReport, RunnerError>;

#if defined(AIFORGE_PROCESS_ISOLATION_TEST_SUPPORT)
namespace test_support {

enum class BootstrapFailurePhase {
  pin_root,
  verify_ownership,
  verify_controllers,
  create_supervisor,
  move_to_supervisor,
  verify_supervisor,
  verify_root_empty,
  enable_controllers,
  verify_enabled_controllers,
  disable_controllers,
  move_to_root,
  await_empty,
  remove_supervisor,
};

[[nodiscard]] auto bootstrap_failure_outcome(BootstrapFailurePhase phase,
                                             bool rollback_complete)
    -> ProbeRecord;
[[nodiscard]] auto cleanup_outcome(ProbeRecord record, bool cleanup_complete)
    -> ProbeRecord;
[[nodiscard]] auto owns_task_cgroup(int process, std::string_view name) -> bool;

} // namespace test_support
#endif

} // namespace aiforge::evaluation::process_isolation::v2
