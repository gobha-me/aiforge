#include <aiforge/adapters/configured_admin_sources.hpp>
#include <aiforge/adapters/linux_ops_source_preparation.hpp>
#include <aiforge/config/file_store.hpp>
#include <algorithm>
#include <array>

namespace aiforge::adapters {
namespace {
using Failure = surfaces::ManualOpsFailure;
using Code = surfaces::ManualOpsErrorCode;
using SourceError = runtime::OpsObservationSourceError;
using Result =
    std::expected<std::shared_ptr<surfaces::AdminSourceCatalog>, Failure>;
auto failure(Code code) -> std::unexpected<Failure> {
  return std::unexpected(Failure{code});
}
class ConfiguredAdminSources final : public surfaces::AdminSourceCatalog {
 public:
  ConfiguredAdminSources(config::OpsTargetsConfig records,
                         std::vector<surfaces::AdminTargetChoice> choices)
      : m_records(std::move(records)), m_choices(std::move(choices)) {}
  [[nodiscard]] auto guarantees_owned_metadata() const noexcept
      -> bool override {
    return true;
  }
  [[nodiscard]] auto targets() const noexcept
      -> std::span<const surfaces::AdminTargetChoice> override {
    return m_choices;
  }
  auto factory(domain::OpsTargetId target,
               domain::OpsConfigurationRevision revision)
      -> std::expected<std::shared_ptr<runtime::OpsSourcePreparationFactory>,
                       SourceError> override {
    try {
      const auto found = std::ranges::find(m_records.targets, target.value(),
                                           &config::OpsTargetConfig::id);
      if (found == m_records.targets.end())
        return std::unexpected(SourceError::invalid_result);
      if (!std::holds_alternative<config::LinuxLocalTargetConfig>(
              found->source))
        return std::unexpected(SourceError::unsupported);
      auto native = LinuxOpsSourcePreparationFactory::create(
          std::move(target), std::move(revision));
      if (!native) return std::unexpected(native.error());
      return std::move(*native);
    } catch (const std::bad_alloc&) {
      return std::unexpected(SourceError::resource_exhausted);
    } catch (...) {
      return std::unexpected(SourceError::internal_failure);
    }
  }

 private:
  const config::OpsTargetsConfig m_records;
  const std::vector<surfaces::AdminTargetChoice> m_choices;
};
} // namespace
auto make_configured_admin_sources(
    const config::ResolvedConfig& resolved) noexcept -> Result {
  try {
    auto records = config::resolve_ops_targets(resolved);
    if (!records) return failure(Code::invalid_input);
    std::vector<surfaces::AdminTargetChoice> choices;
    choices.reserve(records->targets.size());
    for (const auto& record : records->targets) {
      auto id = domain::OpsTargetId::from(record.id);
      if (!id) return failure(Code::invalid_input);
      const auto kind =
          std::holds_alternative<config::LinuxLocalTargetConfig>(record.source)
              ? domain::OpsTargetKind::linux_local
              : domain::OpsTargetKind::kubernetes;
      choices.push_back({std::move(*id), record.display_name, kind});
    }
    return std::make_shared<ConfiguredAdminSources>(std::move(*records),
                                                    std::move(choices));
  } catch (const std::bad_alloc&) {
    return failure(Code::resource_exhausted);
  } catch (...) {
    return failure(Code::internal_failure);
  }
}
auto load_process_admin_sources() noexcept -> Result {
  try {
    auto path = config::process_config_path();
    if (!path) return failure(Code::unavailable);
    const auto& registry = config::builtin_config_registry();
    auto file = config::JsonConfigFileStore{*path}.load(registry);
    if (!file) return failure(Code::unavailable);
    const std::array layers{std::move(*file)};
    auto resolved = config::resolve_config(registry, layers);
    if (!resolved) return failure(Code::invalid_input);
    return make_configured_admin_sources(*resolved);
  } catch (const std::bad_alloc&) {
    return failure(Code::resource_exhausted);
  } catch (...) {
    return failure(Code::internal_failure);
  }
}
} // namespace aiforge::adapters
