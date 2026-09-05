# ADR 0018: Linux restriction-level mechanisms

- Status: Accepted
- Date: 2026-09-04

## Context

ADR 0013 established a non-authoritative Linux evidence gate. Evidence schema
v1 measures individual launch primitives. Evidence schema v2 strengthens that
work with delegated cgroup-v2 containment, hostile descendant shapes,
filesystem and network confinement, private-root candidates, descriptor
identity, setup ordering, and rollback. Neither schema grants runtime
authority or names an isolation level.

Issue 162 defines `none`, `low`, `medium`, and `high` as conjunctive contracts.
This ADR maps positively enforced evidence to candidate Linux mechanisms so a
later production launcher can implement those contracts without treating
compile-time support, cached evidence, capability declarations, or approval as
containment.

The mapping was evaluated against schema-v1 and schema-v2 implementations at
AIForge source revision `8c4f6b6ed8aaa4934544234105ef914953ebf689`.
Exact-revision reports and adversarial test results are engineering inputs to
this decision. They are not copied into application state and cannot authorize
a future process launch. Review of those reports identified missing execution
non-escape proof: the v2 migration row does not exercise file-descriptor-based
cgroup placement, and `low` still permits Unix-socket access to same-UID
execution brokers outside the task cgroup. The private-root rows also do not
prove capability discard before payload execution. Issue #209 tracks an
immutable supplemental schema v3 for those properties. Until that evidence
exists, every restricted level is incomplete.

## Decision

### Scope

This decision applies only to native Linux launches for the bounded argv
process executor. It selects Linux kernel mechanisms and complete level
conjunctions. It does not implement or register the production launcher,
enable a shell, change approval policy, claim container or virtual-machine
isolation, or promise support on every Linux host.

`none` remains the current bounded argv executor. Its executable, arguments,
working directory, configured roots, environment allowlist, closed stdin,
timeout, and output are validated and bounded, but its filesystem roots and
network effects are policy declarations rather than operating-system
confinement.

The other levels are strict supersets. A level is available only if every
property in that level and every lower level is re-established at application
launch. There is no downgrade. An unavailable `high` launch does not become
`medium`, and an unavailable `medium` launch does not become `low`.

### Platform, privilege, and host prerequisites

The selected mechanism family is native Linux only. Evidence from one kernel,
architecture, cgroup hierarchy, security-module configuration, or privilege
set does not establish another. Production availability requires a successful
launch probe on the exact host and architecture, followed by an unchanged
effective user and group identity, supplementary groups, capability set,
no-new-privileges state, delegated-cgroup identity, and security-module state
at invocation. A changed prerequisite makes the requested level unavailable;
it is never repaired by gaining privilege.

`low` and `medium` are designed for an unprivileged AIForge process with an
explicit, exclusively owned cgroup-v2 delegation. They do not retain or invoke
set-user-ID helpers, ambient capabilities, `CAP_SYS_ADMIN`, a privileged
daemon, or a container runtime. `low` requires empty inheritable, permitted,
effective, and ambient capability sets, a bounding set that never expands
beyond the launch-time fingerprint, and failed namespace-creation and
capability-regain attempts; those properties still require the schema-v3
evidence in issue #209. `high`
additionally requires unprivileged user and mount namespaces
whose mapping and mount operations succeed without host privilege. A host that
disables those facilities reports the corresponding stable unavailable reason.
Capabilities obtained only inside the new private user namespace must not
become host authority and must be discarded after private-root setup and before
payload execution; schemas v1 and v2 do not prove that transition.

### Evidence input

The noninstalled evidence assessor accepts one complete schema-v1 document and
one complete schema-v2 document plus the expected evaluator source revision.
It validates both schemas before examining their currently mapped rows. It does
not assess the supplemental properties assigned to schema v3 by issue #209, so
even an otherwise complete v1/v2 assessment is not evidence that a restriction
level is complete. The documents must:

- contain the exact expected lowercase source revision;
- identify Linux;
- agree on source revision, kernel, and architecture; and
- contain every required row exactly once in canonical order.

Missing, malformed, stale, or conflicting documents make every assessed level
incomplete. A required `probe_error` makes that level indeterminate. A required
`unavailable` row makes it incomplete. Only `enforced/none` satisfies a row.
Assessment is deterministic. A required `cleanup_failed` row dominates other
unmet rows for that level; otherwise assessment reports the first unmet
conjunct in the order defined below. This assessor reviews retained engineering
evidence only; it is not linked into installed targets and its answer is never
runtime authority.

### `low`

`low` is `none` plus all of the following:

