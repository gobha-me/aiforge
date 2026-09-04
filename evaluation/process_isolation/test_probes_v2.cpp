#include "probes_v2.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cerrno>
#include <utility>

namespace isolation = aiforge::evaluation::process_isolation;
namespace v2 = aiforge::evaluation::process_isolation::v2;

TEST_CASE("evidence v2 classifies cgroup prerequisites without authority",
          "[process-isolation][evidence-v2][failure]") {
  const auto absent = v2::test_support::cgroup_prerequisite_outcome(
      false, false, false, false, false);
  CHECK(absent.state == isolation::ProbeState::unavailable);
  CHECK(absent.reason == v2::ReasonCode::mechanism_absent);

  const auto undelegated = v2::test_support::cgroup_prerequisite_outcome(
      true, false, true, true, true);
  CHECK(undelegated.state == isolation::ProbeState::unavailable);
  CHECK(undelegated.reason == v2::ReasonCode::missing_delegation);

  for (const auto controllers :
       {std::array{false, true, true}, std::array{true, false, true},
        std::array{true, true, false}}) {
    const auto missing = v2::test_support::cgroup_prerequisite_outcome(
        true, true, controllers[0], controllers[1], controllers[2]);
    CHECK(missing.state == isolation::ProbeState::unavailable);
    CHECK(missing.reason == v2::ReasonCode::missing_controller);
  }

  const auto complete = v2::test_support::cgroup_prerequisite_outcome(
      true, true, true, true, true);
  CHECK(complete.state == isolation::ProbeState::enforced);
  CHECK(complete.reason == v2::ReasonCode::none);
}

TEST_CASE("migration and pid identity uncertainty fail closed",
          "[process-isolation][evidence-v2][failure]") {
  for (const auto denied_error : {EACCES, EPERM}) {
    const auto denied = v2::test_support::migration_attempt_outcome(
        true, denied_error, denied_error);
    CHECK(denied.state == isolation::ProbeState::enforced);
    CHECK(denied.reason == v2::ReasonCode::none);
  }
  for (const auto& result : {std::pair{0, EPERM}, std::pair{EPERM, 0}}) {
    const auto escaped = v2::test_support::migration_attempt_outcome(
        true, result.first, result.second);
    CHECK(escaped.state == isolation::ProbeState::unavailable);
    CHECK(escaped.reason == v2::ReasonCode::enforcement_failed);
  }
  const auto unexpected =
      v2::test_support::migration_attempt_outcome(true, EIO, EPERM);
  CHECK(unexpected.state == isolation::ProbeState::probe_error);
  CHECK(unexpected.reason == v2::ReasonCode::internal_error);
  const auto unconfined =
      v2::test_support::migration_attempt_outcome(false, EPERM, EPERM);
  CHECK(unconfined.state == isolation::ProbeState::unavailable);
  CHECK(unconfined.reason == v2::ReasonCode::unsupported_combination);

  for (const auto& input : {std::pair{false, false}, std::pair{false, true},
                            std::pair{true, false}}) {
    const auto unstable =
        v2::test_support::pid_identity_outcome(input.first, input.second);
    CHECK(unstable.state == isolation::ProbeState::probe_error);
    CHECK(unstable.reason == v2::ReasonCode::pid_reuse);
  }
  CHECK(v2::test_support::pid_identity_outcome(true, true).state ==
        isolation::ProbeState::enforced);
}

TEST_CASE("limit and execute evidence require causal observations",
          "[process-isolation][evidence-v2][failure]") {
  for (const auto& input : {std::pair{false, false}, std::pair{true, false},
                            std::pair{false, true}}) {
    const auto memory =
        v2::test_support::memory_limit_outcome(input.first, input.second);
    CHECK(memory.state == isolation::ProbeState::unavailable);
    CHECK(memory.reason == v2::ReasonCode::limit_not_triggered);
  }
  CHECK(v2::test_support::memory_limit_outcome(true, true).state ==
        isolation::ProbeState::enforced);

  CHECK(v2::test_support::pids_limit_outcome(false, true, true).state ==
        isolation::ProbeState::unavailable);
  CHECK(v2::test_support::pids_limit_outcome(true, false, true).state ==
        isolation::ProbeState::unavailable);
  const auto pids_cleanup =
      v2::test_support::pids_limit_outcome(true, true, false);
  CHECK(pids_cleanup.state == isolation::ProbeState::probe_error);
  CHECK(pids_cleanup.reason == v2::ReasonCode::cleanup_failed);
  CHECK(v2::test_support::pids_limit_outcome(true, true, true).state ==
        isolation::ProbeState::enforced);

  CHECK(v2::test_support::execute_confinement_outcome(false, true).state ==
        isolation::ProbeState::unavailable);
  CHECK(v2::test_support::execute_confinement_outcome(true, false).state ==
        isolation::ProbeState::unavailable);
  CHECK(v2::test_support::execute_confinement_outcome(true, true).state ==
        isolation::ProbeState::enforced);
}

TEST_CASE("combined setup never reports partial setup as enforcement",
          "[process-isolation][evidence-v2][failure]") {
  for (const auto& input : {
           std::array{false, true, true, false},
           std::array{true, false, true, false},
           std::array{true, true, false, false},
       }) {
    const auto partial = v2::test_support::setup_order_outcome(
        input[0], input[1], input[2], input[3], false);
    CHECK(partial.state == isolation::ProbeState::unavailable);
    CHECK(partial.reason == v2::ReasonCode::unsupported_combination);
  }
  const auto race =
      v2::test_support::setup_order_outcome(true, true, true, false, true);
  CHECK(race.state == isolation::ProbeState::probe_error);
  CHECK(race.reason == v2::ReasonCode::setup_race);
  const auto exited =
      v2::test_support::setup_order_outcome(true, true, true, true, false);
  CHECK(exited.state == isolation::ProbeState::probe_error);
  CHECK(exited.reason == v2::ReasonCode::setup_race);
  CHECK(v2::test_support::setup_order_outcome(true, true, true, true, true)
            .state == isolation::ProbeState::enforced);
}
