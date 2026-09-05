#include <aiforge/runtime/automatic_approval_matcher.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <map>
#include <mutex>
#include <ranges>
#include <set>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include <nlohmann/json.hpp>

#include <aiforge/detail/sha256.hpp>
#include <aiforge/detail/utf8_text.hpp>

namespace aiforge::runtime {
namespace {

using Json = nlohmann::json;

class DuplicateJsonKey final {};

[[nodiscard]] auto failure(const AutomaticApprovalMatcherErrorCode code,
                           std::string message)
    -> std::unexpected<AutomaticApprovalMatcherError> {
  return std::unexpected(
      AutomaticApprovalMatcherError{code, std::move(message)});
}

[[nodiscard]] auto valid_root_identity(const std::string_view value) -> bool {
  constexpr std::string_view prefix{"sha256:"};
  return value.size() == prefix.size() + 64U && value.starts_with(prefix) &&
         std::ranges::all_of(value.substr(prefix.size()),
                             [](const unsigned char character) {
                               return (character >= '0' && character <= '9') ||
                                      (character >= 'a' && character <= 'f');
                             });
}

[[nodiscard]] auto valid_text(const std::string_view value,
                              const std::size_t maximum) -> bool {
  return !value.empty() && value.size() <= maximum &&
         detail::is_safe_utf8_text(value) &&
         std::ranges::none_of(value, [](const unsigned char character) {
           return character < 0x20U || character == 0x7fU;
         });
}

[[nodiscard]] auto valid_restriction(const RestrictionLevel value) -> bool {
  switch (value) {
    case RestrictionLevel::high:
    case RestrictionLevel::medium:
    case RestrictionLevel::low:
    case RestrictionLevel::none: return true;
  }
  return false;
}

[[nodiscard]] auto restriction_name(const RestrictionLevel value)
    -> std::string_view {
  switch (value) {
    case RestrictionLevel::high: return "high";
    case RestrictionLevel::medium: return "medium";
    case RestrictionLevel::low: return "low";
    case RestrictionLevel::none: return "none";
  }
  return "invalid";
}

[[nodiscard]] auto effect_name(const domain::Effect value) -> std::string_view {
  switch (value) {
    case domain::Effect::read: return "read";
    case domain::Effect::write: return "write";
    case domain::Effect::remove: return "remove";
    case domain::Effect::execute: return "execute";
    case domain::Effect::network: return "network";
    case domain::Effect::communicate: return "communicate";
    case domain::Effect::spend: return "spend";
    case domain::Effect::change_infrastructure: return "change_infrastructure";
    case domain::Effect::change_privileges: return "change_privileges";
  }
  return "invalid";
}

[[nodiscard]] auto valid_relative_path(const std::string_view value,
                                       const std::size_t maximum,
                                       const bool allow_root) -> bool {
  if (value.empty()) return allow_root;
  if (!valid_text(value, maximum) || value.front() == '/' ||
      value.back() == '/' || value.find("//") != std::string_view::npos ||
      value.find('\\') != std::string_view::npos) {
    return false;
  }
  const std::filesystem::path path{value};
  if (path.is_absolute() || path.has_root_name() || path.has_root_directory() ||
      path.generic_string() != value || path.lexically_normal() != path) {
    return false;
  }
  return std::ranges::none_of(path, [](const auto& component) {
    return component.empty() || component == "." || component == ".." ||
           component == ".git";
  });
}

[[nodiscard]] auto parse_json(const std::string& text) -> Json {
  std::vector<std::set<std::string>> keys;
  const auto callback = [&keys](const int, const Json::parse_event_t event,
                                Json& parsed) {
    if (event == Json::parse_event_t::object_start) {
      keys.emplace_back();
    } else if (event == Json::parse_event_t::key) {
      if (keys.empty() || !keys.back().insert(parsed.get<std::string>()).second)
        throw DuplicateJsonKey{};
    } else if (event == Json::parse_event_t::object_end) {
      keys.pop_back();
    }
    return true;
  };
  return Json::parse(text, callback, true, false);
}

[[nodiscard]] auto has_only_exact_numbers(const Json& value) -> bool {
  std::vector<const Json*> pending{&value};
  while (!pending.empty()) {
    const auto* current = pending.back();
    pending.pop_back();
    if (current->is_number_float()) return false;
    if (current->is_array() || current->is_object()) {
      for (const auto& child : *current)
        pending.push_back(&child);
    }
  }
  return true;
}

auto append_field(detail::Sha256& digest, const std::string_view value)
    -> void {
  std::array<std::byte, 8> length{};
  const auto size = static_cast<std::uint64_t>(value.size());
  for (std::size_t index{}; index < length.size(); ++index) {
    const auto shift = static_cast<unsigned>((length.size() - index - 1U) * 8U);
    length[index] = static_cast<std::byte>((size >> shift) & 0xffU);
  }
  digest.update(length);
  digest.update(std::as_bytes(std::span{value.data(), value.size()}));
}

template <typename Integer>
auto append_integer(detail::Sha256& digest, const Integer value) -> void {
  std::array<std::byte, sizeof(Integer)> bytes{};
  using Unsigned = std::make_unsigned_t<Integer>;
  const auto converted = static_cast<Unsigned>(value);
  for (std::size_t index{}; index < bytes.size(); ++index) {
    const auto shift = static_cast<unsigned>((bytes.size() - index - 1U) * 8U);
    bytes[index] = static_cast<std::byte>((converted >> shift) & 0xffU);
  }
  digest.update(bytes);
}

auto append_matcher_limits(detail::Sha256& digest,
                           const AutomaticApprovalMatcherLimits& limits)
    -> void {
  append_field(digest, "aiforge.automatic-approval-limits.v1");
  append_integer(digest, static_cast<std::uint64_t>(limits.maximum_rules));
  append_integer(digest,
                 static_cast<std::uint64_t>(limits.maximum_tool_name_bytes));
  append_integer(digest,
                 static_cast<std::uint64_t>(limits.maximum_identity_bytes));
  append_integer(digest, static_cast<std::uint64_t>(
                             limits.maximum_canonical_argument_bytes));
  append_integer(
      digest, static_cast<std::uint64_t>(limits.maximum_relative_path_bytes));
  append_integer(digest,
                 static_cast<std::uint64_t>(limits.maximum_total_rule_bytes));
  append_integer(digest, limits.maximum_total_matches);
  append_integer(digest,
                 static_cast<std::int64_t>(limits.maximum_expiry.count()));
}

[[nodiscard]] auto canonical_request_digest(
    const AutomaticApprovalMatchRequest& request) -> std::string {
  detail::Sha256 digest;
  append_field(digest, "aiforge.automatic-approval-request.v1");
  append_field(digest, request.session_id.value());
  append_field(digest, request.run_id.value());
  append_field(digest, request.tool_name);
  append_field(digest, request.arguments.canonicalization_identity);
  append_field(digest, request.arguments.value.media_type);
  append_field(digest, request.arguments.value.data);
  append_field(digest, restriction_name(request.selected_restriction));
  for (const auto effect : request.effects) {
    append_field(digest, effect_name(effect));
  }
  for (const auto& scope : request.scopes) {
    append_field(digest, effect_name(scope.effect));
    append_field(digest, scope.kind);
    append_field(digest, scope.value);
  }
  return digest.finish();
}

[[nodiscard]] auto validate_constraints(
    const AutomaticApprovalRuleConstraints& constraints,
    const AutomaticApprovalMatcherLimits& limits)
    -> std::expected<std::vector<RestrictionLevel>,
                     AutomaticApprovalMatcherError> {
  if (constraints.allowed_restrictions.empty() ||
      constraints.allowed_restrictions.size() > 4U ||
      constraints.maximum_matches == 0 ||
      (constraints.expires_after &&
       (*constraints.expires_after <= std::chrono::milliseconds::zero() ||
        *constraints.expires_after > limits.maximum_expiry))) {
    return failure(AutomaticApprovalMatcherErrorCode::invalid_configuration,
                   "automatic approval rule constraints are invalid");
  }
  auto restrictions = constraints.allowed_restrictions;
  if (std::ranges::any_of(restrictions, [](const auto value) {
        return !valid_restriction(value);
      })) {
    return failure(AutomaticApprovalMatcherErrorCode::invalid_configuration,
                   "automatic approval rule restriction is invalid");
  }
  std::ranges::sort(restrictions, {}, [](const auto value) {
    return static_cast<std::underlying_type_t<RestrictionLevel>>(value);
  });
  if (std::ranges::adjacent_find(restrictions) != restrictions.end()) {
    return failure(AutomaticApprovalMatcherErrorCode::invalid_configuration,
                   "automatic approval rule restrictions must be unique");
  }
  return restrictions;
}

struct CompiledExactCondition {
  std::string tool_name;
  CanonicalToolArguments arguments;
};

struct CompiledRepositoryCondition {
  std::shared_ptr<const DescriptorRelativePathAuthority> root;
  std::string root_identity;
  std::string allowed_relative_path;
};

using CompiledCondition =
    std::variant<CompiledExactCondition, CompiledRepositoryCondition>;

struct CompiledRule {
  CompiledCondition condition;
  std::vector<RestrictionLevel> allowed_restrictions;
  std::uint64_t remaining_matches{};
  std::optional<std::chrono::steady_clock::time_point> expires_at;
  std::uint32_t precedence{};
  std::string identity;
};

struct ReservedDecision {
  std::string request_digest;
  domain::AutomaticApprovalEvidence evidence;
};

[[nodiscard]] auto exact_rule_identity(
    const ExactToolArgumentsApprovalRule& rule,
    const std::vector<RestrictionLevel>& restrictions) -> std::string {
  detail::Sha256 digest;
  append_field(digest, "aiforge.automatic-approval-rule.exact.v1");
  append_field(digest, rule.tool_name);
  append_field(digest, rule.arguments.canonicalization_identity);
  append_field(digest, rule.arguments.value.media_type);
  append_field(digest, rule.arguments.value.data);
  for (const auto restriction : restrictions)
    append_field(digest, restriction_name(restriction));
  append_integer(digest, rule.constraints.maximum_matches);
  append_integer(digest, rule.constraints.precedence);
  append_integer(digest, rule.constraints.expires_after
                             ? rule.constraints.expires_after->count()
                             : std::int64_t{-1});
  return "aiforge.auto-rule.exact.v1.sha256:" + digest.finish();
}

[[nodiscard]] auto repository_rule_identity(
    const RepositoryReadPathApprovalRule& rule,
    const std::string_view root_identity,
    const std::vector<RestrictionLevel>& restrictions) -> std::string {
  detail::Sha256 digest;
  append_field(digest, "aiforge.automatic-approval-rule.repository-read.v1");
  append_field(digest, "aiforge.canonical-tool-json.v1");
  append_field(digest, root_identity);
  append_field(digest, rule.allowed_relative_path);
  for (const auto restriction : restrictions)
    append_field(digest, restriction_name(restriction));
  append_integer(digest, rule.constraints.maximum_matches);
  append_integer(digest, rule.constraints.precedence);
  append_integer(digest, rule.constraints.expires_after
                             ? rule.constraints.expires_after->count()
                             : std::int64_t{-1});
  return "aiforge.auto-rule.repository.v1.sha256:" + digest.finish();
}

[[nodiscard]] auto valid_canonical_arguments(
    const CanonicalToolArguments& arguments,
    const AutomaticApprovalMatcherLimits& limits) -> bool {
  if (arguments.canonicalization_identity != "aiforge.canonical-tool-json.v1" ||
      arguments.value.media_type != "application/json" ||
      arguments.value.data.empty() ||
      arguments.value.data.size() > limits.maximum_canonical_argument_bytes) {
    return false;
  }
  try {
    const auto parsed = parse_json(arguments.value.data);
    return has_only_exact_numbers(parsed) &&
           parsed.dump() == arguments.value.data;
  } catch (...) {
    return false;
  }
}

[[nodiscard]] auto repository_candidate_path(
    const CanonicalToolArguments& arguments,
    const AutomaticApprovalMatcherLimits& limits)
    -> std::optional<std::string> {
  if (!valid_canonical_arguments(arguments, limits)) return std::nullopt;
  try {
    const auto parsed = parse_json(arguments.value.data);
    if (!parsed.is_object() || parsed.size() != 1 ||
        !parsed.contains("relative_path") ||
        !parsed.at("relative_path").is_string()) {
      return std::nullopt;
    }
    auto path = parsed.at("relative_path").get<std::string>();
    if (!valid_relative_path(path, limits.maximum_relative_path_bytes, false))
      return std::nullopt;
    return path;
  } catch (...) {
    return std::nullopt;
  }
}

[[nodiscard]] auto condition_matches(
    const CompiledRule& rule, const AutomaticApprovalMatchRequest& request,
    const AutomaticApprovalMatcherLimits& limits) -> bool {
  if (const auto* exact =
          std::get_if<CompiledExactCondition>(&rule.condition)) {
    return exact->tool_name == request.tool_name &&
           exact->arguments == request.arguments;
  }
  const auto& repository =
      std::get<CompiledRepositoryCondition>(rule.condition);
  if (request.tool_name != "read_repository_file" ||
      repository.root->identity() != repository.root_identity) {
    return false;
  }
  const auto path = repository_candidate_path(request.arguments, limits);
  if (!path) return false;
  const auto contained =
      repository.root->contains(repository.allowed_relative_path, *path);
  return contained.value_or(false);
}

} // namespace

struct AutomaticApprovalMatcher::Impl {
  std::string identity;
  AutomaticApprovalClock clock;
  AutomaticApprovalMatcherLimits limits;
  std::vector<CompiledRule> rules;
  std::vector<std::string> tool_names;
  std::map<std::tuple<domain::SessionId, domain::RunId, domain::InvocationId>,
           ReservedDecision>
      reservations;
  std::mutex mutex;
};

AutomaticApprovalMatcher::AutomaticApprovalMatcher(std::unique_ptr<Impl> impl)
    : m_impl(std::move(impl)) {
}

AutomaticApprovalMatcher::~AutomaticApprovalMatcher() = default;
AutomaticApprovalMatcher::AutomaticApprovalMatcher(
    AutomaticApprovalMatcher&&) noexcept = default;
auto AutomaticApprovalMatcher::operator=(AutomaticApprovalMatcher&&) noexcept
    -> AutomaticApprovalMatcher& = default;

auto AutomaticApprovalMatcher::identity() const noexcept -> std::string_view {
  return m_impl->identity;
}

auto AutomaticApprovalMatcher::tool_names() const noexcept
    -> std::span<const std::string> {
  return m_impl->tool_names;
}

auto AutomaticApprovalMatcher::match(
    const AutomaticApprovalMatchRequest& request)
    -> std::expected<std::optional<domain::AutomaticApprovalEvidence>,
                     AutomaticApprovalMatcherError> {
  try {
    std::scoped_lock lock{m_impl->mutex};
    if (!valid_text(request.tool_name,
                    m_impl->limits.maximum_tool_name_bytes) ||
        !valid_restriction(request.selected_restriction) ||
        !valid_canonical_arguments(request.arguments, m_impl->limits)) {
      return failure(AutomaticApprovalMatcherErrorCode::invalid_request,
                     "automatic approval request is invalid");
    }
    const auto request_digest = canonical_request_digest(request);
    const auto invocation_identity =
        std::tuple{request.session_id, request.run_id, request.invocation_id};
    const auto now = m_impl->clock();
    if (const auto prior = m_impl->reservations.find(invocation_identity);
        prior != m_impl->reservations.end()) {
      if (prior->second.request_digest != request_digest) {
        return failure(
            AutomaticApprovalMatcherErrorCode::invalid_request,
            "automatic approval invocation changed after reservation");
      }
      const auto selected =
          std::ranges::find(m_impl->rules, prior->second.evidence.rule_identity,
                            &CompiledRule::identity);
      if (selected == m_impl->rules.end() ||
          (selected->expires_at && now >= *selected->expires_at) ||
          !std::ranges::contains(selected->allowed_restrictions,
                                 request.selected_restriction) ||
          !condition_matches(*selected, request, m_impl->limits)) {
        return std::optional<domain::AutomaticApprovalEvidence>{};
      }
      return std::optional{prior->second.evidence};
    }

    std::vector<std::size_t> matches;
    std::uint32_t highest_precedence{};
    bool found{};
    for (std::size_t index{}; index < m_impl->rules.size(); ++index) {
      auto& rule = m_impl->rules[index];
      if (rule.remaining_matches == 0 ||
          (rule.expires_at && now >= *rule.expires_at) ||
          !std::ranges::contains(rule.allowed_restrictions,
                                 request.selected_restriction)) {
        continue;
      }
      if (!condition_matches(rule, request, m_impl->limits)) continue;
      if (!found || rule.precedence > highest_precedence) {
        matches = {index};
        highest_precedence = rule.precedence;
        found = true;
      } else if (rule.precedence == highest_precedence) {
        matches.push_back(index);
      }
    }
    if (matches.size() != 1) {
      return std::optional<domain::AutomaticApprovalEvidence>{};
    }
    auto& selected = m_impl->rules[matches.front()];
    --selected.remaining_matches;
    domain::AutomaticApprovalEvidence evidence{m_impl->identity,
                                               selected.identity};
    m_impl->reservations.emplace(std::move(invocation_identity),
                                 ReservedDecision{request_digest, evidence});
    return std::optional{std::move(evidence)};
  } catch (...) {
    return failure(AutomaticApprovalMatcherErrorCode::internal_failure,
                   "automatic approval matching failed internally");
  }
}

auto canonicalize_validated_tool_arguments(
    const domain::StructuredDataBlock& arguments,
    const std::size_t maximum_bytes)
    -> std::expected<CanonicalToolArguments, AutomaticApprovalMatcherError> {
  try {
    if (maximum_bytes == 0 || arguments.media_type != "application/json" ||
        arguments.data.empty() || arguments.data.size() > maximum_bytes) {
      return failure(AutomaticApprovalMatcherErrorCode::invalid_request,
                     "validated tool arguments cannot be canonicalized");
    }
    const auto parsed = parse_json(arguments.data);
    if (!has_only_exact_numbers(parsed)) {
      return failure(AutomaticApprovalMatcherErrorCode::invalid_request,
                     "validated tool arguments contain an unsupported number");
    }
    auto canonical = parsed.dump();
    if (canonical.empty() || canonical.size() > maximum_bytes) {
      return failure(AutomaticApprovalMatcherErrorCode::invalid_request,
                     "canonical tool arguments exceed their bound");
    }
    return CanonicalToolArguments{"aiforge.canonical-tool-json.v1",
                                  {"application/json", std::move(canonical)}};
  } catch (...) {
    return failure(AutomaticApprovalMatcherErrorCode::invalid_request,
                   "validated tool arguments cannot be canonicalized");
  }
}

auto compile_automatic_approval_matcher(
    std::vector<AutomaticApprovalRule> rules, AutomaticApprovalClock clock,
    AutomaticApprovalMatcherLimits limits)
    -> std::expected<std::shared_ptr<AutomaticApprovalMatcher>,
                     AutomaticApprovalMatcherError> {
  try {
    constexpr AutomaticApprovalMatcherLimits maximums;
    if (limits.maximum_rules == 0 ||
        limits.maximum_rules > maximums.maximum_rules ||
        limits.maximum_tool_name_bytes == 0 ||
        limits.maximum_tool_name_bytes > maximums.maximum_tool_name_bytes ||
        limits.maximum_identity_bytes == 0 ||
        limits.maximum_identity_bytes > maximums.maximum_identity_bytes ||
        limits.maximum_canonical_argument_bytes == 0 ||
        limits.maximum_canonical_argument_bytes >
            maximums.maximum_canonical_argument_bytes ||
        limits.maximum_relative_path_bytes == 0 ||
        limits.maximum_relative_path_bytes >
            maximums.maximum_relative_path_bytes ||
        limits.maximum_total_rule_bytes == 0 ||
        limits.maximum_total_rule_bytes > maximums.maximum_total_rule_bytes ||
        limits.maximum_total_matches == 0 ||
        limits.maximum_total_matches > maximums.maximum_total_matches ||
        limits.maximum_expiry <= std::chrono::milliseconds::zero() ||
        limits.maximum_expiry > maximums.maximum_expiry ||
        rules.size() > limits.maximum_rules) {
      return failure(AutomaticApprovalMatcherErrorCode::invalid_configuration,
                     "automatic approval matcher limits are invalid");
    }
    if (!clock) clock = [] { return std::chrono::steady_clock::now(); };
    const auto launch_time = clock();
    std::vector<CompiledRule> compiled;
    compiled.reserve(rules.size());
    std::set<std::string> identities;
    std::set<std::string> tool_names;
    std::size_t total_bytes{};
    std::uint64_t total_matches{};
    for (auto& rule : rules) {
      const auto& rule_constraints = std::visit(
          [](const auto& value) -> const AutomaticApprovalRuleConstraints& {
            return value.constraints;
          },
          rule);
      auto restrictions = validate_constraints(rule_constraints, limits);
      if (!restrictions)
        return std::unexpected(std::move(restrictions.error()));
      if (rule_constraints.maximum_matches >
          limits.maximum_total_matches - total_matches) {
        return failure(AutomaticApprovalMatcherErrorCode::invalid_configuration,
                       "automatic approval match count is overbound");
      }
      total_matches += rule_constraints.maximum_matches;

      std::string identity;
      CompiledCondition condition;
      if (auto* exact = std::get_if<ExactToolArgumentsApprovalRule>(&rule)) {
        if (!valid_text(exact->tool_name, limits.maximum_tool_name_bytes) ||
            !valid_canonical_arguments(exact->arguments, limits)) {
          return failure(
              AutomaticApprovalMatcherErrorCode::invalid_configuration,
              "exact automatic approval rule is invalid");
        }
        identity = exact_rule_identity(*exact, *restrictions);
        total_bytes += exact->tool_name.size() +
                       exact->arguments.canonicalization_identity.size() +
                       exact->arguments.value.media_type.size() +
                       exact->arguments.value.data.size();
        condition = CompiledExactCondition{std::move(exact->tool_name),
                                           std::move(exact->arguments)};
        tool_names.insert(
            std::get<CompiledExactCondition>(condition).tool_name);
      } else {
        auto& repository = std::get<RepositoryReadPathApprovalRule>(rule);
        if (!repository.root) {
          return failure(
              AutomaticApprovalMatcherErrorCode::invalid_configuration,
              "repository-read automatic approval rule is invalid");
        }
        const std::string root_identity{repository.root->identity()};
        if (!valid_root_identity(root_identity) ||
            root_identity.size() > limits.maximum_identity_bytes ||
            !valid_relative_path(repository.allowed_relative_path,
                                 limits.maximum_relative_path_bytes, true)) {
          return failure(
              AutomaticApprovalMatcherErrorCode::invalid_configuration,
              "repository-read automatic approval rule is invalid");
        }
        identity =
            repository_rule_identity(repository, root_identity, *restrictions);
        total_bytes +=
            root_identity.size() + repository.allowed_relative_path.size();
        condition = CompiledRepositoryCondition{
            std::move(repository.root), std::move(root_identity),
            std::move(repository.allowed_relative_path)};
        tool_names.insert("read_repository_file");
      }
      if (total_bytes > limits.maximum_total_rule_bytes ||
          !identities.insert(identity).second) {
        return failure(AutomaticApprovalMatcherErrorCode::invalid_configuration,
                       "automatic approval rules are duplicate or overbound");
      }
      std::optional<std::chrono::steady_clock::time_point> expires_at;
      if (rule_constraints.expires_after) {
        if (launch_time > std::chrono::steady_clock::time_point::max() -
                              *rule_constraints.expires_after) {
          return failure(
              AutomaticApprovalMatcherErrorCode::invalid_configuration,
              "automatic approval expiry cannot be represented");
        }
        expires_at = launch_time + *rule_constraints.expires_after;
      }
      compiled.push_back({std::move(condition), std::move(*restrictions),
                          rule_constraints.maximum_matches, expires_at,
                          rule_constraints.precedence, std::move(identity)});
    }

    detail::Sha256 policy_digest;
    append_field(policy_digest, "aiforge.automatic-approval-policy.v1");
    append_matcher_limits(policy_digest, limits);
    for (const auto& identity : identities)
      append_field(policy_digest, identity);
    auto impl = std::make_unique<AutomaticApprovalMatcher::Impl>();
    impl->identity = "aiforge.auto-policy.v1.sha256:" + policy_digest.finish();
    impl->clock = std::move(clock);
    impl->limits = limits;
    impl->rules = std::move(compiled);
    impl->tool_names.assign(tool_names.begin(), tool_names.end());
    return std::shared_ptr<AutomaticApprovalMatcher>{
        new AutomaticApprovalMatcher{std::move(impl)}};
  } catch (...) {
    return failure(AutomaticApprovalMatcherErrorCode::internal_failure,
                   "automatic approval matcher compilation failed internally");
  }
}

} // namespace aiforge::runtime
