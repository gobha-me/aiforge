# Ticket drafts — venice-cpp (gobha-me/venice-cpp)

Note on existing issue #1 ("AIForge: chat-TUI MVP — compose venice-cpp +
termforge"): the MVP now exists in the aiforge repo and the work is tracked there;
propose closing #1 with a comment linking to the aiforge issues once filed.

---

## VC-01 · Remove template-leftover dependency recipes (fmt, argparse, nats, doctest)

Labels: bug

**Context.** `cmake/dependencies.cmake` globs and includes every
`cmake/deps/*.cmake`. The directory still carries template leftovers: `fmtlib.cmake`
(fmt 11.1.4) and `argparse.cmake` (v3.0) are live and FetchContent-pull libraries
venice-cpp never uses; `nats.cmake` and `doctest.cmake` are empty stubs. Every
consumer that `add_subdirectory`s venice-cpp (aiforge does, via sibling checkout)
inherits the useless fetches and their configure-time cost.

**Scope.** Delete the four files. Keep httplib, nlohmann-json, openssl, catch2.

**Acceptance.** Fresh configure of venice-cpp and of a consumer using
`add_subdirectory` fetches only httplib/json/catch2; build stays green on GCC +
Clang.

---

## VC-02 · ChatRequest: fill out standard sampling/control parameters

Labels: enhancement

**Context.** `ChatRequest` supports only `temperature` and `max_tokens`
(`include/venice/types.hpp`). Missing standard fields a chat client needs to expose:
`top_p`, `stop`, `frequency_penalty`, `presence_penalty`, `seed`,
`response_format` (JSON mode). All are API-supported.

**Scope.** Add as `std::optional` fields following the existing
only-set-fields-serialize idiom in `to_json_body()`. `stop` as
`optional<std::vector<std::string>>`. `response_format` minimally as an enum or
raw-json passthrough — implementer's call, documented. Tool-calling fields are
explicitly *not* this ticket (see tools epic).

**Acceptance.** Serialization round-trip tests per field; absent fields keep today's
byte-identical bodies (regression-tested against existing fixtures).

---

## VC-03 · Model: typed metadata (context window, capabilities, pricing)

Labels: enhancement

**Context.** `Model` carries only `id` + `type`, discarding everything else
`/models` returns. A model picker needs context length, capability flags (vision,
function-calling, reasoning), and pricing; a spend ledger (downstream P2) needs
per-token prices. Consumers currently must re-fetch and re-parse raw JSON.

**Scope.**
- Extend `Model` with typed optional fields for context length, capability flags,
  and pricing buckets (prompt/completion; keep cache buckets distinct, matching the
  existing `Usage` philosophy).
- Preserve a raw `nlohmann::json extra` escape hatch for unmodeled fields, matching
  `VeniceParameters::extra`.
- Tolerant parsing: absent/unknown fields → `nullopt`, never an error.

**Acceptance.** Fixture tests from a captured live `/models` payload; malformed and
partial entries degrade to nullopt fields rather than failing the whole list.

---

## VC-04 · Characters endpoint: typed listing

Labels: enhancement

**Context.** Chat can pass `venice_parameters.character_slug`, but there is no way
to discover characters — venice-py exposes character selection, and a TUI picker
downstream needs a list.

**Scope.** `Client::characters() -> expected<std::vector<Character>, Error>`
against the public characters endpoint; `Character` with slug/name/description plus
`extra` passthrough; same tolerant-parse rules as VC-03.

**Acceptance.** Fixture-based offline tests incl. empty list and malformed entries;
smoke-binary path behind `VENICE_API_KEY` like existing endpoints.

---

## VC-05 · Streaming: structured delta callback (design ticket)

Labels: enhancement

