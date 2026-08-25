#include <aiforge/adapters/process_models.hpp>

#include <aiforge/adapters/process_model_catalog.hpp>

#include <algorithm>
#include <iomanip>
#include <ostream>
#include <ranges>
#include <sstream>
#include <string>
#include <vector>

namespace aiforge::adapters {
namespace {

[[nodiscard]] auto failure(const cli::CommandFailureKind kind,
                           std::string message)
    -> std::unexpected<cli::CommandFailure> {
  return std::unexpected(cli::CommandFailure{kind, std::move(message)});
}

[[nodiscard]] auto capabilities(const model::CatalogEntry& entry)
    -> std::string {
  std::string result;
  for (const auto& capability : entry.capabilities) {
    if (!capability.supported.value_or(false)) continue;
    if (!result.empty()) result.push_back(',');
    result.append(model::capability_name(capability.capability));
  }
  return result.empty() ? "-" : result;
}

[[nodiscard]] auto usd(const std::optional<model::Price>& value) -> std::string {
  if (!value || !value->usd) return "-";
  return "$" + value->usd->to_string();
}

}  // namespace

auto ProcessModelsCommand::execute(cli::CommandEnvironment& environment,
                                   std::ostream& output, std::ostream& error)
    -> std::expected<void, cli::CommandFailure> {
  auto catalog = ProcessModelCatalog::create();
  if (!catalog) {
    return failure(cli::CommandFailureKind::runtime, catalog.error().message);
  }
  auto resolved = (*catalog)->service().snapshot(environment.stop_token);
  if (!resolved) {
    return failure(resolved.error().code == model::CatalogErrorCode::cancelled
                       ? cli::CommandFailureKind::cancelled
                       : cli::CommandFailureKind::runtime,
                   resolved.error().message);
  }
  for (const auto& warning : resolved->get().warnings)
    error << "aiforge: warning: " << warning << '\n';

  output << "ID\tTYPE\tCONTEXT\tCAPABILITIES\tINPUT_USD\tOUTPUT_USD\tSTATUS\n";
  for (const auto& entry : resolved->get().entries) {
    output << entry.id.value() << '\t' << entry.type << '\t';
    if (entry.context_window_tokens)
      output << *entry.context_window_tokens;
    else
      output << '-';
    output << '\t' << capabilities(entry) << '\t';
    if (entry.pricing) {
      output << usd(entry.pricing->base.input) << '\t'
             << usd(entry.pricing->base.output);
    } else {
      output << "-\t-";
    }
    output << '\t' << (entry.offline ? "offline" : "available") << '\n';
  }
  if (!output || !error)
    return failure(cli::CommandFailureKind::runtime,
                   "model catalog output failed");
  return {};
}

}  // namespace aiforge::adapters
