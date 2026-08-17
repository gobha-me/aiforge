# AIForge

AIForge is a C++23 foundation for a terminal AI client. The project is being
built from the durable core outward: provider-neutral run events, deterministic
backends, and replayable projections come before the production TUI and Venice
network adapter.

The current milestone is intentionally offline. Running `aiforge` confirms the
core executable is available; it does not contact a model provider. The command
registry also provides generated help and version output. Commands that depend
on later network, one-shot, or configuration milestones fail explicitly instead
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
- Git when CMake must fetch Catch2

AIForge uses CMake only for dependencies. A configured package is preferred,
then a sibling checkout, with `FetchContent` as the fallback. TermForge and
venice-cpp recipes remain in the tree for their future adapters but are not
active dependencies yet. Their fallbacks are deliberately pinned to compatible
stable baselines (TermForge v0.7.2 and venice-cpp v0.5.0); upgrades should be
made only for a documented compatibility need.

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

Run the offline executable with:

```bash
./build/src/bin/aiforge
./build/src/bin/aiforge --help
./build/src/bin/aiforge --version
```

Command-line mistakes write diagnostics to stderr and return exit code 2. Help
and version output use stdout. The registered `chat` and `models` commands
remain unavailable until their owning feature milestones land.

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
from this file and remain a separate milestone.

The default toolchain respects `CXX`. Sanitizer toolchains are opt-in:

```bash
cmake -B build-asan -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/address.cmake
cmake -B build-tsan -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/thread.cmake
```

## Development layout

- `include/aiforge/` contains the provider-independent public API.
- `src/lib/` implements the run domain and backend ports.
- `src/bin/` is the application entry point.
- `test/<name>/test.cpp` is auto-discovered after re-running `cmake -B`.
- `docs/adr/` records decisions that narrow the north-star guardrails.

The core library must not expose TermForge, venice-cpp, JSON-library, database,
or scripting-runtime types. Future adapters depend inward on the core.

## License

AIForge is available under the BSD 3-Clause License; see
[`LICENSE.md`](LICENSE.md).
