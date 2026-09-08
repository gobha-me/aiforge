#include <aiforge/detail/sha256.hpp>
#include <aiforge/runtime/context_builder.hpp>
#include <aiforge/runtime/local_context_controller.hpp>
#include <catch2/catch_test_macros.hpp>
#include <functional>
#include <limits>
#include <stdexcept>
#include <thread>

namespace {
using namespace aiforge;
auto session() -> domain::SessionId {
  return domain::SessionId::from("session").value();
}
auto source() -> domain::LocalSourceIdentity {
  return {{1, std::string(64, 'b')},
          "notes.txt",
          {"sha256",
           "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824",
           5}};
}
class Reader final : public runtime::LocalSourceReader {
 public:
  bool trusted{true};
  std::size_t calls{};
  std::string text{"hello"};
  std::function<void()> after_read;
  auto guarantees_pinned_read_only_sources() const noexcept -> bool override {
    return trusted;
  }
  auto list(runtime::LocalListRequest, std::stop_token)
      -> std::expected<runtime::LocalListResult,
                       domain::LocalSourceError> override {
    FAIL("context preparation must not list");
    return std::unexpected(domain::LocalSourceError{
        domain::LocalSourceErrorCode::internal_failure, "unexpected"});
  }
  auto preview(runtime::LocalPreviewRequest, std::stop_token)
      -> std::expected<runtime::LocalPreviewResult,
                       domain::LocalSourceError> override {
    FAIL("context preparation must revalidate exact sources");
    return std::unexpected(domain::LocalSourceError{
        domain::LocalSourceErrorCode::internal_failure, "unexpected"});
  }
  auto revalidate(runtime::LocalRevalidateRequest request, std::stop_token)
      -> std::expected<runtime::LocalReadResult,
                       domain::LocalSourceError> override {
    ++calls;
    if (after_read) after_read();
    return runtime::LocalReadResult{request.token, request.expected_source,
                                    text};
  }
};
class Grants final : public runtime::LocalContextGrantResolver {
 public:
  std::shared_ptr<Reader> reader{std::make_shared<Reader>()};
  std::uint64_t generation{1};
  bool absent{};
  std::size_t resolves{};
  auto resolve(const domain::SessionId& owner,
               const domain::LocalRootIdentity& root, std::stop_token)
      -> std::expected<std::optional<runtime::LocalContextGrant>,
                       domain::LocalSourceError> override {
    ++resolves;
    if (absent) return std::nullopt;
    return runtime::LocalContextGrant{owner, root, generation, reader};
  }
};
auto select(const runtime::PreparedLocalContext& prepared,
            std::uint64_t window = 100) -> runtime::ContextSelectionResult {
  domain::InstructionInput instruction{
      domain::ContextEntryId::from("runtime").value(),
      domain::InstructionLayer::application_runtime,
      domain::InstructionOperation::add,
      {},
      domain::Message{domain::MessageId::from("runtime-message").value(),
                      domain::Role::system,
                      {domain::TextBlock{"runtime"}},
                      {}},
      {domain::ContextSourceId::from("runtime-source").value(), {}, {}},
      0,
      1,
      1};
  runtime::ContextSelectionRequest request;
  request.capacity = {window, 10, 0};
  request.instructions = {instruction};
  request.candidates = prepared.candidates;
  auto result = runtime::ContextBuilder{}.select_and_build(std::move(request));
  REQUIRE(result);
  return *result;
}
} // namespace

TEST_CASE(
    "local context preparation refuses absent changed and untrusted sources") {
  auto grants = std::make_shared<Grants>();
  runtime::LocalContextRequest request{session(), 1, {source()}, 7};
  SECTION("missing grant") {
    grants->absent = true;
  }
  SECTION("untrusted reader") {
    grants->reader->trusted = false;
  }
  SECTION("changed bytes") {
    grants->reader->text = "jello";
  }
  SECTION("duplicate source") {
    request.sources.push_back(source());
  }
  SECTION("foreign source digest") {
    request.sources[0].content_digest.value[0] = 'a';
  }
  SECTION("zero order") {
    request.first_order = 0;
  }
  SECTION("zero selection revision") {
    request.selection_revision = 0;
  }
  SECTION("revoked during read") {
    grants->reader->after_read = [grants] { grants->absent = true; };
  }
  SECTION("new generation during read") {
    grants->reader->after_read = [grants] { ++grants->generation; };
  }
  SECTION("reader replacement during read") {
    grants->reader->after_read = [grants] {
      grants->reader = std::make_shared<Reader>();
    };
  }
  runtime::LocalContextController controller{grants};
  REQUIRE_FALSE(controller.prepare(request));
  grants->reader->after_read = {};
}

