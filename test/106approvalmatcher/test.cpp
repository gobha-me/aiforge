#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <aiforge/runtime/automatic_approval_matcher.hpp>
#include <aiforge/runtime/repository_read_tool.hpp>

namespace {

using namespace aiforge;
using namespace std::chrono_literals;

template <typename Id> auto id(const std::string& value) -> Id {
  return Id::from(value).value();
}

class ScriptedPathAuthority final
    : public runtime::DescriptorRelativePathAuthority {
 public:
  explicit ScriptedPathAuthority(std::string identity)
      : m_identity(std::move(identity)) {}

  [[nodiscard]] auto identity() const noexcept -> std::string_view override {
    const auto call = identity_calls.fetch_add(1);
    if (call != 0 && identity_after_first) return *identity_after_first;
    return m_identity;
  }

  [[nodiscard]] auto contains(
      const std::string_view allowed_relative_path,
      const std::string_view candidate_relative_path) const
      -> std::expected<bool, runtime::AutomaticApprovalMatcherError> override {
    ++calls;
    if (fail) {
      return std::unexpected(runtime::AutomaticApprovalMatcherError{
          runtime::AutomaticApprovalMatcherErrorCode::path_unavailable,
          "descriptor-relative path verification is unavailable"});
    }
    if (replaced || symlink) return false;
    if (allowed_relative_path.empty()) return !candidate_relative_path.empty();
    return candidate_relative_path == allowed_relative_path ||
           (candidate_relative_path.starts_with(allowed_relative_path) &&
            candidate_relative_path.size() > allowed_relative_path.size() &&
            candidate_relative_path[allowed_relative_path.size()] == '/');
  }

  std::string m_identity;
  std::optional<std::string> identity_after_first;
  mutable std::atomic<std::size_t> identity_calls{};
  mutable std::atomic<std::size_t> calls{};
  bool fail{};
  bool replaced{};
  bool symlink{};
};

auto canonical(const std::string_view json) -> runtime::CanonicalToolArguments {
  auto value = runtime::canonicalize_validated_tool_arguments(
      {"application/json", std::string{json}});
  REQUIRE(value);
  return std::move(*value);
}

auto constraints(
    const std::uint64_t maximum_matches = 1, const std::uint32_t precedence = 0,
    const std::optional<std::chrono::milliseconds> expires_after = std::nullopt)
    -> runtime::AutomaticApprovalRuleConstraints {
  return {{runtime::RestrictionLevel::high},
          maximum_matches,
          expires_after,
          precedence};
}

auto exact_rule(std::string tool_name, const std::string_view json,
                runtime::AutomaticApprovalRuleConstraints rule_constraints =
                    constraints()) -> runtime::AutomaticApprovalRule {
  return runtime::ExactToolArgumentsApprovalRule{
      std::move(tool_name), canonical(json), std::move(rule_constraints)};
}

auto request(std::string invocation, std::string tool_name,
             const std::string_view json,
             const runtime::RestrictionLevel restriction =
                 runtime::RestrictionLevel::high)
    -> runtime::AutomaticApprovalMatchRequest {
  return {id<domain::SessionId>("session"),
          id<domain::RunId>("run"),
          id<domain::InvocationId>(invocation),
          std::move(tool_name),
          canonical(json),
          restriction,
          {},
          {}};
}

} // namespace

