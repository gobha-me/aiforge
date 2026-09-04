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

Every row has a finite wall timeout capped at 60 seconds. The CPU-limit
assertion has a separate, longer wall allowance because `RLIMIT_CPU` advances
on consumed CPU time rather than elapsed time and the reference runner may be
contended.

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

An evidence artifact is not a runtime capability cache. ADR 0018 selects the
Linux mechanism conjunctions for each restriction level. Review of the v1/v2
evidence found supplemental direct and brokered execution non-escape and
capability-discard proofs that are tracked by issue #209 as schema v3. Until
those proofs exist, v1/v2 alone leave every restricted level incomplete.
Production must still re-establish support at application launch and fail
closed without downgrade.

## Evidence v2

The separately versioned `aiforge_process_isolation_evaluation_v2` executable
extends candidate measurement without changing or loosening schema v1. It
emits `schema_version: 2`, and each schema parser rejects the other version.
Like v1, v2 is opt-in, noninstalled, exact-SHA evidence and grants no runtime
authority or isolation-level claim.

V2 requires `cpu`, `memory`, and `pids` together when assessing a delegated
cgroup-v2 subtree. Its rows measure atomic placement, self-migration denial,
whole-tree enumeration and termination, `populated=0`, session/double-fork/
daemon/fan-out/leader-exit behavior, cancellation cleanup, and each required
controller's limit behavior. PID observations are bound to pidfds; an
unavailable identity proof fails closed rather than relying on a reusable
numeric PID.

The `cgroup_self_migration_denial` row attempts path-based writes to parent and
sibling `cgroup.procs` files after applying its Landlock write guard. It does
not attempt `cgroup.threads` migration, exercise
`clone3(CLONE_INTO_CGROUP)` with its inherited delegated-root descriptor or
another readable or `O_PATH` cgroup descriptor, or borrow a supervisor
descriptor through another process. A denied `cgroup.procs` write is therefore
not proof against those file-descriptor-based escape paths. The row also
observes only direct payload or kernel-descendant migration. It does not prove
that a payload
permitted to create Unix sockets cannot ask a same-UID service manager, D-Bus
broker, or other external broker to execute outside the task cgroup. Issue #209
assigns a broader `payload_execution_nonescape` proof, covering direct and
brokered execution, to schema v3.

Filesystem rows separately measure read, complete mutation, and execute
confinement;
private-root and mount-propagation construction; descriptor-relative launch;
and staged input/output identity. Network evidence deliberately reports
internet families (`AF_INET` and `AF_INET6`) separately from `AF_UNIX`. The
combined-order row descriptor-reexecs the fixed helper and reaches its marker
only after cgroup resource limits, atomic placement, staged input/output
descriptors, complete Landlock handling, and both internet-family and
Unix-socket denial. The private-root combined-order row observes the same
composition with private-root construction before Landlock. Each target then
remains alive until whole-tree cleanup. The private-root row is retained as
truthful candidate evidence when that construction is unavailable; neither row
claims a restriction level. The cancellation row requires an explicit stop
request after a fan-out tree is ready. The partial-setup row measures cleanup
after a later setup step is rejected. These are mechanism observations only:
ADR 0013 reserves any production mechanism or policy mapping for a later
accepted decision.

Capture v2 independently of v1:

```sh
build-evidence/evaluation/process_isolation/aiforge_process_isolation_evaluation_v2 \
  --source-sha "$(git rev-parse HEAD)" \
  --output build-evidence/process-isolation-evidence-v2.json
```

Cgroup-dependent rows require an explicit evaluator-only delegation input:

```sh
build-evidence/evaluation/process_isolation/aiforge_process_isolation_evaluation_v2 \
  --source-sha "$(git rev-parse HEAD)" \
  --output build-evidence/process-isolation-evidence-v2.json \
  --delegated-cgroup-root /sys/fs/cgroup/path-created-for-this-evaluator
```

The supplied absolute path must name an exclusively owned cgroup-v2 delegation
containing only the evaluator process. The runner rejects relative paths,
`.`/`..` traversal, symlinks, foreign processes, and missing `cpu`, `memory`,
or `pids` controllers. It pins the path once by component-wise descriptor
traversal, moves itself to a supervisor leaf, enables the required controllers
on the pinned root, and passes only that descriptor to the fixed probe helper.
Rollback disables controllers, moves the evaluator back, proves the supervisor
empty, and removes it; incomplete rollback dominates the report as
`cleanup_failed`. Omitting the option leaves cgroup-dependent rows
`unavailable/missing_delegation` while independent evidence still runs.

This path is trusted input to the opt-in evaluator only. It is not a production
configuration seam, capability cache, or runtime authority, and the evaluator
never infers a delegation root from its current cgroup or from `..`.

CI creates a disposable systemd service with `Delegate=yes` and a fixed
`DelegateSubgroup=` leaf. Systemd retains the unit cgroup while the evaluator
runs as the sole process and exclusive manager of that leaf. Before execution,
the capture wrapper verifies the leaf identity, ownership, cgroup-v2 type,
writable delegation controls, domain type, required controllers, empty
subtree-control state, sole process, and absence of child cgroups. It then
verifies that every required cgroup row and the non-private-root combined-order
row are `enforced` for the exact source SHA. The private-root combined-order row
remains mandatory in the report but may truthfully be `unavailable` without
weakening that strict gate. A hosted runner without explicit delegation fails
the evidence step. Exit and signal handling stop, if necessary kill, and verify
removal of the exact transient unit; incomplete unit cleanup dominates a
successful capture.

## Restriction-level evidence assessment

`evidence_mapping.hpp` exposes the noninstalled ADR 0018 review helper. It
accepts one complete v1 report, one complete v2 report, and the exact expected
source revision. Missing, malformed, stale, conflicting, unavailable, or
indeterminate evidence leaves the dependent level incomplete. Levels are
cumulative and never downgrade. This result helps reviewers check retained
engineering evidence; it is deliberately not a launch-time availability API.
The helper covers only the v1/v2 row mapping and cannot establish a complete
restriction level while the supplemental schema-v3 proofs in issue #209 are
absent. In particular, v1/v2 do not prove denial of
`clone3(CLONE_INTO_CGROUP)` and borrowed-descriptor cgroup escape, complete
denial or containment of same-UID brokered execution available through Unix
sockets at `low`, complete payload capability non-escalation, or high's
post-private-root capability discard before and across descriptor-relative
execution.
The medium conjunction uses `combined_setup_order`, whose filesystem setup
excludes private-root construction. High separately requires
`private_root_combined_setup_order`. Its descriptor-entered fixed helper proves
private-root construction precedes full-root confinement, network denial,
setup-descriptor closure, and the payload-ready marker; it does not claim a
second descriptor-relative execution after private-root setup.
