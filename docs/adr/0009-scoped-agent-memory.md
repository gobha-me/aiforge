# ADR 0009: Scoped agent memory journals and capture policy

- Status: Accepted
- Date: 2026-08-28

## Context

AIForge needs inspectable long-term memory without turning mutable prompt text
into hidden authority. Memory can apply globally or to one exact repository,
must survive ordinary source-session cleanup, and must remain explainable when
its source later becomes unavailable. Automatic capture is a durable write and
therefore needs an explicit runtime-owned policy rather than trusting a model
tool call.

## Decision

Agents propose memory through a provider-neutral `propose_memory` tool. The
tool has no authority-bearing effects: it only returns a validated proposal
candidate to the runtime. After the source tool result is durable, a capture
coordinator binds the source session, run, invocation, causal event IDs, model
and runtime producer, and appends lifecycle facts to a hidden memory journal.
Processing is idempotent by source session and invocation.

Memory has global and exact-`RepositoryId` scopes and four schema-version-1
kinds: user preference, project convention, workflow, and reusable fact.
Unknown future kinds remain replayable but are never selected for context.
Proposed, policy-decided, accepted, edited-and-accepted, rejected, superseded,
and expired facts build Proposed, Saved, and History projections. Editing,
replacement, and expiry append corrections with exact event preconditions;
history is never rewritten. Lifecycle timestamps come from the durable event
envelope and are retained by the projection rather than duplicated inside
mutable record content.

The existing SQLite event encoding remains durable truth. Storage format 3
marks hidden journal sessions and gives each journal a unique scope key. There
is one global journal and one journal per repository identity. Ordinary session
listing, continuation, and transcript replay exclude journals. Source-session
deletion does not delete journal facts; unavailable provenance remains visible,
but the affected record is omitted from future context until a user reaffirms
or edits it.

`memory.global.capture` defaults to `off` and
`memory.project.capture` defaults to `review`; both accept `off`, `review`, or
`auto`. Review stages a proposal. Auto uses deterministic fail-closed rules:
global accepts only a direct user preference; project accepts direct user
preferences, project conventions, and workflows. Facts, missing exact user
evidence, duplicate content, overlaps, replacements, contradictions, unsafe
text, and unavailable provenance are rejected with a recorded reason.

Current accepted memories enter context only through a dedicated bounded
memory budget, defaulting to 2048 estimated tokens. Current-project records
rank before global records, then by newest acceptance and stable identity.
They are evidence-role content, not an instruction layer, and cannot grant or
broaden capabilities.

## Consequences

- Interactive, one-shot, and child-run surfaces share one proposal pipeline.
- Memory management is a projection/controller concern; TermForge owns only
  generic interaction and rendering.
- Cross-journal bulk mutation is intentionally unavailable because SQLite
  transactions and compare-and-set preconditions remain journal-local.
- Semantic contradiction detection is not guessed. Proposals declare overlap
  and replacement relationships; auto rejects them and review requires an
  explicit replacement decision.