TEST_CASE("automatic approval matcher rejects malformed and overbound rules",
          "[approval-matcher][failure]") {
  auto invalid = exact_rule("read", R"({"path":"a"})");
  std::get<runtime::ExactToolArgumentsApprovalRule>(invalid)
      .constraints.maximum_matches = 0;
  REQUIRE_FALSE(runtime::compile_automatic_approval_matcher({invalid}));

  invalid = exact_rule("read", R"({"path":"a"})");
  std::get<runtime::ExactToolArgumentsApprovalRule>(invalid)
      .constraints.allowed_restrictions.clear();
  REQUIRE_FALSE(runtime::compile_automatic_approval_matcher({invalid}));

  invalid = exact_rule("read", R"({"path":"a"})");
  std::get<runtime::ExactToolArgumentsApprovalRule>(invalid)
      .constraints.allowed_restrictions = {runtime::RestrictionLevel::high,
                                           runtime::RestrictionLevel::high};
  REQUIRE_FALSE(runtime::compile_automatic_approval_matcher({invalid}));

  invalid = exact_rule("read", R"({"path":"a"})");
  std::get<runtime::ExactToolArgumentsApprovalRule>(invalid)
      .constraints.expires_after = 0ms;
  REQUIRE_FALSE(runtime::compile_automatic_approval_matcher({invalid}));

  invalid = exact_rule("read", R"({"path":"a"})");
  std::get<runtime::ExactToolArgumentsApprovalRule>(invalid)
      .constraints.expires_after = std::chrono::hours{24 * 366};
  REQUIRE_FALSE(runtime::compile_automatic_approval_matcher({invalid}));

  invalid = exact_rule("bad\ntool", R"({"path":"a"})");
  REQUIRE_FALSE(runtime::compile_automatic_approval_matcher({invalid}));

  const auto duplicate = exact_rule("read", R"({"path":"a"})");
  REQUIRE_FALSE(
      runtime::compile_automatic_approval_matcher({duplicate, duplicate}));

  runtime::AutomaticApprovalMatcherLimits limits;
  limits.maximum_rules = 1;
  REQUIRE_FALSE(runtime::compile_automatic_approval_matcher(
      {exact_rule("read", R"({"path":"a"})"),
       exact_rule("read", R"({"path":"b"})")},
      {}, limits));

  limits = {};
  limits.maximum_total_matches = 1;
  REQUIRE_FALSE(runtime::compile_automatic_approval_matcher(
      {exact_rule("read", R"({"path":"a"})", constraints(2))}, {}, limits));

  limits = {};
  limits.maximum_total_rule_bytes = 1;
  REQUIRE_FALSE(runtime::compile_automatic_approval_matcher(
      {exact_rule("read", R"({"path":"a"})")}, {}, limits));

  auto noncanonical = exact_rule("read", R"({"path":"a"})");
  std::get<runtime::ExactToolArgumentsApprovalRule>(noncanonical)
      .arguments.value.data = R"({ "path": "a" })";
  REQUIRE_FALSE(runtime::compile_automatic_approval_matcher({noncanonical}));
}

TEST_CASE("matcher identities are deterministic and semantic",
          "[approval-matcher][identity]") {
  const auto first = runtime::compile_automatic_approval_matcher(
      {exact_rule("read", R"({"path":"a"})", constraints(1, 1)),
       exact_rule("write", R"({"path":"b"})", constraints(2, 2))});
  const auto reordered = runtime::compile_automatic_approval_matcher(
      {exact_rule("write", R"({"path":"b"})", constraints(2, 2)),
       exact_rule("read", R"({"path":"a"})", constraints(1, 1))});
  const auto drifted = runtime::compile_automatic_approval_matcher(
      {exact_rule("write", R"({"path":"b"})", constraints(3, 2)),
       exact_rule("read", R"({"path":"a"})", constraints(1, 1))});
  REQUIRE(first);
  REQUIRE(reordered);
  REQUIRE(drifted);
  REQUIRE((*first)->identity() == (*reordered)->identity());
  REQUIRE((*first)->identity() != (*drifted)->identity());
  REQUIRE_FALSE((*first)->identity().contains("path"));
  REQUIRE_FALSE((*first)->identity().contains("read"));
}

