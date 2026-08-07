# Ticket drafts — termforge (gobha-me/termforge)

Checked against existing issues #3–#23; none of these duplicate. termforge#10
(display width) is a chat-app dependency but already filed — referenced, not
re-filed.

---

## TF-01 · TextBox: word-aware wrapping (currently hard byte-column wrap)

Labels: enhancement

**Context.** `TextBox::wrap_into` (`src/lib/widgets/text_box.cpp`) wraps at a fixed
column boundary, only avoiding mid-UTF-8-sequence splits, despite the header
advertising word-wrap. Prose (the main payload of a chat scrollback, TextBox's
stated use case) breaks mid-word on every line.

**Scope.**
- Wrap at whitespace when a break point exists within the line width; fall back to
  hard split for unbroken runs longer than the width.
- Preserve existing behavior guarantees: never split a UTF-8 sequence; stable
  output on re-wrap after resize.
- Column math should go through the same width abstraction #10 introduces (or be
  written so #10 can swap it in) — don't add more byte-length-as-width call sites.

**Acceptance.**
- Failure-matrix tests: width smaller than longest word, trailing/leading/multiple
  spaces, width 1, empty line, multibyte at the break point, line exactly at width.
- `examples/chat.cpp` shows clean prose wrapping.

---

## TF-02 · Styled text spans: per-span fg/bg within a line (Screen + TextBox)

Labels: enhancement

**Context.** `Screen::write_text(x, y, text, fg, bg)` applies one style to the
whole run, and `TextBox::append` hardcodes a single fg for every line. Sanitization
(correctly) strips ANSI from content, so there is currently *no way* to render a
line with mixed colors — which a chat client needs for role prefixes, inline code,
bold/emphasis, syntax-ish highlighting. Consumers can hand-place multiple
`write_text` calls, but TextBox (which owns wrapping/scrollback) cannot.

**Scope.**
- A minimal styled-run type, e.g. `Span{ std::string text; Style style; }` with
  `Style{ Rgb fg; Rgb bg; /* room for bold/underline later */ }`, and a
  `StyledLine = std::vector<Span>`.
- `TextBox::append(StyledLine)` overload (plain-string overload stays and wraps to
  a single-span line). Wrapping (TF-01) must operate across span boundaries.
- Keep the sanitization boundary: span text goes through the same
  `Screen::sanitize` path; styles are data, never escape codes.
- Explicitly out of scope: markup parsing (markdown etc. is consumer-side), OSC 8
  hyperlinks, attributes beyond fg/bg unless free.

**Acceptance.**
- Tests: span boundary exactly at wrap column, zero-length spans, style continuity
  across wrapped continuation lines, sanitization still applied per-span.
- Renderer diff output unchanged for single-span lines (no regression in the
  diff-only present path).

---

## TF-03 · Composer widget: multiline input with edit history

Labels: enhancement

**Context.** `TextInput` is single-line, no history; the chat demo
(`src/bin/main.cpp`) hand-rolls a draft string instead of using it. Any
REPL/chat-style consumer needs a composer: multiline drafts, up/down history
recall, and a growing-height widget.

**Scope.**
- Either a `Composer` widget or a multiline mode on `TextInput` — implementer's
  call; keep the existing convention that Enter/Escape submission semantics are
  configurable/parent-owned (Enter=submit vs newline, e.g. Alt+Enter or
  Shift+Enter for newline where the terminal can report it, with a documented
  fallback).
- Cursor movement across lines (Left/Right/Up/Down/Home/End), insert/delete across
  line boundaries, height = min(content lines, max_height) reported to the parent
  for layout.
- History ring: `push_history(entry)`, Up/Down recall with the classic
  "draft-in-progress is preserved at the bottom of the ring" behavior.
- Bracketed paste (already surfaced by Input) inserts verbatim including newlines.
- Byte-offset cursor is acceptable initially but route column math through the #10
  width abstraction as with TF-01.

**Acceptance.**
- Failure-matrix tests: recall with empty history, editing a recalled entry then
  navigating away and back, paste containing \r\n, cursor at start/end boundaries,
  max_height overflow scrolling.
- `examples/chat.cpp` migrated to the composer.

---

## TF-04 · CMake: clean consumption via add_subdirectory / FetchContent (+ install/export)

Labels: enhancement

**Context.** Downstream projects (aiforge is the first) consume termforge as a
sibling `add_subdirectory` or FetchContent. Today that drags in bin/examples/tests
(options default ON unconditionally), and the root CMakeLists defaults
`CMAKE_TOOLCHAIN_FILE` — behavior a consumer should never inherit. There is no
`install()`/`export()` (root has `# TODO Install Template`), so `find_package`
first never succeeds.

**Scope.**
- Gate consumer-irrelevant behavior on `PROJECT_IS_TOP_LEVEL`: tests/examples/bin
  default OFF when not top-level; never touch `CMAKE_TOOLCHAIN_FILE` when not
  top-level.
- Add `install()` + `export()` + package config so
  `find_package(termforge CONFIG)` works, honoring the ecosystem's
  find_package-first-then-FetchContent convention.
- Document the two consumption paths in README.

**Acceptance.** A minimal external project builds against termforge both ways
(`add_subdirectory` with zero extra options, and installed `find_package`) on GCC
and Clang, building only `termforge_lib`.

---

## TF-05 · App: cross-thread wakeup / event injection (post_event)

Labels: enhancement

**Priority: low / nice-to-have.** Streaming consumers work fine today by mutating
guarded state and letting the ~30fps poll pick it up; this is about letting the
loop idle harder and cutting delta latency, not unblocking anyone.

**Context.** The event loop (`src/lib/core/app.cpp`) polls with a fixed frame
budget; there is no self-pipe/eventfd, so a background thread cannot wake the loop
or inject an `Event`.

**Scope.**
- A thread-safe `App::post(Event)` (or a narrower `App::wake()`) backed by a
  self-pipe/eventfd integrated into the read wait.
- Posted events are delivered through the normal `on_event` path on the loop
  thread; document that this is the *only* thread-safe entry point and widgets
  remain single-threaded.
- With a wakeup available, the idle frame rate could drop when nothing is dirty —
  optional follow-on, note it in the issue.

**Acceptance.** TSan-clean test: N producer threads posting while the loop runs;
events arrive in order per-producer; no wakeup lost when posted during render.
