#pragma once

#include <optional>
#include <utility>
#include <vector>

#include <aiforge/storage/policy_grant_store.hpp>

namespace aiforge::testing {

class ScriptedPolicyGrantStore final : public storage::PolicyGrantStore {
 public:
  explicit ScriptedPolicyGrantStore(
      std::vector<storage::SavedPolicyGrant> grants = {})
      : m_grants(std::move(grants)) {}

  auto fail_load(storage::PolicyGrantStoreError error) -> void {
    m_load_error = std::move(error);
  }
  auto fail_save(storage::PolicyGrantStoreError error) -> void {
    m_save_error = std::move(error);
  }
  auto clear_failures() noexcept -> void {
    m_load_error.reset();
    m_save_error.reset();
  }

  [[nodiscard]] auto load_grants(
      const domain::PermissionProfileId& permission_profile_id)
      -> std::expected<std::vector<storage::SavedPolicyGrant>,
                       storage::PolicyGrantStoreError> override;
  [[nodiscard]] auto save_grant(storage::SavedPolicyGrant grant)
      -> std::expected<void, storage::PolicyGrantStoreError> override;

  [[nodiscard]] auto load_count() const noexcept -> std::size_t {
    return m_load_count;
  }
  [[nodiscard]] auto saved_grants() const noexcept
      -> const std::vector<storage::SavedPolicyGrant>& {
    return m_grants;
  }

 private:
  std::vector<storage::SavedPolicyGrant> m_grants;
  std::optional<storage::PolicyGrantStoreError> m_load_error;
  std::optional<storage::PolicyGrantStoreError> m_save_error;
  std::size_t m_load_count{};
};

} // namespace aiforge::testing
