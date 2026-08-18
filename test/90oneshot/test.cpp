#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <aiforge/surfaces/one_shot.hpp>

namespace {

using namespace std::chrono_literals;
using namespace aiforge;

template <typename IdType>
auto make_id(const std::string& value) -> IdType {
  return IdType::from(value).value();
}

struct End {};
using Item = std::variant<backend::BackendEvent, backend::BackendError, End>;

class VectorStream final : public backend::BackendStream {
 public:
  explicit VectorStream(std::vector<Item> items) : m_items(std::move(items)) {}

  auto next(std::stop_token)
      -> std::expected<std::optional<backend::BackendEvent>,
                       backend::BackendError> override {
    if (m_index >= m_items.size()) return std::optional<backend::BackendEvent>{};
    auto& item = m_items[m_index++];
    if (auto* event = std::get_if<backend::BackendEvent>(&item)) {
      return std::optional<backend::BackendEvent>{std::move(*event)};
    }
    if (auto* error = std::get_if<backend::BackendError>(&item)) {
      return std::unexpected(std::move(*error));
    }
    return std::optional<backend::BackendEvent>{};
  }

 private:
  std::vector<Item> m_items;
  std::size_t m_index{};
};

class CapturingBackend final : public backend::Backend {
 public:
  explicit CapturingBackend(std::vector<Item> items)
      : m_items(std::move(items)) {}

  auto start(backend::BackendRequest request, std::stop_token)
      -> std::expected<std::unique_ptr<backend::BackendStream>,
                       backend::BackendError> override {
    captured = std::move(request);
    ++starts;
    return std::make_unique<VectorStream>(std::move(m_items));
  }

  std::optional<backend::BackendRequest> captured;
  std::size_t starts{};

 private:
  std::vector<Item> m_items;
};

class FakeModels final : public backend::ModelContextProvider {
 public:
  auto lookup(const domain::ModelId& model_id, std::stop_token stop_token)
      -> std::expected<backend::ModelContextInfo,
                       backend::BackendError> override {
    ++lookups;
    if (stop_token.stop_requested()) {
      return std::unexpected(backend::BackendError{
          backend::BackendErrorKind::cancelled, "hidden", false, std::nullopt});
    }
    if (failure) return std::unexpected(*failure);
    auto result = info;
    result.model_id = model_id;
    return result;
  }

  backend::ModelContextInfo info{make_id<domain::ModelId>("model"), 8192,
                                 1024};
  std::optional<backend::BackendError> failure;
  std::size_t lookups{};
};

auto success_items(const domain::MessageId& message_id) -> std::vector<Item> {
  return {
      backend::BackendEvent{backend::ResponseStarted{"response"}},
      backend::BackendEvent{backend::ContentDelta{
          message_id, domain::TextBlock{"hello\x1b[31mred"}}},
      backend::BackendEvent{backend::CitationObserved{
          {"https://example.test/\x1b[2J", "source\nforged\x7f"}}},
      backend::BackendEvent{backend::UsageObserved{{3, 2, 1, 0}}},
      backend::BackendEvent{
          backend::ResponseFinished{domain::FinishReason::stop}},
      End{},
  };
}

class CancelStream final : public backend::BackendStream {
 public:
  explicit CancelStream(domain::MessageId message_id)
      : m_message_id(std::move(message_id)) {}

  auto next(std::stop_token stop_token)
      -> std::expected<std::optional<backend::BackendEvent>,
                       backend::BackendError> override {
    if (m_finished) return std::optional<backend::BackendEvent>{};
    if (m_step++ == 0) {
      return backend::BackendEvent{backend::ResponseStarted{"response"}};
    }
    if (m_step == 2) {
      return backend::BackendEvent{
          backend::ContentDelta{m_message_id, domain::TextBlock{"partial"}}};
    }
    std::mutex mutex;
    std::unique_lock lock(mutex);
    std::condition_variable_any ready;
    ready.wait(lock, stop_token, [] { return false; });
    m_finished = true;
    return backend::BackendEvent{backend::ResponseCancelled{"cancelled"}};
  }

 private:
  domain::MessageId m_message_id;
  int m_step{};
  bool m_finished{};
};

class CancelBackend final : public backend::Backend {
 public:
  auto start(backend::BackendRequest request, std::stop_token)
      -> std::expected<std::unique_ptr<backend::BackendStream>,
                       backend::BackendError> override {
    return std::make_unique<CancelStream>(request.assistant_message_id);
  }
};

}  // namespace

TEST_CASE("one-shot streams only sanitized text and builds neutral evidence",
          "[one-shot]") {
  FakeModels models;
  // The surface owns assistant identity, so replace the scripted placeholder
  // when the backend sees the request.
  class RewritingBackend final : public backend::Backend {
   public:
    auto start(backend::BackendRequest request, std::stop_token)
        -> std::expected<std::unique_ptr<backend::BackendStream>,
                         backend::BackendError> override {
      captured = request;
      return std::make_unique<VectorStream>(success_items(request.assistant_message_id));
    }
    std::optional<backend::BackendRequest> captured;
  } rewriting;

  surfaces::OneShotSurface surface{rewriting, models};
  std::ostringstream output;
  std::ostringstream error;
  const auto result = surface.run(
      {"explain", std::string{"file contents"},
       make_id<domain::ModelId>("model")},
      output, error);

  REQUIRE(result);
  REQUIRE(result->usage == domain::Usage{3, 2, 1, 0});
  REQUIRE(output.str() == "hellored");
  REQUIRE(error.str().find("citation: https://example.test/") !=
          std::string::npos);
  REQUIRE(error.str().find("\x1b") == std::string::npos);
  REQUIRE(error.str().find("\nforged") == std::string::npos);
  REQUIRE(error.str().find("usage: input=3 output=2 cached=1 reasoning=0") !=
          std::string::npos);
  REQUIRE(rewriting.captured);
  REQUIRE(rewriting.captured->tools.empty());
  REQUIRE(rewriting.captured->context.entries.size() == 3);
  REQUIRE(rewriting.captured->context.entries.back().kind ==
          domain::ContextEntryKind::evidence);
  REQUIRE(rewriting.captured->context.entries.back().message.role ==
          domain::Role::evidence);
  REQUIRE(rewriting.captured->context.entries.back().provenance.source_location ==
          "stdin");
}