TEST_CASE("every matcher limit is part of the policy identity",
          "[approval-matcher][identity][limits][failure]") {
  const auto baseline = runtime::compile_automatic_approval_matcher({});
  REQUIRE(baseline);
  const auto baseline_identity = std::string{(*baseline)->identity()};

  runtime::AutomaticApprovalMatcherLimits limits;
  std::vector<runtime::AutomaticApprovalMatcherLimits> variants;
  auto add_variant = [&](auto member) {
    auto variant = limits;
    --(variant.*member);
    variants.push_back(variant);
  };
  add_variant(&runtime::AutomaticApprovalMatcherLimits::maximum_rules);
  add_variant(
      &runtime::AutomaticApprovalMatcherLimits::maximum_tool_name_bytes);
  add_variant(&runtime::AutomaticApprovalMatcherLimits::maximum_identity_bytes);
  add_variant(&runtime::AutomaticApprovalMatcherLimits::
                  maximum_canonical_argument_bytes);
  add_variant(
      &runtime::AutomaticApprovalMatcherLimits::maximum_relative_path_bytes);
  add_variant(
      &runtime::AutomaticApprovalMatcherLimits::maximum_total_rule_bytes);
  add_variant(&runtime::AutomaticApprovalMatcherLimits::maximum_total_matches);
  limits.maximum_expiry -= 1ms;
  variants.push_back(limits);

  for (const auto& variant : variants) {
    const auto matcher =
        runtime::compile_automatic_approval_matcher({}, {}, variant);
    REQUIRE(matcher);
    REQUIRE((*matcher)->identity() != baseline_identity);
  }

  auto authority =
      std::make_shared<ScriptedPathAuthority>("sha256:" + std::string(64, 'a'));
  auto rule = runtime::make_repository_read_approval_rule(authority, "src",
                                                          constraints(1));
  REQUIRE(rule);
  const auto wide = runtime::compile_automatic_approval_matcher({*rule});
  auto narrow_limits = runtime::AutomaticApprovalMatcherLimits{};
  narrow_limits.maximum_relative_path_bytes = 3;
  const auto narrow =
      runtime::compile_automatic_approval_matcher({*rule}, {}, narrow_limits);
  auto narrow_canonical_limits = runtime::AutomaticApprovalMatcherLimits{};
  narrow_canonical_limits.maximum_canonical_argument_bytes = 8;
  const auto narrow_canonical = runtime::compile_automatic_approval_matcher(
      {*rule}, {}, narrow_canonical_limits);
  REQUIRE(wide);
  REQUIRE(narrow);
  REQUIRE(narrow_canonical);
  REQUIRE((*wide)->identity() != (*narrow)->identity());
  REQUIRE((*wide)->identity() != (*narrow_canonical)->identity());
  REQUIRE((*wide)
              ->match(request("wide", "read_repository_file",
                              R"({"relative_path":"src/a"})"))
              .value());
  REQUIRE_FALSE((*narrow)
                    ->match(request("narrow", "read_repository_file",
                                    R"({"relative_path":"src/a"})"))
                    .value());
  const auto overbound_request =
      (*narrow_canonical)
          ->match(request("narrow-canonical", "read_repository_file",
                          R"({"relative_path":"src/a"})"));
  REQUIRE_FALSE(overbound_request);
  REQUIRE(overbound_request.error().code ==
          runtime::AutomaticApprovalMatcherErrorCode::invalid_request);
}

