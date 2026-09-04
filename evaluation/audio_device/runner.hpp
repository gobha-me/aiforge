#pragma once

#include "evidence.hpp"

#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace aiforge::evaluation::audio_device {

struct ChildCommand {
  std::filesystem::path executable;
  std::vector<std::string> argument_prefix;
  auto operator==(const ChildCommand&) const -> bool = default;
};

struct RunnerOptions {
  ChildCommand contract;
  ChildCommand rtaudio;
  ChildCommand miniaudio;
  std::chrono::milliseconds child_timeout{std::chrono::seconds{5}};
  std::size_t maximum_child_output_bytes{maximum_child_report_bytes};
};

enum class RunnerErrorCode {
  invalid_options,
  platform_metadata,
  cleanup_unavailable,
  internal_error,
};

struct RunnerError {
  RunnerErrorCode code{RunnerErrorCode::internal_error};
  std::string message;
  auto operator==(const RunnerError&) const -> bool = default;
};

[[nodiscard]] auto run_evaluation(std::string source_sha,
                                  const RunnerOptions& options)
    -> std::expected<EvidenceReport, RunnerError>;

} // namespace aiforge::evaluation::audio_device
