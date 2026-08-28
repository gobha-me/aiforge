# C++ formatting

AIForge formats every Git-tracked `*.cpp` and `*.hpp` file with clang-format
**20.x**. The checked-in [`.clang-format`](../.clang-format) adopts TermForge's
LLVM-derived policy without divergence. The formatter major is part of the
policy because later majors can produce a different tree from the same
configuration.

Run the same check as CI:

```bash
tools/format.sh --check
```

Apply the policy to the complete tracked C++ scope:

```bash
tools/format.sh --fix
```

The script uses `clang-format-20` by default. If the versioned binary has a
different local name, point to it explicitly; the script still verifies that
it is major version 20:

```bash
CLANG_FORMAT=clang-format tools/format.sh --check
```

## Selected style

The policy uses 80 columns, two-space indentation, attached braces, type-bound
pointers and references, compact simple guard clauses, and expanded compound
control flow. It sorts includes within their existing system/project groups,
adds namespace-end comments, and leaves qualifier order unchanged so formatting
does not become a semantic rewrite.

Formatting changes belong in a dedicated mechanical commit. Feature and bug
fix commits must not hide semantic edits inside a full-tree formatter pass. The
local script enumerates tracked files; format new C++ files explicitly or stage
them before relying on `tools/format.sh --check`.
