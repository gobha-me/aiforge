#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <aiforge/domain/events.hpp>
#include <aiforge/runtime/application_launch_context.hpp>

namespace aiforge::runtime {

struct CanonicalToolArguments {
  std::string canonicalization_identity;
  domain::StructuredDataBlock value;
  auto operator==(const CanonicalToolArguments&) const -> bool = default;
};

enum class AutomaticApprovalMatcherErrorCode {
  invalid_configuration,
  invalid_request,
  path_unavailable,
  internal_failure,
};

struct AutomaticApprovalMatcherError {
  AutomaticApprovalMatcherErrorCode code{
      AutomaticApprovalMatcherErrorCode::internal_failure};
  std::string message;
  auto operator==(const AutomaticApprovalMatcherError&) const -> bool = default;
};

struct AutomaticApprovalMatcherLimits {
  std::size_t maximum_rules{256};
  std::size_t maximum_tool_name_bytes{128};
  std::size_t maximum_identity_bytes{128};
  std::size_t maximum_canonical_argument_bytes{64U * 1024U};
  std::size_t maximum_relative_path_bytes{4096};
  std::size_t maximum_total_rule_bytes{1024U * 1024U};
  std::uint64_t maximum_total_matches{1'000'000};
  std::chrono::milliseconds maximum_expiry{std::chrono::hours{24 * 365}};
  auto operator==(const AutomaticApprovalMatcherLimits&) const
      -> bool = default;
};

struct AutomaticApprovalRuleConstraints {
  std::vector<RestrictionLevel> allowed_restrictions;
  std::uint64_t maximum_matches{};
  std::optional<std::chrono::milliseconds> expires_after{};
  std::uint32_t precedence{};
  auto operator==(const AutomaticApprovalRuleConstraints&) const
      -> bool = default;
};

class DescriptorRelativePathAuthority {
 public:
  virtual ~DescriptorRelativePathAuthority() = default;

  // This is an opaque, bounded, secret-safe identity for the pinned root. It
  // must not contain the root path or an operating-system descriptor value.
  [[nodiscard]] virtual auto identity() const noexcept -> std::string_view = 0;

  // The adapter checks the current pinned-root identity and performs
  // descriptor-relative traversal without following symbolic links.
  [[nodiscard]] virtual auto contains(std::string_view allowed_relative_path,
                                      std::string_view candidate_relative_path)
      const -> std::expected<bool, AutomaticApprovalMatcherError> = 0;
};

struct ExactToolArgumentsApprovalRule {
  std::string tool_name;
  CanonicalToolArguments arguments;
  AutomaticApprovalRuleConstraints constraints;
  auto operator==(const ExactToolArgumentsApprovalRule&) const
      -> bool = default;
};

struct RepositoryReadPathApprovalRule {
  std::shared_ptr<const DescriptorRelativePathAuthority> root;
  std::string allowed_relative_path;
  AutomaticApprovalRuleConstraints constraints;
};

using AutomaticApprovalRule = std::variant<ExactToolArgumentsApprovalRule,
                                           RepositoryReadPathApprovalRule>;

struct AutomaticApprovalMatchRequest {
  domain::SessionId session_id;
  domain::RunId run_id;
  domain::InvocationId invocation_id;
  std::string tool_name;
  CanonicalToolArguments arguments;
  RestrictionLevel selected_restriction{RestrictionLevel::high};
  std::vector<domain::Effect> effects;
  std::vector<domain::CapabilityScope> scopes;
  auto operator==(const AutomaticApprovalMatchRequest&) const -> bool = default;
};

using AutomaticApprovalClock =
    std::function<std::chrono::steady_clock::time_point()>;

class AutomaticApprovalMatcher final {
 public:
  ~AutomaticApprovalMatcher();
  AutomaticApprovalMatcher(const AutomaticApprovalMatcher&) = delete;
  auto operator=(const AutomaticApprovalMatcher&)
      -> AutomaticApprovalMatcher& = delete;
  AutomaticApprovalMatcher(AutomaticApprovalMatcher&&) noexcept;
  auto operator=(AutomaticApprovalMatcher&&) noexcept
      -> AutomaticApprovalMatcher&;

  [[nodiscard]] auto identity() const noexcept -> std::string_view;
  [[nodiscard]] auto tool_names() const noexcept
      -> std::span<const std::string>;
  [[nodiscard]] auto match(const AutomaticApprovalMatchRequest& request)
      -> std::expected<std::optional<domain::AutomaticApprovalEvidence>,
                       AutomaticApprovalMatcherError>;

 private:
  struct Impl;
  explicit AutomaticApprovalMatcher(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> m_impl;

  friend auto compile_automatic_approval_matcher(
      std::vector<AutomaticApprovalRule>, AutomaticApprovalClock,
      AutomaticApprovalMatcherLimits)
      -> std::expected<std::shared_ptr<AutomaticApprovalMatcher>,
                       AutomaticApprovalMatcherError>;
};

[[nodiscard]] auto canonicalize_validated_tool_arguments(
    const domain::StructuredDataBlock& arguments,
    std::size_t maximum_bytes = 64U * 1024U)
    -> std::expected<CanonicalToolArguments, AutomaticApprovalMatcherError>;

[[nodiscard]] auto compile_automatic_approval_matcher(
    std::vector<AutomaticApprovalRule> rules, AutomaticApprovalClock clock = {},
    AutomaticApprovalMatcherLimits limits = {})
    -> std::expected<std::shared_ptr<AutomaticApprovalMatcher>,
                     AutomaticApprovalMatcherError>;

} // namespace aiforge::runtime