**Context.** `chat_stream` delivers content deltas only; role deltas, thinking
tokens, web-search citations, and usage frames are dropped or folded in
(`include/venice/client.hpp` SSE path). Downstream wants to render thinking
distinctly, show citations, and meter usage live — and the venice-py lesson
("tools imply non-streamed", a wart we're not repeating) says the delta model must
be designed with tool-call deltas in mind even if tools land later.

**Scope (design first, then implement).**
- A `StreamDelta` variant/struct: content delta, thinking delta, citations,
  usage frame, finish — with room for tool-call deltas without breaking the ABI of
  the callback signature again.
- New overload `chat_stream(req, std::function<bool(const StreamDelta&)>)`; keep
  the existing `string_view` overload as a thin adapter (no silent behavior change
  for current callers).
- Cancellation semantics unchanged (return false → partial success).

**Acceptance.** Design note in the issue reviewed before implementation; then SSE
fixture tests covering interleaved delta kinds, partial frames across chunk
boundaries, and `[DONE]`.

---

## VC-06 · Transport: per-request timeout override + non-streaming cancellation

Labels: enhancement

**Context.** `make_transport()` hardcodes 300s read / 30s connect; `chat()`
(non-streaming) cannot be cancelled at all, and a TUI cannot hang for minutes on a
dead connection with no recourse.

**Scope.**
- Optional timeout overrides per request (fields on `ChatRequest` or a small
  `RequestOptions` parameter — implementer's call; `RequestOptions` scales better).
- A cancellation mechanism for non-streaming calls, e.g. routing `chat()` through
  the same content-receiver path so a predicate/token can abort mid-body. Aborted
  call returns `ErrorKind::Network` (or a new `Cancelled` kind — decide in-issue)
  rather than partial success, to keep semantics distinct from stream-cancel.

**Acceptance.** Tests with a stalling local httplib server: connect timeout,
read timeout, and cancel-mid-response all return promptly with the documented
error kind on both compilers.

---

## VC-07 · CMake: install/export target

Labels: enhancement

**Context.** No `install()`/`export()` (root `# TODO Install Template`), so the
ecosystem's own `find_package(... QUIET)`-first convention can never hit for
venice-cpp; consumers are forced into FetchContent/add_subdirectory. Header-only
INTERFACE target makes this cheap.

**Scope.** Package config + version file + exported `venice-cpp::lib`; gate
tests/smoke-bin on `PROJECT_IS_TOP_LEVEL` (same shape as the termforge ticket
TF-04). Fix the stale `venice-cpp::venice` alias mention in README while in there.

**Acceptance.** External consumer builds via installed `find_package(venice-cpp
CONFIG)` and via `add_subdirectory` with nothing extra built; OpenSSL/httplib/json
usage requirements propagate correctly through the export.

---

## VC-08 · Epic: tool/function-calling support

Labels: enhancement, epic

**Context.** `Message` is role+content only; `ChatRequest` has no `tools`;
`ChatResponse` collapses to `choices[0].message.content`; streaming drops tool-call
deltas. This is the API-side blocker for aiforge's agent mode (P3) — venice-py's
agent loop (its `--tools` chat and `code` command) is the feature ceiling we're
porting toward.

**Scope (staged, each a child issue when picked up).**
1. Request side: `tools` (JSON-schema function specs), `tool_choice`.
2. Response side: tool-call results in `ChatResponse` (id/name/arguments), multi
   tool-call support; `Message` grows the assistant-tool-call and tool-result
   shapes for the follow-up turn.
3. Streaming: tool-call argument deltas via the VC-05 `StreamDelta` model.

Not scheduled for P1; filed now so VC-05's delta design accounts for it.

---

## VC-09 · Epic: image generation endpoints

Labels: enhancement, epic

**Context.** venice-py's most-used non-chat feature. Out of Phase-0 scope by
design; filed as the P2/P3 marker. Covers `/image/generate` (+ upscale/edit
later), async-job handling, and returning bytes vs saving — API surface to be
designed in-issue. Note: termforge can render results in-terminal via kitty
pixel regions, but decode (PNG→RGBA) belongs to the consumer per termforge's
no-decode policy.
