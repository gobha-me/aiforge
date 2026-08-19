# AIForge

AIForge is a C++23 terminal AI client built from a provider-neutral run kernel
outward. It currently provides a streaming Venice-backed one-shot/pipe surface,
durable and resumable sessions, provider-neutral run events, deterministic
backends, replayable run and transcript projections, Markdown-lite
presentation, typed configuration, and a resumable structured `ask_user` tool
boundary plus a policy-gated bounded argv process executor. The interactive
Chat workspace uses the same run kernel, context builder, and durable event
stream as one-shot mode; running `aiforge` without a prompt in a terminal opens
it. Its slash-command registry keeps dispatch, help, completion, and command
availability on one provider- and terminal-neutral definition.

The command registry provides generated help and version output. Commands that
depend on later model-picker or interactive milestones fail explicitly instead
of reporting false success. The architectural guardrails live in
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
venice-cpp v0.9.0 for transport
cancellation, structured deltas, and tool declarations.

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

Configure a model and Venice API key, then run a one-shot request:

```bash
export AIFORGE_MODEL=model-id
export VENICE_API_KEY=your-key

./build/src/bin/aiforge "Explain append-only event streams"
./build/src/bin/aiforge chat "Explain append-only event streams"
./build/src/bin/aiforge --continue "Build on the prior answer"
./build/src/bin/aiforge --resume session-id "Reopen this session"
./build/src/bin/aiforge --ephemeral "Do not retain this request"
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
failures return 1. The `models` command remains unavailable until its owning
model-picker milestone.

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

## Interactive Chat

With terminal input and output and no prompt argument, the root command opens a
TermForge Chat surface. `--resume <session-id>`, `--continue`, and `--ephemeral`
select the same session modes as one-shot execution. Empty input without a
terminal remains an error rather than attempting to start a TUI.

The composer owns multiline UTF-8 editing, bracketed paste, display-width
wrapping, and draft-local cursor state. Enter submits; Shift+Enter or Alt+Enter
adds a newline when the terminal can distinguish the modifier. Page Up/Down
scroll the transcript, and Escape or Ctrl-C cancels an active run. Ctrl-C exits
when idle. Submission is disabled during inference because steering semantics
remain a separate runtime milestone.

Prompt recall is rebuilt from ordered `UserContentAdded` events in the selected
session. Failed validation or persistence leaves the draft intact and creates
no recall entry. Ctrl+K clears the current recall view without rewriting
durable history, and Ctrl+H disables or re-enables recall. Cross-session prompt
history is not retained.

Tab completes slash-command names from the same registry used by dispatch and
help. `/help [command]` opens transient help that never enters session history;
`/quit` exits; `/clear` clears only the visible transcript projection while
retaining durable events and prompt recall; and `/edit` opens an empty external
draft. Unknown, unavailable, malformed, and oversized slash commands are
rejected locally and never become model content.

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
from this file. The one-shot surface currently reads `VENICE_API_KEY` directly
from the environment; stored credentials and `login` remain a separate
milestone.

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
sessions through command-line selection. In-surface session listing/switching
and richer configuration, credential-source, and runtime-version provenance
remain follow-up work under AF-11 and their owning feature issues.

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
purpose- and task-phase-labelled working set before a later selection policy
hands it to context construction. Validation checks repository and content
identity, half-open source ranges, provenance consistency, artifact linkage,
freshness claims, duplicate evidence IDs, and checked byte/token totals. This
foundation selects no repository cache, persistence encoding, retrieval
ranking, parser, or provider request mapping.

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
