# ADR 0023: Session file browsing and exact ordinary-file evidence

- Status: Accepted
- Date: 2026-09-08
- Owner direction: approved issue #232 solution, after the ten-issue discussion pass

## Context

The owner approved a file browser for repository and ordinary local files,
with session Add folder access, bounded previews, an explicit evidence tray,
and equivalent menus, optional toolbar and commands. Repository evidence
already has ADR0021's exact admission contract. Ordinary files need read
authority and provenance without acquiring repository instructions or model
tool permissions.

## Authority and identity

A user action adds a bounded session read/browse lease for a physical folder.
The lease is separate from repository launch authority and from model tool,
edit, execute or approval grants. Ordinary folders do not discover project
instructions. Any selected AGENTS.md is ordinary untrusted evidence.

Adapter-owned descriptors and absolute paths stay outside public domain types
and durable admission data. Domain source identity records an opaque
physical-root binding, normalized root-relative raw path, content digest/size,
and bounded exact range if a later explicit excerpt operation requires one.
The adapter identity scheme is versioned and valid only within the physical
host/mount identity it can re-establish; device/inode identity is not a
cross-machine portability promise. File metadata detects read races, while
durable file equality uses root/path/content so exact-byte restoration need
not reproduce an old file inode. Initial selected text is exact and bounded;
an oversized source has an explicit unavailable result instead of silent
truncation. A bounded preview may display only a prefix if clearly marked;
selecting evidence must identify the exact admitted bytes separately.

Live lease generation is an asynchronous-result/revocation guard, not durable
source identity. Session switching/reopening expires live leases and transient
previews. Replay creates no permission and performs no filesystem reads. A
restored blocked run can accept a user regrant while active, then revalidate
its recorded physical root, membership and bytes. Current tray choices cannot
replace that evidence. Revocation prevents new reads and inference admission;
already-dispatched usage and results still drain.

## Listing, preview and tray

Listing and preview are separate cancellable operations with
entry/byte/depth/time limits. They run outside the widget thread and return
immutable values tied to session, lease generation, request and selection
revision. Each worker owns its request, lease/port lifetime and result
channel; it must not capture a Chat controller, widgets or borrowed
application state. Application-wide bounded worker capacity includes cancelled
jobs until their filesystem calls finish. Cancellation invalidates visible
work immediately; occupied retired slots reject new work instead of spawning
unbounded replacement threads. Session switch and UI teardown must not
synchronously join stalled filesystem calls. Stale or revoked completion
cannot update widgets, grant access or select evidence. Raw entry identity is
separate from escaped/sanitized display labels; labels are never converted
back into paths.

Directory results expose completion, partial/truncated status and freshness. A
bounded scan does not claim a globally sorted complete directory listing.
Cursor continuation must be bound to the opened directory, lease and filter
generation, invalidate on detected directory changes, charge
skipped/nonmatching entries against scan limits, and reject invalid/stale
cursors without unbounded offset rescans; the implementation may instead
present a clearly partial bounded result with narrower filtering/navigation if
deterministic safe continuation is not available. No recursive full-tree load
is implied.

Preview does not submit, add to the tray or broaden authority. Explicit
Add/Remove changes future evidence selection. Failed/cancelled/stale
operations preserve the composer and previous usable tray. Root/path remain
visible; menus, optional toolbar and commands invoke the same typed controller
operations. Keyboard movement, activation, preview and adding are distinct
actions.

## Context and continuation

Repository selections continue through ADR0021 unchanged. Ordinary files use a
distinct neutral source/admission path and reserved entry namespace. They
become Role::evidence inputs through the existing optional attachment budget
class. Required instructions/input, memory policy, pinned conversation and
active summaries retain current priority; budgets never silently truncate
mandatory content.

Repository parcels and ordinary local candidates participate in the same final
optional select_and_build pass after prepare_session_context. Do not inject
local content into the mandatory preparer or run a second selection pass that
changes priority. Finalize references, order, estimates, inclusion/omission
and the seal only after the actual final context selection, updating
conversation accounting once. Context inspection and summary activation
previews use the same preparation/finalization as submit. Kernel dispatch
verifies local admission and actual request entries in both directions.
Missing, malformed, unsupported or mismatched local proof blocks new work.

Local admission is an inference-linked versioned event committed atomically
with inference start. Preserve the existing adjacent
repository-admission/inference pair by placing any local admission before it;
validate the grammar LocalAdmission -> [RepositoryAdmission] ->
InferenceStarted with the same run/inference ID. Reject duplicate, orphaned,
reordered and unsupported future local admissions. Source recovery gates
direct question/approval decisions as well as ordinary continuation. Do not
alter existing repository v1 or conversation v1/v2 seal bytes. Pure replay
remains event-only; pending continuation reconstructs saved membership and
decisions, then requires explicit live authority and exact source restoration.

## Verification boundary

Failure-first deterministic fakes cover invalid identity, bounds, source
drift, lease/session revocation, stale completion, malformed/missing
admissions, atomic persistence failure, mixed evidence budget pressure and
recovery with an unrelated current tray. Adapter tests add traversal,
symlink/root substitution,
unreadable/disappearing/nonregular/binary/invalid-UTF8 sources and bounded
partial directories. Actual widget tests cover mouse/keyboard,
empty/filter/resize/focus/cancel/reopen/remove/session-switch behavior without
provider calls. Exact GCC/Clang, formatting, configured tidy, independent
review and terminal PR/postmerge CI remain delivery gates.

Ordinary regular-file filesystem calls can stall despite O_NONBLOCK.
Cancellation checks between calls do not prove an interruptible in-flight
kernel read or join. The adapter/worker design must state and test its
supported cancellation/cleanup boundary; do not claim a hard filesystem-stall
guarantee from thread stop tokens alone.

## Delivery

Implement the neutral source/port contract and failure matrix first, then the
bounded filesystem/worker boundary, exact context admission/recovery, and the
shared browser TUI/commands. A foundational port alone does not make file
browsing available and does not close #232. Preserve the existing storage
engine, event encoding, parser, runtime and dependency pins; no new generic
library is introduced.
