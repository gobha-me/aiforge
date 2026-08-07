# ADR 0001: Run vocabulary and event model

- Status: Accepted
- Date: 2026-08-07

## Context

AIForge has several independent product axes and will eventually support more
than a blocking chat request. A session can contain concurrent or suspended
work, one user action can require several model calls, and tool results or
questions must survive replay. Treating a transcript, task list, or TUI widget
as durable truth would make those views disagree as soon as the runtime grows.

The architecture north star therefore requires one append-only stream of typed
events and a provider seam that does not leak Venice types into the domain. This
ADR fixes the vocabulary and initial event contract before those words acquire
different meanings in adapters and surfaces.

## Decision

### Vocabulary

- A **session** is the durable container for an ordered event stream, its runs,
  configuration provenance, and artifact references. A session is not a model
  request and does not imply that only one run is active.
- A **run** is one user-initiated unit of work from `run.started` to exactly one
  terminal event. It may contain several inference attempts, tools, questions,
  and child runs.
- An **inference** is one provider-neutral backend request within a run. A run
  can perform another inference after a tool result or answered question.
- A **message** is role-attributed content used by context construction. A
  transcript turn is a projection and is not a durable source of truth.
- A **content block** is a typed piece of message or result content. Large or
  binary content travels by artifact reference rather than being copied through
  each event.
- An **invocation** is one tool execution, including its proposal, policy,
  approval, progress, and result lifecycle.
- An **artifact** is addressable content with identity, media type, size,
  digest, and provenance. Display placement is a view event, not artifact
  ownership.
- A **projection** is state rebuilt from events, such as a transcript, run
  status, tool panel, usage ledger, or audit view. It may be discarded and
  rebuilt without changing durable history.
- A **surface**, **workspace**, **persona**, and **permission profile** are
  independent run axes. None implies another, and persona never grants
  authority.

### Event envelope and ordering

Every persisted event has:

- a stable, session-unique event ID;
- the run ID it belongs to;
- a one-based sequence that is strictly increasing across the session stream;
- a positive schema version;
- a UTC timestamp at millisecond precision;
- optional causation event, parent run, and invocation IDs when relevant; and
- one typed payload.

Sequence, not timestamp or file order, resolves replay order. A projection for
one run accepts sequence gaps because events for other runs may be interleaved,
but rejects duplicate or regressing sequence values. The session event log
requires each appended sequence to be greater than its predecessor and each
event ID to be unique.

Events describe facts in the past tense. A correction is another event that
references or supersedes earlier evidence; persisted events are never edited in
place. The first in-memory model uses schema version 1. Storage encoding and
schema migration are separate decisions.

An older reader represents an unrecognized payload as `UnknownEvent`, retains
its envelope, advances ordering, and skips its effect on known projections. It
must not crash or reinterpret the event as a known type.

### Typed event families

The domain must represent these families even when their complete projections
or executors arrive later:

| Family | Events |
| --- | --- |
| Run | started, awaiting input, resumed, completion requested, completed, failed, cancel requested, cancelled |
| Content | user content added, assistant content started, delta added, assistant content finished |
| Model | inference started, reasoning metadata added, usage recorded, finished, failed, cancelled |
| Tool | proposed, policy decided, approval requested/decided, started, progressed, result recorded, errored |
| Question | requested, answered, cancelled |
| Artifact/view | created, referenced, displayed, removed from view |
| Multi-run | child run created, inter-run message sent |

Payloads carry the stable IDs that connect their lifecycle. Tool arguments,
results, question answers, and extension values use provider-neutral structured
content. Effects and capability scopes are explicit fields of policy records;
approval and ordinary user questions remain distinct.

### Provider seam

A backend receives a provider-neutral request containing an inference ID, model
ID, messages/content blocks, tool declarations, generation options, and ordered
namespaced extension values. Credentials are adapter configuration and are not
part of this request.

Starting a backend returns a pull-based stream. Calling `next(stop_token)`
yields one typed event, clean end-of-stream, or a redacted typed error. Stream
events cover response start, content and reasoning deltas, structured tool-call
deltas, citations, usage, finish, and cancellation. A response must produce one
terminal finish or cancellation before clean end-of-stream; enforcement belongs
to the run kernel.

The pull boundary is deliberately synchronous. A later worker can own the
blocking cursor without changing the backend API, while deterministic tests can
advance it without threads, timing, or callbacks.

### Safety and data handling

IDs are opaque values, not provider objects. Core errors contain a stable
category, a redacted message, and retryability metadata. API keys,
authorization headers, environment contents, and unredacted provider payloads
are forbidden from backend requests, run events, artifacts, captured fake
requests, and renderable error strings.

Inline content is subject to runtime size budgets. Large or binary values must
become artifacts; selecting exact thresholds is deferred until the artifact
store milestone.

## Consequences

- TermForge and venice-cpp adapters depend inward on stable domain contracts.
- Replay can rebuild projections without rerunning inference or side effects.
- The initial reducer implements run, inference, content, usage, error, and
  cancellation lifecycle only; the remaining typed event families are
  representable before their dedicated state machines exist.
- Serialization format, database choice, context policy, parser, UI threading,
  production provider mapping, tool execution, and persistence migration remain
  intentionally undecided.
