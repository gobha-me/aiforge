#include <aiforge/detail/sha256.hpp>
#include <aiforge/runtime/local_context_controller.hpp>
#include <algorithm>
#include <limits>
#include <span>
#include <string_view>

namespace aiforge::runtime {
namespace {
using Error = domain::LocalContextError;
using Code = domain::LocalContextErrorCode;
using Status = std::expected<void, Error>;
auto failure(Code code, std::string message) -> std::unexpected<Error> {
  return std::unexpected(Error{code, std::move(message)});
}
auto source_failure(domain::LocalSourceErrorCode code)
    -> std::unexpected<Error> {
  using Source = domain::LocalSourceErrorCode;
  switch (code) {
    case Source::cancelled:
      return failure(Code::cancelled, "local preparation cancelled");
    case Source::timed_out:
      return failure(Code::timed_out, "local preparation timed out");
    case Source::stale_lease:
      return failure(Code::stale_lease, "local grant is no longer current");
    case Source::source_mismatch:
    case Source::concurrent_change:
      return failure(Code::stale_source, "local source no longer matches");
    case Source::resource_exhausted:
      return failure(Code::resource_exhausted,
                     "local preparation exceeds resource bounds");
    default:
      return failure(Code::unavailable,
                     "local source is unavailable for exact context");
  }
}
struct Budget {
  std::chrono::steady_clock::time_point started{
      std::chrono::steady_clock::now()};
  std::chrono::milliseconds timeout;
  std::stop_token stop;
  [[nodiscard]] auto remaining() const
      -> std::expected<std::chrono::milliseconds, Error> {
    if (stop.stop_requested())
      return failure(Code::cancelled, "local preparation cancelled");
    const auto elapsed = std::chrono::steady_clock::now() - started;
    if (elapsed >= timeout)
      return failure(Code::timed_out, "local preparation timed out");
    const auto result = std::chrono::duration_cast<std::chrono::milliseconds>(
        timeout - elapsed);
    if (result.count() == 0)
      return failure(Code::timed_out, "local preparation timed out");
    return result;
  }
};
auto resolve(LocalContextGrantResolver& resolver,
             const domain::SessionId& session,
             const domain::LocalRootIdentity& root, const Budget& budget)
    -> std::expected<LocalContextGrant, Error> {
  if (auto valid = budget.remaining(); !valid)
    return std::unexpected(valid.error());
  auto resolved = resolver.resolve(session, root, budget.stop);
  if (!resolved) return source_failure(resolved.error().code);
  if (!*resolved)
    return failure(Code::unavailable,
                   "local source requires a current explicit folder grant");
  auto& grant = **resolved;
  if (grant.session_id != session || grant.root != root ||
      grant.lease_generation == 0 || !grant.reader ||
      !grant.reader->guarantees_pinned_read_only_sources())
    return failure(Code::stale_lease, "local source grant is invalid");
  if (auto valid = budget.remaining(); !valid)
    return std::unexpected(valid.error());
  return std::move(grant);
}
auto identities(const LocalContextRequest& request,
                const domain::LocalSourceIdentity& source) -> std::string {
  detail::Sha256 hash;
  const auto field = [&](std::string_view value) {
    const auto prefix = std::to_string(value.size()) + ":";
    hash.update(std::as_bytes(std::span{prefix.data(), prefix.size()}));
    hash.update(std::as_bytes(std::span{value.data(), value.size()}));
  };
  field("aiforge.local-context-entry.v1");
  field(request.session_id.value());
  field(std::to_string(request.selection_revision));
  field(std::to_string(source.root.version));
  field(source.root.binding);
  field(source.relative_path);
  field(source.content_digest.value);
  return hash.finish();
}
auto append(PreparedLocalContext& prepared, const LocalContextRequest& request,
            LocalReadResult read, std::uint64_t order) -> void {
  const auto suffix = identities(request, read.source);
  domain::LocalContextEvidence ref{
      domain::EvidenceId::from("local-evidence-" + suffix).value(),
      domain::ContextEntryId::from("local-context-entry-" + suffix).value(),
      domain::MessageId::from("local-context-message-" + suffix).value(),
      domain::ContextSourceId::from("local-context-source-" + suffix).value(),
      std::move(read.source),
      order,
      read.text.size(),
      domain::LocalContextDecision::admitted};
  prepared.candidates.push_back(
      {{ref.entry_id,
        domain::ContextContentKind::evidence,
        {ref.message_id,
         domain::Role::evidence,
         {domain::TextBlock{std::move(read.text)}},
         {}},
        {ref.source_id,
         domain::local_context_source_location(ref.source).value(),
         "sha256:" + ref.source.content_digest.value},
        ref.order,
        ref.estimated_tokens},
       ContextBudgetClass::attachment,
       ContextRepresentation::exact,
       domain::EvidenceFreshness::current,
       false,
       order,
       {}});
  prepared.admission.evidence.push_back(std::move(ref));
}
auto preflight(const LocalContextRequest& request,
               const domain::LocalSourceLimits& limits) -> Status {
  if (!domain::validate_local_source_limits(limits) ||
      request.selection_revision == 0 || request.first_order == 0)
    return failure(Code::invalid_request, "local context request is invalid");
  if (!domain::validate_local_source_selection(request.sources, limits))
    return failure(Code::invalid_request,
                   "local context sources are invalid or exceed bounds");
  if (!request.sources.empty() &&
      request.sources.size() - 1 >
          std::numeric_limits<std::uint64_t>::max() - request.first_order)
    return failure(Code::resource_exhausted,
                   "local context order exceeds its bound");
  return {};
}
auto prepare_sources(LocalContextGrantResolver& resolver,
                     const LocalContextRequest& request,
                     const domain::LocalSourceLimits& limits,
                     const Budget& budget)
    -> std::expected<PreparedLocalContext, Error> {
  PreparedLocalContext result{
      {}, {1, request.session_id, request.selection_revision, {}, {}, {}}};
  std::vector<LocalContextGrant> grants;
  for (std::size_t index = 0; index < request.sources.size(); ++index) {
    const auto& source = request.sources[index];
    auto found =
        std::ranges::find(grants, source.root, &LocalContextGrant::root);
    if (found == grants.end()) {
      auto grant = resolve(resolver, request.session_id, source.root, budget);
      if (!grant) return std::unexpected(grant.error());
      grants.push_back(std::move(*grant));
      found = std::prev(grants.end());
    }
    auto remaining = budget.remaining();
    if (!remaining) return std::unexpected(remaining.error());
    auto read_limits = limits;
    read_limits.timeout = *remaining;
    LocalRevalidateRequest read_request{{request.session_id, source.root,
                                         found->lease_generation, index + 1,
                                         request.selection_revision},
                                        source,
                                        read_limits};
    auto read = found->reader->revalidate(read_request, budget.stop);
    if (!read) return source_failure(read.error().code);
    if (auto valid = validate_local_read_result(read_request, *read); !valid)
      return source_failure(valid.error().code);
    append(result, request, std::move(*read), request.first_order + index);
  }
  // Re-resolve all roots after all reads; an earlier source cannot retain a
  // revoked or replaced lease merely because its own read already finished.
  for (const auto& grant : grants) {
    auto current = resolve(resolver, request.session_id, grant.root, budget);
    if (!current) return std::unexpected(current.error());
    if (current->lease_generation != grant.lease_generation ||
        current->reader != grant.reader)
      return failure(Code::stale_lease,
                     "local grants changed during preparation");
  }
  if (auto valid = budget.remaining(); !valid)
    return std::unexpected(valid.error());
  return result;
}
auto decision(ContextSelectionDecision value)
    -> std::optional<domain::LocalContextDecision> {
  switch (value) {
    case ContextSelectionDecision::admitted:
      return domain::LocalContextDecision::admitted;
    case ContextSelectionDecision::omitted_budget:
      return domain::LocalContextDecision::omitted_budget;
    case ContextSelectionDecision::omitted_class_budget:
      return domain::LocalContextDecision::omitted_class_budget;
    default: return std::nullopt;
  }
}
auto finalization_preflight(const PreparedLocalContext& prepared,
                            const ContextSelectionResult& selected) -> Status {
  const domain::LocalSourceLimits maximum;
  if (prepared.admission.evidence.size() > maximum.maximum_selected_files ||
      prepared.candidates.size() != prepared.admission.evidence.size() ||
      selected.decisions.size() > ContextSelectionLimits{}.maximum_candidates)
    return failure(Code::invalid_admission,
                   "local finalization exceeds its bounds");
  for (const auto& ref : prepared.admission.evidence)
    if (!domain::validate_local_source_identity(ref.source))
      return failure(Code::invalid_admission,
                     "local prepared source is invalid");
  if (prepared.admission.admission_digest &&
      !domain::validate_local_context_admission(prepared.admission))
    return failure(Code::invalid_admission,
                   "local recovered admission is invalid");
  return {};
}
auto validate_prepared_candidates(const PreparedLocalContext& prepared)
    -> Status {
  auto proof = prepared.admission;
  proof.capacity = {std::numeric_limits<std::uint64_t>::max(), 0, 0};
  for (auto& ref : proof.evidence)
    ref.decision = domain::LocalContextDecision::admitted;
  if (auto valid = domain::seal_local_context_admission(proof); !valid)
    return std::unexpected(valid.error());
  domain::ConstructedContext context{{}, {}, proof.capacity, 0};
  for (std::size_t index = 0; index < prepared.candidates.size(); ++index) {
    const auto& candidate = prepared.candidates[index];
    const auto& content = candidate.content;
    const auto& ref = proof.evidence[index];
    const auto location = domain::local_context_source_location(ref.source);
    const auto* text =
        content.message.content.size() == 1
            ? std::get_if<domain::TextBlock>(&content.message.content.front())
            : nullptr;
    if (candidate.budget_class != ContextBudgetClass::attachment ||
        candidate.representation != ContextRepresentation::exact ||
        candidate.freshness != domain::EvidenceFreshness::current ||
        candidate.required || candidate.alternative_group ||
        candidate.relevance_rank != ref.order ||
        content.kind != domain::ContextContentKind::evidence ||
        !content.message.tool_calls.empty() || content.message.invocation_id ||
        text == nullptr ||
        text->text.size() != ref.source.content_digest.byte_size || !location ||
        content.provenance.source_location != *location ||
        content.provenance.digest !=
            "sha256:" + ref.source.content_digest.value)
      return failure(Code::invalid_admission,
                     "local prepared candidate does not match its source");
    context.entries.push_back({content.entry_id,
                               domain::ContextEntryKind::evidence,
                               {},
                               content.message,
                               content.provenance,
                               0,
                               content.order,
                               content.estimated_tokens});
  }
  if (!domain::local_context_admission_matches_context(proof, context))
    return failure(Code::invalid_admission,
                   "local prepared candidate bytes or identities changed");
  return {};
}
auto apply_decisions(domain::LocalContextAdmission& admission,
                     const ContextSelectionResult& selection) -> Status {
  for (auto& ref : admission.evidence) {
    const ContextSelectionDecisionRecord* found{};
    for (const auto& record : selection.decisions) {
      if (record.entry_id != ref.entry_id) continue;
      if (found != nullptr || record.evidence_id ||
          record.estimated_tokens != ref.estimated_tokens)
        return failure(Code::invalid_admission,
                       "local selection decisions do not match");
      found = &record;
    }
    if (found == nullptr)
      return failure(Code::invalid_admission,
                     "local selection decision is missing");
    auto mapped = decision(found->decision);
    if (!mapped)
      return failure(Code::invalid_admission,
                     "local selection cannot admit unavailable evidence");
    ref.decision = *mapped;
  }
  return {};
}
} // namespace

LocalContextController::LocalContextController(
    std::shared_ptr<LocalContextGrantResolver> grants,
    domain::LocalSourceLimits limits)
    : m_grants(std::move(grants)), m_limits(limits) {
}
auto LocalContextController::prepare(LocalContextRequest request,
                                     std::stop_token stop) const
    -> std::expected<PreparedLocalContext, Error> {
  try {
    if (!m_grants)
      return failure(Code::invalid_request,
                     "local grant resolver is unavailable");
    if (auto valid = preflight(request, m_limits); !valid)
      return std::unexpected(valid.error());
    const Budget budget{std::chrono::steady_clock::now(), m_limits.timeout,
                        stop};
    return prepare_sources(*m_grants, request, m_limits, budget);
  } catch (...) {
    return failure(Code::internal_failure, "local context preparation failed");
  }
}
auto LocalContextController::revalidate(
    const domain::LocalContextAdmission& original, std::stop_token stop) const
    -> std::expected<PreparedLocalContext, Error> {
  try {
    if (!domain::validate_local_context_admission(original))
      return failure(Code::invalid_admission,
                     "local recovery admission is invalid");
    LocalContextRequest request{
        original.session_id, original.selection_revision, {}, 1};
    for (const auto& ref : original.evidence)
      request.sources.push_back(ref.source);
    auto prepared = prepare(std::move(request), stop);
    if (!prepared) return std::unexpected(prepared.error());
    for (std::size_t index = 0; index < original.evidence.size(); ++index) {
      const auto& ref = original.evidence[index];
      auto& candidate = prepared->candidates[index];
      candidate.content.entry_id = ref.entry_id;
      candidate.content.message.message_id = ref.message_id;
      candidate.content.provenance.source_id = ref.source_id;
      candidate.content.order = ref.order;
      candidate.relevance_rank = ref.order;
    }
    prepared->admission = original;
    return prepared;
  } catch (...) {
    return failure(Code::internal_failure, "local context recovery failed");
  }
}
auto finalize_local_context_admission(const PreparedLocalContext& prepared,
                                      const ContextSelectionResult& selection)
    -> std::expected<domain::LocalContextAdmission, Error> {
  try {
    if (auto valid = finalization_preflight(prepared, selection); !valid)
      return std::unexpected(valid.error());
    if (auto valid = validate_prepared_candidates(prepared); !valid)
      return std::unexpected(valid.error());
    auto admission = prepared.admission;
    admission.capacity = selection.context.capacity;
    if (auto valid = apply_decisions(admission, selection); !valid)
      return std::unexpected(valid.error());
    if (auto valid = domain::seal_local_context_admission(admission); !valid)
      return std::unexpected(valid.error());
    if (!domain::local_context_admission_matches_context(admission,
                                                         selection.context) ||
        (prepared.admission.admission_digest &&
         !domain::local_context_admission_successor(prepared.admission,
                                                    admission)))
      return failure(Code::invalid_admission,
                     "local final context does not match exact admission");
    return admission;
  } catch (...) {
    return failure(Code::internal_failure, "local context finalization failed");
  }
}
} // namespace aiforge::runtime
