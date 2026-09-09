#include <aiforge/runtime/local_source_worker.hpp>

#include <aiforge/detail/utf8_text.hpp>
#include <algorithm>
#include <array>
#include <condition_variable>
#include <limits>
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
using Token = std::variant<LocalSourceRequestToken, LocalFolderGrantToken,
                           LocalContextWorkToken, RepositoryContextWorkToken,
                           OpsObservationWorkToken>;
using Request =
    std::variant<LocalSourceWorkRequest, LocalFolderGrantRequest,
                 LocalContextWorkRequest, RepositoryContextWorkRequest,
                 OpsObservationWorkRequest>;
using Port = std::variant<std::monostate, std::shared_ptr<LocalSourceReader>,
                          std::shared_ptr<LocalSourceGrantFactory>,
                          std::shared_ptr<LocalContextController>,
                          std::shared_ptr<RepositoryContextController>,
                          std::shared_ptr<OpsObservationSource>>;
using ContextOutcome =
    std::expected<PreparedLocalContext, domain::LocalContextError>;
using RepositoryOutcome =
    std::expected<PreparedRepositoryContext, domain::RepositoryContextError>;
using OpsOutcome =
    std::expected<domain::OpsObservation, OpsObservationSourceError>;
using Result = std::variant<LocalSourceWorkResult, LocalFolderGrantResult,
                            ContextOutcome, RepositoryOutcome, OpsOutcome>;
using Outcome = std::expected<Result, domain::LocalSourceError>;

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

auto invoke_factory(LocalSourceGrantFactory& factory,
                    const LocalFolderGrantRequest& request,
                    std::stop_token stop) -> Outcome {
  auto result = factory.grant(request, stop);
  if (!result) return std::unexpected(checked_error(std::move(result.error())));
  if (auto valid = validate_local_folder_grant_result(request, *result); !valid)
    return std::unexpected(std::move(valid.error()));
  return Result{std::move(*result)};
}

