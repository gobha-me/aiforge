# ADR 0016: Durable paid-tool spend reservations

- Status: Accepted
- Date: 2026-09-04

## Context

AIForge already records provider-reported inference cost and can impose a
session USD ceiling. A paid tool adds a different race: approval may succeed,
the process may stop after provider transport begins, and another invocation
may otherwise spend the same apparent remainder. Provider APIs also do not
always return a charge correlated to one invocation. A catalogue calculation
or configured ceiling must not be presented as actual cost.

## Decision

Every paid tool records a nonzero USD maximum before transport. The reservation
is an append-only `ToolSpendReserved` event tied to the exact invocation and to
a SHA-256 digest of the catalogue or policy evidence that established the
maximum. The validated quote and reservation carry the same exclusive
`valid_until` instant; proposal and launch reject the quote at that boundary.
It counts at its full value until exactly one terminal spend event is durable.

A reservation ends as one of:

- `ToolSpendReleased`, only when the runtime proves transport did not start;
- `ToolSpendFinalized`, with an amount no greater than the reservation and an
  explicit basis of provider-reported, catalogue estimate, or policy upper
  bound; or
- `ToolSpendReconciliationRequired` when transport may have started but a safe
  final amount is unavailable or contradictory.

Provider-reported finalization requires a digest of correlation evidence.
Catalogue estimates and policy upper bounds retain those labels in durable
state and presentation. Reconciliation retains the entire maximum against the
ceiling. Terminal facts never rewrite the reservation.

Amounts use the existing checked decimal contract, the exact `USD` unit, and at
most six fractional digits (integral microunits). Reservations are positive;
finalized amounts may be zero. IDs, digests, states, and reason codes are
bounded neutral values. Credentials, provider payloads, request URLs, and raw
diagnostics are forbidden.

The run kernel will append the reservation before `ToolStarted` and before
calling a paid executor. SQLite's existing bounded `BEGIN IMMEDIATE` append and
session-sequence check is the concurrency gate: two stale writers cannot both
commit the same next reservation. A conflict is replayed and recomputed by the
caller; it is never treated as approval to spend. Tool integration must record
one terminal spend fact before its ordinary terminal result. If the process
cannot prove pre-transport failure, recovery records reconciliation-required,
not release.

The spend projection is rebuilt only from events. Unknown future events are
skipped. Replay computes state and performs no provider, tool, or billing work.
In particular, replay does not call a tool executor's validation method: the
schema-v2 proposal's normalized arguments, effects, scopes, and exact unexpired
quote are the durable approval offer. A resumed launch rechecks its exclusive
expiry against the current time before reserving it.
If replay finds an interrupted reservation, it atomically appends a release
when `ToolStarted` was never durable, or reconciliation-required when it was,
then terminates the orphaned tool and run with fixed sanitized errors.

## Consequences

- Session availability includes inference accounting plus finalized paid-tool
  amounts and unresolved maxima.
- Exact provider cost is optional evidence, not an inferred claim based on a
  balance delta or catalogue price.
- A crash can conservatively reduce remaining budget until explicit
  reconciliation; it cannot silently restore possibly spent authority.
- Provider-specific reservation/finalization wiring remains in each focused
  paid-tool child, beginning with issue #170.