1. `PR_SET_NO_NEW_PRIVS` is applied irreversibly before untrusted code. The
   payload has empty inheritable, permitted, effective, and ambient capability
   sets. Its bounding set is a subset of the launch-time fingerprint;
   namespace-creation and capability-regain attempts fail; and those properties
   persist across descriptor-relative execution and descendants. `low` and
   `medium` retain the exact fingerprint. `high` instead follows the separately
   evidenced transition to an empty bounding set. `low` does not claim to drop
   the bounding set or lock securebits without `CAP_SETPCAP`.
2. The child starts atomically inside an exclusively owned delegated cgroup-v2
   subtree. `cpu`, `memory`, and `pids` controllers are present and enabled.
3. CPU, memory plus swap, and process-count limits are installed before the
   child can run. A process-count result is attributed to `pids.max` only when
   `fork()` fails with `EAGAIN` and the cgroup's `pids.events:max` counter
   advances.
   Descriptor-count and file-size limits are also installed.
4. `setsid`, double-fork, daemon, fan-out, and leader-exit descendants remain
   in the task cgroup. Enumeration uses pidfds where a numeric PID could be
   reused. Cancellation and every limit terminate the complete cgroup tree,
   require `populated=0`, reap adopted descendants, and remove every task-owned
   cgroup. These are direct payload and kernel-descendant properties; complete
   execution non-escape additionally requires proof that the payload cannot
   ask a same-UID broker outside the cgroup to execute on its behalf.
5. A narrow Landlock management guard denies the payload access to the
   delegated cgroup root, parent, and sibling migration controls. This guard is
   required because cgroup delegation alone does not prevent same-UID hostile
   migration. Direct `cgroup.procs` and `cgroup.threads` migration,
   `clone3(CLONE_INTO_CGROUP)` through readable or `O_PATH` parent and sibling
   descriptors, and attempts to borrow supervisor authority through another
   process must all fail. Brokered execution through Unix sockets or another
   same-UID IPC boundary must also be denied or contained by a separately
   proven mechanism. The guard makes no general filesystem-scope claim for
   `low`.
6. The executable and working directory are pinned without symlink traversal,
   revalidated by descriptor identity, and entered or executed relative to the
   pinned descriptors. The executable is invoked with an argv vector, never a
   shell or `PATH` lookup.
7. The environment is rebuilt from the explicit allowlist, stdin is closed,
   signals are reset, and every descriptor except the fixed standard streams
   and setup protocol descriptors is closed before execution. Setup
   descriptors are closed before the payload begins.
8. Any rejected partial setup is rolled back completely.

The selected evidence conjunction is:

- v1: `no_new_privileges`, `rlimit_descriptor_count`,
  `rlimit_file_size`, `inherited_descriptors`, `openat2_resolution`,
  `fexecve_identity`, and `fchdir_identity`;
- v2: every `cgroup_*` row, `descriptor_relative_launch`, and
  `partial_setup_cleanup`; and
- the v1 runner adversarial assertion that the fixed child receives neither an
  ambient environment nor unrelated descriptors.

The v2 `cgroup_self_migration_denial` row proves only that its path-based writes
to parent and sibling `cgroup.procs` files are denied after applying the narrow
Landlock guard. It does not try `cgroup.threads`, exercise
`clone3(CLONE_INTO_CGROUP)` with its inherited delegated-root descriptor or
another readable or `O_PATH` cgroup descriptor, or try to borrow another
process's management descriptor. Landlock write denial therefore cannot be
treated as proof of cgroup non-escape. V2 also does not prove that `low`, which
permits new Unix sockets, cannot request execution from a same-UID service such
as a user service manager or D-Bus broker outside the task cgroup. The broader
supplemental `payload_execution_nonescape` proof in schema v3 must resolve
both direct and brokered execution; issue #209 tracks whether that moves Unix
socket denial into `low` or establishes another measured boundary. Its absence
also makes `medium` and `high` incomplete.

### `medium`

`medium` is `low` plus all of the following:

1. Landlock rules bind readable, writable, and executable access to pinned
   roots. Undeclared roots, ambient home directories, credential locations,
   and replacement paths remain inaccessible. Write rights are not implied by
   read rights, and execute rights are not implied by either.
2. Seccomp filters deny creation of `AF_INET`, `AF_INET6`, and `AF_UNIX`
   sockets. All inherited descriptors were already closed by `low`.
3. Cgroup placement, filesystem confinement, no-new-privileges, descriptor
   closure, and network filtering all complete before descriptor-relative
   execution reaches untrusted code.
4. Failure after any setup step kills and reaps the whole task tree, removes
   task-owned cgroups and temporary filesystem state, and publishes no output.

The additional v2 conjunction is `landlock_read_confinement`,
`landlock_write_confinement`, `landlock_execute_confinement`,
`seccomp_internet_socket_family_denial`, `seccomp_unix_socket_denial`, and
`combined_setup_order`.

