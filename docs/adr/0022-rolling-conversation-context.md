# ADR 0022: Explicit rolling conversation context

- Status: Accepted
- Date: 2026-09-08
- Owner direction: issue #225, discussion completed 2026-09-08

## Context

ADR 0002 validates a selected request; it does not choose which historical
conversation to send. Replaying every completed message eventually leaves no
capacity for a new turn. Clearing the display does not change this working set.
The owner approved opt-in rolling context, pinned turns, reviewed summaries and
an explicit pre-compaction handoff, while retaining the original event history.

## Decision

### Complete groups and deterministic selection

Conversation selection is a provider-neutral runtime operation outside
`ContextBuilder`. Its input consists of chronological atomic conversation
groups. Each group identifies its source run and source events and contains all
of the context entries selected for that exchange. A group cannot be partially
admitted. In particular, a tool call and its results cannot be independently
evicted. Constructing a group must validate the exchange before selection;
group selection does not repair an invalid provider conversation. The initial
history projection uses one completed source run per group, including all
eligible messages and complete tool exchanges. Failed/cancelled runs retain
their complete user input as a user-only group; incomplete assistant/tool
exchanges are excluded. Live runs and summary-producing or policy-control runs
do not become ordinary historical groups, even when they contain user and
assistant messages. Successful empty assistant answers retain the user input as
legacy replay did. If an empty final answer follows complete tool exchanges,
the group may end with the last matched tool result; preserving those exchanges
does not require inventing assistant text. Unresolved calls still reject the
group. Groups follow their first source
sequence, while entries retain provider message order. A tool validation error
can be recorded before the assistant's completion event; its tool message still
follows that assistant's tool call. Source completion sequences are positive and
unique, not necessarily increasing in provider order. Extraction validates the
source event graph instead of relabeling events to manufacture chronological
completion references.

The default policy is full history. It continues to reject an oversized request
instead of silently changing a user's context policy. Explicit rolling mode
first reserves required instructions, the current user input, tool declarations,
output capacity, active summaries and pinned groups. It next selects eligible
scoped memory within the existing shared memory budget and remaining capacity;
persona/repository/global ownership and ranking remain unchanged. It then admits the newest
complete historical groups that fit, preserving their original chronology.
After an unpinned group fails to fit, older unpinned groups are omitted too;
selection never fills a gap with an older small exchange. Older pinned groups
remain selected. A pin that cannot fit is an explicit mandatory-capacity error,
not a silently ignored preference.

Every group receives an inspectable admitted or omitted decision. Selection
uses checked arithmetic and explicit limits for group count, entry count,
source references and content. It fails on invalid identities, duplicate or
ambiguous inputs, inconsistent chronology and unsupported representations.
The caller supplies estimates for the actual target model; the selector neither
pretends to tokenize content nor lowers supplied estimates to make it fit.
The admission identifies the estimator/version. External input and tool schemas
are reserved exactly once; memory, summaries and pins are likewise counted once.
`ContextBuilder` remains the final authority on whether the complete request
fits and whether roles, provenance and instruction precedence are valid.

### Durable policy and exact run admission

Enabling/disabling rolling mode, changing pins, and applying/replacing/disabling
summaries are explicit runtime-owned actions recorded as typed append-only
events in the existing session store. A saved job may select that policy for
later top-level turns. Restoration of full-history mode does not erase derived
records or old source events. Persona changes preserve the selected conversation
policy while the existing memory owner rules determine memory eligibility.

A new run records its exact selected source identities, versions/digests,
ordering and budget decisions atomically with its start before provider
dispatch. Continuations use this admitted base plus the active run's complete
exchange; they do not rerun the rolling selector. Recovery reconstructs the
same base from the recorded selection. Missing, changed, unsupported or invalid
admission data blocks continuation and preserves inspection/cancellation.
Changing policy or adding a newer summary only affects a subsequent top-level
run. Pure replay performs no provider call, memory write or tool action.

