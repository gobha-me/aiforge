# Ticket drafts — aiforge (gobha-me/aiforge)

Repo currently has no issues and no commits. See docs/DESIGN.md for the
architecture these tickets implement. Phases: P0 housekeeping, P1 chat v1,
P2 parity, P3 agent.

---

## AF-01 · P0: repo housekeeping — real docs, .clangd fix, initial commit

Labels: documentation

**Context.** README.md and AGENTS.md still describe the generic C++ template this
repo was forked from (STATIC-lib default, example tests that don't exist), while
the code is already a concrete chat app. `.clangd` pins `-std=c++20` against a
C++23 build — false diagnostics on C++23 features. Nothing is committed yet.

**Scope.**
- Rewrite README for AIForge (what it is, build, VENICE_API_KEY/AIFORGE_MODEL,
  sibling-checkout dev flow); rewrite AGENTS.md keeping the shared conventions
  (toolchain rules, find_package→FetchContent, failure-first testing, dual-compiler
  gate) but scoped to this app; keep the template's testing-philosophy section.
- Fix `.clangd` to C++23.
- Land docs/DESIGN.md; make the initial commit.

**Acceptance.** Fresh clone + sibling checkouts builds and runs per README on GCC
and Clang; clangd shows no std-version false positives.

---

## AF-02 · P0: CMake consumption hygiene for termforge/venice-cpp deps

Labels: enhancement

**Context.** `cmake/deps/{termforge,venice_cpp}.cmake` prefer sibling checkouts
then FetchContent — good. But consuming either today drags in their
tests/examples/bins and (venice-cpp) template-leftover fetches of fmt/argparse.

**Scope.** Pass the right options (`*_TESTS=OFF`, `*_EXAMPLES=OFF`) from the dep
recipes; pin FetchContent to tags/commits rather than `main` once upstream tags
exist. Tracks upstream TF-04 (top-level guards, install/export) and VC-01
(dep-glob cleanup); simplify here when those land.

**Acceptance.** `cmake --build` from scratch builds exactly: termforge_lib,
venice-cpp headers' deps (httplib/json/openssl), aiforge lib+bin, Catch2 for our
tests — nothing else.

---

## AF-03 · P1: async streaming chat — worker thread, delta queue, Esc-to-cancel

Labels: enhancement

**Context.** `src/bin/main.cpp` `send()` calls the blocking `venice::Client::chat()`
on the UI thread — the documented MVP limitation: UI freezes for the whole
completion, no streaming, no cancel.

**Scope.**
- `aiforge::bridge::ChatWorker`: one `std::jthread` per in-flight request running
  `chat_stream()`; `on_token` pushes `StreamEvent{delta | done(ChatResponse) |
  error(venice::Error)}` into a mutex-guarded queue.
- UI thread drains the queue in `on_render` and appends to the transcript
  (termforge's ~30fps poll makes this ≤~100ms latency; no upstream changes
  needed).
- Esc during generation → `stop_token` → callback returns `false` → partial
  response kept in transcript, visibly marked cancelled. Composer stays editable
  while streaming; a second submit while in-flight is rejected with a status
  message (v1: one request at a time).
- Errors render as a styled transcript/status line using `Error::kind/status`,
  never lost to stderr in TUI mode.

**Acceptance.** Failure-matrix tests on the queue and worker lifecycle (cancel
before first delta, cancel after done, error mid-stream, worker outliving a closed
request, TSan-clean). Manual: visible token streaming; Esc leaves a marked partial.

---

## AF-04 · P1: typed config system with provenance-tracking precedence

Labels: enhancement

**Context.** venice-py's precedence (flag > env > config.json > default) is the
behavior to keep; its implementation (mutating an untyped args bag, "fill if None")
is the wart to drop.

**Scope.**
- `~/.config/aiforge/config.json` (XDG-respecting; dir 0700, file 0600, atomic
  write-fsync-rename). Malformed file → warn to stderr + defaults, never crash.
- Typed `Options` struct resolved once at startup from cli/env/file layers; each
  field records provenance so `aiforge config show` can print value + source.
- `config get/set/unset` subcommand with dotted keys, à la venice-py.
- Unknown keys in the file are preserved on write (round-trip safety).

**Acceptance.** Failure-matrix tests: every precedence permutation for a scalar, a
bool (the tri-state trap that plagued venice-py), and a string list; malformed
file; unwritable dir; round-trip preserving unknown keys.

---

## AF-05 · P1: parser decision — requirements, spike, ADR (the "third library" question)

Labels: enhancement

**Context.** The venice-py CLI's praised extensibility is mostly the registry +
layered-defaults convention, which we build regardless (AF-06). The open question
is the tokenizer/flag-parser underneath: existing C++ parsers (CLI11, p-ranav
argparse, cxxopts, Lyra) all use exceptions for parse errors, against house style.

**Scope.**
- Write the requirements list into the issue (subcommands; typed values with
  unset-detection for layering; env binding; completion introspection;
  expected-based errors; FetchContent-able; C++23 GCC+Clang clean).
- Timeboxed spike: CLI11 (and/or argparse) behind a thin adapter converting throws
  to `std::expected` at the boundary, wired to two real subcommands.
- Decide: adapter good enough vs build `argforge` (small bespoke parser, ~1–2k
  LOC, natural future home for registry+precedence if extracted). Record as an ADR
  in docs/.

**Acceptance.** ADR merged; follow-on ticket(s) filed for the chosen path. Blocks
final form of AF-06/AF-07 (they can proceed on the adapter meanwhile).

---

## AF-06 · P1: command registry — one entry to add a subcommand

Labels: enhancement

**Context.** Port the venice-py convention ("adding a subcommand = one import +
one tuple entry") to compile-time C++: no string-keyed getattr indirection.

**Scope.**
- `CommandSpec{ name, help, register_flags, handler }`; a static registry table;
  dispatcher maps argv → command → typed options (via AF-04 resolver) → handler
  returning an exit code.
- v1 commands: default (chat TUI), `chat` (one-shot), `models`, `config`,
  `version`. Structure must make the venice-py media commands additive later.
- Registry is introspectable: `help` output and (later) shell completion generate
  from the same table — single source of truth, venice-py's completion trick.

**Acceptance.** Adding a toy command in a test touches exactly one registration
site; help text is generated, not hand-maintained; unknown command/flag errors go
to stderr with exit code 2.

---

## AF-07 · P1: one-shot / pipe mode + stdout–stderr discipline

Labels: enhancement

**Context.** venice-py's second face: `venice chat "msg"` streams to stdout and
pipes cleanly. The MVP is TUI-only.

**Scope.**
- `aiforge "prompt"` / `echo x | aiforge "explain"`: no TUI; stream deltas to
  stdout as they arrive; citations/usage/status to stderr; exit code reflects
  success/API failure. stdin (when not a TTY) is read and appended as context.
- Face selection: TUI iff stdout **and** stdin are TTYs, overridable with
  `--tui/--no-tui`. `--json` for the full response envelope.
- Hard rule repo-wide (documented in AGENTS.md): content → stdout; all human
  chrome → stderr, TTY-gated. Spinners never appear when stderr is piped.

**Acceptance.** Tests run the binary with redirected pipes: stdout contains only
the completion; `--json` parses; exit codes on auth failure (offline mode prints
to stderr, exits nonzero in one-shot).

---

## AF-08 · P1: transcript model + markdown-lite renderer

Labels: enhancement
Depends: termforge TF-01 (word wrap), TF-02 (styled spans); termforge#10 improves CJK/emoji

**Context.** Chat content needs role-styled, word-wrapped, lightly-rich rendering.
termforge (correctly) strips ANSI from content and, pre-TF-02, is single-color per
line.

**Scope.**
- `aiforge::core` transcript model: `Turn{ role, content, state(streaming |
  complete | cancelled | error), usage? }`, independent of both termforge and
  venice types (the testable middle).
- Markdown-lite tokenizer (pure function, `core`): bold/italic/inline
  code/fenced code blocks/headings/lists → `StyledLine` spans. Explicitly not:
  HTML, tables, images, link resolution. Unclosed/nested-weird markup degrades to
  literal text — never panics, never drops content.
- `tui::TranscriptView`: renders turns into the TextBox via TF-02 spans with a
  role color scheme + streaming turn updated in place; graceful plain-text
  fallback path until TF-01/02 land (current single-color behavior).

**Acceptance.** Tokenizer failure-matrix: unterminated fence, nested emphasis,
code span containing backticks, 10MB single line (perf sanity), CRLF input.
Fallback path keeps the app usable against unpatched termforge.

---

## AF-09 · P1: composer — multiline input, history, editor escape

Labels: enhancement
Depends: termforge TF-03

**Context.** The MVP hand-rolls a single-line draft with its own UTF-8 encoder in
`main.cpp`.

**Scope.**
- Adopt the TF-03 composer widget: multiline draft, Enter=submit /
  Shift-or-Alt+Enter=newline (per TF-03 semantics), history recall across the
  session (persisted history file `~/.config/aiforge/history`, 0600, capped).
- `/paste` unnecessary (bracketed paste covers it) but `/edit` opens
  `$VISUAL`/`$EDITOR` on the draft (suspend TUI, restore raw mode after — venice-py
  git-style flow).
- Delete the inline UTF-8 encoder from main.cpp (Input already handles encoding).

**Acceptance.** History survives restart; editor round-trip preserves multiline
drafts; terminal state sane after editor exit (including editor crash — failure
test with `EDITOR=false`).

---

## AF-10 · P1: slash-command registry (single table → dispatch + help + completion)

Labels: enhancement

**Context.** venice-py's `_COMMANDS` tuple driving dispatcher *and* tab-completion
*and* help from one table is the "never drifts" property to keep.

**Scope.**
- One static table `SlashCommand{ name, args_hint, help, handler }`; typing `/`
  in the composer enters command context; `/help` renders from the table.
- v1 set: `/help /quit /clear /model [id] /system [text] /persona [name] /session
  (list|resume <id>|new) /edit /copy`(last response to OSC 52 if feasible, else
  drop). Unknown `/x` → status-line error, message not sent to the API.
- Completion (Tab) over command names + model ids from the same table/catalog.

**Acceptance.** A test asserts help text, dispatch table, and completion source
enumerate identically (the anti-drift property, mechanically enforced).

---

## AF-11 · P1: session persistence — envelopes, resume, continue, ephemeral

Labels: enhancement

**Context.** Port venice-py's proven design: JSON envelope per session (id,
timestamps, model, system, gen params, usage, transcript), ids
`YYYYmmddTHHMMSS-<6hex>` so lexical sort = chronological, atomic autosave after
every committed turn.

**Scope.**
- `~/.config/aiforge/sessions/<id>.json`, 0600, atomic writes;
  `$AIFORGE_SESSIONS_DIR` override.
- `--resume <id|path>`, `--continue` (most recent), `--ephemeral` (no writes);
  `/session list|resume|new` in-TUI (ties into AF-10).
- Version field in the envelope from day one; unknown fields preserved on
  rewrite. Cancelled partials persist with their cancelled mark.

**Acceptance.** Failure-matrix: resume of truncated/corrupt JSON (clear error, no
crash, file untouched), disk-full simulation on autosave (turn survives in memory,
error surfaced), two instances on the same session (last-writer-wins documented),
round-trip byte-stability for unknown fields.

---

## AF-12 · P1: personas — file-backed system prompts

Labels: enhancement

**Context.** venice-py personas: `~/.config/aiforge/personas/<name>.md|.txt`,
bare-name resolution with a realpath containment guard so `--persona
../credentials` can't read outside the personas dir.

**Scope.** `--persona <name>` flag + `/persona` slash command (list = dir listing;
set = swap system prompt, takes effect next turn and is recorded in the session
envelope). Containment guard + symlink-escape test ported as spec.

**Acceptance.** Traversal/symlink escape attempts rejected with a clear error;
persona-vs-`--system` precedence defined (explicit `--system` wins) and tested.

---

## AF-13 · P1: model picker

Labels: enhancement
Depends: venice-cpp VC-03 (typed model metadata)

**Context.** Model choice is env-var-only in the MVP (`AIFORGE_MODEL`); venice-py
validates against the live catalog and browses it.

**Scope.**
- `aiforge models` (one-shot table: id, type, context, capabilities, pricing —
  respecting AF-07 discipline) and an in-TUI picker on `/model` (termforge
  ListWidget) with filter-as-you-type.
- Catalog fetched once per run, cached to config dir with TTL; `--model` values
  validated against it with a did-you-mean on miss (venice-py behavior).
  Degrades gracefully offline: picker shows cached, warns stale.

**Acceptance.** Picker works from cache with network down; invalid `--model` in
one-shot mode exits 2 with suggestions on stderr.

---

## AF-14 · P1: credentials handling

Labels: enhancement

**Context.** MVP reads `$VENICE_API_KEY` only. venice-py stores a plaintext key
with atomic 0600 writes and a never-print convention; we keep the storage (keychain
integration is future work) but make non-exposure structural.

**Scope.**
- Precedence `$VENICE_API_KEY` > `~/.config/aiforge/credentials` (0600, atomic
  write; warn on loose perms). `aiforge login` prompts with echo off (one-shot
  mode only; no key entry inside the TUI).
- The key lives only in `bridge` (constructor-injected into `venice::Client`);
  never stored on any type the render path can reach — enforced by layering, and a
  test greps rendered frames for the key during a fake session.
- Missing key → existing offline-notice behavior in TUI; nonzero exit in one-shot.

**Acceptance.** Perm-warning, echo-off prompt, and the frame-grep test pass;
`credentials` file never written world-readable even under concurrent writes.

---

## AF-15 · P1: failure-matrix test scaffolding

Labels: enhancement

**Context.** AGENTS.md preaches failure-first testing; the repo has zero tests
(only the shared Catch2 main). The template's test harness (per-dir discovery,
fixtures) is in place and unused.