`combined_setup_order` proves the measured v2 medium composition without a
private root. It does not prove the supplemental v3 non-escape or capability
properties, does not depend on private-root availability, and cannot, by
itself, satisfy the additional high composition.

The selected network policy is denial of new IPv4, IPv6, and Unix-domain
sockets after inherited descriptor closure. The evidence does not support an
allowlisted egress policy, DNS exception, inherited broker socket, proxy
escape, or a broader claim about unmeasured address families. A future policy
that permits one of these three families or makes any promise about another
family needs separate evidence and an accepted decision; an unmeasured family
must never be inferred safe from these rows.

### `high`

`high` is `medium` plus all of the following:

1. The payload enters a new user and mount namespace, makes mount propagation
   private, constructs a disposable owner-only root, pivots into it, detaches
   the old root, and cannot reopen the host root.
2. An adversarial mount created by a child inside the private namespace is not
   visible in the parent namespace.
3. Inputs are copied or materialized into the disposable root through pinned
   source and destination descriptors. Identity is checked before the payload
   starts.
4. Outputs are written only to pinned staging descriptors. Publication to a
   durable artifact or approved host destination occurs after successful tree
   termination and cleanup, through a trusted parent operation. The payload
   never receives the publication destination as ambient authority.
5. Network denial is the `medium` deny-all contract; no namespace alone is
   treated as network confinement.
6. A production launch constructs the private root before full-root Landlock
   confinement, network denial, and execution of the pinned untrusted
   descriptor. Setup descriptors close before the payload begins. The evidence
   row enters a fixed trusted helper descriptor-relatively, then proves that
   helper constructs the private root before confinement, network denial,
   setup-descriptor closure, and its payload-ready marker; it does not claim a
   second descriptor-relative execution after private-root setup.
7. Capabilities required to construct the private root are discarded after the
   final namespace and mount operation and before the payload-ready boundary.
   Empty capability sets, locked securebits, failed regain attempts, and their
   persistence across descriptor-relative execution and descendants require
   the supplemental schema-v3 evidence in issue #209; no v1 or v2 row proves
   this ordering.

The additional evidence conjunction is v1 `disposable_workspace` plus v2
`private_root_construction`, `private_mount_propagation`,
`staged_input_identity`, `staged_output_identity`, and
`private_root_combined_setup_order`.

