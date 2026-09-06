# ADR 0017: Provider-neutral durable video lifecycle

- Status: Accepted
- Date: 2026-09-04

## Context

Bounded MP4 validation and content-addressed publication exist, but asynchronous
video generation adds a paid quote, a remotely queued job, repeated status
retrieval, media retrieval, and explicit remote cleanup. A process can stop
between any remote effect and its durable observation. A presigned media URL is
a secret capability and cannot be made durable merely to simplify recovery.
URL transcription has the same secrecy requirement and is an explicit caller
action, not ambient authority.

This decision establishes the neutral lifecycle before a Venice adapter,
command, presentation, catalogue integration, spend settlement, or
model-callable tool exists.

## Decision

AIForge defines `backend::VideoService` with bounded typed operation and job
identities. Its operations quote one text-to-video specification, queue it,
retrieve either typed status or owned media, explicitly clean up a completed
remote job, and explicitly transcribe a caller-supplied transient URL. A quote
is exactly one `MonetaryAmount`; it is not actual spend and is not a
`ReportedCost`.

The initial generation specification is closed to a model identity, bounded
UTF-8 text prompt, and a positive duration of at most one hour. It has no URL,
upload, image, audio, document, keyframe, or provider-extension field. Model
catalogue constraints and provider mapping remain later work.

`queue(operation_id, spec)` has hard semantic idempotency. Repeating an equal
operation returns the same job. Reusing its operation identity with a different
spec fails closed. This is the recovery mechanism for the otherwise ambiguous
crash window after the provider accepted a job but before `video.job_queued`
was committed. A pinned provider contract must positively guarantee these
semantics and that replay does not create or charge for another job. Generic
idempotency-header support, empirical success, and scripted fakes are not proof.
An adapter without that guarantee must reject queueing before the provider
effect.

A dedicated `VideoCoordinator` owns orchestration beside `RunKernel`. Each
workflow is one normal run beginning with `RunStarted` and ending with exactly
one of `RunCompleted`, `RunFailed`, or `RunCancelled`. Creation atomically
records the run start and either `video.generation_requested` or
`video.transcription_requested` before any provider effect. A transcription
request records only operation and model identity; its URL must be explicitly
resupplied by the caller on recovery. That rule applies only to an explicitly
caller-supplied transcription URL. A provider-generated generation capability
is never caller-resupplied or persisted.

The coordinator has three explicit entry modes: create a durable session and
its first run, start another run in an existing durable session, or resume one
exact existing run. A session is never inferred to contain only one video run.
Every resume target is the pair `(SessionId, RunId)`; no "latest run" or
single-row shortcut may select authority or recovery state.

Generation follows this exact durable order:

```text
RunStarted
  -> VideoGenerationRequested
  -> VideoQuoteObserved
  -> VideoJobQueued
  -> VideoJobStatusObserved(queued|processing)*
  -> VideoJobStatusObserved(completed)
  -> ArtifactCreated + VideoArtifactPublished + VideoCleanupPending(1)
  -> [VideoCleanupFailed(n) -> VideoCleanupPending(n+1)]*
  -> VideoCleanupCompleted(n) + RunCompleted
```

A failed job status is followed atomically by `RunFailed`. Before publication,
provider, protocol, media, artifact, deadline, poll-limit, or cancellation
failures produce an ordinary terminal envelope. Poll numbers are positive and
consecutive, so repeated provider states are observable while duplicated,
stale, regressing, and out-of-order observations fail closed. Poll count and
the run-start timestamp make recovery preserve the maximum-poll and total-time
bounds.

Media is legal only after a durable completed status. The coordinator requires
the exact `video/mp4` media type and passes owned bytes through the accepted
bounded MP4 publication path. Generic and video-specific publication facts,
plus the first cleanup-pending fact, are one append. No cleanup call occurs
before that append succeeds.

Every service call carries the caller's cancellation token, one positive
remaining-time ceiling derived from a single clock sample, and a positive
response-byte ceiling that the transport enforces before buffering. Retrieval
uses the MP4 byte ceiling. Transcription uses the checked aggregate of the
bounded text, language, and neutral envelope allowance; domain validation still
checks the decoded fields exactly.

Cleanup is attempted at most once per coordinator invocation. A false result,
error, cancellation, or deadline leaves the already-published artifact as
durable success and appends a fixed, retryable cleanup failure when a call was
made. Recovery appends the next numbered pending attempt before retrying.
Successful cleanup and run completion are one append. A completed or published
run never repeats quote, queue, retrieval, publication, transcription, or
completed cleanup.

URL transcription follows this exact durable order:

```text
RunStarted
  -> VideoTranscriptionRequested
  -> VideoTranscriptionObserved
  -> RunCompleted
```

The caller-supplied transient capability must be an absolute `https://` URL
with a nonempty authority and no userinfo or control bytes. This is bounded
input validation, not selection of a foreign-URL client: AIForge does not
dereference it in this slice.

Transcription output is nonempty bounded UTF-8 text with an optional bounded
UTF-8 language tag. A presigned or transcription URL never enters events,
SQLite, transcript content, ordinary errors, provenance, or scripted-fake call
captures. Output containing the supplied URL is rejected before append.
Provider diagnostics are replaced with fixed neutral messages.

All lifecycle events have canonical SQLite codecs. The dedicated projection
rebuild API takes the complete `SessionEventLog` and one explicit target
`RunId`. It first validates positive envelope fields, unique event identities,
and strictly increasing sequence across the whole session, including rows for
other interleaved runs. Only after those session invariants hold does it apply
the exact video state machine to events whose run identity equals the target;
sequence gaps caused by other runs are legal. Corruption in another run is not
hidden by filtering, while valid unrelated known and opaque future events do
not alter the target projection. An absent target fails closed.

After a known cancellation request only opaque future rows and the matching
cancellation terminal are legal; after a failed job status only opaque future
rows and the failure terminal are legal. Pure replay reads events and rebuilds
projections only; it performs no provider, network, artifact write, cleanup, or
transcription action.

## Consequences

- A queue adapter must implement or prove operation-ID semantic idempotency;
  an adapter that cannot do so is not conforming.
- Content-addressed artifact publication and cleanup are intentionally
  independently retryable across their narrow crash windows. Durable event
  publication is never duplicated.
- Cleanup failure does not erase or terminally fail a published artifact. The
  run remains live and visibly retryable until cleanup completes.
- Provider-generated generation capabilities are never persisted. A conforming
  adapter must reacquire one from durable operation or job identity without
  creating another job; if the provider cannot guarantee that recovery, the
  adapter must reject queueing before the provider effect. Only explicitly
  caller-supplied transcription URLs may be resupplied after a crash.
- Interleaved video runs share session ordering and storage transactions but do
  not share operation, job, polling, cleanup, URL, or completion state.
- There is still no Venice adapter, arbitrary downloader, private upload,
  reference generation, command, renderer, player, spend settlement,
  permission-profile change, or model-callable paid tool.