**Scope.** Stand up the first suites mirroring the lib layering — `test/01config`,
`test/02bridge` (queue/worker with a fake streaming client behind an interface —
decide seam: template injection vs small virtual client wrapper), `test/03markdown`,
`test/04session` — each written failure-matrix-first per AGENTS.md. Wire CI later
(separate ticket when repo goes public-active); locally `ctest` green on GCC +
Clang + ASan/TSan toolchains.

**Acceptance.** `ctest` runs all suites via the existing harness; AGENTS.md's
referenced-but-missing canonical example replaced by pointing at these real suites.

---

## AF-16 · P2 Epic: Venice feature parity — web search, citations, thinking, characters

Labels: enhancement, epic
Depends: venice-cpp VC-04, VC-05

Surface `venice_parameters` in config/flags/slash-commands (web search on/off/auto,
citations, thinking control); render citations as a distinct styled block and
thinking as a collapsed/dim region (needs VC-05 structured deltas); `/character`
picker over VC-04. Child issues on pickup.

---

## AF-17 · P2 Epic: spend safety rails

Labels: enhancement, epic

Port venice-py's rails: balance display (status bar), per-session usage ledger
metered from catalog pricing (cache buckets distinct — needs VC-03 pricing),
`--max-spend`/`--session-max-spend` caps, quote→confirm for future paid media
commands. Design the `Usage` accumulation into the session envelope (AF-11) now so
ledgers backfill.

---

## AF-18 · P3 Epic: agent mode — tools, exec, memory

Labels: enhancement, epic
Depends: venice-cpp VC-08 (tools epic)

The venice-py agent loop (`--tools`, `code`/vcoder, scout/spawn, confirm gates,
write-roots) is the reference. Deliberately not designed yet; this epic exists so
P1/P2 decisions (VC-05 delta model, AF-06 registry, AF-17 rails) keep it reachable.
First child issue when opened: port the venice-py tool-table design (typed
ToolSpec registry, capability categories, paid flags) to compile-time C++.