TEST_CASE("canonical tool values preserve types and normalize JSON syntax",
          "[approval-matcher][canonical]") {
  const auto first = canonical(R"({"name":"café","count":1,"ok":true})");
  const auto reordered =
      canonical(" { \"ok\" : true, \"count\" : 1, \"name\" : \"café\" } ");
  REQUIRE(first == reordered);
  REQUIRE(first != canonical(R"({"name":"café","count":"1","ok":true})"));
  REQUIRE(first != canonical(R"({"name":"café","count":1,"ok":true})"));

  const auto maximum_unsigned = canonical(R"({"value":18446744073709551615})");
  const auto previous_unsigned = canonical(R"({"value":18446744073709551614})");
  REQUIRE(maximum_unsigned != previous_unsigned);
  REQUIRE(maximum_unsigned.value.data == R"({"value":18446744073709551615})");
  REQUIRE_FALSE(runtime::canonicalize_validated_tool_arguments(
      {"application/json", R"({"value":18446744073709551616})"}));
  REQUIRE_FALSE(runtime::canonicalize_validated_tool_arguments(
      {"application/json", R"({"value":18446744073709551617})"}));
  REQUIRE_FALSE(runtime::canonicalize_validated_tool_arguments(
      {"application/json", R"({"value":1.0})"}));
  REQUIRE_FALSE(runtime::canonicalize_validated_tool_arguments(
      {"application/json", R"({"value":1e0})"}));

  const auto numeric_matcher =
      runtime::compile_automatic_approval_matcher({exact_rule(
          "numeric", R"({"value":18446744073709551615})", constraints(1))});
  REQUIRE(numeric_matcher);
  REQUIRE_FALSE((*numeric_matcher)
                    ->match(request("previous", "numeric",
                                    R"({"value":18446744073709551614})"))
                    .value());
  REQUIRE((*numeric_matcher)
              ->match(request("maximum", "numeric",
                              R"({"value":18446744073709551615})"))
              .value());

  REQUIRE_FALSE(runtime::canonicalize_validated_tool_arguments(
      {"application/json", R"({"path":"a","path":"b"})"}));
  REQUIRE_FALSE(
      runtime::canonicalize_validated_tool_arguments({"text/plain", "path=a"}));
  REQUIRE_FALSE(runtime::canonicalize_validated_tool_arguments(
      {"application/json", std::string(64U * 1024U + 1U, 'x')}));
}

TEST_CASE("matcher denies misses ambiguity restriction drift and exhaustion",
          "[approval-matcher][failure]") {
  auto low = constraints(2, 1);
  low.allowed_restrictions = {runtime::RestrictionLevel::low};
  auto matcher = runtime::compile_automatic_approval_matcher(
      {exact_rule("read", R"({"path":"a"})", low)});
  REQUIRE(matcher);

  REQUIRE_FALSE((*matcher)
                    ->match(request("wrong-tool", "write", R"({"path":"a"})"))
                    .value());
  REQUIRE_FALSE((*matcher)
                    ->match(request("wrong-args", "read", R"({"path":"b"})"))
                    .value());
  REQUIRE_FALSE((*matcher)
                    ->match(request("wrong-level", "read", R"({"path":"a"})"))
                    .value());

  auto allowed = (*matcher)->match(request("one", "read", R"({"path":"a"})",
                                           runtime::RestrictionLevel::low));
  REQUIRE(allowed);
  REQUIRE(allowed->has_value());
  REQUIRE((*allowed)->policy_identity == (*matcher)->identity());
  REQUIRE_FALSE((*allowed)->rule_identity.empty());

  REQUIRE((*matcher)
              ->match(request("two", "read", R"({"path":"a"})",
                              runtime::RestrictionLevel::low))
              .value());
  REQUIRE_FALSE((*matcher)
                    ->match(request("three", "read", R"({"path":"a"})",
                                    runtime::RestrictionLevel::low))
                    .value());

  auto restarted = runtime::compile_automatic_approval_matcher(
      {exact_rule("read", R"({"path":"a"})", low)});
  REQUIRE(restarted);
  REQUIRE((*restarted)
              ->match(request("new-launch", "read", R"({"path":"a"})",
                              runtime::RestrictionLevel::low))
              .value());

  auto ambiguous = runtime::compile_automatic_approval_matcher(
      {exact_rule("read", R"({"path":"a"})", constraints(1, 7)),
       exact_rule("read", R"({"path":"a"})", constraints(2, 7))});
  REQUIRE(ambiguous);
  REQUIRE_FALSE((*ambiguous)
                    ->match(request("ambiguous", "read", R"({"path":"a"})"))
                    .value());
}