Recovery resolves the recorded immutable summary versions, not the current
summary selection. A version disabled or superseded before unresolved recovery
is unavailable for continuation; the run stays inspectable/cancellable instead
of substituting another summary. Once sources resolve successfully, their
snapshots remain pinned while active work and usage drain, even after a later
disable or replacement. Memory continues to use its existing exact recovery
rules under ADR 0009; summary actions do not modify memory capture semantics.

The manifest and lineage themselves are bounded. An explicit empty admission
must remain distinguishable from absent legacy admission. Legacy full-history
runs retain their documented reconstruction behavior; no reader guesses that
an unknown selection payload means full history.

### Explicit summary and pre-compaction handoff

The initial summary lifecycle is generate, review/edit, then apply. Generation
is one explicit tool-free inference through ordinary accounting and spend
policy. It has its own durable identity and never submits the composer's
preserved draft or silently applies the result. Before dispatch, a durable
generation intent binds the operation/candidate identity, source selection and
producer. If inference output is durable but candidate publication fails,
recovery derives the same candidate from that output and intent idempotently;
it never generates again to repair this append gap. Indeterminate dispatch
remains an explicit failed/recovery state, not an automatic paid retry.

The request identifies the exact historical range proposed for removal from
active context while that source remains available. It asks the model to
preserve relevant facts, constraints, decisions, task/story state, unfinished
work, uncertainty and source references. That warning and the summary are one
request, not an extra inference or recursive agent workflow. Source ranges are
bounded to fit the summarizing model; an unfit source is an explicit failure.

A candidate records source events, producer/model/inference, creation event and
version. Applying an edited or unchanged candidate checks its exact source and
candidate revision before appending an activation event. Failure, cancellation,
budget refusal, stale review or persistence error preserves the previous usable
policy, original history and draft. Summary content enters requests as derived
untrusted evidence, never as a new instruction layer. Active summaries are
mandatory within the selected policy and must fit; they are not silently cut.
Active summaries are ordered by the earliest covered source sequence and
stable identity, and precede the retained historical groups. Initial active
summary coverage cannot overlap; replacing overlapping coverage is an explicit
atomic supersession action. Covered original groups are omitted from rolling
history unless explicitly pinned. Original source events remain inspectable.

Session summaries and scoped long-term memories are separate. A handoff does
not grant permission to write memory. Any memory capture follows the existing
off/review/auto and immutable-owner rules. Smaller-model summarization,
automatic background generation and automatic activation remain deferred.

### Surfaces and delivery

The Context panel is available through menus, an optional toolbar and equivalent
commands. It exposes capacity, selected and omitted history, pins and summaries,
including generation, review/edit/apply and full-history restoration. One-shot
resume and saved unattended jobs use the same selector and recorded admission.
The running run remains fixed even while the UI displays later policy choices.

Implementation proceeds in reviewable increments: bounded complete-group
selection; durable policy/admission and Chat recovery; accounted summary
lifecycle; TUI/command/noninteractive integration and full-session acceptance.
The selector alone does not resolve #225 or make rolling context available in
production. No new storage engine, tokenizer or generic library is introduced.

## Required failure evidence

- Tiny model capacity rejects full history but admits a new turn after explicit
  rolling selection; mandatory input, pins or summaries that cannot fit fail.
- Arithmetic overflow, invalid/duplicate identities, bad source lineage,
  malformed groups and input resource limits fail before partial output.
- Multiple tool exchanges remain atomic; UTF-8 and large groups do not acquire
  partial boundaries; shrinking model capacity recomputes only future runs.
- Recovery restores exact admitted history and summary versions; policy changes,
  persona changes and newer summaries cannot replace an active run's base.
- Summary failure, stale review, cancellation, exhausted spending and storage
  failure preserve source history and the previously applied policy.
- Replay performs no external work; clearing the transcript is presentation
  only; automated checks do not claim human product acceptance.
