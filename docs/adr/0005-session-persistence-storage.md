# ADR 0005: Session persistence encoding and storage

- Status: Accepted
- Date: 2026-08-18

## Context

AIForge needs to persist append-only session events and rebuild projections
without rerunning inference or tools. The store must preserve event identity and
ordering, survive an interrupted append, serialize concurrent writers, retain
unknown future payloads, and report corruption or unsupported versions through
typed errors. Session discovery and resume also need an efficient catalog that
does not become a second mutable transcript.

The architecture north star deliberately deferred the storage engine, file
layout, and persisted encoding until this milestone. ADR 0001 fixes the event
envelope and replay rules but does not choose how they are stored. ADR 0004's
JSON decision applies only to the small user configuration file and therefore
does not settle session persistence.

The candidates considered were a SQLite database with JSON payloads, an
append-only newline or framed JSON log per session, and one atomically published
file per event. The file designs avoid a database dependency, but require
AIForge to invent and maintain crash-tail recovery, indexes, schema migration,
session discovery, and cross-process writer coordination. A file per streaming
event also creates unbounded directory pressure.

## Decision

### Storage boundary and location

Use SQLite 3 as the session storage engine. SQLite and JSON-library types remain
inside an AIForge adapter behind a provider-neutral storage port whose fallible
operations return `std::expected`. The implementation will prefer an installed
SQLite package and otherwise use a deliberately pinned official amalgamation;
the pin and compatibility reason belong to the dependency recipe.

Use one database at
`$XDG_STATE_HOME/aiforge/sessions.sqlite3`, falling back to
`$HOME/.local/state/aiforge/sessions.sqlite3`. This is application state, not
configuration. The AIForge state directory and database target cannot be
symlinks. New directories use mode 0700, and the database and SQLite sidecar
files use mode 0600. Existing insecure or non-regular paths are rejected before
read-write access. Initial database creation includes syncing the containing
directory.

Ephemeral sessions do not create or open the durable store. Artifact bytes use
their owning artifact store when that milestone arrives; session storage holds
only typed artifact events and references.

### Logical representation

The database contains a session catalog and an append-only event table. A
session catalog row records the stable session ID, creation time, and storage
format version. It is not a transcript or mutable run projection.

Each event row stores the ADR 0001 envelope as independently validated columns:
session ID, positive sequence, event ID, run ID, positive event schema version,
UTC millisecond timestamp, optional causation event, parent run, and invocation
IDs, plus a stable payload type name and payload document. The primary key is
`(session_id, sequence)`, and `(session_id, event_id)` is unique. Sequence gaps
are valid, but every append must be greater than the session's current maximum.
SQLite's positive signed 64-bit range is the persisted sequence limit; larger
domain values fail before mutation.

Typed payloads use strict UTF-8 JSON encoded through the existing private
nlohmann/json boundary. Writers produce one canonical representation. Readers
reject duplicate keys, invalid Unicode, malformed values, and values outside
the declared type or resource budgets. Known payloads are decoded into neutral
domain types. An unrecognized payload type remains an `UnknownEvent` carrying
its type name and opaque structured payload so an older reader can retain and
re-emit it without guessing its semantics. JSON objects and library exceptions
do not cross the adapter API.

Configuration provenance, backend/model identity, credential-source
references, tool/runtime versions, policy and usage facts, run relationships,
and artifact references enter storage only through their neutral typed records.
Raw credentials, authorization headers, environment contents, unredacted
provider payloads, and secret-bearing diagnostic text are forbidden.

### Transactions, replay, and migration

Use rollback-journal mode with foreign keys enabled and full synchronous
durability. An append of one event or an explicitly ordered batch runs under a
bounded `BEGIN IMMEDIATE` transaction. The adapter checks the current sequence,
validates every envelope and payload, inserts the complete batch, and commits
once. A constraint, serialization, I/O, sync, disk-full, or cancellation failure
rolls back the whole append and leaves all earlier history readable. Busy or
locked databases produce a typed retryable contention error after a bounded
wait; writers never use last-writer-wins replacement.

Replay uses a consistent read transaction ordered by session sequence, applies
the same envelope and payload validation, and advances past unknown events
without applying their effects to known projections. Opening, listing, or
replaying a session performs no inference, tool call, approval, artifact fetch,
or other side effect. Missing referenced artifacts remain explicit projection
state rather than causing replay to recreate them.

The database schema uses SQLite's `user_version` as its storage-format version.
Supported forward migrations run transactionally before any ordinary write. A
failed migration rolls back, an unsupported newer version is opened no further
and returns a typed error, and downgrade rewriting is not attempted. Event
schema versions remain independent of the database format so unknown event
payloads do not require a storage migration.

## Spike evidence

An isolated SQLite 3.45.1 probe created the proposed catalog and event
constraints, round-tripped an opaque nested future JSON payload, demonstrated
that a duplicate event ID rolls back the complete transaction, and confirmed
that a concurrent `BEGIN IMMEDIATE` writer receives bounded lock contention.

## Consequences

- SQLite owns transaction, recovery, indexing, and writer serialization rather
  than AIForge maintaining a custom append-log format.
- The adapter must explicitly map SQLite and JSON failures to bounded neutral
  errors and must failure-test permissions, corruption, resource exhaustion,
  contention, and migration.
- Session catalog queries and ordered replay do not require scanning many files,
  while projections remain disposable and rebuildable.
- The implementation milestone still owns the storage port, serializers,
  migrations, session commands, resume behavior, and deterministic failure
  fakes. This decision does not select artifact storage, repository caches, or a
  general application database.
