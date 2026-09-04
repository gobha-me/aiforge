# AIForge architecture north star

*Status: architectural guardrails · 2026-08-07*

This document records the decisions that keep future product directions open.
It is not a feature specification and deliberately does not choose every
library, storage format, scripting language, or screen layout.

The July `DESIGN.md` remains useful historical context. Where it conflicts with
this document, these guardrails govern.

`CONTEXT.md` applies these guardrails to repository-aware context construction.
It is a subordinate proposal; accepted ADRs and this north star continue to
govern where the proposal is incomplete or conflicts.

## Product model: orthogonal axes

Do not encode the product as two separate Chat and Code applications. The
useful concepts vary independently:

| Axis | Initial values | Later possibilities |
| --- | --- | --- |
| Surface | interactive TUI, one-shot/pipe | JSONL/headless, remote controller |
| Workspace | Chat, Code | Ops, research, media |
| Persona | user-selected system role | project and task overlays |
| Permission profile | observe/plan, ask, approved automation | custom capability sets |
| View | transcript, diff, files, task | resources, logs, images, dashboards |

A persona changes behavior and presentation defaults; it does not silently
increase authority. A workspace selects useful views and tool sets; it does not
own a separate inference engine. All surfaces use the same run kernel.

Ops should become a first-class workspace once it is useful. Hiding it forever
inside Code would lose important concepts such as active cluster context,
namespace, freshness, resource identity, watch state, and mutation safety.

## One run kernel, several projections

The durable core is an append-only stream of typed run events. At minimum it
must be able to represent:

- user and assistant content;
- model deltas, reasoning metadata, usage, finish, error, and cancellation;
- tool proposal, policy decision, approval, start, progress, result, and error;
- structured question requested, answered, or cancelled;
- artifacts created, referenced, displayed, or removed from a view;
- child run creation and inter-run messages when multi-agent work arrives.

The transcript, task list, tool activity panel, cost ledger, audit history, and
session summary are projections of those events. They are not separate sources
of truth that must be kept in sync.

Each persisted event needs a stable event ID, run ID, ordering field, schema
version, timestamp, and relevant parent/invocation IDs. Corrections are new
events; persisted history is not silently rewritten. Unknown future event types
must be skippable so a newer session does not make an older reader crash.

Provider payloads may be retained only through a redacted/debug policy. API
keys, authorization headers, and raw secrets never enter ordinary events,
renderable strings, or artifacts.

## Provider seam

Venice is the first and primary backend. Still, the run kernel must not expose
venice-cpp types in its domain model.

Use a small backend seam primarily to enable deterministic fakes and replay:

- request takes provider-neutral messages, content blocks, tool declarations,
  and generation options;
- response is a structured event stream, not a text callback;
- structured tool-call deltas, usage, citations, reasoning, cancellation, and
  provider errors remain representable even before every provider supports
  them;
- provider-specific options can travel in an explicit extension field without
  contaminating every core type.

Do not build a provider marketplace before a second real backend exists. Do
build the fake backend before the production UI depends on networking.

## Prompt and context construction

The durable session history is not the prompt sent to a model. A context
builder projects selected events and artifacts into a bounded provider request.
This boundary must exist before “send the whole transcript” becomes an
assumption throughout the application.

Prompt inputs retain provenance and precedence. Expected layers include:

- application/runtime contract;
- user-global instructions;
- workspace instructions;
- project instructions such as AGENTS files;
- persona;
- session/task instructions;
- conversation content and selected evidence.

ADR 0012 defines the exact precedence policy. User-global, persona, and project
content cannot override runtime safety or capability policy. Content
read from files, web results, cluster resources, tool output, and artifacts is
untrusted evidence by default, not a new instruction layer merely because it
contains imperative text.

Context budgeting is explicit across instructions, recent turns, summaries,
tool schemas, tool results, and attachments. Large results become artifacts
with bounded excerpts or summaries rather than silently consuming the entire
window.

Summaries and memories are derived records with provenance: source event IDs,
model/tool identity, creation time, and version. They never delete or rewrite
the underlying event history. A user can inspect, replace, disable, or rebuild
them. Retrieval is a context-builder policy, not hidden global state inside a
backend.

## Tools are asynchronous invocations

The architectural contract is not “call a function” or “spawn a process.” It
is:

```text
ToolInvocation -> ordered events -> ToolResult
```

An invocation has stable and parent IDs, validated arguments, declared effects,
capabilities, deadlines and resource budgets. A result can contain structured
data, content blocks, logs, and artifact references. Output size is bounded.

