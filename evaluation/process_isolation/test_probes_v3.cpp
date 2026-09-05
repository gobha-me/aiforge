#include "probes_v3.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cerrno>
#include <cstdint>
#include <string_view>
#include <utility>

namespace isolation = aiforge::evaluation::process_isolation;
namespace v3 = aiforge::evaluation::process_isolation::v3;

namespace {

[[nodiscard]] auto complete_checks() -> v3::test_support::DirectTreeChecks {
  return {
      true, true, true, true, true, true, true, true, true,
      true, true, true, true, true, true, true, true, true,
  };
}

[[nodiscard]] auto complete_capability_checks()
    -> v3::test_support::LowCapabilityChecks {
  return {true, true, true, true, true, true, true, true, true, true, true};
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

TEST_CASE("every low-capability conjunct fails closed independently",
          "[process-isolation][evidence-v3][capability][failure]") {
  using Checks = v3::test_support::LowCapabilityChecks;
  constexpr std::array members{
      &Checks::pre_exec_verified,
      &Checks::descriptor_exec,
      &Checks::setup_descriptors_closed,
      &Checks::no_new_privileges,
      &Checks::capability_sets_empty,
      &Checks::ambient_empty,
      &Checks::bounding_subset,
      &Checks::namespace_creation_denied,
      &Checks::capability_regain_denied,
      &Checks::fork_descendant_rechecked,
      &Checks::clone_descendant_rechecked,
  };
  for (std::size_t index{}; index < members.size(); ++index) {
    CAPTURE(index);
    auto checks = complete_capability_checks();
    checks.*members[index] = false;
    const auto outcome = v3::test_support::low_capability_outcome(checks, true);
    CHECK(outcome.probe_id == v3::ProbeId::low_capability_nonescalation);
    CHECK(outcome.state == isolation::ProbeState::unavailable);
    CHECK(outcome.reason == v3::ReasonCode::enforcement_failed);
  }
}

TEST_CASE("low-capability cleanup uncertainty dominates enforcement",
          "[process-isolation][evidence-v3][capability][failure]") {
  const auto outcome = v3::test_support::low_capability_outcome(
      complete_capability_checks(), false);
  CHECK(outcome.state == isolation::ProbeState::probe_error);
  CHECK(outcome.reason == v3::ReasonCode::cleanup_failed);
}

TEST_CASE("low-capability prerequisites use stable closed reasons",
          "[process-isolation][evidence-v3][capability][failure]") {
  const auto architecture =
      v3::test_support::capability_prerequisite_outcome(false, true, true);
  CHECK(architecture.state == isolation::ProbeState::unavailable);
  CHECK(architecture.reason == v3::ReasonCode::unsupported_architecture);

  const auto unreadable =
      v3::test_support::capability_prerequisite_outcome(true, false, true);
  CHECK(unreadable.state == isolation::ProbeState::unavailable);
  CHECK(unreadable.reason == v3::ReasonCode::unsupported_kernel);

  const auto excessive =
      v3::test_support::capability_prerequisite_outcome(true, true, false);
  CHECK(excessive.state == isolation::ProbeState::unavailable);
  CHECK(excessive.reason == v3::ReasonCode::unsupported_kernel);

  const auto supported =
      v3::test_support::capability_prerequisite_outcome(true, true, true);
  CHECK(supported.state == isolation::ProbeState::enforced);
  CHECK(supported.reason == v3::ReasonCode::none);
}

TEST_CASE("complete low-capability observations are evidence only",
          "[process-isolation][evidence-v3][capability][smoke]") {
  const auto outcome = v3::test_support::low_capability_outcome(
      complete_capability_checks(), true);
  CHECK(outcome.state == isolation::ProbeState::enforced);
  CHECK(outcome.reason == v3::ReasonCode::none);
}

TEST_CASE("capability limit parsing rejects malformed and excessive inputs",
          "[process-isolation][evidence-v3][capability][failure]") {
  for (const auto input : {std::string_view{}, std::string_view{"-1"},
                           std::string_view{"+1"}, std::string_view{"40x"},
                           std::string_view{"40\n"}, std::string_view{"64"}}) {
    CAPTURE(input);
    const auto outcome = v3::test_support::cap_last_outcome(input);
    CHECK(outcome.state == isolation::ProbeState::unavailable);
    CHECK(outcome.reason == v3::ReasonCode::unsupported_kernel);
  }
  for (const auto input : {std::string_view{"0"}, std::string_view{"63"}}) {
    CAPTURE(input);
    const auto outcome = v3::test_support::cap_last_outcome(input);
    CHECK(outcome.state == isolation::ProbeState::enforced);
    CHECK(outcome.reason == v3::ReasonCode::none);
  }
}

TEST_CASE("bounding fingerprints reject every growth vector",
          "[process-isolation][evidence-v3][capability][failure]") {
  for (const auto& [launch, current] :
       {std::pair{std::uint64_t{0}, std::uint64_t{0}},
        std::pair{std::uint64_t{0xF}, std::uint64_t{0x5}},
        std::pair{std::uint64_t{1} << 63, std::uint64_t{1} << 63}}) {
    CAPTURE(launch, current);
    const auto outcome =
        v3::test_support::bounding_subset_outcome(launch, current);
    CHECK(outcome.state == isolation::ProbeState::enforced);
    CHECK(outcome.reason == v3::ReasonCode::none);
  }
  for (const auto& [launch, current] :
       {std::pair{std::uint64_t{0}, std::uint64_t{1}},
        std::pair{std::uint64_t{1}, std::uint64_t{2}},
        std::pair{std::uint64_t{0x7F}, std::uint64_t{0x80}},
        std::pair{std::uint64_t{0}, std::uint64_t{1} << 63}}) {
    CAPTURE(launch, current);
    const auto outcome =
        v3::test_support::bounding_subset_outcome(launch, current);
    CHECK(outcome.state == isolation::ProbeState::unavailable);
    CHECK(outcome.reason == v3::ReasonCode::enforcement_failed);
  }
}

TEST_CASE("capability mechanism failures keep stable closed classes",
          "[process-isolation][evidence-v3][capability][failure]") {
  for (const auto error_number : {EINVAL, ENOSYS}) {
    const auto outcome = v3::test_support::bounding_read_outcome(error_number);
    CHECK(outcome.state == isolation::ProbeState::unavailable);
    CHECK(outcome.reason == v3::ReasonCode::unsupported_kernel);
  }
  const auto denied = v3::test_support::bounding_read_outcome(EPERM);
  CHECK(denied.state == isolation::ProbeState::unavailable);
  CHECK(denied.reason == v3::ReasonCode::permission_denied);
  const auto unexpected = v3::test_support::bounding_read_outcome(EIO);
  CHECK(unexpected.state == isolation::ProbeState::probe_error);
  CHECK(unexpected.reason == v3::ReasonCode::internal_error);
}

TEST_CASE("x32 namespace alternatives cannot bypass the denial",
          "[process-isolation][evidence-v3][capability][failure]") {
  for (const auto error_number : {EPERM, ENOSYS}) {
    const auto outcome =
        v3::test_support::x32_namespace_outcome(-1, error_number);
    CHECK(outcome.state == isolation::ProbeState::enforced);
    CHECK(outcome.reason == v3::ReasonCode::none);
  }
  const auto escaped = v3::test_support::x32_namespace_outcome(0, 0);
  CHECK(escaped.state == isolation::ProbeState::unavailable);
  CHECK(escaped.reason == v3::ReasonCode::enforcement_failed);
  const auto unexpected = v3::test_support::x32_namespace_outcome(-1, EACCES);
  CHECK(unexpected.state == isolation::ProbeState::probe_error);
  CHECK(unexpected.reason == v3::ReasonCode::internal_error);
}
