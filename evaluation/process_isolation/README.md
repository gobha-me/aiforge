# Linux process-isolation evidence evaluator

This directory contains AIForge's opt-in, noninstalled evaluator for Linux
launch-isolation primitives. It gathers bounded semantic evidence only. It
does not select a sandbox, advertise `low`, `medium`, or `high`, alter
`run_process`, enable shell, or grant production authority.

Configure and build it explicitly:

```sh
cmake -B build-evidence -Daiforge_PROCESS_ISOLATION_EVALUATION=ON
cmake --build build-evidence --parallel 2
```

Explicit enablement on a non-Linux target is a configuration error. Evaluator
tests are built and registered only when `aiforge_TESTS=ON`.

Capture a report for the exact source revision:

```sh
build-evidence/evaluation/process_isolation/aiforge_process_isolation_evaluation \
  --source-sha "$(git rev-parse HEAD)" \
  --output build-evidence/process-isolation-evidence.json
```

The evaluator launches its sibling
`aiforge_process_isolation_probe` executable in bounded disposable children.
It opens that fixed executable once, executes the pinned descriptor, adopts
orphaned descendants as a Linux subreaper, and rejects unexpected descendant
residue after every row. This is a lifecycle guarantee for the evaluator's
finite probes, not an arbitrary-program sandbox or production containment
mechanism.
Each stable probe row reports `enforced`, `unavailable`, or `probe_error`.
Only a completed adversarial enforcement assertion may report `enforced`;
missing primitives, permission denial, malformed output, timeouts, signals,
partial execution, and cleanup failure never do.

The schema-v1 names state the measured property rather than a broader future
claim: Landlock covers read confinement, seccomp covers creation of one
`AF_INET` stream socket, the disposable-workspace row is not a private-root
claim, namespace rows prove distinct namespace identity, and the two
`subreaper_*` rows prove pidfd cleanup and reaping of finite session and
double-fork escapes. A later ADR must combine and strengthen evidence before
claiming a complete filesystem, network, or descendant-containment contract.

Reports contain bounded redacted platform metadata and reason codes. They must
not contain environment values, host paths, usernames, hostnames, credentials,
or raw child diagnostics. CI builds the evaluator with GCC and Clang on Ubuntu
24.04 Linux x86-64, runs the ordinary test suite first, and uploads one report
per compiler with the exact GitHub source SHA in both the report and artifact
name.

An evidence artifact is not a runtime capability cache. A later accepted ADR
must select mechanisms and map every promised primitive to complete isolation
levels. Production must then re-establish support at application launch and
fail closed without downgrade.
