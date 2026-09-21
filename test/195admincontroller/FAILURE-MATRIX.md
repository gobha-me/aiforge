# Admin controller failure matrix

Written before implementation. Validate missing/borrowed catalog/worker, unsafe
or duplicate metadata and limits, invalid owner/session identity and counter
exhaustion before source work. Metadata inspection and view-only actions do no IO.
First unbound manual session remains eligible for native bind despite available
false; closed/busy/fatal sessions cannot collect. Test actual shared worker with
unrelated stalled work, producer-ready unclaimed result, claimed physical
retirement, source errors, expiry, superseded results and exact cancellation.
PendingB never replaces activeA until native binding succeeds. Ordinary refusal
preservesA; successful same-target or A-to-B selection preserves truthful
freshness for retained evidence; fatal errors latch. Test old
session/target/generation/event/index row callbacks versus explicit name-only
reads; no implicit retry or log grant.
Cached evidence comes only from exact committed current success, deduplicated by
session and event, bounded to eight operation snapshots, and retains original
target on failure. Kubernetes selection grants only workloads, Pod health and
events. Cached Pod callbacks bind session, inventory event, selection generation,
row, namespace, name and UID; stale/replaced/non-Pod rows fail before submission.
Namespace events carry no resource, while exact-Pod events reuse the same bound
identity. Every displayed refresh binds its session, full target, generation,
event, operation and resource scope to both the retained snapshot and current
selection; selecting B makes every A refresh stale before submission. Pending,
failed and disconnected freshness never erase or relabel the last successful
observation; captured time remains the age source. Only the
manual session's typed source-disconnection state marks retained evidence
disconnected; storage, history, internal and closed-session failures do not.
Session detach clears borrowed pointers before owner destruction, preserving
logical cancellation with assertion-safe owning source gates. Detach preserves
last-success, refresh-failed and actual source-disconnected evidence states;
only an in-flight refreshing slot returns to its retained-evidence or unavailable
state. Controller fakes do not prove actual Chat/SQLite policy or GUI behavior:
those integration tests are required before full feature completion.
On durable reopen, the entire eight-slot historical catalog is prevalidated and
copied transactionally, absent slots replace prior-session state, and each
present slot is visibly unverified. Evidence captured from A remains A while a
later historical selection identifies B. Replay never restores active authority
or performs collection. Legacy histories with observation generations but no
selection event seed the next generation without inventing a target; exhausted
generation refuses before source preparation.

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

Log consent starts exactly once per broker activation with the target kind's
log operation present but disabled. Enable, disable and read actions require a
committed displayed service invocation or Pod/container runtime proof bound to
the current session, full target, selection and evidence event. Policy changes
advance both counters and rebind only the retained source. Disabling invalidates
in-flight publication before cancellation. Requesting another target during an
in-flight log read revokes the enabled source before cancellation or new source
preparation; completed target replacement starts disabled from the consent
owner's current counters. Any mutated-consent bind failure is fatal, unbound and
explicitly revoked; detach and session replacement revoke it. Service and Pod
logs occupy separate appended slots without shifting the six existing
observation slots.
Disable-side cancellation refusal or exception revokes the already-mutated
authority before failing closed, without attempting a bind. Disable-side bind
refusal or exception likewise revokes the authority, clears live controller
state and preserves committed evidence.
Storage failure, invalid history, resource exhaustion and internal failure also
revoke any enabled activation and release current source/binding state. The old
authority fails broker preflight and cannot publish late log text, while all
previously committed snapshots retain their original provenance.
