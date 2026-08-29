#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>

namespace aiforge::evaluation::drawforge {

struct RunAccounting {
  std::uint64_t input_tokens{};
  std::uint64_t output_tokens{};
  double cost_usd{};
  auto operator==(const RunAccounting&) const -> bool = default;
};

enum class RunFinalizationErrorCode {
  metadata_unavailable,
  metadata_write_failed,
  submission_missing,
};

struct RunFinalizationError {
  RunFinalizationErrorCode code{RunFinalizationErrorCode::metadata_unavailable};
  std::string message;
  auto operator==(const RunFinalizationError&) const -> bool = default;
};

[[nodiscard]] auto record_accounting_and_validate_submission(
    const std::filesystem::path& metadata_path, RunAccounting accounting)
    -> std::expected<void, RunFinalizationError>;

} // namespace aiforge::evaluation::drawforge
