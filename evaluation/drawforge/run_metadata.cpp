#include "run_metadata.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <exception>
#include <fstream>
#include <string>
#include <utility>

namespace aiforge::evaluation::drawforge {
namespace {

using Json = nlohmann::json;

[[nodiscard]] auto read_metadata(const std::filesystem::path& path)
    -> std::expected<Json, RunFinalizationError> {
  try {
    std::ifstream input{path};
    if (!input) {
      return std::unexpected(
          RunFinalizationError{RunFinalizationErrorCode::metadata_unavailable,
                               "run metadata disappeared"});
    }
    auto value = Json::parse(input);
    if (!value.is_object()) {
      return std::unexpected(
          RunFinalizationError{RunFinalizationErrorCode::metadata_unavailable,
                               "run metadata is malformed"});
    }
    return value;
  } catch (const std::exception&) {
    return std::unexpected(
        RunFinalizationError{RunFinalizationErrorCode::metadata_unavailable,
                             "run metadata is malformed"});
  }
}

[[nodiscard]] auto write_metadata(const std::filesystem::path& path,
                                  const Json& value)
    -> std::expected<void, RunFinalizationError> {
  try {
    const auto temporary = path.string() + ".new";
    {
      std::ofstream output{temporary, std::ios::trunc};
      output << value.dump(2) << '\n';
      if (!output) {
        return std::unexpected(RunFinalizationError{
            RunFinalizationErrorCode::metadata_write_failed,
            "could not write run metadata"});
      }
    }
    std::filesystem::rename(temporary, path);
    return {};
  } catch (const std::filesystem::filesystem_error&) {
    return std::unexpected(
        RunFinalizationError{RunFinalizationErrorCode::metadata_write_failed,
                             "could not replace run metadata"});
  }
}

} // namespace

auto record_accounting_and_validate_submission(
    const std::filesystem::path& metadata_path, RunAccounting accounting)
    -> std::expected<void, RunFinalizationError> {
  auto metadata = read_metadata(metadata_path);
  if (!metadata) return std::unexpected(std::move(metadata.error()));

  try {
    (*metadata)["usage"]["input_tokens"] = accounting.input_tokens;
    (*metadata)["usage"]["output_tokens"] = accounting.output_tokens;
    (*metadata)["usage"]["cost_usd"] = accounting.cost_usd;
  } catch (const std::exception&) {
    return std::unexpected(
        RunFinalizationError{RunFinalizationErrorCode::metadata_unavailable,
                             "run metadata is malformed"});
  }

  auto written = write_metadata(metadata_path, *metadata);
  if (!written) return std::unexpected(std::move(written.error()));

  const auto events = metadata->find("events");
  if (events == metadata->end() || !events->is_array() ||
      std::find(events->begin(), events->end(), "submission_accepted") ==
          events->end()) {
    return std::unexpected(
        RunFinalizationError{RunFinalizationErrorCode::submission_missing,
                             "no submission was accepted"});
  }
  return {};
}

} // namespace aiforge::evaluation::drawforge
