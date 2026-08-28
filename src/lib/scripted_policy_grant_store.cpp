#include <aiforge/testing/scripted_policy_grant_store.hpp>

#include <algorithm>

namespace aiforge::testing {

auto ScriptedPolicyGrantStore::load_grants(
    const domain::PermissionProfileId& permission_profile_id)
    -> std::expected<std::vector<storage::SavedPolicyGrant>,
                     storage::PolicyGrantStoreError> {
  ++m_load_count;
  if (m_load_error) return std::unexpected(*m_load_error);
  std::vector<storage::SavedPolicyGrant> result;
  std::ranges::copy_if(
      m_grants, std::back_inserter(result), [&](const auto& grant) {
        return grant.permission_profile_id == permission_profile_id;
      });
  return result;
}

auto ScriptedPolicyGrantStore::save_grant(storage::SavedPolicyGrant grant)
    -> std::expected<void, storage::PolicyGrantStoreError> {
  if (m_save_error) return std::unexpected(*m_save_error);
  m_grants.push_back(std::move(grant));
  return {};
}

} // namespace aiforge::testing