auto valid_context_work(const LocalContextWorkRequest& request) -> bool {
  const auto& token = request.token;
  if (token.session_epoch == 0 || token.request_id == 0 ||
      token.selection_revision == 0)
    return false;
  return std::visit(
      [&](const auto& operation) {
        if (operation.session_id != token.session_id ||
            operation.selection_revision != token.selection_revision)
          return false;
        using T = std::decay_t<decltype(operation)>;
        if constexpr (std::same_as<T, LocalContextRequest>) {
          return operation.first_order != 0 &&
                 domain::validate_local_source_selection(operation.sources)
                     .has_value() &&
                 (operation.sources.empty() ||
                  operation.sources.size() - 1 <=
                      std::numeric_limits<std::uint64_t>::max() -
                          operation.first_order);
        } else {
          return domain::validate_local_context_admission(operation)
              .has_value();
        }
      },
      request.operation);
}
auto context_error(domain::LocalContextError error)
    -> domain::LocalContextError {
  using ContextCode = domain::LocalContextErrorCode;
  if (error.code < ContextCode::invalid_request ||
      error.code > ContextCode::internal_failure)
    return {ContextCode::internal_failure,
            "local context returned an invalid error"};
  static constexpr std::array<std::string_view, 9> messages{
      "local context request is invalid",
      "local context admission is invalid",
      "local context source is unavailable",
      "local context source changed",
      "local context lease is stale",
      "local context exceeds resource bounds",
      "local context work was cancelled",
      "local context work timed out",
      "local context failed internally"};
  return {error.code,
          std::string{messages[static_cast<std::size_t>(error.code)]}};
}
auto context_result_matches(const LocalContextWorkRequest& request,
                            const PreparedLocalContext& result) -> bool {
  const auto& admission = result.admission;
  if (admission.session_id != request.token.session_id ||
      admission.selection_revision != request.token.selection_revision ||
      result.candidates.size() != admission.evidence.size())
    return false;
  return std::visit(
      [&](const auto& operation) {
        using T = std::decay_t<decltype(operation)>;
        if constexpr (std::same_as<T, domain::LocalContextAdmission>) {
          return admission == operation;
        } else {
          if (admission.version != 1 || admission.admission_digest ||
              admission.capacity != domain::ContextCapacity{} ||
              admission.evidence.size() != operation.sources.size())
            return false;
          for (std::size_t index = 0; index < operation.sources.size();
               ++index) {
            const auto& ref = admission.evidence[index];
            if (ref.source != operation.sources[index] ||
                ref.order != operation.first_order + index ||
                ref.decision != domain::LocalContextDecision::admitted)
              return false;
          }
          return true;
        }
      },
      request.operation);
}
auto invoke_context(LocalContextController& controller,
                    const LocalContextWorkRequest& request,
                    std::stop_token stop) -> Outcome {
  auto result = std::visit(
      [&](const auto& operation) {
        using T = std::decay_t<decltype(operation)>;
        if constexpr (std::same_as<T, LocalContextRequest>)
          return controller.prepare(operation, stop);
        else
          return controller.revalidate(operation, stop);
      },
      request.operation);
  if (!result)
    result = std::unexpected(context_error(std::move(result.error())));
  else if (!context_result_matches(request, *result))
    result = std::unexpected(domain::LocalContextError{
        domain::LocalContextErrorCode::invalid_admission,
        "local context result does not match its work request"});
  return Result{std::move(result)};
}
auto valid_repository_work(const RepositoryContextWorkRequest& request)
    -> bool {
  const auto& token = request.token;
  if (token.session_id.value().empty() || token.session_epoch == 0 ||
      token.request_id == 0 || token.selection_revision == 0)
    return false;
  return std::visit(
      [&](const auto& operation) {
        if (operation.selection_revision != token.selection_revision)
          return false;
        using T = std::decay_t<decltype(operation)>;
        if constexpr (std::same_as<T, RepositoryContextRequest>)
          return validate_repository_context_request(operation).has_value();
        else
          return domain::validate_repository_context_admission(operation)
              .has_value();
      },
      request.operation);
}
auto repository_error(domain::RepositoryContextError error)
    -> domain::RepositoryContextError {
  using RepoCode = domain::RepositoryContextErrorCode;
  if (error.code < RepoCode::invalid_request ||
      error.code > RepoCode::internal_failure)
    return {RepoCode::internal_failure,
            "repository context returned an invalid error"};
  static constexpr std::array<std::string_view, 8> messages{
      "repository context request is invalid",
      "repository context admission is invalid",
      "repository context source is unavailable",
      "repository context source changed",
      "repository context exceeds resource bounds",
      "repository context work was cancelled",
      "repository context work timed out",
      "repository context failed internally"};
  return {error.code,
          std::string{messages[static_cast<std::size_t>(error.code)]}};
}
auto prepared_repository_matches(
    const RepositoryContextRequest& operation,
    const domain::RepositoryContextAdmission& admission) -> bool {
  const auto target = operation.target_subtree == "."
                          ? std::string_view{}
                          : std::string_view{operation.target_subtree};
  if (admission.version != 1 || admission.admission_digest ||
      admission.capacity != domain::ContextCapacity{} ||
      admission.target_subtree != target ||
      admission.evidence.size() != operation.evidence_paths.size())
    return false;
  for (std::size_t index = 0; index < operation.evidence_paths.size(); ++index)
    if (admission.evidence[index].source.relative_path !=
            operation.evidence_paths[index] ||
        admission.evidence[index].decision !=
            domain::RepositoryContextDecision::admitted)
      return false;
  return true;
}
auto repository_result_matches(const RepositoryContextWorkRequest& request,
                               const PreparedRepositoryContext& result)
    -> bool {
  const auto& admission = result.admission;
  if (admission.selection_revision != request.token.selection_revision ||
      admission.source_snapshot != domain::snapshot_identity(result.snapshot))
    return false;
  return std::visit(
      [&](const auto& operation) {
        using T = std::decay_t<decltype(operation)>;
        if constexpr (std::same_as<T, domain::RepositoryContextAdmission>)
          return domain::validate_repository_context_admission(admission)
                     .has_value() &&
                 domain::repository_context_admission_successor(operation,
                                                                admission);
        else
          return prepared_repository_matches(operation, admission);
      },
      request.operation);
}
auto invoke_repository(RepositoryContextController& controller,
                       const RepositoryContextWorkRequest& request,
                       std::stop_token stop) -> Outcome {
  auto result = std::visit(
      [&](const auto& operation) {
        using T = std::decay_t<decltype(operation)>;
        if constexpr (std::same_as<T, RepositoryContextRequest>)
          return controller.prepare(operation, stop);
        else
          return controller.revalidate(operation, stop);
      },
      request.operation);
  if (!result)
    result = std::unexpected(repository_error(std::move(result.error())));
  else if (!repository_result_matches(request, *result))
    result = std::unexpected(domain::RepositoryContextError{
        domain::RepositoryContextErrorCode::invalid_admission,
        "repository context result does not match its work request"});
  return Result{std::move(result)};
}
auto valid_ops_work(const OpsObservationWorkRequest& request) -> bool {
  const auto& token = request.token;
  return token.session_epoch != 0 && token.request_id != 0 &&
         token.session_id == token.observation.session_id &&
         request.authority.validate(token.observation).has_value();
}
auto ops_error(OpsObservationSourceError error) -> OpsObservationSourceError {
  if (error < OpsObservationSourceError::unavailable ||
      error > OpsObservationSourceError::internal_failure)
    return OpsObservationSourceError::invalid_result;
  return error;
}
auto invoke_ops(OpsObservationSource& source,
                const OpsObservationWorkRequest& request, std::stop_token stop)
    -> Outcome {
  auto result = source.observe(request.token.observation, stop);
  if (!result)
    result = std::unexpected(ops_error(result.error()));
  else if (!domain::validate_ops_observation(
               request.authority, request.token.observation, *result))
    result = std::unexpected(OpsObservationSourceError::invalid_result);
  return Result{std::move(result)};
}
template <typename Value>
auto completion_error(domain::LocalSourceError error) {
  if constexpr (std::same_as<Value, ContextOutcome>) {
    const auto code = error.code == SourceCode::cancelled
                          ? domain::LocalContextErrorCode::cancelled
                          : domain::LocalContextErrorCode::internal_failure;
    return std::unexpected(context_error({code, {}}));
  } else if constexpr (std::same_as<Value, RepositoryOutcome>) {
    const auto code =
        error.code == SourceCode::cancelled
            ? domain::RepositoryContextErrorCode::cancelled
            : domain::RepositoryContextErrorCode::internal_failure;
    return std::unexpected(repository_error({code, {}}));
  } else if constexpr (std::same_as<Value, OpsOutcome>) {
    return std::unexpected(error.code == SourceCode::cancelled
                               ? OpsObservationSourceError::cancelled
                               : OpsObservationSourceError::internal_failure);
  } else {
    return std::unexpected(std::move(error));
  }
}

