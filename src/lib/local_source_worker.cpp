#include <aiforge/runtime/local_source_worker.hpp>

#include <aiforge/detail/utf8_text.hpp>
#include <algorithm>
#include <array>
#include <condition_variable>
#include <mutex>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace aiforge::runtime {
namespace {
using Code = LocalSourceWorkerErrorCode;
using SourceCode = domain::LocalSourceErrorCode;
using Outcome = std::expected<LocalSourceWorkResult, domain::LocalSourceError>;

auto failure(Code code, std::string message)
    -> std::unexpected<LocalSourceWorkerError> {
  return std::unexpected(LocalSourceWorkerError{code, std::move(message)});
}
auto source_failure(SourceCode code, std::string message)
    -> std::unexpected<domain::LocalSourceError> {
  return std::unexpected(domain::LocalSourceError{code, std::move(message)});
}
auto request_token(const LocalSourceWorkRequest& request)
    -> const LocalSourceRequestToken& {
  return std::visit(
      [](const auto& value) -> const LocalSourceRequestToken& {
        return value.token;
      },
      request);
}
auto validate_request(const LocalSourceWorkRequest& request)
    -> std::expected<void, domain::LocalSourceError> {
  return std::visit(
      [](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::same_as<T, LocalListRequest>)
          return validate_local_list_request(value);
        else if constexpr (std::same_as<T, LocalPreviewRequest>)
          return validate_local_preview_request(value);
        else
          return validate_local_revalidate_request(value);
      },
      request);
}
auto checked_error(domain::LocalSourceError error) -> domain::LocalSourceError {
  if (error.code < SourceCode::invalid_request ||
      error.code > SourceCode::internal_failure ||
      error.message.size() > 1024 || !detail::is_safe_utf8_text(error.message))
    return {SourceCode::invalid_result,
            "local reader returned an invalid error"};
  // A reader's safe-message contract is not proof that an implementation has
  // obeyed it. Delivery uses only canonical text, never port diagnostics.
  static constexpr std::array<std::string_view, 15> messages{
      "local source request is invalid",
      "local source result is invalid",
      "local source version is unsupported",
      "local source is unavailable",
      "local source read permission is unavailable",
      "local entry is unsupported",
      "local source is not supported text",
      "local source bytes changed",
      "local source lease is stale",
      "local source changed during observation",
      "local source resource limit exceeded",
      "local source work was cancelled",
      "local source work timed out",
      "local source read failed",
      "local source failed internally"};
  return {error.code,
          std::string{messages[static_cast<std::size_t>(error.code)]}};
}
template <typename Request>
auto invoke_reader(LocalSourceReader& reader, Request request,
                   std::stop_token stop) -> Outcome {
  auto result = [&] {
    if constexpr (std::same_as<Request, LocalListRequest>)
      return reader.list(request, stop);
    else if constexpr (std::same_as<Request, LocalPreviewRequest>)
      return reader.preview(request, stop);
    else
      return reader.revalidate(request, stop);
  }();
  if (!result) return std::unexpected(checked_error(std::move(result.error())));
  auto valid = [&] {
    if constexpr (std::same_as<Request, LocalListRequest>)
      return validate_local_list_result(request, *result);
    else if constexpr (std::same_as<Request, LocalPreviewRequest>)
      return validate_local_preview_result(request, *result);
    else
      return validate_local_read_result(request, *result);
  }();
  if (!valid) return std::unexpected(std::move(valid.error()));
  return LocalSourceWorkResult{std::move(*result)};
}

struct Job {
  const LocalSourceRequestToken token;
  std::mutex mutex;
  std::condition_variable changed;
  std::stop_source stop;
  bool started{};
  bool discarded{};
  bool reader_done{};
  bool relay_done{};
  std::optional<Outcome> result;

  explicit Job(LocalSourceRequestToken value) : token(std::move(value)) {}
  auto discard() -> void {
    {
      const std::lock_guard lock{mutex};
      discarded = true;
      result.reset();
    }
    changed.notify_all();
  }
  auto retired() -> bool {
    const std::lock_guard lock{mutex};
    return discarded && reader_done && relay_done;
  }
};

auto deliver_cancellation(const std::shared_ptr<Job>& job) -> void {
  bool cancel{};
  {
    std::unique_lock lock{job->mutex};
    job->changed.wait(lock, [&] { return job->discarded || job->reader_done; });
    cancel = job->discarded;
  }
  if (cancel) job->stop.request_stop();
  {
    const std::lock_guard lock{job->mutex};
    job->relay_done = true;
  }
}
auto read_source(const std::shared_ptr<Job>& job,
                 std::shared_ptr<LocalSourceReader> reader,
                 LocalSourceWorkRequest request) -> void {
  auto result = [&]() -> Outcome {
    try {
      {
        std::unique_lock lock{job->mutex};
        job->changed.wait(lock, [&] { return job->started || job->discarded; });
        if (job->discarded)
          return source_failure(SourceCode::cancelled, "local work cancelled");
      }
      return std::visit(
          [&](const auto& value) {
            return invoke_reader(*reader, value, job->stop.get_token());
          },
          request);
    } catch (...) {
      return source_failure(SourceCode::internal_failure,
                            "local reader failed internally");
    }
  }();
  // Release the owning port on its worker before freeing this slot. A port
  // destructor is outside the owner-thread cancellation/cleanup path too.
  reader.reset();
  {
    const std::lock_guard lock{job->mutex};
    if (!job->discarded) job->result.emplace(std::move(result));
    job->reader_done = true;
  }
  job->changed.notify_all();
}
auto launch(const std::shared_ptr<Job>& job,
            std::shared_ptr<LocalSourceReader> reader,
            LocalSourceWorkRequest request)
    -> std::expected<void, LocalSourceWorkerError> {
  bool relay_started{};
  try {
    std::thread relay{[job] { deliver_cancellation(job); }};
    relay.detach();
    relay_started = true;
    std::thread source{[job, reader = std::move(reader),
                        request = std::move(request)]() mutable {
      read_source(job, std::move(reader), std::move(request));
    }};
    source.detach();
    {
      const std::lock_guard lock{job->mutex};
      job->started = true;
    }
    job->changed.notify_all();
    return {};
  } catch (...) {
    {
      const std::lock_guard lock{job->mutex};
      job->discarded = true;
      job->reader_done = true;
      job->relay_done = !relay_started;
    }
    job->changed.notify_all();
    return failure(Code::internal_failure, "local worker could not start");
  }
}
} // namespace

