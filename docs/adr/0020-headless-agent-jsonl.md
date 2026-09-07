# ADR 0020: Bounded noninteractive agent JSONL

- Status: Accepted
- Date: 2026-09-07

## Contract

`aiforge agent --jsonl` consumes one UTF-8 JSON object followed by EOF. Optional
`--repository PATH`, `--tool-restriction LEVEL`, and `--tool-approval MODE`
select application-lifetime launch policy using the same production assembly as
interactive Chat. Configuration remains the authority for process executables,
roots, environment, network contract, and automatic approval rules. Protocol
input cannot define or expand those grants. There is no terminal fallback.

Protocol version 1 accepts either:

```json
{"version":1,"operation":"submit","profile":"dev","tools":["read_repository_file","run_process"],"prompt":"Read the source and run the configured tests"}
{"version":1,"operation":"replay","session_id":"session-id"}
```

Submit may additionally select `model` and `session_id`. A session ID selects
completed history for a new run; any unresolved recovered run is refused before
draining or dispatching effects. Replay reads only durable events and never
opens a backend or repeats a tool. There is no request-ID retry/idempotency
promise. A caller must inspect durable identities before deciding to retry.

Only repository-read and configured bounded argv tools can be requested. An
explicit nonempty subset and profile are required. Missing declarations,
profile/model ceilings, or unavailable policy fail before inference; they are
not guessed or silently dropped. Questions, prompt approvals, shell and paid
media have no response protocol or callable registration. Unexpected interaction
ends the invocation with `interaction_required` after cancellation. Automatic
approval is bounded by the existing positive rules and counters, not inferred
from executable allowlisting alone.

Input is limited to 1 MiB, one nonempty JSON line, nesting depth 8, and two tool
names. Unknown/duplicate keys, unsupported operations/versions, extra records,
invalid UTF-8, malformed or truncated JSON, and control-bearing identity fields
are errors. A trailing LF or CRLF is allowed. EOF collection is cancellable.

Output is versioned JSON Lines with `type` equal to `accepted`, `event`, `error`,
or `terminal`. Accepted records identify the durable session/run. Event records
include session/run/event IDs and sequence; the payload is a documented bounded
projection, not the SQLite encoding. Required content, invocation, policy,
artifact, usage and cost fields are represented; other event kinds remain
identity-only state records. Credentials, raw configuration and opaque provider
metadata are not emitted. JSON records are limited to 2 MiB each; encoding or
output refusal cancels active work rather than truncating a record.

Exactly one terminal record is attempted when the sink remains usable. Terminal
status is `completed`, `failed`, `cancelled`, `interaction_required`, or
`recovery_required`, with a `durable_terminal` flag. Persistence failure never
claims a durable cancellation; recovery requires reopening the session. Tool
failures and already-captured usage/spend remain durable and are drained before
terminal reporting. Exit codes are 0 for completed/replayed, 2 for malformed or
unsupported input, 130 for cancellation, and 1 for runtime/interaction/recovery
or output failure. Diagnostics use stderr only. Terminals carry the session ID,
run ID when known, and a reason. Replay `completed` describes the replay
operation only, with `durable_terminal: false`; historical runs retain their
recorded outcomes.

A run has a five-minute deadline, reported as failure rather than user
cancellation. Cancellation has a separate two-second accounting-drain deadline;
exhaustion returns `cleanup_incomplete` and requires reopening rather than
claiming durable completion. Tests inject shorter positive deadlines. This
bounds surface waiting and reporting, not arbitrary C++ executor destruction;
production adapters remain responsible for cooperative cancellation and child
cleanup.

Real descriptor output is nonblocking, handles partial writes and EINTR, and
polls cancellation with a bounded stall deadline of 2 seconds per record. A
broken pipe or stall cancels the active run, including tools, and never waits
indefinitely for a consumer. A partial final record lacks its LF and must be
ignored by consumers. The executable ignores SIGPIPE and propagates SIGINT
through its existing stop source. Descriptor flags are restored on teardown.
One bounded record is buffered at a time; no unbounded JSONL output queue exists.

## Implementation boundaries

ChatSession remains the surface over the same RunKernel, with explicit agent
surface identity and existing Chat defaults preserved. Production repository
ownership and process/approval assembly are shared with interactive Chat.
Protocol parsing/encoding and descriptor transport remain separate from the
run driver so deterministic fakes can prove failure and accounting behavior.
No storage format, parser library, runtime, plugin ABI, isolation mechanism, or
new execution engine is selected by this additive protocol.
