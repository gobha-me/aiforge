# Static analysis

AIForge treats clang-tidy as a required build gate, pinned to LLVM 20.x. Run
the same check locally with:

```bash
tools/lint.sh
```

The runner configures a Release compilation database with Clang 20, disables
tests, and analyzes every shipped translation unit under `src/lib`,
`src/adapters`, and `src/bin`. Project headers are included through the header
filter; generated, dependency, and system headers are excluded. Parallelism is
bounded to two processes by default and may be changed with
`AIFORGE_TIDY_JOBS`.

## Policy

The checked-in [`.clang-tidy`](../.clang-tidy) enables whole high-signal check
families: Clang's analyzer, bug-prone, CERT, concurrency, C++ Core Guidelines,
miscellaneous, modernization, performance, portability, and readability. This
keeps new checks in the pinned LLVM major visible instead of relying on a thin
allowlist that silently omits relevant defects.

Exclusions must express a durable project decision whose rationale is recorded
here. The current global exclusions are limited to analyzers for foreign
platforms/frameworks, magic-number checks that duplicate domain value type
review without understanding protocol constants, and these deliberate project
conventions:

- Public members are the API of small domain records, not leaked class state.
- Enum storage is chosen for domain clarity and durable compatibility, not the
  smallest current enumerator set.
- Positional aggregate initialization and explicit `{}` initialization remain
  valid when they preserve compact value construction.
- Short guard clauses follow the formatter's established brace policy.
- Identifier-length rules conflict with conventional names such as `id`, `db`,
  and `i`.
- Include-cleaner is deferred until it can distinguish an implementation's
  associated public header from accidental dependency includes; compiler
  self-containment and warnings remain mandatory.
- Required non-owning dependencies use reference members, while abstract ports
  and non-copyable RAII adapters deliberately own only a virtual/default
  destructor or a subset of special members.
- Dynamic indexing into bounded `std::array` storage is valid; pointer
  arithmetic, C-array decay, and unchecked optional access remain enabled.
- Passing cheap token/value types by value is part of the public ownership
  contract. Moving trivially copyable values unnecessarily remains enabled.
- Const-local, emplace, ranges, nested-conditional, static-member conversion,
  parameter-name, qualified-auto, and function-line-count checks are style or
  broad refactoring advice. Cognitive complexity remains the required function
  design gate.

`readability-function-cognitive-complexity` is mandatory with a threshold of
20. Function line/statement counts are intentionally advisory; the enforced
metric measures branching and nesting rather than treating necessary
straight-line validation as equivalent complexity.

## Legacy baseline

The first whole-project run found existing diagnostics across mature state
machines, parsers, and operating-system adapters. They are recorded by exact
source context, check, and message in `tools/clang-tidy-baseline.tsv`; the
configuration is not weakened to make them disappear. CI requires the observed
set to match exactly, so a new or changed finding fails, while a repaired
finding requires its stale baseline entry to be removed.

The baseline is legacy debt and may only shrink. Do not add an entry for new
code. Fix the diagnostic or use a narrow justified suppression as described
below. This gives the portfolio a broad forward policy without a noisy
repository-wide annotation commit or a thin allowlist.

## Suppressions

Prefer repairing the diagnostic. When the checked operation is intentional,
use the narrowest suppression and explain why the rule does not apply:

```cpp
// NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) -- wire bytes are decoded through the protocol's fixed representation
auto header = reinterpret_cast<const Header*>(bytes.data());

consume(result); // NOLINT(bugprone-unused-return-value) -- best-effort cleanup cannot change the primary result
```

`tools/check-nolint.sh` rejects bare `NOLINT`, wildcard check names,
`NOLINTBEGIN`/`NOLINTEND`, and suppressions without a meaningful same-line
justification. Multiple exact checks may be comma-separated. This contract is
deliberately mechanical so the policy can be reused consistently by other C++
projects.