struct Job {
  const Token token;
  Request request;
  Port port;
  std::mutex mutex;
  std::condition_variable changed;
  std::stop_source stop;
  bool started{};
  bool discarded{};
  bool consumed{};
  bool ready{};
  bool reader_done{};
  bool relay_done{};
  std::optional<Outcome> result;

  Job(Token value, Request input)
      : token(std::move(value)), request(std::move(input)) {}
  auto discard() -> void {
    {
      const std::lock_guard lock{mutex};
      discarded = true;
    }
    changed.notify_all();
  }
  auto retired() -> bool {
    const std::lock_guard lock{mutex};
    return (discarded || consumed) && reader_done && relay_done;
  }
};

auto deliver_cancellation(const std::shared_ptr<Job>& job) -> void {
  bool cancel{};
  {
    std::unique_lock lock{job->mutex};
    job->changed.wait(lock, [&] { return job->discarded || job->ready; });
    cancel = job->discarded;
  }
  if (cancel) job->stop.request_stop();
  {
    const std::lock_guard lock{job->mutex};
    job->relay_done = true;
  }
  job->changed.notify_all();
}
auto invoke_job(const std::shared_ptr<Job>& job) -> Outcome {
  {
    std::unique_lock lock{job->mutex};
    job->changed.wait(lock, [&] { return job->started || job->discarded; });
    if (job->discarded)
      return source_failure(SourceCode::cancelled, "local work cancelled");
  }
  if (const auto* request =
          std::get_if<OpsObservationWorkRequest>(&job->request))
    return invoke_ops(
        *std::get<std::shared_ptr<OpsObservationSource>>(job->port), *request,
        job->stop.get_token());
  if (const auto* request =
          std::get_if<RepositoryContextWorkRequest>(&job->request))
    return invoke_repository(
        *std::get<std::shared_ptr<RepositoryContextController>>(job->port),
        *request, job->stop.get_token());
  if (const auto* request = std::get_if<LocalFolderGrantRequest>(&job->request))
    return invoke_factory(
        *std::get<std::shared_ptr<LocalSourceGrantFactory>>(job->port),
        *request, job->stop.get_token());
  if (const auto* request = std::get_if<LocalContextWorkRequest>(&job->request))
    return invoke_context(
        *std::get<std::shared_ptr<LocalContextController>>(job->port), *request,
        job->stop.get_token());
  auto& reader = *std::get<std::shared_ptr<LocalSourceReader>>(job->port);
  return std::visit(
      [&](const auto& request) {
        return invoke_reader(reader, request, job->stop.get_token());
      },
      std::get<LocalSourceWorkRequest>(job->request));
}
auto read_source(const std::shared_ptr<Job>& job) -> void {
  std::optional<Outcome> result;
  try {
    result.emplace(invoke_job(job));
  } catch (...) {
    result.emplace(source_failure(SourceCode::internal_failure,
                                  "local source failed internally"));
  }
  // All potentially blocking port/lease destruction stays on this producer.
  job->port.emplace<std::monostate>();
  const bool grant = std::holds_alternative<LocalFolderGrantToken>(job->token);
  {
    const std::lock_guard lock{job->mutex};
    if (!job->discarded) job->result = std::move(result);
    job->ready = true;
  }
  result.reset();
  job->changed.notify_all();
  if (grant) {
    {
      std::unique_lock lock{job->mutex};
      job->changed.wait(lock, [&] { return job->discarded || job->consumed; });
      result = std::move(job->result);
      job->result.reset();
    }
    result.reset();
  }
  {
    const std::lock_guard lock{job->mutex};
    job->reader_done = true;
  }
  job->changed.notify_all();
}
template <typename PortType>
auto launch(const std::shared_ptr<Job>& job,
            const std::shared_ptr<PortType>& port)
    -> std::expected<void, LocalSourceWorkerError> {
  bool relay_started{};
  try {
    std::thread relay{[job] { deliver_cancellation(job); }};
    relay.detach();
    relay_started = true;
    std::thread source{[job] { read_source(job); }};
    source.detach();
    {
      const std::lock_guard lock{job->mutex};
      job->port = port;
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
auto identity(const Token& token) -> std::uint64_t {
  return std::visit([](const auto& value) { return value.request_id; }, token);
}
auto session(const Token& token) -> const domain::SessionId& {
  return std::visit(
      [](const auto& value) -> const domain::SessionId& {
        return value.session_id;
      },
      token);
}
} // namespace

struct LocalSourceWorker::Impl {
  std::size_t capacity;
  std::uint64_t last_request_id{};
  std::uint64_t last_allocated_request_id{};
  std::vector<std::shared_ptr<Job>> jobs;
  explicit Impl(std::size_t value) : capacity(value) { jobs.reserve(value); }
  auto reap() -> void {
    std::erase_if(jobs, [](const auto& job) { return job->retired(); });
  }
  auto find(const Token& token) {
    return std::ranges::find_if(
        jobs, [&](const auto& job) { return job->token == token; });
  }
  template <typename PortType>
  auto submit(const std::shared_ptr<PortType>& port, Token token,
              Request request) -> std::expected<void, LocalSourceWorkerError> {
    if (identity(token) <= last_request_id)
      return failure(Code::stale_request,
                     "local request identity was already used");
    reap();
    if (jobs.size() >= capacity)
      return failure(Code::busy, "local worker capacity remains occupied");
    auto job = std::make_shared<Job>(std::move(token), std::move(request));
    jobs.push_back(job);
    last_request_id = identity(job->token);
    return launch(job, port);
  }
  template <typename Completion, typename Value, typename TokenType>
  auto poll(const TokenType& token)
      -> std::expected<std::optional<Completion>, LocalSourceWorkerError> {
    reap();
    const auto found = find(token);
    if (found == jobs.end())
      return failure(Code::stale_request, "local work is no longer available");
    const auto& job = *found;
    std::optional<Completion> completion;
    {
      const std::lock_guard lock{job->mutex};
      if (job->discarded || job->consumed)
        return failure(Code::stale_request,
                       "local work was cancelled or consumed");
      if (!job->ready || !job->relay_done ||
          (!std::same_as<Value, LocalFolderGrantResult> && !job->reader_done))
        return std::nullopt;
      if (!job->result)
        return failure(Code::internal_failure,
                       "local completion is unavailable");
      auto& result = *job->result;
      if (result)
        completion.emplace(
            Completion{token, std::move(std::get<Value>(*result))});
      else
        completion.emplace(Completion{
            token, completion_error<Value>(std::move(result.error()))});
      job->consumed = true;
    }
    job->changed.notify_all();
    reap();
    return completion;
  }
  auto cancel(const Token& token)
      -> std::expected<void, LocalSourceWorkerError> {
    const auto found = find(token);
    if (found == jobs.end())
      return failure(Code::stale_request, "local work is no longer available");
    (*found)->discard();
    reap();
    return {};
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
auto LocalSourceWorker::allocate_request_id()
    -> std::expected<std::uint64_t, LocalSourceWorkerError> {
  try {
    const auto previous =
        std::max(m_impl->last_request_id, m_impl->last_allocated_request_id);
    if (previous == std::numeric_limits<std::uint64_t>::max())
      return failure(Code::resource_exhausted,
                     "Source worker request identities are exhausted");
    m_impl->last_allocated_request_id = previous + 1;
    return m_impl->last_allocated_request_id;
  } catch (...) {
    return failure(Code::internal_failure,
                   "Source worker request allocation failed");
  }
}
auto LocalSourceWorker::submit(const std::shared_ptr<LocalSourceReader>& reader,
                               LocalSourceWorkRequest request)
    -> std::expected<void, LocalSourceWorkerError> {
  try {
    if (!reader || !reader->guarantees_pinned_read_only_sources())
      return failure(Code::invalid_request,
                     "local reader has no pinned authority");
    if (auto valid = validate_request(request); !valid)
      return failure(Code::invalid_request, valid.error().message);
    Token token = request_token(request);
    return m_impl->submit(reader, std::move(token), std::move(request));
  } catch (...) {
    return failure(Code::internal_failure, "local work submission failed");
  }
}
auto LocalSourceWorker::submit(
    const std::shared_ptr<LocalSourceGrantFactory>& factory,
    LocalFolderGrantRequest request)
    -> std::expected<void, LocalSourceWorkerError> {
  try {
    if (!factory || !factory->guarantees_pinned_read_only_sources())
      return failure(Code::invalid_request,
                     "local factory has no pinned authority");
    if (auto valid = validate_local_folder_grant_request(request); !valid)
      return failure(Code::invalid_request, valid.error().message);
    Token token = request.token;
    return m_impl->submit(factory, std::move(token), std::move(request));
  } catch (...) {
    return failure(Code::internal_failure, "local grant submission failed");
  }
}
auto LocalSourceWorker::submit(
    const std::shared_ptr<LocalContextController>& controller,
    LocalContextWorkRequest request)
    -> std::expected<void, LocalSourceWorkerError> {
  try {
    if (!controller || !valid_context_work(request))
      return failure(Code::invalid_request,
                     "local context work request is invalid");
    Token token = request.token;
    return m_impl->submit(controller, std::move(token), std::move(request));
  } catch (...) {
    return failure(Code::internal_failure,
                   "local context work submission failed");
  }
}
auto LocalSourceWorker::poll(const LocalContextWorkToken& token)
    -> std::expected<std::optional<LocalContextWorkCompletion>,
                     LocalSourceWorkerError> {
  try {
    return m_impl->poll<LocalContextWorkCompletion, ContextOutcome>(token);
  } catch (...) {
    return failure(Code::internal_failure, "local context delivery failed");
  }
}
auto LocalSourceWorker::cancel(const LocalContextWorkToken& token)
    -> std::expected<void, LocalSourceWorkerError> {
  try {
    return m_impl->cancel(token);
  } catch (...) {
    return failure(Code::internal_failure, "local context cancellation failed");
  }
}
auto LocalSourceWorker::submit(
    const std::shared_ptr<RepositoryContextController>& controller,
    RepositoryContextWorkRequest request)
    -> std::expected<void, LocalSourceWorkerError> {
  try {
    if (!controller || !controller->owns_source() ||
        !valid_repository_work(request))
      return failure(Code::invalid_request,
                     "owned repository context work request is invalid");
    Token token = request.token;
    return m_impl->submit(controller, std::move(token), std::move(request));
  } catch (...) {
    return failure(Code::internal_failure, "repository work submission failed");
  }
}
auto LocalSourceWorker::submit(
    const std::shared_ptr<OpsObservationSource>& source,
    OpsObservationWorkRequest request)
    -> std::expected<void, LocalSourceWorkerError> {
  try {
    if (!source || !source->guarantees_bound_read_only_observations() ||
        !valid_ops_work(request) ||
        source->target_binding() != request.token.observation.target)
      return failure(Code::invalid_request,
                     "bound Ops observation work request is invalid");
    Token token = request.token;
    return m_impl->submit(source, std::move(token), std::move(request));
  } catch (...) {
    return failure(Code::internal_failure, "Ops observation submission failed");
  }
}
auto LocalSourceWorker::poll(const OpsObservationWorkToken& token)
    -> std::expected<std::optional<OpsObservationWorkCompletion>,
                     LocalSourceWorkerError> {
  try {
    return m_impl->poll<OpsObservationWorkCompletion, OpsOutcome>(token);
  } catch (...) {
    return failure(Code::internal_failure, "Ops observation delivery failed");
  }
}
auto LocalSourceWorker::cancel(const OpsObservationWorkToken& token)
    -> std::expected<void, LocalSourceWorkerError> {
  try {
    return m_impl->cancel(token);
  } catch (...) {
    return failure(Code::internal_failure,
                   "Ops observation cancellation failed");
  }
}
auto LocalSourceWorker::poll(const RepositoryContextWorkToken& token)
    -> std::expected<std::optional<RepositoryContextWorkCompletion>,
                     LocalSourceWorkerError> {
  try {
    return m_impl->poll<RepositoryContextWorkCompletion, RepositoryOutcome>(
        token);
  } catch (...) {
    return failure(Code::internal_failure, "repository work delivery failed");
  }
}
auto LocalSourceWorker::cancel(const RepositoryContextWorkToken& token)
    -> std::expected<void, LocalSourceWorkerError> {
  try {
    return m_impl->cancel(token);
  } catch (...) {
    return failure(Code::internal_failure,
                   "repository work cancellation failed");
  }
}
auto LocalSourceWorker::poll(const LocalSourceRequestToken& token)
    -> std::expected<std::optional<LocalSourceWorkCompletion>,
                     LocalSourceWorkerError> {
  try {
    return m_impl->poll<LocalSourceWorkCompletion, LocalSourceWorkResult>(
        token);
  } catch (...) {
    return failure(Code::internal_failure, "local work delivery failed");
  }
}
auto LocalSourceWorker::poll(const LocalFolderGrantToken& token)
    -> std::expected<std::optional<LocalFolderGrantCompletion>,
                     LocalSourceWorkerError> {
  try {
    return m_impl->poll<LocalFolderGrantCompletion, LocalFolderGrantResult>(
        token);
  } catch (...) {
    return failure(Code::internal_failure, "local grant delivery failed");
  }
}
auto LocalSourceWorker::cancel(const LocalSourceRequestToken& token)
    -> std::expected<void, LocalSourceWorkerError> {
  try {
    return m_impl->cancel(token);
  } catch (...) {
    return failure(Code::internal_failure, "local work cancellation failed");
  }
}
auto LocalSourceWorker::cancel(const LocalFolderGrantToken& token)
    -> std::expected<void, LocalSourceWorkerError> {
  try {
    return m_impl->cancel(token);
  } catch (...) {
    return failure(Code::internal_failure, "local grant cancellation failed");
  }
}
auto LocalSourceWorker::invalidate_session(const domain::SessionId& session_id)
    -> std::expected<void, LocalSourceWorkerError> {
  try {
    for (const auto& job : m_impl->jobs)
      if (session(job->token) == session_id) job->discard();
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
auto LocalSourceWorker::ready_result(
    const RepositoryContextWorkToken& token) const -> bool {
  const auto found = m_impl->find(token);
  if (found == m_impl->jobs.end()) return false;
  const auto& job = *found;
  const std::lock_guard lock{job->mutex};
  return !job->discarded && !job->consumed && job->reader_done &&
         job->relay_done && job->result.has_value();
}
auto LocalSourceWorker::ready_results() const -> std::size_t {
  return static_cast<std::size_t>(
      std::ranges::count_if(m_impl->jobs, [](const auto& job) {
        const std::lock_guard lock{job->mutex};
        const bool producer_ready =
            std::holds_alternative<LocalFolderGrantToken>(job->token)
                ? job->ready
                : job->reader_done;
        return !job->discarded && !job->consumed && producer_ready &&
               job->relay_done && job->result.has_value();
      }));
}
} // namespace aiforge::runtime
