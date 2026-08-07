# Ticket drafts — cpp-template (gobha-me/cpp-template)

The template is the common ancestor of termforge, venice-cpp, and aiforge, so
every defect here ships N times. Several downstream issues trace directly back to
it (termforge#14's sanitizer no-ops and overbroad .gitignore; venice-cpp VC-01's
leftover dep fetches; aiforge AF-01's stale docs and .clangd pin). Two themes:
**usability** (the "hard to remember how to use it" pain) and **upstreaming the
fixes the forks taught us**.

---

## CT-01 · New-project bootstrap: checklist + prune script (the "easy button" for the easy button)

Labels: enhancement

**Context.** The template's stated purpose is an easy-button starter, but
"copy/paste then remember what to edit" demonstrably fails: aiforge shipped with
the template's README/AGENTS verbatim (docs describe a template, code is a chat
app), and venice-cpp shipped dep recipes that FetchContent-pull fmt and argparse
it never uses. There is no written path from "copied the tree" to "clean new
project," so each fork reinvents it and misses steps.

**Scope.**
- `NEW_PROJECT.md`: an ordered checklist — rename/verify `project()` (or accept
  auto-naming), prune `cmake/deps/` to what you use (see CT-02), gut the demo
  `main.cpp`/`lib.cpp`, replace README/AGENTS intro with project-specific text
  (keeping the shared-conventions sections), pick STATIC vs INTERFACE in
  `src/lib/`, tag `v0.1.0` so versioning works, first build on GCC **and** Clang.
- Optionally a `bootstrap` script (`cmake -P` or shell) that does the mechanical
  parts interactively: project name, which deps to keep, STATIC/INTERFACE, wipe
  demo code, re-init git. Script is nice-to-have; the checklist is the ticket.
- README gains a "Cheat sheet" section: the five commands you forget (configure
  with a toolchain, add a dep, add a test, run one test, cut a release tag).

**Acceptance.** A from-scratch dry run following only NEW_PROJECT.md produces a
clean project with no template artifacts (no unused dep fetches, no template
prose in docs) — verified by doing one.

---

## CT-02 · Dependencies: declarative opt-in list instead of glob-include-everything

Labels: enhancement

**Context.** `cmake/dependencies.cmake` globs `cmake/deps/*.cmake` and includes
all of them. Whatever files exist get fetched — which is how venice-cpp ended up
pulling fmt+argparse forever (VC-01). Empty stubs (`nats.cmake`, `doctest.cmake`)
add confusion. The recipes themselves (find_package-first, overridable URI/TAG)
are good; the activation mechanism is the footgun.

