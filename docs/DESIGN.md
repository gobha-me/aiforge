# AIForge — Design

*Status: draft for review · 2026-07-24*

> **Historical draft:** this captures the first product pass, but several
> architectural assumptions were superseded by the 2026-08 takeover review.
> Read `ARCHITECTURE-NORTH-STAR.md` first. Where the two conflict, the north-star
> guardrails govern. In particular, workspace is no longer fused to execution
> mode, tools are not compile-time-only, and frame polling is not the intended
> long-term worker-to-UI bridge.

AIForge is the C++ successor to `venice-cli` (venice-py): a terminal AI chat client
for the Venice.ai API, built on **termforge** (TUI) and **venice-cpp** (API client).
venice-py is the working prototype we mine for ideas; it is not being ported
line-for-line. The TUI chat experience is the product; venice-py's 20-command
media/agent surface arrives later, in phases, only where it earns its place.

## Product shape

Two faces, one core (venice-py's `AgentProfile` insight, applied earlier):

1. **Interactive TUI** — `aiforge` in a terminal: full-screen chat with streaming,
   scrollback, slash commands, model picker, sessions.
2. **One-shot / pipe mode** — `aiforge "why is the sky blue"` or
   `cat err.log | aiforge "explain"`: streams the answer to stdout, all status to
   stderr, exits. TTY detection picks the face; `--no-tui`/`--tui` override.

The stdout/stderr discipline from venice-py is a hard rule from day one: content on
stdout, everything human (spinners, estimates, errors) on stderr, so piping always works.

## Inherited constraints (all three repos share these)

- C++23, CMake ≥ 3.28, `find_package` first → `FetchContent` fallback, 100% CMake
  (no conan/vcpkg). GCC 13+ **and** Clang 17+ must both pass.
- `std::expected` for fallible operations; exceptions never cross API boundaries.
- Catch2 v3; failure-matrix-first testing ("test how code fails").
- Style: `PascalCase` types, `snake_case` members, `m_` privates, trailing return
  types, `[[nodiscard]]`.

## Steal / drop (from venice-py)

| Steal | Why |
|---|---|
| Command registry: "adding a subcommand = one entry" | Extensibility with zero framework |
| Config precedence **flag > env > file > default** | Predictable; users rely on it |
| Sessions as JSON envelopes; ids where lexical = chronological; atomic 0600 writes | Proven; trivially portable |
| Personas as files with realpath containment guard | Simple, safe |
| REPL slash commands from a **single table** driving dispatch + help + completion | Never drifts |
| stdout/stderr discipline; TTY-gated spinners/prompts | Automation-clean |
| Spend rails (quote → confirm → cap; cost ledger with cache buckets) | Design types for it now, build in P2 |
| Near-1:1 test ratio, issue-referencing docstrings | Culture worth keeping |

| Drop | Replacement |
|---|---|
| Mutable argparse-`args` bag, "fill if None" convention | Typed options structs + explicit precedence resolver that records provenance |
| String-keyed tool indirection (`getattr(_mcp, name)`) | Compile-time registration (function pointers / concepts) |
| `_agent.py` god-module (2144 lines) | Enforced by library layering below |
| "Tools disable streaming" | Design streaming + tool deltas together (venice-cpp delta model, VC-05) |
| Plaintext key guarded by convention only | Same storage initially, but key never enters widget/render paths by construction; keychain integration is a future ticket |

## Architecture

```
┌───────────────────────────────────────────────────────────┐
│ aiforge::cli     arg parsing, command registry,           │
│                  config resolution → typed Options        │
├───────────────────────────────────────────────────────────┤
│ aiforge::tui     termforge App subclass, layout math,     │
│                  TranscriptView, Composer, StatusBar,     │
│                  pickers; drains bridge queue in on_render│
├───────────────────────────────────────────────────────────┤
│ aiforge::core    Transcript/Message, SessionStore,        │
│                  Config types, Personas, ModelCatalog,    │
│                  markdown-lite tokenizer → styled spans   │
├───────────────────────────────────────────────────────────┤
│ aiforge::bridge  ChatWorker: std::jthread +               │
│                  venice::Client::chat_stream, thread-safe │
│                  StreamEvent queue, stop_token cancel     │
└───────────────────────────────────────────────────────────┘
        ▼                                   ▼
   termforge::lib                      venice-cpp::lib
```

`core` depends on neither termforge nor venice-cpp types where avoidable — it is the
testable middle. `tui` renders core state; `bridge` mutates core state from network
results. This is also what keeps `src/lib` from becoming the god-module.

### Threading & streaming model

- The **UI thread owns every widget**. Nothing in termforge is thread-safe and we
  don't pretend otherwise.
- One `std::jthread` worker per in-flight request runs the **blocking**
  `venice::Client::chat_stream()`. Its `on_token` callback pushes
  `StreamEvent{delta | done(response) | error(Error)}` into a mutex-guarded queue.
- termforge's loop renders every frame (~30fps) and idle-wakes ~10×/s, so the UI
  thread simply **drains the queue in `on_render`** — worst-case delta latency
  ≈ 33–100 ms, unnoticeable for streaming text. **No termforge changes required.**
  (An upstream `post_event`/wakeup would let the loop idle harder; filed as
  nice-to-have, not a dependency.)
- **Cancellation**: Esc during generation sets the stop flag; the SSE callback
  returns `false`; venice-cpp returns the partial response as success; the partial
  stays in the transcript marked `(cancelled)`. This maps exactly onto venice-cpp's
  existing cancel semantics.
- venice-cpp's `Client` is immutable with per-call transports — safe to share
  across workers without locking.

### Config & credentials

- `~/.config/aiforge/config.json` (dir 0700, files 0600, atomic
  write-fsync-rename), XDG-respecting.
- Precedence resolved **once at startup** into a typed `Options` struct: each field
  layered as `optional<T> cli / env / file` + compiled default, with provenance
  retained (so `aiforge config show` can say *where* a value came from — a thing
  venice-py couldn't do cleanly).
- API key: `$VENICE_API_KEY` > `~/.config/aiforge/credentials` (0600). The key
  lives only in `bridge`; it is never placed in any renderable string. Missing key
  → offline mode with a visible notice (already in the MVP).

### The parser question (the "third library")

What venice-py's CLI actually gets praised for is not argparse — it's the
**registry convention + layered defaults + introspective completion** built on top.
That layer must be built regardless of the underlying tokenizer, and it's
app-shaped, not library-shaped, until a second consumer exists.

Requirements for the underlying parser, written down before evaluating:
subcommands; typed values with **unset-detection** (so config layering can tell
"user passed `--temp 0.7`" from "default 0.7"); env-var binding; introspection
sufficient to generate bash/zsh completion; `std::expected`-style error reporting
(no exceptions for control flow); header-only or trivially FetchContent-able;
C++23-clean under GCC and Clang.

Known candidates (CLI11, p-ranav argparse, cxxopts, Lyra) all fail at least the
no-exceptions bar natively — parse errors throw. Options, in order of preference:

1. **Thin adapter over CLI11** — wrap parse in one try/catch at the boundary,
   convert to `expected`. Cheapest if the adapter stays thin.
2. **Build `argforge`** — a small (~1–2k LOC) parser to house style: subcommands,
   flags, positionals, typed unset-aware values, env binding, completion
   introspection. Defensible given the *forge ethos, and it becomes the natural
   home for the registry + precedence layer when extracted.

**Decision path**: timeboxed spike (AF-05) producing an ADR. Don't build argforge
preemptively; do build it without guilt if the adapter turns ugly.

### Rendering chat content

termforge sanitizes ANSI out of all text (correctly — model output is untrusted),
and today offers only one color per written run. So rich chat rendering is:

- **aiforge side**: a markdown-*lite* tokenizer (bold/italic/inline code/fenced
  code/headings/lists — no HTML, no tables v1) producing styled spans + role
  styling. Lives in `core`, pure, heavily failure-tested.
- **termforge side** (upstream tickets): styled-span write API (TF-02), word-aware
  wrap (TF-01), multiline composer with history (TF-03). Plus existing termforge#10
  (display-width) which chat hits immediately via CJK/emoji.

This split keeps termforge stdlib-only and unopinionated; markdown opinions stay in
aiforge.

## Phases

- **P0 — housekeeping**: aiforge README/AGENTS rewritten for the actual app (they
  still describe the C++ template); `.clangd` C++20→C++23 fix; **initial commit**;
  upstream consumption fixes (TF-04, VC-01) so `add_subdirectory`/FetchContent is
  clean.
- **P1 — chat v1** (the real milestone): async streaming + cancel; typed config +
  credentials; transcript renderer; composer; slash commands; sessions
  (resume/continue/ephemeral); one-shot pipe mode; model picker; personas;
  failure-matrix test scaffolding. Upstream deps: TF-01/02/03, termforge#10,
  VC-02/03.
- **P2 — Venice parity**: web search + citations + thinking surfacing (needs
  VC-05 structured deltas); characters (VC-04); spend rails + usage ledger.
- **P3 — agent mode**: tool calling end-to-end (VC epic), sandboxed exec, memory —
  deliberately not designed in detail yet; venice-py's `AgentProfile`/tool-table
  design is the reference when we get there.

## Cross-repo dependency map

| aiforge ticket | needs |
|---|---|
| Transcript renderer (AF-08) | TF-01 word wrap, TF-02 styled spans, termforge#10 |
| Composer (AF-09) | TF-03 composer widget |
| Model picker (AF-13) | VC-03 model metadata |
| Clean builds (AF-01/P0) | TF-04 consumer CMake, VC-01 dep-glob cleanup |
| P2 parity epic | VC-04 characters, VC-05 delta model |
| P3 agent epic | VC tool-calls epic |

Everything else in aiforge P1 has no upstream blocker and can start immediately.
