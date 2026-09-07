# ADR 0021: Exact Dev repository context and continuation

- Status: Accepted
- Date: 2026-09-07
- Owner decision: issue #227, continuation after unrelated repository changes
  requires exact revalidation of selected evidence and project instructions.

## Context

Dev sessions need an explicit repository target, applicable project
instructions, and inspectable selected source evidence. Treating the entire
repository fingerprint as an immutable run input would interrupt normal tool
work after an unrelated file changed. Refreshing all context implicitly would
instead substitute instructions or evidence into an existing run.

## Decision

One repository root is bound at application launch. Dev target selection is a
normalized relative directory within that root. Target and evidence selection
changes are idle-only and do not alter the persona, selected tool profile,
launch capability ceiling, or approval policy. Changing repository roots
requires a new launch.

Project instructions retain ADR 0002's root-to-target ordering and ADR 0012's
authority precedence. Evidence selection admits bounded tracked regular UTF-8
files through the exact repository read boundary. Selecting an `AGENTS.md` as
evidence does not grant it instruction authority. Missing optional instruction
documents constitute an explicit empty or partial chain; unreadable, malformed,
unstable, or unsupported sources fail preparation. Present but empty instruction
documents fail closed; empty selected files are reported as unavailable evidence.

Context construction charges all mandatory instructions and conversation before
selecting saved memory and optional repository evidence. Instructions are never
silently truncated or omitted. Evidence inclusion and omission are inspectable.
The existing conservative byte-based estimates remain explicit estimates for
the selected model; no tokenizer or parser dependency is introduced.

Every Dev inference has a bounded, sealed, references-only repository context
admission committed atomically before provider dispatch. It records the exact
repository snapshot, target, instruction-chain identities and order, selected
source identities and digests, estimates, and inclusion decisions. Source text
and absolute filesystem paths do not enter the admission. The kernel verifies
the constructed request against the admission in both directions.

Before a continuation, preparation re-establishes the same launch root and
repository identity, target, applicable instruction-chain membership and bytes,
and selected evidence identities and bytes. If these are unchanged, unrelated
repository changes are allowed: a successor admission records the newly
observed snapshot before that inference. A changed, added, or removed applicable
instruction, or changed selected source, blocks continuation. The original
input is never silently replaced.

This project-source revalidation does not change ADR 0012's user-global
instruction pinning or ADR 0009's exact saved-memory recovery. An authorized
tool's completed results and usage must still drain when repository validation
fails; source drift blocks new inference or new approval authority, not durable
accounting for work already dispatched.

Pending-run recovery reconstructs the recorded selection rather than the
current UI selection. Missing or changed sources leave the run inspectable and
cancellable. Restoring the exact sources permits revalidation; a new run can
load current sources. Pure replay uses durable admissions and events only and
performs no repository reads, provider requests, or tool execution.

Repository preparation runs on a cancellable worker for interactive use.
Workers receive immutable requests and return immutable results; they do not
access widgets or mutate Chat state. Owner-thread commit checks the captured
session and selection revision. Failed or stale preparation preserves the old
selection and draft. Cancellation and resize remain responsive.

## Consequences

- File browsing in #232 can reuse the same preparation and selection boundary.
- Source edits in #228 need an invocation-linked validated edit receipt to
  advance an intentionally changed source. This decision does not permit a
  generic refresh or automatic acceptance of arbitrary changed inputs.
- Completed historical runs remain inspectable without their source files.
  An unprovable pending Dev admission cannot authorize continued work.
- No storage engine, encoding, parser, scripting runtime, plugin ABI, or
  reusable library is selected by this integration.
