#include <aiforge/repository/exact_source_edit.hpp>
#include <aiforge/testing/scripted_exact_source_editor.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <stop_token>
#include <string>
#include <utility>

namespace {

using namespace aiforge;

template <typename Id> auto id(std::string value) -> Id {
  auto result = Id::from(std::move(value));
  REQUIRE(result);
  return std::move(*result);
}

auto digest(std::string value = "aaaaaaaaaaaaaaaa",
            const std::uint64_t bytes = 6) -> domain::ContentDigest {
  return {"test-sha256", std::move(value), bytes};
}

auto baseline(std::string fingerprint = "aaaaaaaaaaaaaaaa")
    -> domain::RepositorySnapshot {
  return {{id<domain::RepositoryId>("repository"), "/repository"},
          std::nullopt,
          {},
          digest(std::move(fingerprint), 0),
          std::chrono::sys_time<std::chrono::milliseconds>{
              std::chrono::milliseconds{100}}};
}

auto source(const domain::RepositorySnapshot& value,
            std::string content_digest = "bbbbbbbbbbbbbbbb",
            const std::uint64_t bytes = 6) -> domain::RepositorySourceIdentity {
  return {domain::snapshot_identity(value), "src/file.cpp",
          digest(std::move(content_digest), bytes), std::nullopt};
}

auto read_request() -> repository::ExactSourceReadRequest {
  return {baseline(), "src/file.cpp", {}};
}

auto edit_request() -> repository::ExactSourceEditRequest {
  auto base = baseline();
  return {base, source(base), {1, 3}, "xy", {}};
}

auto receipt(const repository::ExactSourceEditRequest& request)
    -> repository::ExactSourceEditReceipt {
  auto after = request.expected_source;
  after.snapshot.fingerprint = digest("cccccccccccccccc", 0);
  const auto result_bytes = request.expected_source.content_digest.byte_size -
                            (request.range.end - request.range.begin) +
                            request.replacement.size();
  after.content_digest = digest("dddddddddddddddd", result_bytes);
  return {
      request.expected_source,
      after,
      request.range,
      {request.range.begin, request.range.begin + request.replacement.size()},
      request.expected_source.snapshot,
      after.snapshot};
}

} // namespace

TEST_CASE("exact-source requests reject malformed paths limits and baselines",
          "[repository][edit][failure]") {
  auto read = read_request();
  read.relative_path = "../escape";
  REQUIRE_FALSE(repository::validate_exact_source_read_request(read));
  read = read_request();
  read.limits.timeout = std::chrono::milliseconds::zero();
  REQUIRE_FALSE(repository::validate_exact_source_read_request(read));
  read = read_request();
  read.baseline.root.canonical_path = "relative";
  REQUIRE_FALSE(repository::validate_exact_source_read_request(read));

  auto edit = edit_request();
  edit.expected_source.snapshot.fingerprint = digest("eeeeeeeeeeeeeeee", 0);
  REQUIRE_FALSE(repository::validate_exact_source_edit_request(edit));
  edit = edit_request();
  edit.expected_source.range = domain::SourceByteRange{0, 1};
  REQUIRE_FALSE(repository::validate_exact_source_edit_request(edit));
  edit = edit_request();
  edit.range = {4, 3};
  REQUIRE_FALSE(repository::validate_exact_source_edit_request(edit));
  edit = edit_request();
  edit.range.end = 7;
  REQUIRE_FALSE(repository::validate_exact_source_edit_request(edit));
  edit = edit_request();
  edit.limits.maximum_replacement_bytes = 1;
  const auto oversized = repository::validate_exact_source_edit_request(edit);
  REQUIRE_FALSE(oversized);
  REQUIRE(oversized.error().code ==
          repository::ExactSourceEditErrorCode::resource_exhausted);
}

TEST_CASE("exact-source receipt validation rejects invented outcomes",
          "[repository][edit][receipt][failure]") {
  const auto request = edit_request();
  auto result = receipt(request);
  result.previous_source.relative_path = "src/other.cpp";
  REQUIRE_FALSE(
      repository::validate_exact_source_edit_receipt(request, result));

  result = receipt(request);
  result.resulting_range.end += 1;
  REQUIRE_FALSE(
      repository::validate_exact_source_edit_receipt(request, result));

  result = receipt(request);
  result.resulting_source.content_digest.byte_size += 1;
  REQUIRE_FALSE(
      repository::validate_exact_source_edit_receipt(request, result));

  result = receipt(request);
  result.after_snapshot.repository_id =
      id<domain::RepositoryId>("other-repository");
  result.resulting_source.snapshot = result.after_snapshot;
  REQUIRE_FALSE(
      repository::validate_exact_source_edit_receipt(request, result));
}

TEST_CASE("scripted exact-source editor fails closed on cancellation and "
          "script drift",
          "[repository][edit][scripted][failure]") {
  const auto read = read_request();
  const repository::ExactSourceReadResult read_result{source(read.baseline),
                                                      "abcdef"};
  testing::ScriptedExactSourceEditor editor{{{read, read_result}}, {}};

  auto wrong = read;
  wrong.relative_path = "src/other.cpp";
  REQUIRE_FALSE(editor.read(wrong));
  REQUIRE(editor.remaining_read_exchanges() == 1);

  std::stop_source cancelled;
  cancelled.request_stop();
  const auto stopped = editor.read(read, cancelled.get_token());
  REQUIRE_FALSE(stopped);
  REQUIRE(stopped.error().code ==
          repository::ExactSourceEditErrorCode::cancelled);
  REQUIRE(editor.recorded_read_requests().size() == 1);

  const auto accepted = editor.read(read);
  REQUIRE(accepted == read_result);
  REQUIRE(editor.remaining_read_exchanges() == 0);
  REQUIRE_FALSE(editor.read(read));
}

TEST_CASE("scripted exact-source editor preserves typed failures and receipts",
          "[repository][edit][scripted]") {
  const auto request = edit_request();
  const auto expected_receipt = receipt(request);
  const repository::ExactSourceEditError conflict{
      repository::ExactSourceEditErrorCode::concurrent_change,
      "changed",
      request.expected_source.snapshot,
      request.expected_source,
      true,
      false};
  testing::ScriptedExactSourceEditor editor{
      {}, {{request, conflict}, {request, expected_receipt}}};

  const auto failed = editor.apply(request);
  REQUIRE_FALSE(failed);
  REQUIRE(failed.error() == conflict);
  const auto applied = editor.apply(request);
  REQUIRE(applied == expected_receipt);
  REQUIRE(editor.recorded_edit_requests().size() == 2);
  REQUIRE(editor.remaining_edit_exchanges() == 0);
}

TEST_CASE("valid exact-source insertion replacement and receipt are accepted",
          "[repository][edit][smoke]") {
  auto insertion = edit_request();
  insertion.range = {3, 3};
  insertion.replacement = "++";
  REQUIRE(repository::validate_exact_source_edit_request(insertion));
  REQUIRE(repository::validate_exact_source_edit_receipt(insertion,
                                                         receipt(insertion)));

  auto replacement = edit_request();
  replacement.range = {0, replacement.expected_source.content_digest.byte_size};
  replacement.replacement.clear();
  REQUIRE(repository::validate_exact_source_edit_request(replacement));
  REQUIRE(repository::validate_exact_source_edit_receipt(replacement,
                                                         receipt(replacement)));
}
