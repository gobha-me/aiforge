# Admin controller failure matrix

Written before implementation. Validate missing/borrowed catalog/worker, unsafe
or duplicate metadata and limits, invalid owner/session identity and counter
exhaustion before source work. Metadata inspection and view-only actions do no IO.
First unbound manual session remains eligible for native bind despite available
false; closed/busy/fatal sessions cannot collect. Test actual shared worker with
unrelated stalled work, producer-ready unclaimed result, claimed physical
retirement, source errors, expiry, superseded results and exact cancellation.
PendingB never replaces activeA until native binding succeeds. Ordinary refusal
preservesA; fatal errors latch. Test old session/target/generation/event/index row
callbacks versus explicit name-only reads; no implicit retry or log grant.
Cached evidence comes only from exact committed current success, deduplicated by
session and event, bounded to3 snapshots, and retains original target on failure.
Session detach clears borrowed pointers before owner destruction, preserving
logical cancellation with assertion-safe owning source gates. Controller fakes do
not prove actual Chat/SQLite policy or GUI behavior: those integration tests are
required before full feature completion.

Specific evidence boundaries: the actual shared-worker request counter is driven
to UINT64_MAX through normal admission, then controller selection must refuse
before factory creation. Session-epoch and selection-generation overflow have
explicit source guards; their public API has no artificial counter seed and no
runtime maximum-value claim is made. Snapshot replacement builds a complete local
copy before a statically checked nothrow move; allocation failure is not injected
in this fixture. The actual delayed physical retirement predicate is exercised in
194;195 independently checks producer-owned readiness while busy and a distinct
claimed/retiring phase before binding. Fakes cannot prove durable Chat/SQLite
publication, policy enforcement or GUI behavior.