TEST_CASE("invalid and oversized one-shot input never reaches a backend",
          "[one-shot][failure]") {
  FakeModels models;
  CapturingBackend backend{{}};
  surfaces::OneShotSurface surface{backend, models, {8, 2}};
  std::ostringstream output;
  std::ostringstream error;
  const auto model = make_id<domain::ModelId>("model");

  auto result = surface.run({"", std::nullopt, model}, output, error);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == surfaces::OneShotErrorCode::invalid_input);

  result = surface.run({std::string{"bad\0text", 8}, std::nullopt, model},
                       output, error);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == surfaces::OneShotErrorCode::invalid_input);

  result = surface.run({std::string{"\xc0\xaf", 2}, std::nullopt, model},
                       output, error);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == surfaces::OneShotErrorCode::invalid_input);

  result = surface.run({"12345678", std::string{"x"}, model}, output, error);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == surfaces::OneShotErrorCode::input_too_large);
  REQUIRE(backend.starts == 0);
  REQUIRE(models.lookups == 0);
}

TEST_CASE("model and context failures are typed before inference",
          "[one-shot][failure]") {
  FakeModels models;
  models.failure = backend::BackendError{backend::BackendErrorKind::network,
                                         "secret provider body", true, 500};
  CapturingBackend backend{{}};
  surfaces::OneShotSurface surface{backend, models};
  std::ostringstream output;
  std::ostringstream error;
  auto result = surface.run(
      {"hello", std::nullopt, make_id<domain::ModelId>("model")}, output,
      error);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          surfaces::OneShotErrorCode::model_lookup_failed);
  REQUIRE(result.error().message.find("secret") == std::string::npos);

  models.failure.reset();
  models.info.context_window_tokens = 100;
  models.info.maximum_output_tokens = 50;
  surfaces::OneShotSurface small{backend, models, {1024, 50}};
  result = small.run(
      {std::string(60, 'x'), std::nullopt,
       make_id<domain::ModelId>("model")},
      output, error);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == surfaces::OneShotErrorCode::context_failed);
  REQUIRE(backend.starts == 0);
}

TEST_CASE("backend failure preserves partial output and redacts diagnostics",
          "[one-shot][failure]") {
  FakeModels models;
  class PartialFailureBackend final : public backend::Backend {
   public:
    auto start(backend::BackendRequest request, std::stop_token)
        -> std::expected<std::unique_ptr<backend::BackendStream>,
                         backend::BackendError> override {
      std::vector<Item> items{
          backend::BackendEvent{backend::ResponseStarted{"response"}},
          backend::BackendEvent{backend::ContentDelta{
              request.assistant_message_id, domain::TextBlock{"partial"}}},
          backend::BackendError{backend::BackendErrorKind::authentication,
                                "secret-token", false, 401},
          End{}};
      return std::make_unique<VectorStream>(std::move(items));
    }
  } backend;
  surfaces::OneShotSurface surface{backend, models};
  std::ostringstream output;
  std::ostringstream error;
  const auto result = surface.run(
      {"hello", std::nullopt, make_id<domain::ModelId>("model")}, output,
      error);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == surfaces::OneShotErrorCode::run_failed);
  REQUIRE(result.error().message == "backend authentication failed");
  REQUIRE(result.error().message.find("secret-token") == std::string::npos);
  REQUIRE(output.str() == "partial");
}

TEST_CASE("one-shot cancellation keeps streamed partial content",
          "[one-shot][failure][cancel]") {
  FakeModels models;
  CancelBackend backend;
  surfaces::OneShotSurface surface{backend, models};
  std::ostringstream output;
  std::ostringstream error;
  std::stop_source cancellation;
  std::jthread requester{[&] {
    std::this_thread::sleep_for(20ms);
    cancellation.request_stop();
  }};
  const auto result = surface.run(
      {"hello", std::nullopt, make_id<domain::ModelId>("model")}, output,
      error, cancellation.get_token());
  REQUIRE_FALSE(result);
  INFO(result.error().message);
  REQUIRE(result.error().code == surfaces::OneShotErrorCode::cancelled);
  REQUIRE(output.str() == "partial");
}

TEST_CASE("broken output cancels the one-shot operation",
          "[one-shot][failure][output]") {
  FakeModels models;
  class TextBackend final : public backend::Backend {
   public:
    auto start(backend::BackendRequest request, std::stop_token)
        -> std::expected<std::unique_ptr<backend::BackendStream>,
                         backend::BackendError> override {
      return std::make_unique<VectorStream>(std::vector<Item>{
          backend::BackendEvent{backend::ResponseStarted{"response"}},
          backend::BackendEvent{backend::ContentDelta{
              request.assistant_message_id, domain::TextBlock{"answer"}}},
          backend::BackendEvent{
              backend::ResponseFinished{domain::FinishReason::stop}},
          End{}});
    }
  } backend;
  surfaces::OneShotSurface surface{backend, models};
  std::ostringstream output;
  output.setstate(std::ios::badbit);
  std::ostringstream error;
  const auto result = surface.run(
      {"hello", std::nullopt, make_id<domain::ModelId>("model")}, output,
      error);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == surfaces::OneShotErrorCode::output_failed);
}