TEST_CASE(
    "unique higher precedence rule wins and invocation retries are stable",
    "[approval-matcher][concurrency]") {
  auto broad = constraints(1, 1);
  auto exact = constraints(1, 2);
  auto matcher = runtime::compile_automatic_approval_matcher(
      {exact_rule("read", R"({"path":"a"})", broad),
       exact_rule("read", R"({"path":"a"})", exact)});
  REQUIRE(matcher);

  const auto original = request("same", "read", R"({"path":"a"})");
  const auto first = (*matcher)->match(original);
  REQUIRE(first);
  REQUIRE(first->has_value());
  REQUIRE((*matcher)->match(original) == first);
  REQUIRE_FALSE((*matcher)->match(request("same", "read", R"({"path":"b"})")));
  auto changed_authority = original;
  changed_authority.effects = {domain::Effect::read};
  changed_authority.scopes = {
      {domain::Effect::read, "filesystem.root", "/different"}};
  REQUIRE_FALSE((*matcher)->match(changed_authority));

  auto one_slot = runtime::compile_automatic_approval_matcher(
      {exact_rule("read", R"({"path":"a"})")});
  REQUIRE(one_slot);
  std::atomic<std::size_t> allowed{};
  std::vector<std::jthread> workers;
  for (std::size_t index{}; index < 16; ++index) {
    workers.emplace_back([&, index] {
      auto matched = (*one_slot)->match(
          request("race-" + std::to_string(index), "read", R"({"path":"a"})"));
      if (matched && matched->has_value()) ++allowed;
    });
  }
  workers.clear();
  REQUIRE(allowed == 1);
}

TEST_CASE("matcher expiry is an exclusive monotonic application deadline",
          "[approval-matcher][expiry][failure]") {
  auto now = std::chrono::steady_clock::time_point{};
  std::size_t clock_samples{};
  auto clock = [&] {
    ++clock_samples;
    return now;
  };
  auto matcher = runtime::compile_automatic_approval_matcher(
      {exact_rule("read", R"({"path":"a"})", constraints(2, 0, 10ms))}, clock);
  REQUIRE(matcher);
  REQUIRE(clock_samples == 1);

  now += 9ms;
  const auto before = request("before", "read", R"({"path":"a"})");
  REQUIRE((*matcher)->match(before).value());
  REQUIRE(clock_samples == 2);
  now += 1ms;
  REQUIRE_FALSE((*matcher)
                    ->match(request("boundary", "read", R"({"path":"a"})"))
                    .value());
  REQUIRE_FALSE((*matcher)->match(before).value());
  REQUIRE(clock_samples == 4);
}

TEST_CASE("repository rules require descriptor-relative pinned-root membership",
          "[approval-matcher][repository][failure]") {
  auto authority =
      std::make_shared<ScriptedPathAuthority>("sha256:" + std::string(64, 'a'));
  auto rule = runtime::make_repository_read_approval_rule(authority, "src",
                                                          constraints(4));
  REQUIRE(rule);
  auto matcher = runtime::compile_automatic_approval_matcher({*rule});
  REQUIRE(matcher);

  const auto inside = request("inside", "read_repository_file",
                              R"({"relative_path":"src/lib/file.cpp"})");
  const auto inside_match = (*matcher)->match(inside);
  REQUIRE(inside_match);
  REQUIRE(inside_match->has_value());
  REQUIRE_FALSE((*inside_match)->rule_identity.contains("src"));
  REQUIRE_FALSE((*matcher)
                    ->match(request("prefix", "read_repository_file",
                                    R"({"relative_path":"src2/file.cpp"})"))
                    .value());
  REQUIRE_FALSE((*matcher)
                    ->match(request("control", "read_repository_file",
                                    R"({"relative_path":"src/\u0000x"})"))
                    .value());

  authority->symlink = true;
  REQUIRE_FALSE((*matcher)
                    ->match(request("symlink", "read_repository_file",
                                    R"({"relative_path":"src/link/file"})"))
                    .value());
  authority->symlink = false;
  authority->replaced = true;
  REQUIRE_FALSE((*matcher)->match(inside).value());
  REQUIRE_FALSE((*matcher)
                    ->match(request("replaced", "read_repository_file",
                                    R"({"relative_path":"src/lib/file.cpp"})"))
                    .value());
  authority->replaced = false;
  authority->fail = true;
  REQUIRE_FALSE((*matcher)
                    ->match(request("unavailable", "read_repository_file",
                                    R"({"relative_path":"src/lib/file.cpp"})"))
                    .value());

  authority->fail = false;
  authority->m_identity = "sha256:" + std::string(64, 'b');
  REQUIRE_FALSE((*matcher)
                    ->match(request("root-changed", "read_repository_file",
                                    R"({"relative_path":"src/lib/file.cpp"})"))
                    .value());

  REQUIRE_FALSE(runtime::make_repository_read_approval_rule(authority, "../src",
                                                            constraints()));
  authority->m_identity = "/secret/root";
  REQUIRE_FALSE(runtime::make_repository_read_approval_rule(authority, "src",
                                                            constraints()));
}

