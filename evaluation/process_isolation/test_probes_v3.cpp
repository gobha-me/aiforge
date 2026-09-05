#include "probes_v3.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cerrno>

namespace isolation = aiforge::evaluation::process_isolation;
namespace v3 = aiforge::evaluation::process_isolation::v3;

namespace {

[[nodiscard]] auto complete_checks() -> v3::test_support::DirectTreeChecks {
  return {
      true, true, true, true, true, true, true, true, true,
      true, true, true, true, true, true, true, true, true,
  };
}

} // namespace

TEST_CASE("every direct-tree conjunct fails closed independently",
          "[process-isolation][evidence-v3][direct-tree][failure]") {
  using Checks = v3::test_support::DirectTreeChecks;
  constexpr std::array members{
      &Checks::payload_identity,
      &Checks::descriptor_exec,
      &Checks::setup_descriptors_closed,
      &Checks::path_procs_denied,
      &Checks::path_threads_denied,
      &Checks::readable_descriptor_procs_denied,
      &Checks::readable_descriptor_threads_denied,
      &Checks::path_descriptor_procs_denied,
      &Checks::path_descriptor_threads_denied,
      &Checks::clone_into_parent_denied,
      &Checks::clone_into_sibling_denied,
      &Checks::fork_created,
      &Checks::clone_created,
      &Checks::thread_created,
      &Checks::session_detached,
      &Checks::reparented_descendant_created,
      &Checks::processes_contained,
      &Checks::threads_contained,
  };
  for (std::size_t index{}; index < members.size(); ++index) {
    CAPTURE(index);
    auto checks = complete_checks();
    checks.*members[index] = false;
    const auto outcome = v3::test_support::direct_tree_outcome(checks, true);
    CHECK(outcome.probe_id ==
          v3::ProbeId::direct_process_tree_cgroup_nonescape);
    CHECK(outcome.state == isolation::ProbeState::unavailable);
    CHECK(outcome.reason == v3::ReasonCode::enforcement_failed);
  }
}

TEST_CASE("direct-tree cleanup uncertainty dominates enforcement",
          "[process-isolation][evidence-v3][direct-tree][failure]") {
  const auto outcome =
      v3::test_support::direct_tree_outcome(complete_checks(), false);
  CHECK(outcome.state == isolation::ProbeState::probe_error);
  CHECK(outcome.reason == v3::ReasonCode::cleanup_failed);
}

TEST_CASE("direct-tree prerequisites use stable unavailable reasons",
          "[process-isolation][evidence-v3][direct-tree][failure]") {
  const auto architecture =
      v3::test_support::prerequisite_outcome(false, true, true);
  CHECK(architecture.state == isolation::ProbeState::unavailable);
  CHECK(architecture.reason == v3::ReasonCode::unsupported_architecture);

  const auto delegation =
      v3::test_support::prerequisite_outcome(true, false, true);
  CHECK(delegation.state == isolation::ProbeState::unavailable);
  CHECK(delegation.reason == v3::ReasonCode::missing_delegation);

  const auto controller =
      v3::test_support::prerequisite_outcome(true, true, false);
  CHECK(controller.state == isolation::ProbeState::unavailable);
  CHECK(controller.reason == v3::ReasonCode::missing_controller);

  const auto supported =
      v3::test_support::prerequisite_outcome(true, true, true);
  CHECK(supported.state == isolation::ProbeState::enforced);
  CHECK(supported.reason == v3::ReasonCode::none);
}

TEST_CASE("direct-tree escape observations retain closed failure classes",
          "[process-isolation][evidence-v3][direct-tree][failure]") {
  const std::array<int, 4> denied_paths{EACCES, EPERM, EACCES, EPERM};
  const std::array<int, 8> denied_descriptors{EACCES, EPERM, EACCES, EPERM,
                                              EACCES, EPERM, EACCES, EPERM};
  const std::array<int, 4> denied_clones{EPERM, EPERM, EPERM, EPERM};

  const auto enforced = v3::test_support::escape_attempt_outcome(
      denied_paths, denied_descriptors, denied_clones);
  CHECK(enforced.state == isolation::ProbeState::enforced);
  CHECK(enforced.reason == v3::ReasonCode::none);

  auto escaped_paths = denied_paths;
  escaped_paths[2] = 0;
  const auto escaped = v3::test_support::escape_attempt_outcome(
      escaped_paths, denied_descriptors, denied_clones);
  CHECK(escaped.state == isolation::ProbeState::unavailable);
  CHECK(escaped.reason == v3::ReasonCode::enforcement_failed);

  auto invalid_descriptors = denied_descriptors;
  invalid_descriptors[5] = ENOENT;
  const auto invalid = v3::test_support::escape_attempt_outcome(
      denied_paths, invalid_descriptors, denied_clones);
  CHECK(invalid.state == isolation::ProbeState::probe_error);
  CHECK(invalid.reason == v3::ReasonCode::internal_error);

  auto invalid_clones = denied_clones;
  invalid_clones[1] = EACCES;
  const auto unexpected = v3::test_support::escape_attempt_outcome(
      denied_paths, denied_descriptors, invalid_clones);
  CHECK(unexpected.state == isolation::ProbeState::probe_error);
  CHECK(unexpected.reason == v3::ReasonCode::internal_error);
}

TEST_CASE("complete direct-tree observations are evidence only",
          "[process-isolation][evidence-v3][direct-tree][smoke]") {
  const auto outcome =
      v3::test_support::direct_tree_outcome(complete_checks(), true);
  CHECK(outcome.state == isolation::ProbeState::enforced);
  CHECK(outcome.reason == v3::ReasonCode::none);
}
