# ADR 0008: Plan-task control and project-backlog facts

- Status: Accepted
- Date: 2026-08-27

## Context

AIForge already persists exact plan revisions, decisions, materialized session
tasks, and child-run results. It needs interactive and headless controls for
reviewing that state, and it needs a way to retain unfinished work when a
session closes. A mutable task table or a second project database would compete
with the append-only session stream and make replay, correction, and automation
semantics disagree.

The architecture north star also identifies TUI and JSONL as peer surfaces over
one run kernel. The headless contract therefore cannot be terminal output
parsing, and the TUI cannot own plan decisions or backlog state.

## Decision

### Runtime-owned control

`PlanTaskController` is the surface-neutral boundary for inspecting a session's
current plan, pending exact-revision decision, schedule, active session tasks,
and an optional repository backlog. It delegates mutations to `RunKernel`.
Public failures are typed `std::expected` values; adapters contain parser,
terminal, filesystem, Git, SQLite, and JSON details.

Approval, revision request, and rejection continue to append exact-revision
decision facts. A revision request from a control surface requires a bounded,
nonempty reason. Approval re-observes any bound repository snapshot immediately
before the kernel records it. A surface that cannot re-establish bound evidence
must reject approval instead of treating missing evidence as current.

### Two task scopes

A session task remains part of one accepted plan revision. It retains its plan,
revision, task identity, dependencies, acceptance criteria, intended effects,
resource intents, and attempt history.

An unresolved session task may be promoted to project scope by appending
`ProjectBacklogItemPromoted` to its source session. The fact contains a stable
item ID, exact source session/plan/revision/task identity, repository identity,
an exact copy of the task, and whether the user or policy made the choice. Only
the current accepted, materialized, unresolved task can be promoted, and an
origin can be promoted once.

Project backlog state is a projection aggregated from matching facts in session
streams for one repository identity. There is no separate task store. Promotion
starts an item as open. Reopening or resolving appends
`ProjectBacklogItemStatusChanged` to the same source session. Status changes use
the prior status event ID as a compare-and-set precondition, must change state,
and resolution requires a bounded reason. Stale, cross-repository,
cross-session, duplicate, and malformed facts fail before persistence.

SQLite storage format version 2 adds expression indexes for repository identity
on the two fact types. The indexes select candidate source sessions; complete
ordered session events are still decoded and replayed through the ordinary
event contract. Migration from version 1 is transactional.

### Surfaces

Interactive Chat reviews each newly pending plan revision once through a
bounded choice dialog. Approve, Revise, and Reject map to the same controller;
revision requests collect a reason. `/plan` displays the exact revision and
derived schedule. `/tasks` separates active session work, repository backlog,
and completed session history.

Closing or switching a session with unresolved, unpromoted tasks opens one
bounded workflow: select tasks, then promote them, leave them session-local, or
cancel closing. Promotion is unavailable when the current Git repository
identity cannot be established. This prompt does not imply task execution or
grant capabilities.

`aiforge plan --jsonl` is the versioned headless surface. It requires exactly
one of `--resume <session-id>` or `--continue`, rejects terminal stdin, and
accepts strict UTF-8 JSON objects one per line. Every input line produces one
JSON response line on stdout. The schema-version-1 operations are `inspect`,
`decide`, `promote`, and `set_backlog_status`. Request IDs are caller supplied,
bounded, and unique within the stream. Mutation requests also supply stable run
and item IDs and exact revision/status preconditions. Unknown fields, duplicate
JSON keys, invalid envelopes, stale state, and resource-limit violations fail
closed without partial events. Diagnostics remain on stderr.

## Consequences

- TUI and automation observe and mutate the same replayable domain state.
- Repository backlog queries cost an indexed candidate lookup plus bounded full
  session replay; no mutable cross-session projection becomes authoritative.
- Closing a session can preserve unresolved intent without silently promoting
  or discarding it.
- Project identity follows the existing repository snapshot contract; work from
  distinct repository identities is never merged by path or display name.
- A future remote controller can implement the same neutral controller without
  inheriting TermForge or the process JSONL adapter.
