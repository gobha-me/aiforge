#include <aiforge/runtime/repository_context_controller.hpp>

#include <aiforge/detail/sha256.hpp>
#include <aiforge/detail/utf8_text.hpp>
#include <aiforge/runtime/project_instructions.hpp>
#include <algorithm>
#include <set>
#include <span>
#include <utility>

namespace aiforge::runtime {
namespace {
using Error = domain::RepositoryContextError;
using Code = domain::RepositoryContextErrorCode;
using Result = std::expected<void, Error>;
auto failure(Code code, std::string message) -> std::unexpected<Error> {
  return std::unexpected(Error{code, std::move(message)});
}
auto digest(std::string_view text) -> domain::ContentDigest {
  detail::Sha256 hash;
  hash.update(std::as_bytes(std::span{text.data(), text.size()}));
  return {"sha256", hash.finish(), text.size()};
}
auto normalized_path(std::string_view value, bool empty = false) -> bool {
  if (value.empty()) return empty;
  if (value.size() > domain::repository_context_maximum_path_bytes ||
      !detail::is_safe_utf8_text(value) || value.front() == '/' ||
      value.back() == '/' ||
      value.find_first_of("\\:\r\n\t") != std::string_view::npos)
    return false;
  std::size_t start{};
  while (start < value.size()) {
    auto end = value.find('/', start);
    auto part =
        value.substr(start, end == std::string_view::npos ? end : end - start);
    if (part.empty() || part == "." || part == "..") return false;
    if (end == std::string_view::npos) return true;
    start = end + 1;
  }
  return false;
}
struct Budget {
  std::chrono::steady_clock::time_point started{
      std::chrono::steady_clock::now()};
  std::chrono::milliseconds timeout;
  std::stop_token stop;
  [[nodiscard]] auto check() const -> Result {
    if (stop.stop_requested())
      return failure(Code::cancelled,
                     "repository context preparation cancelled");
    if (std::chrono::steady_clock::now() - started >= timeout)
      return failure(Code::timed_out,
                     "repository context preparation timed out");
    return {};
  }
  [[nodiscard]] auto remaining() const -> std::chrono::milliseconds {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    return std::max(std::chrono::milliseconds{1}, timeout - elapsed);
  }
};
template <class ExternalError>
auto source_failure(const ExternalError& error, std::string message)
    -> std::unexpected<Error> {
  using ExternalCode = decltype(error.code);
  auto code = Code::stale_source;
  if (error.code == ExternalCode::cancelled)
    code = Code::cancelled;
  else if (error.code == ExternalCode::timed_out)
    code = Code::timed_out;
  else if (error.code == ExternalCode::resource_exhausted)
    code = Code::resource_exhausted;
  return failure(code, std::move(message));
}
auto valid_snapshot_limits(const repository::RepositorySnapshotLimits& limits)
    -> bool {
  const repository::RepositorySnapshotLimits maximum;
  return limits.maximum_entries > 0 &&
         limits.maximum_entries <= maximum.maximum_entries &&
         limits.maximum_path_bytes > 0 &&
         limits.maximum_path_bytes <= maximum.maximum_path_bytes &&
         limits.maximum_file_bytes > 0 &&
         limits.maximum_file_bytes <= maximum.maximum_file_bytes &&
         limits.maximum_total_bytes > 0 &&
         limits.maximum_total_bytes <= maximum.maximum_total_bytes &&
         limits.maximum_command_output_bytes > 0 &&
         limits.maximum_command_output_bytes <=
             maximum.maximum_command_output_bytes &&
         limits.command_timeout > std::chrono::milliseconds::zero() &&
         limits.command_timeout <= maximum.command_timeout &&
         limits.observation_timeout > std::chrono::milliseconds::zero() &&
         limits.observation_timeout <= maximum.observation_timeout &&
         limits.command_timeout <= limits.observation_timeout;
}
auto valid_limits(const RepositoryContextLimits& limits) -> bool {
  const RepositoryContextLimits maximum;
  return valid_snapshot_limits(limits.snapshot) &&
         limits.instructions.timeout > std::chrono::milliseconds::zero() &&
         limits.instructions.timeout <= maximum.instructions.timeout &&
         limits.maximum_evidence_files > 0 &&
         limits.maximum_evidence_files <= maximum.maximum_evidence_files &&
         limits.maximum_evidence_file_bytes > 0 &&
         limits.maximum_evidence_file_bytes <=
             maximum.maximum_evidence_file_bytes &&
         limits.maximum_evidence_total_bytes > 0 &&
         limits.maximum_evidence_total_bytes <=
             maximum.maximum_evidence_total_bytes &&
         limits.timeout > std::chrono::milliseconds::zero() &&
         limits.timeout <= maximum.timeout &&
         limits.instructions.maximum_documents > 0 &&
         limits.instructions.maximum_documents <=
             maximum.instructions.maximum_documents &&
         limits.instructions.maximum_path_bytes > 0 &&
         limits.instructions.maximum_path_bytes <=
             maximum.instructions.maximum_path_bytes &&
         limits.instructions.maximum_document_bytes > 0 &&
         limits.instructions.maximum_document_bytes <=
             maximum.instructions.maximum_document_bytes &&
         limits.instructions.maximum_total_bytes > 0 &&
         limits.instructions.maximum_total_bytes <=
             maximum.instructions.maximum_total_bytes;
}
auto valid_request(const RepositoryContextRequest& request,
                   const RepositoryContextLimits& limits) -> Result {
  if (!valid_limits(limits) || !normalized_path(request.target_subtree, true) ||
      request.selection_revision == 0 ||
      request.evidence_paths.size() > limits.maximum_evidence_files)
    return failure(Code::invalid_request,
                   "repository context selection or limits are invalid");
  std::set<std::string> paths;
  for (const auto& path : request.evidence_paths)
    if (!normalized_path(path) || !paths.insert(path).second)
      return failure(
          Code::invalid_request,
          "repository context evidence paths are invalid or duplicated");
  return {};
}
auto same_snapshot(const domain::RepositorySnapshot& left,
                   const domain::RepositorySnapshot& right) -> bool {
  return left.root == right.root && left.vcs == right.vcs &&
         left.changes == right.changes && left.fingerprint == right.fingerprint;
}
auto checked_observe(RepositoryContextSource& source,
                     const domain::RepositoryRootIdentity& root,
                     std::string_view binding,
                     repository::RepositorySnapshotLimits limits,
                     const Budget& budget)
    -> std::expected<domain::RepositorySnapshot, Error> {
  if (auto ready = budget.check(); !ready)
    return std::unexpected(ready.error());
  if (!source.guarantees_pinned_read_only_sources() ||
      source.identity() != binding)
    return failure(
        Code::unavailable,
        "repository context requires the original pinned read-only sources");
  limits.observation_timeout =
      std::min(limits.observation_timeout, budget.remaining());
  limits.command_timeout = std::min(limits.command_timeout, budget.remaining());
  auto observed = source.observe(limits, budget.stop);
  if (auto ready = budget.check(); !ready)
    return std::unexpected(ready.error());
  if (!observed)
    return source_failure(observed.error(),
                          "repository context observation failed");
  if (source.identity() != binding || observed->root != root ||
      !repository::validate_repository_snapshot(*observed, limits) ||
      !observed->vcs || observed->vcs->system != "git")
    return failure(Code::stale_source,
                   "repository context root or snapshot identity changed");
  return std::move(*observed);
}
auto verify_text(const repository::ExactSourceReadResult& read,
                 const domain::RepositorySnapshotIdentity& snapshot,
                 std::string_view path, std::uint64_t maximum) -> Result {
  if (read.content.empty())
    return failure(Code::unavailable,
                   "empty files cannot be selected as exact inline evidence");
  if (read.source.snapshot != snapshot || read.source.relative_path != path ||
      read.source.range || read.content.empty() ||
      read.content.size() > maximum ||
      read.source.content_digest.byte_size != read.content.size() ||
      !detail::is_safe_utf8_text(read.content))
    return failure(
        Code::stale_source,
        "repository evidence is not bounded whole-file UTF-8 source");
  // Native Git object identity remains adapter-owned. Plain SHA256 identities
  // can additionally be checked here; every admission gets its own text SHA256.
  if (read.source.content_digest.algorithm == "sha256" &&
      read.source.content_digest != digest(read.content))
    return failure(Code::stale_source,
                   "repository evidence digest does not match its bytes");
  return {};
}
auto discovery_inputs(const domain::ProjectInstructionDiscovery& discovery,
                      const domain::RepositorySnapshot& snapshot,
                      const RepositoryContextLimits& limits)
    -> std::expected<std::vector<domain::InstructionInput>, Error> {
  if (discovery.documents.size() > limits.instructions.maximum_documents)
    return failure(Code::resource_exhausted,
                   "repository instruction count exceeds its bound");
  std::vector<ProjectInstructionTokenEstimate> estimates;
  std::uint64_t total{};
  for (const auto& document : discovery.documents) {
    if (document.text.size() > limits.instructions.maximum_total_bytes - total)
      return failure(Code::resource_exhausted,
                     "repository instructions exceed their byte bound");
    auto verified = verify_text({document.source, document.text},
                                domain::snapshot_identity(snapshot),
                                document.source.relative_path,
                                limits.instructions.maximum_document_bytes);
    if (!verified) return std::unexpected(verified.error());
    estimates.push_back({document.instruction_id, document.text.size()});
    total += document.text.size();
  }
  auto inputs = project_instruction_inputs(
      discovery, domain::snapshot_identity(snapshot), estimates);
  if (!inputs)
    return failure(Code::stale_source,
                   "repository instruction membership or ordering is invalid");
  return std::move(*inputs);
}
auto make_evidence_ids(std::string_view binding, std::uint64_t revision,
                       std::string_view path) -> std::string {
  return digest(std::to_string(binding.size()) + ":" + std::string{binding} +
                ":" + std::to_string(revision) + ":" + std::string{path})
      .value;
}
auto append_evidence(PreparedRepositoryContext& prepared,
                     repository::ExactSourceReadResult read,
                     std::string_view binding, std::uint64_t revision,
                     std::uint64_t order) -> void {
  const auto suffix =
      make_evidence_ids(binding, revision, read.source.relative_path);
  const auto evidence_id =
      domain::EvidenceId::from("repository-evidence-" + suffix).value();
  const auto entry_id =
      domain::ContextEntryId::from("repository-context-entry-" + suffix)
          .value();
  const auto message_id =
      domain::MessageId::from("repository-context-message-" + suffix).value();
  const auto source_id =
      domain::ContextSourceId::from("repository-context-source-" + suffix)
          .value();
  const auto tokens = static_cast<std::uint64_t>(read.content.size());
  const auto text_digest = digest(read.content);
  prepared.admission.evidence.push_back(
      {evidence_id, entry_id, message_id, source_id, read.source, order, tokens,
       domain::RepositoryContextDecision::admitted, text_digest});
  prepared.evidence.items.push_back({evidence_id,
                                     entry_id,
                                     message_id,
                                     source_id,
                                     ContextBudgetClass::repository_evidence,
                                     ContextRepresentation::exact,
                                     false,
                                     order,
                                     order,
                                     {}});
  prepared.evidence.parcel.items.push_back(
      {evidence_id,
       domain::ExactSourceEvidence{read.source},
       domain::EvidenceFreshness::current,
       {domain::EvidenceDerivation::observed,
        "aiforge.repository-context",
        "1",
        prepared.snapshot.observed_at,
        domain::snapshot_identity(prepared.snapshot),
        {},
        {},
        {}},
       {domain::TextBlock{std::move(read.content)}},
       tokens,
       tokens});
}
auto gather_evidence(RepositoryContextSource& source,
                     PreparedRepositoryContext& prepared,
                     const RepositoryContextRequest& request,
                     const RepositoryContextLimits& limits,
                     const Budget& budget) -> Result {
  std::uint64_t total{};
  std::uint64_t order{};
  for (const auto& path : request.evidence_paths) {
    if (auto ready = budget.check(); !ready) return ready;
    repository::ExactSourceEditLimits read_limits;
    read_limits.maximum_source_bytes = limits.maximum_evidence_file_bytes;
    read_limits.timeout = budget.remaining();
    auto read =
        source.read({prepared.snapshot, path, read_limits}, budget.stop);
    if (auto ready = budget.check(); !ready) return ready;
    if (!read)
      return source_failure(read.error(),
                            "repository context evidence read failed");
    if (auto verified =
            verify_text(*read, domain::snapshot_identity(prepared.snapshot),
                        path, limits.maximum_evidence_file_bytes);
        !verified)
      return verified;
    if (read->content.size() > limits.maximum_evidence_total_bytes - total)
      return failure(
          Code::resource_exhausted,
          "repository context evidence exceeds its total byte bound");
    total += read->content.size();
    append_evidence(prepared, std::move(*read), prepared.admission.root_binding,
                    request.selection_revision, ++order);
  }
  return {};
}
auto map_decision(ContextSelectionDecision decision)
    -> std::optional<domain::RepositoryContextDecision> {
  switch (decision) {
    case ContextSelectionDecision::admitted:
      return domain::RepositoryContextDecision::admitted;
    case ContextSelectionDecision::omitted_budget:
      return domain::RepositoryContextDecision::omitted_budget;
    case ContextSelectionDecision::omitted_class_budget:
      return domain::RepositoryContextDecision::omitted_class_budget;
    default: return {};
  }
}
auto verify_instruction_chain(RepositoryContextSource& source,
                              const PreparedRepositoryContext& prepared,
                              repository::ProjectInstructionLimits limits,
                              const Budget& budget) -> Result {
  if (auto ready = budget.check(); !ready) return ready;
  limits.timeout = std::min(limits.timeout, budget.remaining());
  auto current = source.discover(
      {prepared.snapshot, prepared.discovery.target_subtree, limits},
      budget.stop);
  if (auto ready = budget.check(); !ready) return ready;
  if (!current)
    return source_failure(current.error(),
                          "repository instruction revalidation failed");
  if (*current != prepared.discovery)
    return failure(Code::stale_source,
                   "repository instruction membership or bytes changed "
                   "during context preparation");
  return {};
}
} // namespace

RepositoryContextController::RepositoryContextController(
    RepositoryContextSource& source, domain::RepositoryRootIdentity root,
    RepositoryContextLimits limits)
    : m_source(source), m_root(std::move(root)), m_binding(source.identity()),
      m_limits(limits) {
}

auto RepositoryContextController::prepare(RepositoryContextRequest request,
                                          std::stop_token stop) const
    -> std::expected<PreparedRepositoryContext, Error> {
  try {
    if (request.target_subtree == ".") request.target_subtree.clear();
    if (auto valid = valid_request(request, m_limits); !valid)
      return std::unexpected(valid.error());
    const Budget budget{std::chrono::steady_clock::now(), m_limits.timeout,
                        stop};
    auto snapshot =
        checked_observe(m_source, m_root, m_binding, m_limits.snapshot, budget);
    if (!snapshot) return std::unexpected(snapshot.error());
    auto instruction_limits = m_limits.instructions;
    instruction_limits.timeout =
        std::min(instruction_limits.timeout, budget.remaining());
    auto discovery = m_source.discover(
        {*snapshot, request.target_subtree, instruction_limits}, stop);
    if (auto ready = budget.check(); !ready)
      return std::unexpected(ready.error());
    if (!discovery)
      return source_failure(discovery.error(),
                            "repository context instruction discovery failed");
    if (discovery->target_subtree != request.target_subtree)
      return failure(
          Code::stale_source,
          "repository instruction discovery targeted another subtree");
    auto instructions = discovery_inputs(*discovery, *snapshot, m_limits);
    if (!instructions) return std::unexpected(instructions.error());
    const auto parcel_id =
        domain::ContextParcelId::from(
            "repository-context-parcel-" +
            make_evidence_ids(m_binding, request.selection_revision,
                              request.target_subtree))
            .value();
    PreparedRepositoryContext prepared{*snapshot,
                                       *discovery,
                                       std::move(*instructions),
                                       {{parcel_id,
                                         "explicit repository context",
                                         domain::TaskPhase::orientation,
                                         domain::snapshot_identity(*snapshot),
                                         {}},
                                        {}},
                                       {1,
                                        m_binding,
                                        domain::snapshot_identity(*snapshot),
                                        request.target_subtree,
                                        request.selection_revision,
                                        {},
                                        {},
                                        {},
                                        {}}};
    for (const auto& document : discovery->documents)
      prepared.admission.instructions.push_back(
          {document.instruction_id, document.source,
           document.applicable_subtree, document.specificity,
           document.discovery_order, document.text.size(),
           digest(document.text)});
    if (auto gathered =
            gather_evidence(m_source, prepared, request, m_limits, budget);
        !gathered)
      return std::unexpected(gathered.error());
    // Ignored instruction files need not participate in the Git fingerprint.
    // Recheck the complete applicable chain after reading selected evidence.
    if (auto verified = verify_instruction_chain(m_source, prepared,
                                                 m_limits.instructions, budget);
        !verified)
      return std::unexpected(verified.error());
    auto after =
        checked_observe(m_source, m_root, m_binding, m_limits.snapshot, budget);
    if (!after) return std::unexpected(after.error());
    if (!same_snapshot(*snapshot, *after))
      return failure(Code::stale_source,
                     "repository changed during context preparation");
    // Validate unfinalized references using a temporary generous capacity.
    auto check = prepared.admission;
    check.capacity = {std::uint64_t{16} * 1024 * 1024, 1, 0};
    if (auto sealed = domain::seal_repository_context_admission(check); !sealed)
      return std::unexpected(sealed.error());
    if (auto ready = budget.check(); !ready)
      return std::unexpected(ready.error());
    return prepared;
  } catch (...) {
    return failure(Code::internal_failure,
                   "repository context preparation failed internally");
  }
}

auto RepositoryContextController::revalidate(
    const domain::RepositoryContextAdmission& original,
    std::stop_token stop) const
    -> std::expected<PreparedRepositoryContext, Error> {
  try {
    if (!domain::validate_repository_context_admission(original) ||
        original.root_binding != m_binding ||
        original.source_snapshot.repository_id != m_root.repository_id)
      return failure(Code::invalid_admission,
                     "repository context recovery admission is invalid or "
                     "bound elsewhere");
    RepositoryContextRequest request{
        original.target_subtree, original.selection_revision, {}};
    for (const auto& evidence : original.evidence)
      request.evidence_paths.push_back(evidence.source.relative_path);
    auto prepared = prepare(std::move(request), stop);
    if (!prepared) return std::unexpected(prepared.error());
    if (prepared->admission.evidence.size() != original.evidence.size())
      return failure(Code::stale_source,
                     "repository context evidence membership changed");
    prepared->admission.capacity = original.capacity;
    for (std::size_t index{}; index < original.evidence.size(); ++index) {
      prepared->admission.evidence[index].decision =
          original.evidence[index].decision;
      prepared->admission.evidence[index].order =
          original.evidence[index].order;
      prepared->evidence.items[index].order = original.evidence[index].order;
    }
    if (auto sealed =
            domain::seal_repository_context_admission(prepared->admission);
        !sealed)
      return std::unexpected(sealed.error());
    if (!domain::repository_context_admission_successor(original,
                                                        prepared->admission))
      return failure(Code::stale_source,
                     "selected repository evidence or instruction "
                     "membership/bytes changed");
    return prepared;
  } catch (...) {
    return failure(Code::internal_failure,
                   "repository context revalidation failed internally");
  }
}

auto finalize_repository_context_admission(
    const domain::RepositoryContextAdmission& prepared,
    const ContextSelectionResult& selection)
    -> std::expected<domain::RepositoryContextAdmission, Error> {
  try {
    auto admission = prepared;
    admission.capacity = selection.context.capacity;
    for (auto& evidence : admission.evidence) {
      const ContextSelectionDecisionRecord* found{};
      for (const auto& decision : selection.decisions) {
        if (decision.entry_id != evidence.entry_id) continue;
        if (found != nullptr || decision.evidence_id != evidence.evidence_id ||
            decision.estimated_tokens != evidence.estimated_tokens)
          return failure(
              Code::invalid_admission,
              "repository selection decisions are duplicated or mismatched");
        found = &decision;
      }
      if (found == nullptr)
        return failure(Code::invalid_admission,
                       "repository selection decision is missing");
      const auto mapped = map_decision(found->decision);
      if (!mapped)
        return failure(
            Code::invalid_admission,
            "repository selection cannot admit stale or unsupported evidence");
      evidence.decision = *mapped;
    }
    if (auto sealed = domain::seal_repository_context_admission(admission);
        !sealed)
      return std::unexpected(sealed.error());
    if (!domain::repository_context_admission_matches_context(
            admission, selection.context))
      return failure(Code::invalid_admission,
                     "repository context does not match its exact admission");
    return admission;
  } catch (...) {
    return failure(Code::internal_failure,
                   "repository admission finalization failed internally");
  }
}
auto finalize_repository_context_admission(
    const PreparedRepositoryContext& prepared,
    const ContextSelectionResult& selection)
    -> std::expected<domain::RepositoryContextAdmission, Error> {
  return finalize_repository_context_admission(prepared.admission, selection);
}
} // namespace aiforge::runtime