Execution backends may eventually include:

- native C++ tools;
- argv-based subprocesses and, separately, high-risk shell evaluation;
- an embedded or isolated JavaScript/Lua runtime;
- WASM or native plugins;
- MCP and other remote tools;
- user interaction such as `ask_user`.

The registry is runtime-queryable even when some registrations are compiled
in. Help, model tool schemas, completion, policy inspection, and UI lists must
derive from the same registry rather than parallel tables.

Executors may suspend. The UI loop, cluster watches, cancellation, and other
runs continue while an invocation awaits a user answer, approval, remote job,
or child result. Nested invocations carry parent IDs so policy and audit remain
intelligible.

No scripting language is selected now. The boundary must permit a future
runtime without assuming that it is in-process or a security boundary. Script
host functions receive explicit capabilities; untrusted scripts get no ambient
filesystem, environment, module loader, FFI, process, or network access.

## Effects, capabilities, and approvals

A single `paid` or `dangerous` boolean is insufficient. Tool declarations and
runtime observations use a set of effects such as:

- read;
- write;
- delete;
- execute;
- network access;
- communicate externally;
- spend;
- change infrastructure or privileges.

Capabilities name the concrete allowed scope: roots, hosts, cluster contexts,
namespaces, resource kinds, commands, monetary limits, or artifact IDs.

Policy is enforced at the effect boundary, not by trusting the model's tool
choice or inspecting generated source text. Approval prompts are runtime-owned
and auditable. They are distinct from `ask_user`, which gathers information but
grants no authority.

An approval can be scoped to one invocation, a bounded session rule, or a saved
policy only through an explicit user action. A persona, script, restored
session, or child run cannot broaden the parent capability set.

## Process execution

The ordinary process tool accepts an executable plus an argument vector. Shell
evaluation is a distinct, more powerful tool rather than an invisible
implementation detail.

Execution policy must be able to constrain working directory, readable and
writable roots, environment variables, stdin, runtime, output bytes, process
tree, and cancellation. stdout and stderr remain separate event channels.

Operating-system isolation is an evidence-backed launch property, not an
inference from compile-time APIs or a tool's declared capability scopes. An
opt-in, noninstalled evaluator may record bounded primitive evidence for an
exact source revision and host. Such a report grants no production authority,
does not advertise an isolation level, and is never reused as launch-time
availability. A later accepted mechanism ADR must map positively enforced
primitives to each complete isolation contract; production must re-establish
that contract at application launch and fail closed without downgrading.

Never make the UI parse human terminal output when a structured adapter is
available. Initial Kubernetes and Ceph adapters may wrap their CLIs with JSON
output, but their domain results should not expose command-line formatting.

## Questions and suspended interaction

`ask_user` is the first low-risk proof of the complete tool loop:

```text
model tool call
  -> question.requested
  -> run.awaiting_input
  -> modal answer or cancellation
  -> structured tool result
  -> inference resumes in the same run
```

Several questions form one wizard/paged dialog. One-of, many-of, optional
free-form answers, stable IDs, validation, and explicit cancellation are core
semantics; TermForge chrome is only one projection.

Outstanding questions survive session persistence without duplicate answers.
A noninteractive surface does not advertise interactive tools unless it has an
explicit input protocol; it fails fast rather than hanging.

## Steering, cancellation, and concurrency

Do not equate a session with one blocking request. A session may contain
multiple runs, and the runtime may have bounded concurrent model, tool, watch,
or child-run work.

While a run is active, user input has explicit semantics rather than being
silently appended to whichever prompt happens to be under construction:

- queue for the next model turn;
- steer the active run at a supported boundary;
- cancel the active operation while preserving partial evidence;
- start a separate run or side thread.

Each action is represented by an event and targets a stable run or invocation
ID. Cancellation propagates down the invocation tree but does not erase
completed child results. Concurrency and queue limits are runtime policy so a
model cannot create an unbounded fan-out of tools or agents.

## Artifacts and rich content

Large or binary values do not travel inline through every event or model turn.
An artifact store provides stable IDs and metadata including media type, byte
size, digest, provenance, producing invocation, and optional dimensions.

Content blocks may reference text, structured data, images, files, diffs, or
other artifacts. Each surface decides how to render them. Prompt construction
decides explicitly whether to inline, summarize, attach, or omit an artifact.

Media ownership is divided as follows:

- venice-cpp owns image API calls and encoded response bytes;
- RasterForge owns decode, validated RGBA, crop/scale/fit, and compositing;
- TermForge owns terminal capability detection, placement, layering, Kitty
  transport, and ANSI degradation;
- AIForge owns generation/display tools, semantic slots, themes, permissions,
  cost, cache policy, and session provenance.

The model requests semantic slots such as `inline-after-turn`, `right-pane`, or
`background`, plus fit and intent. It does not control raw terminal escape
sequences or unrestricted coordinates. AIForge advertises only slots and media
tools the active terminal can actually support.

Capability tiers are based on detected terminal behavior, not shell name or
`TERM` alone:

1. native graphics, initially Kitty;
2. truecolor ANSI half-block rendering;
3. text-only, with visual placement tools unavailable.

## UI and concurrency

The UI thread owns all TermForge widgets and presentation state. Workers emit
events through a thread-safe channel and wake/post to the UI loop. Rendering
projects current run state; workers never mutate widgets.

Polling every frame may be an interim implementation but is not the long-term
contract. Backpressure, cancellation, and bounded queues are required for model
streams, subprocess logs, file watches, and cluster watches.

Terminal resize can change available layouts and media capabilities. Tool and
slot availability therefore can be recomputed at runtime; it is not frozen at
startup.

## Sessions, replay, and provenance

A session records runs and events, configuration provenance, backend/model
identity, tool/runtime versions, policy decisions, usage, and artifact
references. Secrets are referenced by credential source, never serialized.

Replay means rebuilding projections from recorded events. It must never rerun a
side-effecting tool merely because its result is being displayed again.

Configuration resolution retains source provenance using the precedence rule:
command line, environment, file, compiled default. Session restore does not
silently override an explicit current-session flag.

Generated or user-installed scripts/tools record source digest, runtime and
version, declared schema, and granted capabilities. Session-local definitions
are ephemeral by default; making one persistent is a separate authorized act.

## Ops workspace guardrails

The first cluster-management milestone is read-only and evidence-oriented:

- active context and namespace are always visible;
- Kubernetes nodes, workloads, events, logs, and health summaries;
- Ceph health IDs, quorum, OSD/PG state, and capacity;
- drill-down preserves the source evidence and observation time;
- list/watch caches expose freshness and disconnection rather than presenting
  stale state as current.

Mutation comes later through typed domain operations, explicit target
resolution, diffs/plans where possible, scoped approvals, and postcondition
verification. A generic shell remains available only under its own stronger
policy; it is not the primary Ops API.

## Multi-agent compatibility

Do not implement multi-agent orchestration early, but keep it representable.
A child agent is a child run with a bounded context and a capability subset.
Messages, task assignment, results, cancellation, and steering are events.

Fresh context is a feature: a child need not inherit the parent's entire
transcript. Worktrees or other filesystem isolation are executor policy, not a
property of the conversation model.

## Module direction, not a frozen file tree

Keep dependency direction recognizable:

```text
surfaces (TUI / CLI / JSONL)
          |
          v
application runtime (runs / tools / policy / projections)
          |
          v
domain types and ports (events / content / artifacts / storage interfaces)
          ^
          |
adapters (Venice / TermForge / process / filesystem / cluster)
```

Domain types do not depend on TermForge, venice-cpp, a JSON implementation, a
database, or an embedded scripting runtime. Adapters may depend inward; the
domain does not depend outward.

Do not extract an agent-kernel library until a second real consumer proves the
boundary. Do extract narrowly reusable media processing into RasterForge,
because its ownership and multiple consumers are already visible.

## Decisions intentionally deferred

The architecture must not make these impossible, but implementation should not
pretend they are settled:

- JavaScript versus Lua versus WASM for local scripting;
- JSON library and persisted session encoding;
- relational database versus files/content-addressed storage;
- parser adapter versus a future argument-parser library;
- plugin packaging and ABI;
- exact multi-agent scheduler;
- GPU media processing;
- additional model providers.

Choose each with a timeboxed spike and ADR when a real milestone reaches it.

## Near-term dependency order

This is sequencing, not a promise of the final interface:

1. clean bootstrap, vocabulary ADR, and parser decision;
2. typed events, run state, small backend seam, fake backend, and persistence
   envelope;
3. Chat workspace and noninteractive surface on the same kernel;
4. `ask_user`, tool registry, effects, and policy foundations;
5. Code workspace, bounded process execution, artifacts, and read-only Ops;
6. mutating Ops, scripting, multi-agent orchestration, and rich generated media
   only when their prerequisites and safety contracts exist.
