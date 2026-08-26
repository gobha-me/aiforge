# AIForge

AIForge is a C++23 terminal AI client built from a provider-neutral run kernel
outward. It currently provides a streaming Venice-backed one-shot/pipe surface,
durable and resumable sessions, provider-neutral run events, deterministic
backends, replayable run and transcript projections, Markdown-lite
presentation, typed configuration, file-backed personas, model discovery and
selection, and a resumable
structured `ask_user` tool
boundary plus a policy-gated bounded argv process executor. Exact-candidate
review receipts now materialize merge authorization only from current required
evidence or an explicit trusted override. Bounded plan revisions, task graphs,
and exact-revision decisions now have a provider-neutral durable contract and
replayable projection. The interactive
Chat workspace uses the same run kernel, context builder, and durable event
stream as one-shot mode; running `aiforge` without a prompt in a terminal opens
it. Its slash-command registry keeps dispatch, help, completion, and command
availability on one provider- and terminal-neutral definition.

The command registry provides generated help and version output. Commands that
depend on later milestones fail explicitly instead of reporting false success.
The architectural guardrails live in
[`docs/ARCHITECTURE-NORTH-STAR.md`](docs/ARCHITECTURE-NORTH-STAR.md).

Architecture documents have explicit authority levels. The north star and
accepted records in [`docs/adr/`](docs/adr/) govern. The repository-context
proposal in [`docs/CONTEXT.md`](docs/CONTEXT.md) elaborates those decisions for
the Code workspace and reusable context construction. The July
[`docs/DESIGN.md`](docs/DESIGN.md) is retained only as historical product
context.

## Requirements

- CMake 3.28 or newer
- GCC 13+ or Clang 17+ with a C++23 standard library
- Git when CMake must fetch dependencies

AIForge uses CMake only for dependencies. A configured package is preferred,
then a sibling checkout, with `FetchContent` as the fallback. Adapter builds use
TermForge and venice-cpp; consumed core-only builds may set
`aiforge_BUILD_ADAPTERS=OFF`. Their fallbacks are pinned to compatible stable
baselines: TermForge v0.55.0 for cross-thread posting, styled word wrapping,
bounded mutable transcript streaming, multiline composition, history recall,
and multi-page choice dialogs; and
venice-cpp v0.23.0 for transport cancellation, structured deltas, tool
declarations, explicit public authentication, and typed model metadata.

Durable session storage uses SQLite 3 behind a neutral storage port. CMake
prefers an installed SQLite 3.45.1 or newer and otherwise builds the pinned
official 3.53.4 amalgamation. SQLite and JSON-library types remain private to
the adapter.

## Build and test

```bash
cmake -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The supported Clang path is separate and must also stay green:

```bash
cmake -B build-clang -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/clang.cmake
cmake --build build-clang --parallel
ctest --test-dir build-clang --output-on-failure
```

Configure a model and either store or export a Venice API key, then run a
one-shot request:

```bash
export AIFORGE_MODEL=model-id
./build/src/bin/aiforge login       # terminal-only, input echo is disabled
# Or for the current process:
export VENICE_API_KEY=your-key      # takes precedence over the stored key