TEST_CASE("local context cancellation and order overflow fail before reads") {
  auto grants = std::make_shared<Grants>();
  runtime::LocalContextController controller{grants};
  auto other = source();
  other.relative_path = "other";
  runtime::LocalContextRequest request{
      session(),
      1,
      {source(), other},
      std::numeric_limits<std::uint64_t>::max()};
  CHECK_FALSE(controller.prepare(request));
  CHECK(grants->reader->calls == 0);
  std::stop_source stop;
  stop.request_stop();
  request.first_order = 1;
  CHECK_FALSE(controller.prepare(request, stop.get_token()));
  CHECK(grants->reader->calls == 0);
}

TEST_CASE("local context finalization binds actual optional selection") {
  auto grants = std::make_shared<Grants>();
  runtime::LocalContextController controller{grants};
  auto prepared = controller.prepare({session(), 3, {source()}, 7});
  REQUIRE(prepared);
  REQUIRE(prepared->candidates.size() == 1);
  CHECK(prepared->candidates[0].budget_class ==
        runtime::ContextBudgetClass::attachment);
  CHECK_FALSE(prepared->candidates[0].required);
  CHECK_FALSE(prepared->admission.admission_digest);
  auto selected = select(*prepared);
  auto sealed = runtime::finalize_local_context_admission(*prepared, selected);
  REQUIRE(sealed);
  CHECK(domain::local_context_admission_matches_context(*sealed,
                                                        selected.context));
  SECTION("missing decision") {
    selected.decisions.clear();
  }
  SECTION("prepared candidate changed") {
    prepared->candidates[0].content.message.content = {
        domain::TextBlock{"jello"}};
  }
  SECTION("prepared candidate authority changed") {
    prepared->candidates[0].required = true;
  }
  SECTION("duplicate decision") {
    selected.decisions.push_back(selected.decisions.back());
  }
  SECTION("altered text") {
    selected.context.entries.back().message.content = {
        domain::TextBlock{"jello"}};
  }
  SECTION("wrong estimate") {
    ++selected.decisions.back().estimated_tokens;
  }
  REQUIRE_FALSE(runtime::finalize_local_context_admission(*prepared, selected));
}

TEST_CASE("recovery retains original decisions and permits only a new matching "
          "live lease") {
  auto grants = std::make_shared<Grants>();
  runtime::LocalContextController controller{grants};
  auto prepared = controller.prepare({session(), 3, {source()}, 7});
  REQUIRE(prepared);
  auto selected = select(*prepared, 12);
  auto original =
      runtime::finalize_local_context_admission(*prepared, selected);
  REQUIRE(original);
  CHECK(original->evidence[0].decision ==
        domain::LocalContextDecision::omitted_budget);
  ++grants->generation;
  auto recovered = controller.revalidate(*original);
  REQUIRE(recovered);
  CHECK(recovered->admission == *original);
  CHECK(recovered->candidates[0].content.order == 7);
  auto replacement = select(*recovered);
  CHECK_FALSE(
      runtime::finalize_local_context_admission(*recovered, replacement));
  grants->reader->text = "changed";
  CHECK_FALSE(controller.revalidate(*original));
}

TEST_CASE("local preparation discards cancelled late and exceptional reader "
          "results") {
  auto grants = std::make_shared<Grants>();
  domain::LocalSourceLimits limits;
  std::stop_source stop;
  SECTION("cancel after read") {
    grants->reader->after_read = [&stop] { stop.request_stop(); };
  }
  SECTION("deadline after read") {
    limits.timeout = std::chrono::milliseconds{20};
    grants->reader->after_read = [] {
      std::this_thread::sleep_for(std::chrono::milliseconds{30});
    };
  }
  SECTION("exception text never escapes") {
    grants->reader->after_read = [] {
      throw std::runtime_error{"private source contents"};
    };
  }
  runtime::LocalContextController controller{grants, limits};
  const auto result =
      controller.prepare({session(), 1, {source()}, 1}, stop.get_token());
  REQUIRE_FALSE(result);
  CHECK(result.error().message.find("private") == std::string::npos);
}
TEST_CASE(
    "empty local selection creates no implicit grant or source activity") {
  auto grants = std::make_shared<Grants>();
  runtime::LocalContextController controller{grants};
  const auto prepared = controller.prepare({session(), 1, {}, 1});
  REQUIRE(prepared);
  CHECK(grants->resolves == 0);
  CHECK(grants->reader->calls == 0);
  const auto sealed =
      runtime::finalize_local_context_admission(*prepared, select(*prepared));
  REQUIRE(sealed);
  CHECK(sealed->evidence.empty());
}
