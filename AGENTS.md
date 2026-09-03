# AGENTS.md — conventions for AI agents working in this repo

AIForge is a C++ terminal AI client built from a provider-neutral run kernel
outward. Read `docs/ARCHITECTURE-NORTH-STAR.md` and accepted ADRs before changing
architecture. `docs/CONTEXT.md` is a subordinate proposal for repository-aware
context construction; `docs/DESIGN.md` is historical. Where either conflicts
with the north star or an accepted ADR, the governing document wins.

## Current baseline

- CMake 3.28+, C++23, GCC 13+ and Clang 17+.
- The default compiler respects the environment. Clang and sanitizers are
  opt-in toolchain files in `cmake/toolchain/`.
- Catch2 v3 for tests.
- Dependencies use `find_package` first and `FetchContent` fallback, entirely
  through CMake. A recipe is active only when named in `${PROJECT_NAME}_DEPS`.

## Architecture rules

- Dependency direction is surfaces -> runtime -> domain/ports <- adapters.
- Public domain types never expose TermForge, venice-cpp, a JSON library, a
  database, or an embedded runtime.
- Durable truth is an append-only stream of typed run events. Projections may be
  rebuilt; corrections append events instead of rewriting history.
- Provider requests contain neutral messages, content blocks, tools, and
  generation options. Credentials and raw secrets never enter events,
  renderable errors, artifacts, or captured fake requests.
- Provider characters are transient request options, not personas or an
  instruction/authority layer. They never switch models, grant tools, broaden
  permissions, or enter prompt construction as AIForge-authored text.
- Build deterministic fakes and failure cases before a production UI or network
  path depends on a new boundary.
- Fallible public operations return `std::expected`; exceptions do not cross
  API boundaries.

## Dependency and library ownership

- If a feature needed by AIForge belongs in a dependency we control, agents may
  file a feature request against that dependency. The currently controlled
  dependencies are TermForge, RasterForge, and venice-cpp.
- If functionality being added to AIForge appears broadly reusable as a generic
  library, raise it with the maintainers before embedding or extracting it. It
  may warrant a separate project; do not create one without agreement.

## C++ style

Use `PascalCase` types, `snake_case` functions and members, `m_` private data,
trailing return types, and `[[nodiscard]]` for values callers must inspect.
Prefer small value types with explicit invariants over stringly typed state.
The tree is formatted with clang-format 20.x; run `tools/format.sh --check` and
keep mechanical formatting changes separate from semantic edits.

## Tests

Tests are auto-discovered from `test/*/`. A new test normally needs only
`test/<name>/test.cpp`; re-run `cmake -B` after adding a directory. Add a local
`CMakeLists.txt` only for custom build control.

**Test how code fails, not just that it produces the right output.** Write the
failure matrix first: invalid input, boundaries, overflow, malformed external
data, cancellation, illegal state transitions, and error paths. A happy-path
check comes last as a smoke test.

## Required verification

```bash
cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure
cmake -B build-clang -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/clang.cmake \
  && cmake --build build-clang && ctest --test-dir build-clang --output-on-failure
```

A change that builds on only one supported compiler is not complete.

## Attribution

Agent-authored commits carry trailers naming the model, for example:

```text
Co-authored-by: OpenAI Codex <noreply@openai.com>
Agent: Codex / GPT-5
```

Record the verification commands actually run in PRs.

## Repository notes

- `version.hpp` is generated under each build tree. Edit
  `include/version.hpp.in.cmake`, retaining `<cstdint>`; never generate into the
  source tree.
- Build directories are ignored and must not be committed.
- Dependency pins are bumped deliberately, with the compatibility reason noted.
- Do not select a storage encoding, parser, scripting runtime, or plugin ABI
  without the milestone and ADR that make the choice necessary.