./build/src/bin/aiforge "Explain append-only event streams"
./build/src/bin/aiforge chat "Explain append-only event streams"
./build/src/bin/aiforge --continue "Build on the prior answer"
./build/src/bin/aiforge --resume session-id "Reopen this session"
./build/src/bin/aiforge --ephemeral "Do not retain this request"
./build/src/bin/aiforge --model model-id "Use this model"
./build/src/bin/aiforge --session-max-spend 5.00 "Bound this session"
./build/src/bin/aiforge models
./build/src/bin/aiforge                 # interactive Chat
./build/src/bin/aiforge --continue      # interactive latest session
git diff | ./build/src/bin/aiforge "Review this diff"
./build/src/bin/aiforge --help
./build/src/bin/aiforge --version
```

Prompt text is user conversation content. Non-TTY stdin is bounded to 1 MiB,
validated as UTF-8 text, and enters model context separately as untrusted
evidence with stdin provenance. A prompt is required when piping input.

Completion text streams only to stdout. The selected session ID, citations,
usage, configuration warnings, and failures use stderr, so stdout remains safe
to pipe. Unsafe terminal control sequences are removed from provider text.
Ctrl-C requests transport cancellation, preserves already-written partial
output, and returns 130; command-line/input mistakes return 2 and runtime
failures return 1.

## Run kernel

`aiforge::runtime::RunKernel` drives the provider-neutral pull stream on a
worker, transfers observations through a bounded channel, and commits typed
events and projections on the owning thread. Cancellation targets stable run
and inference IDs and preserves partial assistant content. Backend errors are
mapped to bounded generic domain errors before entering durable or renderable
state.

The Venice adapter translates neutral context, generation options, tool
declarations, structured deltas, usage, model context capacity, and transport
cancellation without exposing Venice types through the runtime API. The
one-shot surface uses the selected model's reported context window and a
conservative input estimate before inference. The TermForge bridge uses
`App::post` only as a wake signal; event-log, projection, and widget mutation
remain on the UI thread.

## Replayable plan graphs

`aiforge::domain::PlanRevision` records a stable plan and revision identity,
goal, optional repository snapshot, predecessor revision, and bounded task
graph. Tasks retain hierarchy and dependency edges, acceptance criteria,
intended effects, and typed resource intents. A resource intent describes
prospective scope for later conflict analysis; it is never a capability grant.

`PlanRevisionProposed` and `PlanRevisionDecisionRecorded` append exact-revision
facts. `PlanGraphProjection` validates revision chains and task DAGs, retains
prior revisions, and distinguishes proposed, revision-requested, approved, and
rejected current state. SQLite replay reconstructs the same projection without
running inference, tools, or child tasks. Runtime approval gates, accepted-task
materialization, dispatch, and plan-review surfaces remain later children of
issue #50.

## Interactive Chat

With terminal input and output and no prompt argument, the root command opens a
TermForge Chat surface. `--resume <session-id>`, `--continue`, and `--ephemeral`
select the same session modes as one-shot execution. Empty input without a
terminal remains an error rather than attempting to start a TUI.

The composer owns multiline UTF-8 editing, bracketed paste, display-width
wrapping, and draft-local cursor state. Enter submits; Shift+Enter or Alt+Enter
adds a newline when the terminal can distinguish the modifier. Page Up/Down
scroll the transcript, and Escape or Ctrl-C cancels an active run. When idle,
Ctrl-C clears the complete draft, Ctrl-D exits, and Escape has no exit behavior.
Ctrl-D is unavailable during an active run. Submission is disabled during
inference because steering semantics remain a separate runtime milestone.

Prompt recall is rebuilt from ordered `UserContentAdded` events in the selected
session. Failed validation or persistence leaves the draft intact and creates
no recall entry. Ctrl+K clears the current recall view without rewriting
durable history, and Ctrl+H disables or re-enables recall. Cross-session prompt
history is not retained.

Tab completes slash-command names from the same registry used by dispatch and
help. `/help [command]` opens transient help that never enters session history;
`/quit` exits; `/clear` clears only the visible transcript projection while
retaining durable events and prompt recall; and `/edit` opens an empty external
draft. `/usage` opens the selected session's event-derived token, cost, and
spend ceiling summary. Unknown, unavailable, malformed, and oversized
slash commands are rejected locally and never become model content.

## File-backed personas

Personas are bounded system instructions stored as `.md` or `.txt` files in
`$XDG_CONFIG_HOME/aiforge/personas`, or
`$HOME/.config/aiforge/personas` when the XDG location is unset or relative.
A persona is selected by its bare filename without the extension:

```bash
./build/src/bin/aiforge --persona reviewer "Review this design"
./build/src/bin/aiforge chat --persona reviewer "Review this design"
./build/src/bin/aiforge --no-persona "Ignore the session persona"
```

In interactive Chat, `/persona list` shows available personas,
`/persona set <name>` selects one for the next turn, and `/persona off`
disables it. New sessions carry the current selection. Resumed sessions reload
the recorded file and require an explicit `set` or `off` decision when its
content has changed or disappeared.

Names are ASCII letters or digits followed by letters, digits, `_`, or `-`.
AIForge accepts at most 256 personas, reads at most 1 MiB per file, rejects
empty or malformed UTF-8 and unsafe controls, and refuses symlinked roots or
entries. Case and extension aliases are rejected. Each run records the selected
persona's portable source path, byte size, and SHA-256 digest in the append-only
event stream; persona text enters model context as an attributed persona-layer
system instruction, not as conversation history.

## Model catalog and picker

`aiforge models` reads Venice's public catalog without sending an API key and
prints a tab-separated table containing model ID, modality, context capacity,
capabilities, base input/output USD pricing, and availability. Catalog warnings
use stderr, leaving stdout safe to pipe.

`--model <id>` overrides `AIFORGE_MODEL` and the configuration file. Unknown
command-line text models exit 2 with deterministic suggestions before a paid
request starts. In interactive Chat, `/model` opens a filter-as-you-type picker
and `/model <id>` selects directly; changes are allowed only while idle and
take effect on the next run, whose ordinary provenance records the chosen
model.

The bounded catalog cache is
`$XDG_CACHE_HOME/aiforge/model-catalog.json`, or
`$HOME/.cache/aiforge/model-catalog.json` when the XDG location is unset. A
fresh snapshot is reused for 24 hours. If refresh fails, a valid stale cache is
shown with a warning; malformed, insecure, symlinked, or oversized cache data
is never trusted. The cache contains neutral catalog metadata, never
credentials or provider request payloads.

## Replayable usage ledger

`aiforge::domain::UsageLedgerProjection` rebuilds session usage from durable
inference events instead of maintaining a mutable session counter. Each
inference record retains its run, model, timestamps, terminal status, and
separate input, output, cached-input, and reasoning-token totals. Interleaved
runs, retries, failures, cancellation, and partial live streams remain
inspectable, while checked aggregation refuses overflow without partially
changing the projection. Replay performs no provider call or other side effect.

Interactive Chat rebuilds the ledger when a session opens or switches and
advances it from the same events as the transcript. Its header shows compact
input/output usage and any reported amounts; `/usage` shows all four token
buckets, inference lifecycle counts, and cost-report coverage. Missing and
partial reports remain explicit, including when another unit reports zero.

## Durable session inference spend ceiling

`--session-max-spend <USD>` sets a positive decimal USD ceiling, with at most
six fractional digits, on a new or existing session. The ceiling is an
append-only session policy fact: omitting the option on resume inherits it,
repeating it is idempotent, and a later command may narrow but never widen or
remove it. The root one-shot command, `chat`, and interactive startup accept the
same option. Ephemeral sessions enforce the ceiling for their lifetime but do
not persist it.

Before starting each inference, AIForge accounts for every prior inference in
the selected session. Provider-reported USD is authoritative when present;
otherwise a durable catalog-derived USD estimate is used. Other units such as
`venice.diem` are neither converted nor combined with USD. If any inference has
neither reported USD nor a valid USD estimate, accounting is unavailable and
the next inference is refused. The request that crosses the ceiling completes,
because there is no trustworthy pre-call quote; subsequent requests are
blocked. One-shot diagnostics print `spend:` on stderr, while the interactive
header and `/usage` show accounted spend, ceiling, remaining amount, coverage,
and whether the ceiling has been reached.

## Provider-reported inference cost

When a backend reports what an inference actually charged, AIForge records the
amount once as a provider-neutral `InferenceCostRecorded` event. Monetary
values use checked canonical decimals and bounded unit identifiers rather than
floating-point durable state. Run and session usage projections retain the
per-inference report and aggregate each unit independently, including failed or
cancelled inference attempts; missing cost stays unknown.

One-shot mode prints the report to stderr after usage, for example
`cost: USD=0 venice.diem=0.0645375 (provider-reported)`, or
`cost: unavailable` when the backend supplied no amount. These are actual
provider observations, not estimates reconstructed from catalog rates. In
particular, a reported zero in one unit is preserved as data and is not treated
as proof that the call was free. Cost diagnostics never enter completion
stdout, and replay performs no provider or billing request.

## Durable catalog pricing observations

When the selected text model has catalog pricing, AIForge records one
provider-neutral `InferencePricingObserved` event before starting each backend
request. The observation retains model and catalog identity, retrieval time,
live/fresh-cache/stale-cache origin, optional source revision, exact normalized
decimal rates, and a deterministic rate-card digest. Input, output, cache-read,
and cache-write buckets remain distinct, as do base and extended tiers.

These observations are estimate provenance; they are not quotes or actual
charges. AIForge never guesses whether an extended threshold counts prompt or
total tokens, or reinterprets a reported cache-read count as a cache write. The
replaceable model-catalog cache stores decimal rates as strings in schema
version 2; older cache schemas are refreshed through the existing catalog
fallback policy. Session replay rebuilds the same pricing context without
consulting the cache or provider.

## Conservative catalog cost estimates

AIForge derives catalog estimates from the durable usage and pricing facts for
an inference. They remain separate from provider-reported actual charges and
from future pre-run quotes. Input tokens reported as cache reads are subtracted
from full-rate input and use the cache-input rate; output pricing already covers
reasoning tokens, so reasoning is not counted twice. Arithmetic is exact and
checked at the catalog's per-million-token basis rather than rounded.

An extended tier is selected only when both plausible interpretations of
Venice's context threshold agree: base when input plus output is at or below the
threshold, extended when input alone exceeds it. The estimate is explicitly
unavailable in the overlap. It is also unavailable when cache-write pricing is
present because current usage has no cache-write bucket, or when usage, rates,
precision, or arithmetic are insufficient. USD and Venice diem coverage are
reported independently. One-shot mode writes estimates only to stderr; the TUI
header and `/usage` label partial coverage and keep catalog-derived amounts
visibly distinct from provider observations. Replay performs no catalog or
provider request.

Ctrl+E restores the terminal and opens the current draft in `$VISUAL` or
`$EDITOR`. The editor setting names one executable only; arguments and shell
syntax are rejected, and users who need flags may select a wrapper executable.
The adapter resolves the executable explicitly, uses an owned 0700 temporary
directory and 0600 draft, passes only selected environment variables, validates
the bounded edited text, and restores the original draft after failure,
cancellation, or timeout.

## Tool invocation foundation

`aiforge::runtime::ToolRegistry` is the authoritative source for model-facing
tool declarations and their executors. Immutable snapshots preserve
registration order and keep backend schemas, future help/completion surfaces,
argument validation, and execution dispatch on the same definition. Executors
receive provider-neutral validated arguments, granted capability scopes,
deadlines, output budgets, and cancellation tokens; deterministic scripted
executors cover the boundary without production side effects.

A model tool call records a durable proposal but never grants itself authority.
After validation, the run kernel asks its `ToolPolicy` to allow, deny, or
request approval; surfaces cannot inject a policy decision. Missing policy
configuration allows only tools that declare no authority-bearing effects and
denies effectful work.

`CapabilityPolicy` applies a typed permission profile with automatic grants and
an approval ceiling. Filesystem roots use lexical containment, network hosts
are exact and normalized, commands and artifact or cluster identities are
exact, and spend scopes use bounded integer microunits. Approved authority can
last for one invocation, the current session, or an explicitly saved grant.
Saved grants pass through a neutral `PolicyGrantStore`, are re-intersected with
the current profile when loaded, and have no selected production encoding yet.
Policy explanations and storage failures are redacted before becoming durable
events.

Tool progress and exactly one bounded result or redacted error are appended as
run events. A subsequent inference is explicit and must include every terminal
tool result as a tool-role context message. Replay rebuilds the same state
without running validation, policy, tools, or inference again. Validators may
narrow a declaration's effect set for a specific invocation, and tool-created
artifact metadata is committed before the terminal result that references it.

## Bounded process execution

`aiforge::adapters::register_process_tool` adds the model-facing `run_process`
tool from caller-supplied command, filesystem-root, environment, time, and
output ceilings. Each invocation provides one exact executable and argument
vector, a normalized absolute working directory, narrowed readable/writable
roots, selected environment-variable names, closed stdin, and smaller runtime
and output limits. Execution uses no implicit shell and inherits neither the
ambient environment nor unrelated file descriptors.

The POSIX adapter rechecks command and configured-root identity at the effect
boundary, runs each child in its own process group, captures stdout and stderr
independently without blocking, and terminates descendants on cancellation,
timeout, or output exhaustion. Small safe UTF-8 streams remain inline; binary
or larger streams pass through a caller-supplied neutral `ArtifactStore` and
enter the event stream as typed creation and reference records. No production
artifact encoding is selected yet.

Filesystem roots are capability-policy authority and constrain the working
directory; they are not an operating-system sandbox. The current one-shot
surface does not register `run_process` because it has no process approval or
artifact-storage profile.

## Structured user questions

`ask_user` is a no-authority model-facing tool for one or more bounded single-
or multiple-choice questions, recommended defaults, selection limits, and an
optional Other answer. The executor emits a typed input request and ends; no
provider cursor or tool worker stays blocked while a person answers. The run
kernel records the invocation-scoped questions, enters `awaiting_input`, and
accepts exactly one answer or cancellation before recording the structured tool
result and permitting another inference in the same run.

Pending questions replay from the durable event stream without rerunning a
tool, callback, or inference. The TermForge adapter maps stable question and
option IDs to the reusable multi-page choice wizard and translates its
presentation indices back at the runtime boundary. The current one-shot surface
has no question-input protocol and therefore does not register or advertise
`ask_user`; resuming a pending interactive question there fails explicitly
instead of hanging.

## Configuration

AIForge resolves registered settings in command-line, environment, file, then
compiled-default order and retains the winning source. The first registered
application setting is the optional `model` value, bound to `AIFORGE_MODEL`.

```bash
aiforge config show
aiforge config get model
aiforge config set model model-id
aiforge config unset model
```

The configuration file is `$XDG_CONFIG_HOME/aiforge/config.json`, or
`$HOME/.config/aiforge/config.json` when the XDG location is unset or relative.
It is strict UTF-8 JSON. AIForge creates its directory and file with restrictive
permissions, preserves unknown fields during known updates, and refuses to
overwrite malformed, symlinked, or loosely permissioned files. Configuration
diagnostics use stderr; requested values use stdout. Credentials are excluded
from this file.

## Credentials

`aiforge login` reads one credential from terminal input with echo disabled and
stores it at `$XDG_CONFIG_HOME/aiforge/credentials`, or
`$HOME/.config/aiforge/credentials` when the XDG location is unset or relative.
The AIForge directory is mode 0700 and credential, lock, and temporary files are
mode 0600. Writes are locked, synced, and atomically renamed. Symlinked,
non-regular, or loosely permissioned paths are rejected; login never accepts a
credential from a pipe or from inside the TUI.

An explicitly present `VENICE_API_KEY` overrides the stored source. An empty,
oversized, whitespace-bearing, or control-bearing explicit value is an error
and does not fall through to the file. Stored-source failures are reported as
warnings and the source is ignored. Missing credentials fail one-shot
inference, while interactive Chat and public/cached model discovery remain
available; submitting a turn records and renders a fixed
credential-not-configured failure.

Credential bytes are consumed only by production backend construction. They do
not enter provider-neutral requests, events, artifacts, session storage,
diagnostics, or terminal frames. Durable provenance records only the source
locator `VENICE_API_KEY` or `aiforge/credentials`. Local plaintext storage is a
restricted bootstrap mechanism, not a replacement for a platform keychain.

## Durable sessions and replay

`aiforge::storage::SessionStore` defines bounded session creation, discovery,
atomic event append, and ordered replay without exposing SQLite or JSON types.
`aiforge::adapters::SqliteSessionStore` implements the accepted ADR 0005
contract at `$XDG_STATE_HOME/aiforge/sessions.sqlite3`, or
`$HOME/.local/state/aiforge/sessions.sqlite3` when the XDG state location is
unset or relative. It uses restrictive paths and permissions, full synchronous
rollback-journal transactions, schema migrations, and bounded writer
contention. Unknown future event payloads remain opaque and replayable.

One-shot requests create durable sessions by default. `--resume <session-id>`
reopens an exact session, `--continue` selects the most recently active session,
and `--ephemeral` neither creates nor opens the store. These options are
mutually exclusive and are available on the root and `chat` commands. A resumed
request rebuilds projections without rerunning inference or tools, then starts a
new run whose context contains completed prior conversation. Cancelled or
failed partial assistant output remains inspectable in durable history but is
not presented to the next model call as a completed answer.

Interactive startup supports new, exact resume, continue-latest, and ephemeral
sessions through command-line selection. Inside the TUI, `/session` and
`/session list` show the 100 most recently active durable sessions with their
IDs, creation and activity times, and run counts; the current session is marked.
`/session resume <session-id>` replays and switches to an exact durable session,
while `/session new` starts a fresh session without leaving the terminal. A
switch opens and validates the candidate before replacing the current kernel or
transcript, so failure leaves the current session usable. Session actions are
refused while a run or question is active rather than implicitly cancelling it.
In ephemeral mode listing explains that durable history is unavailable, resume
is rejected, and `/session new` starts another ephemeral session.

## Run provenance

Every run may record a `run.provenance_recorded` event immediately after
`run.started`, so a replayed run explains what produced it: resolved
configuration with its precedence decisions, backend and model identity, the
credential source, the tool declarations the run actually offered, and runtime
component versions. Each run carries its own record, including each interactive
turn.

A sensitive configuration key contributes presence, source, and its decision
trail; its resolved value never enters an event. Credential sources are named by
a non-secret locator such as an environment variable name, never by any part of
the credential. `aiforge::domain::validate_run_provenance` enforces both rules
before the run kernel records anything, so an ephemeral run cannot carry a
secret into the in-memory event log either, and the storage adapter enforces
them again on encode and parse. Tool identity is kernel-owned: a caller submits
the record with no tools and the kernel fills them from its own registry
snapshot.

`RunProjection::provenance` exposes the restored record after replay. A record
is refused before `run.started`, after a terminal event, and a second time
within one run. An older reader retains the event opaquely and replays past it.

## Repository snapshot identity

`aiforge::repository::RepositorySnapshotSource` defines a bounded, cancellable
port for observing repository identity without exposing filesystem or Git
types. Snapshots distinguish branch, detached, unborn, and non-VCS roots;
retain staged, worktree, and untracked state; attach algorithm-labelled content
digests; and compare source state through a canonical root identity plus a
stable manifest fingerprint. A scripted source covers deterministic callers.

`aiforge::adapters::GitRepositorySnapshotSource` is the first production
adapter. It receives an explicit Git executable, invokes only bounded argument
vectors, resolves root aliases, records porcelain-v2 status, and scans non-VCS
trees without following symlinks. It observes twice and rejects a repository
that changes between passes. The adapter is not yet registered by a surface;
project-instruction discovery, evidence selection, caching, and edits remain
separate milestones.

## Repository evidence and context parcels

Repository evidence keeps the source state used for one inference distinct
from durable run history and rebuildable repository knowledge. Exact source,
diagnostics, diffs, tool results, and derived records carry stable identities,
snapshot provenance, freshness, producer metadata, and bounded content or
artifact references. Unknown future evidence kinds remain opaque rather than
being guessed into a known semantic category.

`aiforge::repository::validate_context_parcel` validates an ordered,
purpose- and task-phase-labelled working set before the runtime hands it to
context construction. Validation checks repository and content
identity, half-open source ranges, provenance consistency, artifact linkage,
freshness claims, duplicate evidence IDs, and checked byte/token totals. This
foundation selects no repository cache, persistence encoding, retrieval
ranking, parser, or provider request mapping.

`aiforge::runtime::ContextBuilder::select_and_build` combines validated parcels
with candidate conversation, summaries, tool results, and attachments under the
actual model capacity. Mandatory inputs fail rather than disappear; optional
inputs receive deterministic task-phase, freshness, relevance, representation,
cost, and identity ordering. Per-class token ceilings and an explainable result
record every admission or omission. Editing requires current exact source, and
repository text always enters as untrusted evidence rather than instructions.

## Derived repository knowledge

`aiforge::domain::RepositoryKnowledgeGraph` represents rebuildable symbols,
relationships, diagnostics, semantic summaries, and opaque future records
without turning them into durable run history. Records retain exact repository
and source identities, producer and build-configuration versions, derivation
dependencies, confidence, freshness, and explicit invalidation rules.

Repository knowledge validation rejects malformed or conflicting identities,
unbounded metadata, missing current dependencies, and cycles in the derivation
graph while allowing ordinary semantic cycles such as mutually calling
functions. Pure freshness assessment distinguishes current, uncertain, stale,
and unavailable inputs. Atomic generation-checked updates provide deterministic
replacement and rebuild semantics without selecting a persistent cache or
storage encoding.

## Language-analysis capability port

`aiforge::repository::LanguageAnalysisSource` lets optional language-specific
adapters advertise symbols, references, relationships, signatures, and
diagnostics for an exact repository source and build configuration. Typed
queries return the neutral repository-knowledge records above; parser,
compiler, and language-server types remain behind adapters.

Capability and result validation preserves analyzer producer, snapshot, source,
and build provenance; bounds records and diagnostic notices; and represents
partial, ambiguous, unsupported, and temporarily unavailable analysis without
guessing. A deterministic scripted source covers callers before a production
analyzer is selected. When no analyzer is present, repository traversal, exact
search and reads, build evidence, and VCS observations remain available.

## Exact-source edit guard

`aiforge::repository::ExactSourceEditor` reads bounded complete files against a
captured repository snapshot and applies byte-range replacements only when the
repository identity and whole-file digest still match. Existing dirty state is
valid when it is part of the baseline; later branch, worktree, index, untracked,
path-identity, or target-content changes fail closed.

`aiforge::adapters::GitExactSourceEditor` rejects traversal, symlink, hard-link,
and non-regular targets, serializes cooperating writers, prepares replacement
bytes outside the observed worktree, and rechecks the snapshot and source at the
effect boundary before an atomic rename. Preparatory failures leave the target
unchanged. Errors after commit are marked as potentially applied so callers do
not retry blindly. Successful receipts retain the prior and resulting source
and snapshot identities for later tool-result evidence.

The editor does not grant write authority or register a model-facing tool. A
future executor must first receive the applicable `Effect::write` capability
and record the receipt through the ordinary invocation lifecycle. File creation,
deletion, rename, multi-file transactions, merge resolution, and patch-language
selection remain outside this boundary.

## Code verification evidence

Build, test, static-analysis, diagnostic, diff, and runtime observations use
provider-neutral `VerificationEvidence` records tied to an exact repository
snapshot, optional build configuration, producing tool invocation, observation
time, bounded output excerpts, structured diagnostics, and artifact references.
The corresponding typed run event is stored and replayed through the existing
append-only session stream; replay projects a transcript summary and never
re-executes the producing tool.

Validation rejects malformed provenance, unsafe or oversized text, duplicate
invocation or artifact identities, invalid diagnostics, and inconsistent diff
snapshots. Freshness is assessed against the current repository,
configuration, and artifact availability without rewriting the recorded
observation. Valid records can enter verification and review context parcels as
untrusted evidence, where they receive phase-specific selection priority.

This boundary does not select a build system, CI service, diagnostic parser, or
new executor. Producers use the shared tool invocation, policy, and artifact
lifecycle.

## Deterministic TUI scenarios

`aiforge::testing::run_tui_scenario` drives versioned, bounded application
scenarios over TermForge's synthetic clock and input-trace seam. A scenario
orders semantic input, terminal resize, and scripted backend/tool releases at
stable rendered-frame boundaries. The runner records an opaque TermForge trace,
then replays it against a fresh application and fresh fakes.

Results retain the scenario/corpus/application identities, terminal
capabilities, bounded normalized frames, rendered bytes, semantic state, and
`fnv1a64` trace/script identities. The FNV values identify reproducibility
inputs; they are not signatures or trust evidence. Record and replay must agree
byte-for-byte and semantically. Malformed traces, incompatible capabilities,
changed scenario provenance, script mismatch or exhaustion, resource overruns,
and replay divergence are typed failures. Result-aware replay verifies all
recorded provenance and digests before starting the application.

Interactive chat exposes deterministic identity, timestamp, tool-registry, and
policy injection only through explicit dependencies; ordinary construction
keeps the production defaults. During playback its live wake post is disabled
because the recorded wake marker is the source of frame ordering. Scenario
artifacts are test evidence, not durable session events or another persistence
format.

## Review receipts and merge authorization

Review receipts bind an exact repository snapshot and candidate revision to
versioned verification and scenario requirements, result digests, and artifact
digests. Scenario bindings also retain the application revision, fake-script
digest, terminal-capabilities digest, and corpus version. Drafting, review
requests, findings, resolutions, verdicts,
revocations, and explicit human overrides are append-only run events. The
`ReviewReceiptProjection` rebuilds lifecycle state without rerunning a test,
scenario, hosted check, or merge operation.

`ReviewMergeGate` materializes an opaque `MergeAuthorization` only for the
exact current candidate when every policy-required result remains available
and unchanged, no finding is open, and one active approving verdict exists.
Candidate drift, stale verifier or corpus versions, missing artifacts,
conflicting verdicts, and revoked approval fail closed. Human overrides are
candidate-bound, reason-bearing, visibly identified, and usable only for an
explicitly configured trusted actor. A provider-neutral hosted-check port can
make successful publication part of authorization without exposing GitHub or
another forge's types through the domain API.

## Project instruction discovery

`aiforge::repository::ProjectInstructionSource` discovers bounded `AGENTS.md`
documents from the repository root toward a selected target subtree while
retaining exact source identity, digest, applicability, specificity, and stable
order. The Git/filesystem adapter rejects path and symlink escapes, malformed
text, stale baselines, and repositories that change during discovery; a
scripted source supports deterministic callers.

The runtime handoff admits these documents only through the accepted project
instruction layer and requires both a current matching repository snapshot and
explicit target-model token estimates. Project text does not grant capabilities
or replace runtime-owned policy. Surface integration remains deferred until the
Code workspace has an explicit target-selection flow.

The default toolchain respects `CXX`. Sanitizer toolchains are opt-in:

```bash
cmake -B build-asan -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/address.cmake
cmake -B build-tsan -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/thread.cmake
```

## Development layout

- `include/aiforge/` contains the provider-independent public API.
- `src/lib/` implements the run domain and backend ports.
- `src/adapters/` maps the neutral ports to TermForge and Venice.
- `src/bin/` is the application entry point.
- `test/<name>/test.cpp` is auto-discovered after re-running `cmake -B`.
- `docs/adr/` records decisions that narrow the north-star guardrails.

The core library must not expose TermForge, venice-cpp, JSON-library, database,
or scripting-runtime types. Future adapters depend inward on the core.

## License

AIForge is available under the BSD 3-Clause License; see
[`LICENSE.md`](LICENSE.md).
