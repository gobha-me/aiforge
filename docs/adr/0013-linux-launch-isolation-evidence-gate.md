# ADR 0013: Linux launch-isolation evidence gate

- Status: Accepted
- Date: 2026-09-04

## Context

AIForge has a bounded argv process executor, but its configured filesystem
roots are capability-policy ceilings rather than an operating-system sandbox.
Before process or shell execution can claim stronger containment, the project
needs semantic evidence that candidate Linux primitives actually enforce their
promises on the host where they are used.

Headers, syscalls, libraries, kernel versions, and successful setup calls are
not enforcement evidence by themselves. A primitive may be compiled in yet be
disabled by the runtime environment, rejected by policy, only partially
applied, or escapable by a descendant. CI evidence also cannot authorize a
later production launch on a different host.

Issue 162 defines `none`, `low`, `medium`, and `high` as conjunctive isolation
contracts. Selecting mechanisms or mapping primitive observations to those
levels before gathering evidence would turn an incomplete probe into a
security claim.

## Decision

AIForge provides an opt-in, noninstalled Linux launch-isolation evaluator. It
is built only when `aiforge_PROCESS_ISOLATION_EVALUATION=ON`; explicit
enablement on a non-Linux target fails configuration. The evaluator is an
engineering evidence tool, not a production adapter, runtime setting, tool
policy, or availability service.

Each probe executes in a bounded disposable child with closed stdin, a
sanitized environment, controlled descriptors, a timeout, an output limit,
and required cleanup. The evaluator pins the fixed probe executable by an open
descriptor before launch. On Linux it acts as a child subreaper, drains adopted
descendants after every row, and rejects a row if an unexpected descendant or
unverified cleanup remains. This lifecycle guarantee is scoped to the fixed,
finite evaluator probes; it is not authority to execute an arbitrary hostile
program and is not a production containment choice. A primitive result is one
of:

- `enforced`: an adversarial assertion demonstrated the promised behavior;
- `unavailable`: the primitive could not positively enforce that behavior;
- `probe_error`: the evaluator could not establish a trustworthy result.

Missing, unknown, malformed, duplicated, oversized, truncated, timed-out,
signaled, partially executed, permission-denied, and cleanup-failed results do
not count as enforced. A callable primitive whose adversarial assertion fails
is not enforced.

The evidence report contains stable probe identities, the closed result state,
bounded redacted reason codes, exact AIForge source revision, evaluator schema
identity, and bounded platform, kernel, and architecture metadata. It contains
no environment values, host paths, usernames, hostnames, credentials, or raw
child diagnostics. Report parsing rejects missing, duplicate, unknown, invalid,
and over-limit records.

The probe families cover:

- irreversible no-new-privileges behavior;
- CPU, address-space, process-count, descriptor, and file-size limits;
- inherited descriptor control;
- descendant containment, including session and double-fork escape attempts;
- candidate filesystem-confinement primitives;
- candidate network-denial and network-policy primitives;
- disposable workspace or root prerequisites; and
- descriptor-relative executable, working-directory, and staged-input
  identity support.

Schema v1 probe identities are deliberately narrow. The two `subreaper_*`
rows prove pidfd termination and reaping of a known finite session or
double-fork escape. `landlock_read_confinement` proves one allowed read and one
read denied outside the ruleset. `seccomp_socket_creation_denial` proves denial
of an `AF_INET` stream-socket creation syscall, not complete network denial.
`disposable_workspace` proves creation and cleanup of an owner-only temporary
workspace, not a private root. Namespace rows prove only that the process
obtained a distinct namespace identity. The `openat2`, descriptor-execution,
descriptor-cwd, and staged-input rows prove only their named identity
primitives. None of these rows alone establishes an isolation level.

Ubuntu 24.04 Linux x86-64 is the reference evidence environment because it is
the current CI host. A successful report establishes evidence only for that
exact run and does not create a general Linux or production support promise.
Reports from GCC and Clang builds are retained as exact-source-revision CI
artifacts.

This ADR deliberately does not choose a container, virtual machine, namespace,
Linux security module, sandbox library, or portable abstraction. It does not
map probe rows to `low`, `medium`, or `high`, alter `run_process`, enable shell,
or change approval modes, tool profiles, provenance, or capability authority.
`none` remains the existing bounded argv executor with no operating-system
filesystem or network-isolation claim.

A subsequent accepted mechanism ADR must consume positive evidence, select the
mechanisms for every promised part of each supported level, specify
adversarial tests and failure ordering, and define stable unavailable reasons.
A level is unavailable if any conjunct is absent. Setup failure never silently
downgrades to another level.

Production support must re-probe the selected contract at application launch
and revalidate effect-bound inputs at each execution boundary. Cached CI
evidence, a previously successful launch, or a stored session cannot grant
availability or authority.

## Consequences

- Candidate primitives can be compared using bounded, reviewable evidence
  without exposing an unfinished sandbox through production surfaces.
- CI artifacts support later mechanism decisions but never authorize runtime
  execution.
- Unsupported, indeterminate, and incomplete environments fail closed.
- Process and shell implementation remains blocked on a later mechanism ADR;
  shell remains a separate, stronger executor.
- Non-Linux evaluation and production isolation require their own evidence and
  accepted decisions.