TEST_CASE("repository compilation snapshots one root identity",
          "[approval-matcher][repository][identity][failure]") {
  auto changing =
      std::make_shared<ScriptedPathAuthority>("sha256:" + std::string(64, 'a'));
  changing->identity_after_first = "/secret/replaced-root";
  runtime::AutomaticApprovalRule changing_rule{
      runtime::RepositoryReadPathApprovalRule{changing, "src", constraints()}};
  const auto matcher =
      runtime::compile_automatic_approval_matcher({changing_rule});
  REQUIRE(matcher);
  REQUIRE(changing->identity_calls == 1);
  REQUIRE_FALSE((*matcher)
                    ->match(request("changed", "read_repository_file",
                                    R"({"relative_path":"src/file"})"))
                    .value());
  REQUIRE(changing->identity_calls == 2);

  auto invalid = std::make_shared<ScriptedPathAuthority>("SecretToken123");
  const runtime::AutomaticApprovalRule invalid_rule{
      runtime::RepositoryReadPathApprovalRule{invalid, "src", constraints()}};
  REQUIRE_FALSE(runtime::compile_automatic_approval_matcher({invalid_rule}));
  REQUIRE(invalid->identity_calls == 1);

  auto changing_factory =
      std::make_shared<ScriptedPathAuthority>("sha256:" + std::string(64, 'd'));
  changing_factory->identity_after_first = "/secret/factory-drift";
  REQUIRE(runtime::make_repository_read_approval_rule(changing_factory, "src",
                                                      constraints()));
  REQUIRE(changing_factory->identity_calls == 1);

  auto invalid_factory =
      std::make_shared<ScriptedPathAuthority>("AnotherSecretToken456");
  REQUIRE_FALSE(runtime::make_repository_read_approval_rule(
      invalid_factory, "src", constraints()));
  REQUIRE(invalid_factory->identity_calls == 1);
}

TEST_CASE("automatic approval evidence uses closed digest identities",
          "[approval-matcher][identity][failure]") {
  const domain::AutomaticApprovalEvidence exact{
      "aiforge.auto-policy.v1.sha256:" + std::string(64, 'a'),
      "aiforge.auto-rule.exact.v1.sha256:" + std::string(64, 'b')};
  auto repository = exact;
  repository.rule_identity =
      "aiforge.auto-rule.repository.v1.sha256:" + std::string(64, 'c');
  REQUIRE(domain::valid_automatic_approval_evidence(exact));
  REQUIRE(domain::valid_automatic_approval_evidence(repository));

  auto invalid = exact;
  invalid.policy_identity = "SecretLikePolicyToken123";
  REQUIRE_FALSE(domain::valid_automatic_approval_evidence(invalid));
  invalid = exact;
  invalid.rule_identity = "SecretLikeRuleToken456";
  REQUIRE_FALSE(domain::valid_automatic_approval_evidence(invalid));
  invalid = exact;
  invalid.rule_identity.back() = 'F';
  REQUIRE_FALSE(domain::valid_automatic_approval_evidence(invalid));
}

TEST_CASE("empty automatic approval policy is valid and denies every request",
          "[approval-matcher][failure]") {
  auto first = runtime::compile_automatic_approval_matcher({});
  auto second = runtime::compile_automatic_approval_matcher({});
  REQUIRE(first);
  REQUIRE(second);
  REQUIRE((*first)->identity() == (*second)->identity());
  REQUIRE_FALSE((*first)->identity().contains("path"));
  REQUIRE_FALSE((*first)
                    ->match(request("denied", "read", R"({"secret":"value"})"))
                    .value());
}