struct LocalSourceWorker::Impl {
  std::size_t capacity;
  std::uint64_t last_request_id{};
  std::vector<std::shared_ptr<Job>> jobs;
  explicit Impl(std::size_t value) : capacity(value) { jobs.reserve(value); }
  auto reap() -> void {
    std::erase_if(jobs, [](const auto& job) { return job->retired(); });
  }
  auto find(const LocalSourceRequestToken& token) {
    return std::ranges::find_if(
        jobs, [&](const auto& job) { return job->token == token; });
  }
};

LocalSourceWorker::LocalSourceWorker(std::unique_ptr<Impl> impl)
    : m_impl(std::move(impl)) {
}
LocalSourceWorker::~LocalSourceWorker() {
  for (const auto& job : m_impl->jobs)
    job->discard();
}
auto LocalSourceWorker::create(std::size_t capacity)
    -> std::expected<std::unique_ptr<LocalSourceWorker>,
                     LocalSourceWorkerError> {
  if (capacity == 0 || capacity > maximum_capacity)
    return failure(Code::invalid_request, "local worker capacity is invalid");
  try {
    return std::unique_ptr<LocalSourceWorker>{
        new LocalSourceWorker{std::make_unique<Impl>(capacity)}};
  } catch (...) {
    return failure(Code::internal_failure, "local worker allocation failed");
  }
}
auto LocalSourceWorker::submit(std::shared_ptr<LocalSourceReader> reader,
                               LocalSourceWorkRequest request)
    -> std::expected<void, LocalSourceWorkerError> {
  try {
    if (!reader || !reader->guarantees_pinned_read_only_sources())
      return failure(Code::invalid_request,
                     "local reader has no pinned authority");
    if (auto valid = validate_request(request); !valid)
      return failure(Code::invalid_request, valid.error().message);
    const auto& token = request_token(request);
    if (token.request_id <= m_impl->last_request_id)
      return failure(Code::stale_request,
                     "local request identity was already used");
    m_impl->reap();
    if (m_impl->jobs.size() >= m_impl->capacity)
      return failure(Code::busy, "local worker capacity remains occupied");
    auto job = std::make_shared<Job>(token);
    m_impl->jobs.push_back(job);
    m_impl->last_request_id = job->token.request_id;
    return launch(job, std::move(reader), std::move(request));
  } catch (...) {
    return failure(Code::internal_failure, "local work submission failed");
  }
}
auto LocalSourceWorker::poll(const LocalSourceRequestToken& token)
    -> std::expected<std::optional<LocalSourceWorkCompletion>,
                     LocalSourceWorkerError> {
  try {
    m_impl->reap();
    const auto found = m_impl->find(token);
    if (found == m_impl->jobs.end())
      return failure(Code::stale_request, "local work is no longer available");
    std::optional<LocalSourceWorkCompletion> completion;
    {
      const std::lock_guard lock{(*found)->mutex};
      if ((*found)->discarded)
        return failure(Code::stale_request, "local work was cancelled");
      if (!(*found)->reader_done || !(*found)->relay_done) return std::nullopt;
      auto& result = (*found)->result;
      if (!result)
        return failure(Code::internal_failure,
                       "local completion is unavailable");
      completion.emplace(
          LocalSourceWorkCompletion{(*found)->token, std::move(*result)});
    }
    m_impl->jobs.erase(found);
    return completion;
  } catch (...) {
    return failure(Code::internal_failure, "local work delivery failed");
  }
}
auto LocalSourceWorker::cancel(const LocalSourceRequestToken& token)
    -> std::expected<void, LocalSourceWorkerError> {
  try {
    const auto found = m_impl->find(token);
    if (found == m_impl->jobs.end())
      return failure(Code::stale_request, "local work is no longer available");
    (*found)->discard();
    m_impl->reap();
    return {};
  } catch (...) {
    return failure(Code::internal_failure, "local work cancellation failed");
  }
}
auto LocalSourceWorker::invalidate_session(const domain::SessionId& session)
    -> std::expected<void, LocalSourceWorkerError> {
  try {
    for (const auto& job : m_impl->jobs)
      if (job->token.session_id == session) job->discard();
    m_impl->reap();
    return {};
  } catch (...) {
    return failure(Code::internal_failure, "local session invalidation failed");
  }
}
auto LocalSourceWorker::occupied_slots() const -> std::size_t {
  return static_cast<std::size_t>(std::ranges::count_if(
      m_impl->jobs, [](const auto& job) { return !job->retired(); }));
}
auto LocalSourceWorker::ready_results() const -> std::size_t {
  return static_cast<std::size_t>(
      std::ranges::count_if(m_impl->jobs, [](const auto& job) {
        const std::lock_guard lock{job->mutex};
        return !job->discarded && job->reader_done && job->relay_done &&
               job->result.has_value();
      }));
}
} // namespace aiforge::runtime
