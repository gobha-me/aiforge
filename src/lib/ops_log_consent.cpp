#include <aiforge/runtime/ops_log_consent.hpp>

#include "ops_log_consent_state.hpp"
#include <aiforge/runtime/ops_observation_broker.hpp>
#include <algorithm>
#include <limits>
#include <utility>

namespace aiforge::runtime {
namespace {
using Error = domain::OpsTargetError;
using Code = domain::OpsTargetErrorCode;
auto failure(Code code, std::string message) -> std::unexpected<Error> {
  return std::unexpected(Error{code, std::move(message)});
}
constexpr auto maximum_revision =
    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
auto disabled(const domain::OpsObservationAuthoritySpec& value) -> bool {
  return !value.logs.enabled && value.logs.permitted_sources.empty();
}
auto source_valid(const domain::OpsObservationAuthoritySpec& authority,
                  const domain::OpsResourceIdentity& source) -> bool {
  return domain::validate_ops_resource_identity(authority.target, source, true)
      .has_value();
}
auto unavailable() -> std::unexpected<Error> {
  return failure(Code::stale_log_policy,
                 "Ops log consent activation is no longer current");
}
} // namespace

auto ops_log_consent_matches(
    const std::shared_ptr<OpsLogConsentActivation>& activation,
    const domain::OpsObservationAuthoritySpec& authority) -> bool {
  if (!activation) return false;
  const std::lock_guard lock{activation->mutex};
  return activation->active && activation->current &&
         *activation->current == authority;
}

auto revoke_ops_log_consent(
    const std::shared_ptr<OpsLogConsentActivation>& activation) -> void {
  if (!activation) return;
  const std::lock_guard lock{activation->mutex};
  activation->active = false;
  if (activation->current) {
    activation->current->logs.enabled = false;
    activation->current->logs.permitted_sources.clear();
  }
}

auto revoke_stale_ops_log_consent(
    const std::shared_ptr<OpsLogConsentActivation>& activation,
    const domain::OpsObservationAuthoritySpec& selected) -> void {
  if (!activation) return;
  const std::lock_guard lock{activation->mutex};
  if (!activation->active || !activation->current ||
      *activation->current == selected)
    return;
  activation->active = false;
  activation->current->logs.enabled = false;
  activation->current->logs.permitted_sources.clear();
}

OpsSessionLogConsent::OpsSessionLogConsent(
    std::shared_ptr<OpsLogConsentActivation> activation)
    : m_activation(std::move(activation)) {
}

auto OpsSessionLogConsent::start(
    const OpsObservationEndpoint& endpoint,
    domain::OpsObservationAuthoritySpec disabled_authority)
    -> std::expected<OpsSessionLogConsent, Error> {
  try {
    const auto& activation = endpoint.m_log_consent_activation;
    if (!activation) return unavailable();
    const std::lock_guard lock{activation->mutex};
    if (!activation->active || activation->current) return unavailable();
    if (disabled_authority.session_id != activation->session_id ||
        !disabled(disabled_authority))
      return failure(Code::invalid_authority,
                     "Ops session log consent must start disabled");
    auto checked = domain::OpsObservationAuthority::create(disabled_authority);
    if (!checked) return std::unexpected(checked.error());
    activation->current = std::move(disabled_authority);
    return OpsSessionLogConsent{activation};
  } catch (const std::bad_alloc&) {
    return failure(Code::resource_exhausted,
                   "Ops session log consent capacity is exhausted");
  } catch (...) {
    return failure(Code::internal_failure,
                   "Ops session log consent initialization failed");
  }
}

auto OpsSessionLogConsent::authority() const
    -> std::expected<domain::OpsObservationAuthority, Error> {
  try {
    if (!m_activation) return unavailable();
    const std::lock_guard lock{m_activation->mutex};
    if (!m_activation->active || !m_activation->current) return unavailable();
    return domain::OpsObservationAuthority::create(*m_activation->current);
  } catch (...) {
    return failure(Code::internal_failure,
                   "Ops log consent authority is unavailable");
  }
}

auto OpsSessionLogConsent::apply(const OpsLogConsentChange& change)
    -> std::expected<domain::OpsObservationAuthority, Error> {
  try {
    if (!m_activation) return unavailable();
    const std::lock_guard lock{m_activation->mutex};
    if (!m_activation->active || !m_activation->current) return unavailable();
    const auto& current = *m_activation->current;
    if (change.session_id != current.session_id ||
        change.target != current.target ||
        change.selection_generation != current.selection_generation ||
        change.log_policy_revision != current.logs.revision)
      return failure(Code::stale_log_policy,
                     "Ops log consent change is no longer current");
    if (!source_valid(current, change.source))
      return failure(Code::resource_mismatch,
                     "Ops log consent requires an exact application source");
    const auto found =
        std::ranges::find(current.logs.permitted_sources, change.source);
    if (change.enabled == (found != current.logs.permitted_sources.end()))
      return failure(Code::stale_log_policy,
                     "Ops log consent change has already been applied");
    if (current.selection_generation == maximum_revision ||
        current.logs.revision == maximum_revision) {
      m_activation->active = false;
      m_activation->current->logs.enabled = false;
      m_activation->current->logs.permitted_sources.clear();
      return failure(Code::resource_exhausted,
                     "Ops log consent revision is exhausted");
    }
    auto next = current;
    ++next.selection_generation;
    ++next.logs.revision;
    if (change.enabled)
      next.logs.permitted_sources.push_back(change.source);
    else
      next.logs.permitted_sources.erase(
          std::ranges::find(next.logs.permitted_sources, change.source));
    next.logs.enabled = !next.logs.permitted_sources.empty();
    auto checked = domain::OpsObservationAuthority::create(next);
    if (!checked) return std::unexpected(checked.error());
    m_activation->current = std::move(next);
    return checked;
  } catch (const std::bad_alloc&) {
    return failure(Code::resource_exhausted,
                   "Ops log consent capacity is exhausted");
  } catch (...) {
    return failure(Code::internal_failure,
                   "Ops log consent change failed internally");
  }
}

auto OpsSessionLogConsent::replace_selection(
    domain::OpsObservationAuthoritySpec disabled_authority)
    -> std::expected<domain::OpsObservationAuthority, Error> {
  try {
    if (!m_activation) return unavailable();
    const std::lock_guard lock{m_activation->mutex};
    if (!m_activation->active || !m_activation->current) return unavailable();
    const auto& current = *m_activation->current;
    if (disabled_authority.session_id != current.session_id ||
        disabled_authority.owner_id != current.owner_id ||
        disabled_authority.selection_generation <=
            current.selection_generation ||
        disabled_authority.logs.revision <= current.logs.revision ||
        !disabled(disabled_authority))
      return failure(Code::stale_selection,
                     "Replacement Ops selection must revoke log consent");
    auto checked = domain::OpsObservationAuthority::create(disabled_authority);
    if (!checked) return std::unexpected(checked.error());
    m_activation->current = std::move(disabled_authority);
    return checked;
  } catch (const std::bad_alloc&) {
    return failure(Code::resource_exhausted,
                   "Ops log consent replacement capacity is exhausted");
  } catch (...) {
    return failure(Code::internal_failure,
                   "Ops log consent replacement failed internally");
  }
}

} // namespace aiforge::runtime