**Scope.**
- Root CMakeLists declares `set(${PROJECT_NAME}_DEPS catch2 fmtlib argparse)`
  (the template's demo set); `dependencies.cmake` includes exactly those. Adding
  a dep = drop a recipe file **and** list it — one visible line at top level.
- Delete the empty `nats.cmake` and `doctest.cmake` stubs.
- Recipe template documented in a comment header of one canonical recipe
  (find_package QUIET → FetchContent with `<Dep>_URI`/`<Dep>_TAG` overrides).

**Acceptance.** Removing a dep from the list (without deleting its recipe file)
skips its fetch entirely; fresh configure fetches only listed deps. NEW_PROJECT.md
(CT-01) references the list as the prune point.

---

## CT-03 · Bug: sanitizer toolchains are silent no-ops; flags gaps

Labels: bug

**Context.** `address.cmake` does `find_library(ASAN asan)` then guards on
`ASAN_FOUND` — a variable `find_library` never sets (it sets `ASAN`). Same
pattern in `thread.cmake` with `TSAN_FOUND`. Both sanitizer toolchains therefore
add **no flags, ever**, while printing nothing — you think you ran under ASan and
didn't. Inherited by every fork (filed downstream as part of termforge#14).
Additional gaps: sanitize flags are appended only to `CMAKE_CXX_FLAGS_DEBUG` (a
`-DCMAKE_BUILD_TYPE=Release` + address.cmake combo silently does nothing), no
linker flags are set, and `default.cmake` sets `CMAKE_CXX_FLAGS_RELWITHDEBINFO ""`
— wiping CMake's default `-O2 -g` for that build type.

**Scope.**
- Fix the guards (check the result variable, or better: drop find_library —
  compiler support is what matters — and add flags unconditionally with a
  `message(STATUS)`).
- Apply `-fsanitize=...` to compile **and** link flags, independent of build
  type; keep `-fno-omit-frame-pointer` (address) and the `-O2` recommendation
  (thread) documented.
- Remove or properly set the RelWithDebInfo override in `default.cmake`.
- Add a UBSan toolchain (`undefined.cmake`) while in there — same shape, cheap,
  and the family's AGENTS files already talk about UBSan-clean.

**Acceptance.** A deliberately-broken test (heap overflow / data race) fails
under the respective toolchain and passes under default — proving the sanitizers
actually engage. Verified on GCC and Clang.

---

## CT-04 · Consumer-clean CMake: PROJECT_IS_TOP_LEVEL guards + install/export (resolve the TODO)

Labels: enhancement

**Context.** Root CMakeLists unconditionally defaults `CMAKE_TOOLCHAIN_FILE`,
builds `src/bin`, and enables tests — so any project consumed via
`add_subdirectory`/FetchContent drags all of it along. The file ends with `# TODO
Install Template`, and because the template never answered it, termforge (TF-04)
and venice-cpp (VC-07) are now each solving install/export from scratch — the
exact "reinventing per project" this repo exists to prevent.

**Scope.**
- Gate on `PROJECT_IS_TOP_LEVEL`: toolchain-file defaulting, `src/bin`, tests,
  and any `option()` defaults flip to OFF when consumed.
- Add working `install()` + `export()` + package-config for both lib variants
  (STATIC and INTERFACE), replacing the TODO. `find_package(<project> CONFIG)`
  becomes real, closing the loop on the family's find_package-first convention.
- Drop `find_package(Git REQUIRED)` from the root (version.cmake already
  degrades gracefully without git; REQUIRED breaks tarball consumers).

**Acceptance.** A demo consumer builds against the template project three ways —
`add_subdirectory`, FetchContent, installed `find_package` — building only the
library target each time. Downstream tickets TF-04/VC-07 can then crib the
pattern instead of inventing two.

---

## CT-05 · Finish version.cmake: harden tag parsing, define dirty/distance semantics

Labels: enhancement

**Context.** README marks git-tag versioning as WIP, and the history agrees
(commit "git tag str meet realworld, I think it might be a bug in cmake"). The
regex `^[rv]?([0-9]+([^-][0-9]+)+)((-.+)+)?$` is fragile against real `git
describe` output (`v1.2.3-5-gabc1234-dirty`), the captured dirty/distance suffix
is stored in `DIRTY_BRANCH` and never used, and the no-tag fallback `0.0.0.1`
bakes a meaningless 4th component into `project(VERSION)`.

**Scope.**
- Parse `git describe --tags --dirty` properly: tag → MAJOR.MINOR.PATCH,
  commits-since-tag → TWEAK (this is the "ease release, no commits just to roll
  a version" intent, finished), dirty → recorded and exposed (e.g. a
  `VERSION_DIRTY` define in version.hpp.in) rather than dropped.
- Fallback without git/tags: `0.0.0` + a STATUS message saying why.
- Because this is pure string logic, add a `cmake -P` self-test script with a
  table of realistic describe strings (annotated/lightweight tags, `v`/`r`
  prefixes, dirty, no-tag hash-only output) — runnable in CI (CT-08).

**Acceptance.** Self-test table passes; `message(STATUS "${PROJECT_NAME}:${VERSION}")`
shows sane values in: tagged clean, tagged+ahead, dirty, and untagged clones.

---

## CT-06 · .clangd: stop contradicting the build (C++20 pin, hardcoded flags)

Labels: bug

**Context.** `.clangd` adds `-std=c++20`, `-xc++`, and `Compiler: clang++` while
the build is C++23 — false diagnostics on C++23 code, and the file was copied
verbatim into at least aiforge (AF-01 fixes its copy; this fixes the source).
The template also exports `compile_commands.json` by default, which clangd
prefers — most of the hardcoded flags fight the accurate source of truth.

**Scope.** Set `-std=c++23` (or drop the std/compiler overrides entirely and
lean on the compile database — decide in-issue; document the choice in a comment
at the top of `.clangd`). Keep the clang-tidy check configuration as-is.

**Acceptance.** clangd on a file using C++23-only features (e.g. `std::expected`)
shows no std-version diagnostics in a fresh checkout after `cmake -B build`.

---

## CT-07 · .gitignore: overbroad patterns (`*build*`, `Testing*`)

Labels: bug

**Context.** `*build*` ignores anything with "build" in its name anywhere in the
tree — `docs/building.md`, `src/rebuild.cpp`, a `buildinfo.hpp` — silently
excluded from git. Inherited downstream (called out in termforge#14). `Testing*`
(for CTest's dir) has the same shape-risk for files like `TestingNotes.md`.

**Scope.** Narrow to directories: `/build*/`, `/Testing/` (CTest's output dir at
root), keep `include/version.hpp`. Audit forks' .gitignore in their own repos
(termforge already tracked in #14).

**Acceptance.** `git check-ignore -v docs/building.md src/rebuild.cpp` matches
nothing; `git check-ignore build/ build-clang/ Testing/` still matches.

---

## CT-08 · CI: dual-compiler + sanitizer matrix enforcing the AGENTS.md rule

Labels: enhancement

**Context.** AGENTS.md's core verification rule is "both compilers, always" (and
it credits that rule for catching real breakage) — but nothing enforces it; the
template itself can rot unnoticed, and every fork writes CI from scratch or goes
without (aiforge has none).

**Scope.**
- GitHub Actions workflow: {GCC 13, Clang 17+} × {default, address.cmake,
  thread.cmake toolchains}, build + ctest, plus the CT-05 version.cmake
  self-test. Sanitizer jobs depend on CT-03 actually working.
- Written to be copy-able: no template-specific hardcoding beyond what
  NEW_PROJECT.md (CT-01) tells you to search-replace — this becomes the family's
  reference workflow.

**Acceptance.** CI green on the template; a PR that builds on only one compiler
fails visibly. NEW_PROJECT.md references the workflow as a copy step.

---

## CT-09 · Test harness polish: link the lib by default, quieter runs, honest docs

Labels: enhancement

**Context.** Auto-discovered tests link only `Catch2::Catch2` — not
`${PROJECT_NAME}::lib` — so testing your actual library means either a per-dir
CMakeLists or copying sources into the test dir (which the `01example/file.cpp`
pattern demonstrates, teaching the wrong habit). `add_test(... COMMAND
${TEST_NAME} -s)` passes `-s` (report successful assertions), making output
noisy by default. README claims `##` name prefixes "force test ordering," but
glob order affects configure order, not ctest execution order (and parallel ctest
ignores it entirely) — fixtures are the real mechanism and deserve the ink.

**Scope.**
- Auto-discovered tests link `${PROJECT_NAME}::lib` when the target exists
  (covers both STATIC and INTERFACE variants); update the example tests to test
  lib code through the public headers instead of the copied-source pattern.
- Drop `-s` (developers can pass it via `ctest -V` / args when wanted).
- Fix the README ordering claim; document the startup/shutdown fixture scripts'
  intended use (service dependencies) in place of the current bare echo stubs.

**Acceptance.** A new `test/<name>/test.cpp` that exercises a lib symbol builds
with no per-test CMakeLists; example tests updated; README matches observable
ctest behavior.

---

## CT-10 · Docs: the template↔fork update path (stop the drift both directions)

Labels: documentation

**Context.** Four repos now carry copies of this scaffolding, and fixes flow
badly both ways: template bugs (sanitizer no-ops, .clangd pin, gitignore) were
found and fixed *downstream* without the template hearing about it, and template
improvements have no route into existing forks. "Always reinventing things on
each new project" is this missing loop.

**Scope.**
- Document the **upstream-first convention** in AGENTS.md: if a fix touches
  `cmake/`, toolchains, `.clangd`, `.gitignore`, or the test harness, fix it in
  cpp-template first (or file the ticket there), then port — the family's
  AGENTS.md files reference this rule.
- Document a pull-updates recipe for existing forks: `git remote add template
  https://github.com/gobha-me/cpp-template.git` + cherry-pick guidance, and an
  honest list of files where forks legitimately diverge (README, AGENTS intro,
  src/, deps list) vs files that should track the template (toolchains, test
  harness, .clangd, .gitignore).
- Add the pointer in the forks' AGENTS.md files as they get touched (aiforge:
  fold into AF-01).

**Acceptance.** A template fix (use CT-03 or CT-07 as the guinea pig) ports to
one fork following only the documented recipe.
