# AIForge

AIForge is a C++23 terminal AI client built from a provider-neutral run kernel
outward. It currently provides a streaming Venice-backed one-shot/pipe surface,
durable and resumable sessions, provider-neutral run events, deterministic
backends, replayable run and transcript projections, Markdown-lite
presentation, and typed configuration.
The interactive Chat workspace is the next surface; running `aiforge` without a
prompt in a terminal still reports the current bootstrap status.

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
baselines: TermForge v0.44.0 for cross-thread posting, styled word wrapping,
and bounded mutable transcript streaming; and venice-cpp v0.9.0 for transport
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

## Tool invocation foundation

`aiforge::runtime::ToolRegistry` is the authoritative source for model-facing
tool declarations and their executors. Immutable snapshots preserve
registration order and keep backend schemas, future help/completion surfaces,
argument validation, and execution dispatch on the same definition. Executors
receive provider-neutral validated arguments, granted capability scopes,
deadlines, output budgets, and cancellation tokens; deterministic scripted
executors cover the boundary without production side effects.

A model tool call records a durable proposal but never grants itself authority.
The run remains live after the calling inference ends, and an explicit runtime
policy decision must allow, deny, or request approval before execution. Tool
progress and exactly one bounded result or redacted error are appended as run
events. A subsequent inference is explicit and must include every terminal tool
result as a tool-role context message. Replay rebuilds the same state without
running validation, policy, tools, or inference again.

No production tool or general capability policy is registered yet. AF-24 owns
the policy engine and approval rules; AF-25 owns bounded process execution.

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

The interactive bootstrap does not yet expose session list/resume/new actions,
and richer configuration, credential-source, and runtime-version provenance
remain follow-up work under AF-11.

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
