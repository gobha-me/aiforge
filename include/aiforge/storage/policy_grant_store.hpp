#pragma once

#include <expected>
#include <string>
#include <vector>

#include <aiforge/domain/events.hpp>

namespace aiforge::storage {

enum class PolicyGrantStoreErrorCode {
  unavailable,
  invalid_data,
  permission_denied,
  internal_failure,
};

struct PolicyGrantStoreError {
  PolicyGrantStoreErrorCode code;
  std::string message;
  bool retryable{};
  auto operator==(const PolicyGrantStoreError&) const -> bool = default;
};

struct SavedPolicyGrant {
  domain::PermissionProfileId permission_profile_id;
  std::string tool_name;
  std::vector<domain::Effect> effects;
  std::vector<domain::CapabilityScope> scopes;
  auto operator==(const SavedPolicyGrant&) const -> bool = default;
};

class PolicyGrantStore {
 public:
  virtual ~PolicyGrantStore() = default;

  [[nodiscard]] virtual auto load_grants(
      const domain::PermissionProfileId& permission_profile_id)
      -> std::expected<std::vector<SavedPolicyGrant>, PolicyGrantStoreError> = 0;

  [[nodiscard]] virtual auto save_grant(SavedPolicyGrant grant)
      -> std::expected<void, PolicyGrantStoreError> = 0;
};

}  // namespace aiforge::storage