The exact post-merge [reference run
33961158809](https://github.com/gobha-me/aiforge/actions/runs/33961158809)
evaluated revision `8c4f6b6ed8aaa4934544234105ef914953ebf689`. Its GCC v1
artifact `process-isolation-evidence-GCC-8c4f6b6ed8aaa4934544234105ef914953ebf689`
(`9968341653`, archive digest
`sha256:18351adc71aff485896defce72f340aa3a637ca14acf5fd15159779a59827978`)
and Clang v1 artifact
`process-isolation-evidence-Clang-8c4f6b6ed8aaa4934544234105ef914953ebf689`
(`9968467632`, archive digest
`sha256:42a469c6be042062ccbe38e32d7e90f553026c9414512f32ce0b225ad18a60a0`)
each contain a report whose SHA-256 is
`d867092b7e29b0432960ccd27173470d8b834de27305d1313908ecf4ae921c30`.
Its GCC v2 artifact
`process-isolation-evidence-v2-GCC-8c4f6b6ed8aaa4934544234105ef914953ebf689`
(`9968342291`, archive digest
`sha256:16268cf3f5375034aac15465f5bde9c00f89207fc697f69aec52fc410e62ab2b`)
and Clang v2 artifact
`process-isolation-evidence-v2-Clang-8c4f6b6ed8aaa4934544234105ef914953ebf689`
(`9968468197`, archive digest
`sha256:007eae7e96f7f6d728901615a7f25f729706becb37a109e31a8ebf7cd9d930dc`)
each contain a report whose SHA-256 is
`6f21453cff361d1c0659781cbd561f249a183663f8689d8fbce655f41e4a1797`.
Both compiler-specific report pairs satisfy the v1/v2 rows currently selected
for `low` and `medium`, but they do not make either level complete. They contain
no proof against `clone3(CLONE_INTO_CGROUP)` or borrowed cgroup descriptors and
do not prove that `low` prevents brokered same-UID execution while allowing new
Unix sockets. They also contain no complete payload capability non-escalation
proof. Issue #209 assigns those missing properties to supplemental schema v3.
Because levels are cumulative, that missing `low` evidence also makes `medium`
and `high` incomplete. For both report pairs, the existing v2 high rows are
independently incomplete first at
`private_root_construction/unavailable/permission_denied`; the later
`private_mount_propagation` and `private_root_combined_setup_order` rows carry
the same state and reason. V1 and v2 also do not prove that namespace
capabilities are discarded after private-root setup and remain absent across
descriptor-relative execution. The other positive rows do not permit a partial
restriction-level claim, and this retained result grants no launch-time
authority.

### Launch-time establishment

Production must probe and establish the selected contract once per application
launch and bind the result to an immutable launch context. Retained CI reports,
the noninstalled assessor, a previous application run, durable session state,
or a restored projection never grants availability.

The launch probe uses the production mechanism and an adversarial fixed helper;
it does not run a user command. It pins an explicitly configured delegated
cgroup root by descriptor traversal, requires exclusive ownership and no
foreign processes or subgroups, creates a supervisor leaf, moves the launcher
there, enables `cpu`, `memory`, and `pids` on the pinned root, and closes the
management descriptor in every payload. It never infers a delegation root from
the current cgroup or `..`.

Each invocation then revalidates executable, working directory, requested
roots, environment names, limits, and staged input identities against the
immutable launch context before mutation. The child is placed atomically in a
new task cgroup. Setup proceeds in this order:

1. create and configure the task cgroup and resource controls;
2. atomically create the child in that cgroup;
3. create the private root and stage inputs when `high` is selected;
4. reset signals, close stdin, rebuild the environment, and close unrelated
   descriptors;
5. install the cgroup-management Landlock guard;
6. install the full filesystem rules for `medium` and `high`;
7. set no-new-privileges and install all network filters;
8. verify setup completion through a bounded trusted-parent handshake; and
9. execute the pinned descriptor.

No untrusted instruction runs between these steps. Failure before the handshake
cannot be reported as payload execution.

### Stable unavailability and failure precedence

The production availability boundary uses closed reason identities rather than
raw syscall text or host paths:

- `unsupported_platform`;
- `unsupported_architecture`;
- `unsupported_kernel`;
- `missing_delegation`;
- `missing_controller`;
- `permission_denied`;
- `privilege_changed`;
- `mechanism_absent`;
- `unsupported_combination`;
- `setup_race`;
- `enforcement_failed`;
- `cleanup_failed`; and
- `internal_error`.

Evidence assessment additionally distinguishes `missing_evidence`,
`malformed_evidence`, `stale_evidence`, and `conflicting_evidence`. These are
engineering-report states, never production launch authority.

Validation and authority denial occur before any setup mutation. Once mutation
begins, incomplete cleanup dominates cancellation, timeout, limit, setup,
protocol, or child-exit failures. If cleanup is proven complete, explicit
cancellation dominates timeout; otherwise the earliest failed setup conjunct
is reported. Raw kernel diagnostics may be logged only through a separately
bounded, secret-safe diagnostic channel and never enter durable events or model
context.

### Required production adversarial tests

Before a level can be exposed by a production process tool, hermetic fakes must
cover its complete failure matrix and Linux integration must demonstrate its
actual mechanisms. Tests include:

- unsupported platforms and kernels; missing or malformed launch evidence;
  missing delegation or each controller; foreign processes or subgroups; and
  privilege changes between launch and invocation;
- executable, working-directory, root, and staged-input replacement; symlink
  traversal; root swaps; `PATH` ambiguity; argv metacharacters; environment and
  descriptor leakage; and inherited socket attempts;
- parent and sibling cgroup migration; `setsid`, double fork, daemonization,
  clone/fork fan-out, leader exit, PID reuse, and descendants that outlive the
  leader;
- CPU, memory plus swap, process-count, descriptor, file-size, wall-time,
  stdin, output, and artifact limits;
- filesystem read, write, execute, rename, link, and Unix-socket escapes;
  internet and local socket creation; unmeasured socket-family attempts; and
  attempts before and after every setup boundary;
- private-root escape, parent-visible child mounts, input identity drift,
  output publication before cleanup, and replacement publication targets;
- cancellation and failure after every setup phase, including a killed helper
  that leaves populated and empty task cgroups; recursive task-owned cleanup,
  foreign-cgroup preservation, and cleanup-failure dominance; and
- stale, replayed, or conflicting evidence and approvals; restored sessions;
  concurrent launches; and proof that replay performs no execution.

GCC and Clang must exercise the same contract. A positive test on one host is
not a generic Linux support claim.

## Consequences

- `low` can be implemented only where exclusive cgroup-v2 delegation,
  controller control, pidfds, no-new-privileges, descriptor-relative launch,
  and the narrow Landlock management guard are all enforceable.
- `medium` adds full Landlock scope and deny-all network filtering. Network
  allowlisting remains unavailable.
- `high` remains unavailable wherever private-root or private-mount propagation
  evidence is absent, including the current reference environment.
- Approval mode remains orthogonal. `prompt`, `auto`, and `allow-all` cannot
  change level availability or bypass any conjunct.
- A later production child may implement this ADR without changing the
  provider-neutral run kernel. Shell remains separately blocked on its stronger
  executor contract.
