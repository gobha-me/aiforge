# ADR 0024: Read-only Admin observations

- Status: Proposed for issue #226 implementation review
- Date: 2026-09-09

## Context and delivery boundary

Issue #226 requires a complete Observe journey for Linux-local and explicitly
configured Kubernetes targets. A user must be able to investigate an unhealthy
service or workload using bounded, timestamped evidence, through optional TUI
controls and equivalent commands. The same observations must be available to
model tools under the shared run kernel. A generic process tool is not this
workflow.

The owner approved certificate/token authentication first, application log
excerpts disabled by default and explicitly enabled per job, and Linux and
Kubernetes before Ceph. Observe is a read-only capability profile. Ask,
Automate and Full describe approval behavior and do not turn target selection
into authority. Resident management, generic trigger intake, reliable job
submission, remote decisions, Kubernetes Dev workers and Ceph inspection remain
separately tracked follow-ons. This ADR does not authorize deploying or changing
infrastructure.

This first implementation establishes the neutral target/request contract and
failure tests. It does not itself complete the Observe product journey. Later
slices must deliver production sources, durable execution, evidence inspection,
TUI/command parity and the two diagnostic smoke journeys before #226 closes.

## Neutral binding and request authority

A target has a stable configured identity and a separate live binding revision.
A display alias is not its identity. Kubernetes bindings include the exact
context label, explicit namespace, HTTPS endpoint and non-secret trust identity.
Credential source revisions are opaque values; they are never digests of secret
bytes. Linux bindings identify the actual execution environment and available
boot/namespace identity. A process inside a container cannot advertise physical
host observations without an explicitly established host source. Unknown or
unavailable identity remains visible, rather than being invented.

A request names the existing binding and selection generation, a closed read
operation and a typed resource identity. It cannot supply a command, executable,
credential source, arbitrary API path, namespace override or additional flags.
The runtime checks it against an independently established target authority.
Resource identity includes source UID and container identity where relevant;
names alone do not prove continuity across replacement. Cluster-scoped node
reads require their own grant rather than following from namespace access.

Log access is disabled by default. The enabled policy binds a selected
application source and positive time, line and byte bounds. Requests carry that
policy revision, but cannot enable or expand it. The owner's enabled setting
covers subsequent reads within those limits without repeated confirmations.
Before saved jobs exist, this is an explicit current-session policy over selected
application resources, reset for a new session and revalidated on target change.
It is not silently persisted as a grant for future jobs or restored sessions.

Changing a target or its namespace invalidates pending delivery immediately.
Already dispatched work retains its original identity through result accounting
and cleanup. A new selection never relabels an old observation or imports its
live permissions. Disabling or revising a log policy also invalidates pending
log delivery; check the current policy before dispatch and before publishing new
log text. Already dispatched work still drains cleanup. Historical evidence
retains its original provenance and is not erased or treated as a permission to
collect or fetch more text. Limits apply to active, retired and undelivered work
until physical resources have been released.

## Source and credential custody

Production sources implement a neutral typed observation port. The initial
Linux Kubernetes adapter may use fixed kubectl read operations and structured
JSON resource output. Its private bootstrap must consume exactly the selected
configuration and static credential binding, rejecting unsupported auth helpers
and ambient context, namespace, proxy or credential fallback before requests.
The adapter must construct an immutable minimal configuration for the child;
checking a mutable source filename only before and after dispatch is inadequate.

A proposed Linux implementation uses an adapter-private sealed input descriptor.
Current generic process launches close unrelated descriptors and have no private
credential transfer member. Preserve that public contract: factor necessary
private launch mechanics beneath the adapters, without adding arbitrary FD or
credential fields to ProcessLaunchRequest or generic tool arguments. Prove
remapping, descriptor hygiene, startup failure, cancellation and the supported
kubectl compatibility with subprocess fixtures before advertising this source.
Sealing establishes immutability, not an OS sandbox or secrecy from processes
with equivalent host authority.

Raw credential/configuration bytes and raw child stderr remain adapter-owned.
They never enter neutral requests, fake captures, progress events, durable
observations, artifacts, terminal frames or mapped errors. The existing Venice
printable-token credential format is not a PEM or kubeconfig credential store.
ADRs 0004 and 0005 continue to own JSON configuration and session storage; no new
parser, storage engine, scripting runtime, plugin ABI or library is selected.
Any required kubeconfig conversion happens privately, with bounded input/output,
and must not authenticate or invoke helpers during configuration inspection.
Conversion is not strict validation: the config-only kubectl v1.34.0 probe
accepted duplicate YAML and JSON mapping keys and retained the last value.
Reject ambiguous keys before conversion or authentication; a private structured
output check alone cannot recover discarded input ambiguity. The adapter
milestone must record any necessary strict YAML parser choice before adding a
dependency or enabling this path.

## Observation evidence and lifecycle

Observations contain captured target/resource identity, operation, start/end
UTC timestamps, source versions where available, explicit completeness and
bounded neutral status data. Default resource projections use an allowlist;
whole resource documents, environment values and arbitrary diagnostic fields
are not ordinary evidence. Enabled application log excerpts retain source,
policy and time-range provenance and known credential exclusions. Enabling text
is explicit authorization to send the selected text to the configured model;
it is not a claim that arbitrary text can be perfectly redacted.

Keep the last successful snapshot distinct from current refresh/connectivity
state. Age derives from the original observation timestamp. A failed refresh
cannot become a fresh empty success; an incomplete list cannot imply healthy
resources by omission. Initial refreshes are explicit and finite. Redrawing,
resizing, selecting an inspection view and replaying history do not collect.

Manual TUI/command reads require no inference. They need a kernel-owned control
operation that records human-origin intent before dispatch and applies the same
registered operation, target, policy, effect and execution limits as model tools.
Do not fabricate inference IDs or dispatch adapters directly from widgets.
Target selection is a durable control fact, not a grant or refresh. The exact
additive event contract must be reviewed with the corresponding kernel slice;
existing inference-linked tool events cannot silently change meaning.

Manual controls use the explicitly granted session/target read capabilities.
Model tool support determines whether the model can request those operations;
it does not prevent a human from inspecting an authorized source offline.
Neither path can exceed launch/target policy. Completed observations rebuild
from durable evidence. Reopening unfinished manual reads reports interruption
without automatically collecting again. Failed persistence must never claim a
durable observation or success.

The historical observation codec uses ADR0005's private nlohmann/json boundary
and a version-1 neutral document. It encodes no source handle or live authority.
Historical structure validation checks the recorded request, hard limits and
payload without reconstructing consent; current authority remains a separate
dispatch/publication requirement. Exact field sets, variant and enum names,
integer types, decoded mapping-key uniqueness and UTF-8 are validated. The
encoded document has an independent 512 KiB ceiling, parsing depth 16 and
32,768 parser-event ceiling; neutral evidence accounting remains 64 KiB.
Null optional fields retain unknown/absent semantics, zero remains a value,
and source-ordered log lines retain blank lines. The codec alone does not append
events or complete replay integration. The later kernel slice must atomically
pair typed observations with their invocation result, reject mismatched model
content and recheck current authority before publishing either representation.

## Owner-thread observation broker

The broker uses the application's shared local-source worker for physical
execution and a separate bounded mailbox for queued, running and completed but
unpublished requests. Executor threads retain only an owning mailbox endpoint.
They do not capture the application, UI, source or worker. Admission binds a
session-unique invocation and a complete typed request; the kernel remains the
durable invocation-uniqueness owner. The mailbox detects duplicates among its
bounded pending entries and does not grow a second replay ledger.

An executor receives an opaque issuer-bound receipt, not observation text or
an artifact. The owner consumes it once, checking the exact invocation and
request, current target and log authority, deadline and cancellation. Changing
selection invalidates pending delivery; replacing a session permanently closes
its previous endpoint, even when the session ID is reused. Every selection
advances generation, and changed log policy for the same target advances its
policy revision. A receipt is runtime identity and grants no durable authority.
The later kernel integration must render and atomically persist a successful
observation after consuming the receipt; the broker itself does not append.

Owner pumping performs dispatch and completion checks without joining source
IO. An expired or cancelled wait does not release a physical worker slot while
its source still runs or cleans up. Completed receipts retain mailbox capacity
until consumed or invalidated. A failed attempt is not automatically retried.
Close wakes waiting executors before the kernel joins its tool thread. Last
shared-owner destruction of arbitrary source graphs remains a caller cleanup
responsibility, so this boundary does not promise nonblocking source destructors.

The durable manual admission grammar uses `run.started` schema 6 with a
mandatory true `manual_observation_required` flag, followed by one
`ops.human_observation_requested` and one `tool.proposed` schema 3. The flag
makes a missing human intent a replay error rather than ordinary control work.
The human marker retains exact request, versioned tool registration and launch
policy provenance without inventing a backend or model. Proposal schema 3
requires a non-null typed observation request, normalized arguments and no
spend quote; legacy proposal schemas reject the new reserved proof field.

`ops.observation_recorded` schema 1 retains the typed historical snapshot.
A pure runtime history validator checks exact cross-event identity, declared
scope/effect correspondence, policy/approval/start ordering, cancellation,
atomic observation/result pairs and manual terminal outcomes. Valid unfinished
manual admissions are reported without granting retry authority; missing
atomic admission or result partners are corruption. Its explicit limits are
one million input events, 4096 Ops invocations and 16 MiB of conservatively
accounted captured requests. It stores references to evidence events rather
than copies of their observation payloads.

Version-1 canonical tool content is one TextBlock with fixed-order `key=value`
lines. Strings are quoted with quote/backslash escaping, numbers are decimal,
absent values are `unknown`, and row indices preserve source order, including
blank log lines. All allowlisted typed request/provenance, timestamp,
completeness and payload fields are included. A separate 256 KiB output ceiling
fails rather than omitting fields. Typed observation/result equality is checked
against this pure formatter; runtime formatting does not call a JSON adapter.
The independent version-1 request JSON document has a 16 KiB ceiling.

This foundation does not enable collection or implement the manual kernel
entry point. Typed historical proof consistency does not yet establish that
raw/normalized executor JSON semantically names that request. The future Ops
argument adapter and kernel preparation hook must establish that binding,
assign owner/session/request identity internally, and apply current authority
before dispatch/publication. Ordinary kernel entry points reject the new
manual-start flag until that explicit admission path exists.

## Native observation tool preparation

The final native `OpsObservationTool` registers `observe_target` from one frozen
selection and log policy. Human typed intent and model JSON use the same closed
operation, exact resource identity and narrowing limits. Preparation binds the
kernel-assigned invocation to an observation request and supplies owner, session,
binding and consent revisions internally. Pure argument validation creates no
invocation proof. JSON arguments cannot provide these authority fields.

`ops.target` scopes match opaque configured target IDs exactly and cover only
the operation's declared read, execute and network effects. They do not cover
generic filesystem, process, network or infrastructure scopes. The runtime
rechecks normalized arguments, typed proof, invocation identity and granted
scopes before an executor can submit to the broker. Its only successful stream
update is an opaque receipt; it cannot return observation content or artifacts.

This boundary is deliberately unavailable through ordinary kernel dispatch
until the dedicated durable admission and receipt-publication integration is
present. An ordinary executor returning a receipt fails the run. Registration
name, category and version alone never enable the native contract. Replay
integration remains separate; this preparation API does not authorize retries.

## Historical observation recovery

Kernel replay validates the complete typed Ops history before classifying or
restoring pending tool authority. Every valid unfinished manual observation is
closed by an atomic ToolErrored/RunFailed interruption pair, retaining its
invocation and result-message identity. Missing admission or terminal partners
remain replay errors rather than repairable prefixes. A failed append exposes
no recovered kernel and grants no retry. Completed manual evidence remains
historical, with no source call, approval restoration, or backend invocation.
Known control and summary runs are excluded from both provider tool-turn and
artifact continuation projections; already projected conversation spans without
RunStarted retain their existing interpretation. This replay integration does
not enable manual admission, collection, or normalized-argument proof binding.
Until that binding exists, unfinished model-origin typed Ops invocations reject
replay before recovery writes or policy/registry restoration. Matching a tool's
name or version cannot reconstruct missing execution proof. Completed model
observations remain readable historical evidence.

## Kernel observation admission and publication

The kernel accepts a typed human observation intent through a dedicated control
entry point. It derives the manual contract flag, invocation-bound request,
native registration provenance and launch policy provenance. RunStarted,
HumanObservationRequested and the typed ToolProposed are one durable transaction
before policy evaluation or source IO. This path needs no backend or model.
Model proposals use the same native preparation, require actual recorded tool
and policy provenance, and retain both normalized arguments and typed proof.
The final native executor type is required; ordinary executors cannot adopt
the contract by copying its name, version, proof or receipt.

The application supplies an optional shared broker, retains it past its kernels,
and owns activation, selection, pumping and shutdown. Kernel mutations, draining
and broker owner methods run on the same owner thread. Pure preflight checks
the exact endpoint issuer, kernel session, selected source and current request
authority before policy admission and again before launch. Kernel destruction
stops its captured operation token; it never closes the application's broker or
cancels a replacement session by a reused invocation identifier.

Only a validated native receipt can publish an observation. The owner gate
rechecks current authority before producing canonical model text and recording
OpsObservationRecorded plus ToolResultRecorded atomically. Manual success adds
RunCompleted in the same transaction. Native progress, input, arbitrary content,
artifacts and spend are protocol failures. Manual failure and cancellation
record complete tool/run terminal pairs while retaining worker state until its
end event drains. Approval-port failure also terminates a model-origin native
run so its pending approval cannot become stranded. Every commit validates the
complete historical grammar; store failure or rejection during live work closes
the kernel without publishing a fresh observation claim. Recovery remains the
bounded, non-retrying behavior above, including rejection of unfinished model
proof; this slice adds no UI or configuration surface.

## Linux systemd service-read milestone

The Linux service-read adapter uses libdbus privately, through the fixed Unix
system-bus address `unix:path=/run/dbus/system_bus_socket`. The released 1.16.2
maintenance baseline is selected after checking the official release index,
stable-branch NEWS and maintenance policy on 2026-09-09; 1.16.4 was still marked
unreleased. CMake prefers a compatible installed DBus1 package, then the official
1.16.2 archive with its published SHA256. The named recipe is active only for
Linux adapter builds. No D-Bus type, arbitrary address, path, descriptor or RPC
method enters public domain/runtime APIs. This is a closed adapter dependency,
not a generic RPC library or an application shell facility.

An observation creates its own private connection on its existing source worker.
Factories and admission perform no bus or manager queries. Before polling, the
adapter sets message/queue/descriptor limits and verifies protected socket-path
custody, actual peer credentials and the selected PID/mount/user namespaces.
It then registers with Hello, resolves the unique owner of
`org.freedesktop.systemd1`, and requires that owner's UID/PID to identify root
and PID 1 in the same verified execution environment. All manager calls address
that unique owner. Final owner, boot and namespace checks reject replacement;
there is no reconnect, private-socket fallback or ambient address selection.
The bus daemon itself need not have UID zero. Permission or namespace-proof
failure stays explicit; unprivileged bus access does not imply permission to
inspect another process's procfs namespaces. Container/unknown scope is retained.

The first slice implements transport identity and a private fixed request table.
It does not decode service replies or expose them through the observation port.
The next decoder must verify GetUnit's object path and canonical Id before
reading properties, reject malformed/oversized results and prove these service
semantics before availability is advertised.

Only loaded-service discovery, exact unit lookup and allowlisted individual
property reads are permitted. There is no activation, interactive authorization,
introspection-driven API expansion, GetAll, mutation or application log read.
Invocation identity is checked around exact service inspection. Name-only health
requests retain name-only result identity. A systemd active state does not prove
application readiness or physical-host health. Unknown states and incomplete
populations remain visible in neutral evidence. Replay never recollects.

Positive remaining deadlines and cancellation checks apply between finite
poll/decode steps. Libdbus queue limits are backpressure thresholds with a
maximum-message/read-buffer overshoot allowance, separate from neutral evidence
limits; they are not a cumulative wire-byte ceiling. Authentication has its own
library buffer bound. Unix connect and other stalled syscalls or final cleanup
can retain the existing physical worker slot after logical cancellation. There
is no second pool and no promise of hard physical cleanup latency.

After fixed endpoint custody and peer checks, ordinary libdbus authentication
is permitted. Its client starts with EXTERNAL but may fall back to cookie
authentication, which can resolve user identity through NSS and read the user's
home/keyring files. The public client API does not provide an EXTERNAL-only
selector. Client keyring loading uses `add_new=FALSE`, without server keyring
writes. Authentication bytes and raw library errors remain private. This is
not a procfs/socket-only or no-ambient-I/O guarantee, and there is no network
isolation claim for OS identity resolution. The adapter neither changes the
environment nor uses private authentication symbols. A zero per-message FD cap
rejects descriptors; the aggregate FD threshold must be positive (one), because
zero stops all reads, including messages carrying no descriptors.

The private foundation remains unavailable to product assembly until its
transport, binding and neutral service tests pass. The existing Linux health
source continues to advertise only health reads. Journal support requires a
later, separately tested source/descriptor boundary; the generic public process
launch request remains unchanged. Installed and forced-fallback dependency
consumers must both compile/link, without building or installing daemon tools
as an embedding side effect. The initial fallback needs only narrowly checked
upstream subproject-path and build-interface corrections, not an alternate
hand-maintained implementation of libdbus. The fallback is a shared client, not
a bundled static runtime. Deployment consumers must retain a compatible
`libdbus-1.so.3`; a pure version check rejects a loaded library older than 1.16.2
before socket access. Dependency probes execute against both the selected build
library and a staged upstream client install. This does not invent application
install/export rules that AIForge does not currently have.

Sources: [published releases](https://dbus.freedesktop.org/releases/dbus/),
[stable maintenance NEWS](https://gitlab.freedesktop.org/dbus/dbus/-/blob/dbus-1.16/NEWS),
[upstream CMake client target](https://gitlab.freedesktop.org/dbus/dbus/-/blob/dbus-1.16.2/dbus/CMakeLists.txt),
[connection limits](https://dbus.freedesktop.org/doc/api/html/group__DBusConnection.html),
[owner and credentials protocol](https://dbus.freedesktop.org/doc/dbus-specification.html).

## Linux service payload and source integration

The next Linux source slice implements loaded-service discovery and exact service
health through the private system-bus transport. Existing kernel memory/uptime
health remains procfs-only and does not open the bus. Source construction retains
its health identity even when service access cannot be proved. Each concurrent
service observation owns its own connection on the existing source worker; no
cached mutable connection or additional pool is introduced.

Request validation precedes bus creation. Factory pinning, authentication,
handshake, property reads, decoding and final proof share one original deadline
and stop token. A separate per-request cumulative captured/decoded budget counts
accepted D-Bus message envelopes and fields before copies, including handshake
and unrelated messages. It also counts ignored fields in list replies. This is
separate from neutral evidence accounting and library message/queue bounds: it
does not account authentication, OS identity IO or total wire bytes. A received
message exceeding the remaining capture budget fails before further dispatch;
its already-received library buffer remains governed by the transport cap.

GetUnit's path and Unit.Id must match the exact canonical service name before
other properties are read. Returned paths never select arbitrary endpoints.
Properties use a closed schema and fixed neutral mappings. InvocationID is read
before and after each retained service's state/details; an explicit selected
invocation must match, while name-only health retains name-only result identity.
Restart, disappearance and changed final transport binding return no evidence.
These checks produce sampled facts, not an atomic freeze of systemd state.

Discovery describes loaded services and independently binds retained rows to
their observed invocation. It validates the bounded whole list, admits an output
prefix within entry/evidence/call limits and reserves final verification calls.
Known omitted rows are reported as truncation; an oversized unpaged reply fails.
Unknown service states/reasons remain partial neutral evidence. A signal number
is not an exit code, and active is not application readiness or host health.
Unsupported getters and transport failures stay terminal without retry.

Only after deterministic decoder/source tests and review pass may the source
advertise health, service discovery and service health as implemented operations.
That advertisement does not claim the running environment passed a live access
probe. Logs, journal custody, Kubernetes observation, surfaces and replay refresh
remain outside this slice. The broker/kernel retain current-authority and durable
publication ownership.

## Static Kubernetes configuration parser milestone

The first Kubernetes source needs a strict original-input boundary before any
child launch can receive configuration. Select yaml-cpp 0.9.0's event API for
this private adapter milestone, with installed-package preference and a pinned
official archive fallback. Its release tag identifies commit
`56e3bb550c91fd7005566f19c079cb7a503223cf`; the recipe records the archive digest.
Strict JSON uses a new private nlohmann SAX handler feeding the same closed
schema sink. Neither parser nor credential types enter public interfaces.

Parsing accepts immutable bytes, explicit syntax, context, namespace and a stop
token. It reads no files or environment, invokes no helper and makes no network
request. The move-only result retains minimal generated JSON for exactly one
cluster, context and user, separately from neutral target identity. Only embedded
CA with static token or a complete embedded certificate/key pair is supported.
Unknown fields, file credentials, auth helpers, proxies and extensions reject
the whole document, including unselected entries. This intentionally excludes
many otherwise valid shared kubeconfigs. The owner-selected namespace overrides
the optional context default; neither that default nor current-context supplies
authority. Non-secret trust identity is SHA-256 over admitted decoded CA PEM
bytes. Target ID and opaque configuration revision remain owner responsibilities.

Input is limited to 256 KiB, one scalar to 128 KiB, aggregate decoded scalar
bytes to 256 KiB, nesting to 16, callbacks to 32,768, each named list to 128,
names to 256 bytes and tokens to 16 KiB. Generated JSON has a 512 KiB ceiling
checked before emission. A closed typed sink rejects duplicate decoded keys,
wrong scalar/container types, complex/merge keys, anchors, aliases, nulls,
unsupported actual-node tags and multiple documents. Ambiguous plain YAML
boolean/numeric spellings require quoting for string fields. Standard string
tags explicitly preserve string intent. Strict JSON never retries as YAML.

Callbacks check cancellation and application budgets before deeper traversal.
The parser may scan scalars or directives before invoking a callback; these
limits do not prove strict scanner allocation or hard cancellation bounds, nor
detection of unused directives. Strict base64 and PEM envelope checks do not
establish certificate validity or matching private keys. Failures contain only
fixed enums, never parser diagnostics or input excerpts. This milestone does
not enable Kubernetes collection or prove later sealed transfer, credential
custody, launch-time environment isolation or absence of ambient CLI fallback.

## Idle native Ops binding milestone

Target changes use a narrow owner-thread kernel operation that constructs the
native observation registration internally. It prepares the current kernel
registry and the genuine launch policy together, validates the endpoint/source
selection, then selects the broker and commits prepared state with no-throw
moves. It rejects active runs/children, unavailable kernels, custom policies and
foreign registrations; it never accepts a replacement approval mode or arbitrary
policy from a widget or model. Selection performs no collection or durable append.

Kernel and policy preserve their unrelated registries independently. Memory
capture may narrow the current executor while its original launch-policy ceiling
must remain available for a later capture change. Only the native observation
registration is identical across the two new snapshots. Rebinding retains the
actual launch configuration and shared automatic matcher with its counters; it
never reconstructs authority from recorded provenance or carries invocation
approval to another target. Preparation failures preserve the old selection;
exceptional broker cleanup failure closes admission. Existing registry-only
memory rebinding remains unchanged. This seam alone adds no Admin surface.

## Private Kubernetes TLS material and structured projection milestone

The accepted source direction supersedes the earlier proposed kubectl launch:
reuse one coordinated private HTTPS transport after its dependency compatibility
and exact delivery gates pass. This milestone only retains decoded selected TLS
material in the existing move-owned private configuration and projects bounded
core/v1 PodList, Pod and EventList JSON. It advertises no production source,
changes no public launch/kernel/event contract and initiates no IO.

Selected decoded CA plus token or certificate-chain/key payload has a separate
256 KiB aggregate ceiling, in addition to the existing generated JSON 512 KiB
ceiling. Borrowed private views are valid only for their immutable config owner.
PEM envelope checks remain structural; a future TLS client must validate actual
certificates, chains and key consistency. No secure-erasure guarantee is made.

Projection uses private strict SAX traversal with duplicate decoded keys rejected
including ignored fields. Input, callbacks, nesting, scalars, active key bytes
and retained typed values each have explicit rejection budgets; none claims a
formal parser-allocation or hard cancellation guarantee. Default fields are an
allowlist, never whole resource documents. Unknown state/count/time remains
unknown, absent or an explicitly unsupported row. The source must apply known
credential exclusion before publishing; pure projection has no credentials.

The initial workload operation is presented as Pod inventory. Exact Pod health
flattens declared regular/init/ephemeral container names without inventing public
category labels; missing status stays unknown and init/ephemeral exits do not
infer Pod readiness. Namespace event samples accept the existing closed workload
kinds; exact Pod events require matching namespace/name/UID. Missing occurrence
counts cannot become one. Continued lists retain unknown omission totals;
remainingItemCount is an estimate, not exact evidence. All responses retain their
own actual list/resource version, and neutral bounds remain authoritative.

Kubernetes log collection still requires the separately reviewed typed evidence
extension: owner allowance for name-addressed reads with before/after identity
checks, default exact-only, with policy revision invalidating old grants. No
atomic log-to-runtime binding or reinterpretation of legacy records is implied.

## Required failure evidence

- Wrong/foreign target, namespace, generation, resource or log-policy revision;
  missing identity and unsupported target/operation combinations.
- Missing CLI/credentials, authentication/TLS failure, denied reads,
  disconnection, stale results and source replacement without fallback.
- Malformed, duplicate/deep/oversized structured data, numeric overflow,
  truncation and partial resource lists without false completeness.
- Disabled logs issuing zero requests; enabled logs restricted to the selected
  source, time/line/byte budget and known credential exclusions.
- Credential/config source replacement, descriptor collisions and partial
  startup failures; no secret-bearing neutral capture or renderable errors.
- Cancellation, stalled collection/cleanup, target/session replacement and
  late completion retaining bounded capacity and original attribution.
- Store/artifact refusal, replay with zero collection, zero paid calls for
  manual inspection, and equivalent toolbar/menu/command behavior at tiny sizes.

The full delivery must pass deterministic service and Kubernetes diagnostic
journeys, both supported compiler matrices, format/tidy checks, independent
review, PR CI and exact post-merge verification. Tests must establish absence
of infrastructure mutation, Kubernetes exec/attach/port-forward and equivalent
remote effects. The fixed local subprocesses implementing typed reads remain
explicit adapter mechanics, not a claim that no OS execution occurred.


## Provider-independent standalone manual session

The first standalone Ops session creates one fresh durable kernel and exposes a
small borrowed manual-observation surface port. It never constructs a provider,
model catalog, credential resolver or fake model identity. A private fail-closed
no-inference backend satisfies the kernel dependency and outlives kernel work.
The application supplies identity generation, frozen surface/workspace/launch
configuration, the existing shared broker and borrowed durable store/wake sink.
The initial registry marks only observe_target unavailable until owner binding;
this permits exact automatic rules without an executable or source grant.
Explicit Observe allow_all, prompt and automatic configurations retain their real
launch-policy semantics; a profile label alone never changes permission behavior.

Public standalone open is create-only. A duplicate session ID fails storage create
and never replays or interrupts an existing writer. This is not an exclusive-writer
lease or a resume/attach protocol. Chat integration borrows its existing kernel;
historical read-only inspection never invokes resumable durable open. Candidate
open does not activate the application broker, so failure preserves old selection.
All practical preparation/allocation precedes durable create; no fallible surface
initialization follows a successful kernel open.

Owner binding uses the atomic idle native registry/policy transaction. Widgets see
only typed submit/cancel/approval, owner pumping and cached read-only inspection.
Pumping services the broker before and after kernel drain without inference work.
The shared pure projector validates exact human request, observation/result linkage
and terminal completion using the existing history validator. Accepted submission
and RunCompleted alone cannot substitute for committed observation success. Current
operation remains separate from the latest successful timestamped original-target
snapshot. Redraw and inspection perform no IO or implicit refresh.

Explicit close stops admission and cancels the owned manual run durably where the
store permits; failure is reported. Destructor cleanup is best effort and stops
only its captured kernel operation, never a replacement broker issuer. A terminal
run can remain busy until ToolEnded, and retired source work retains bounded shared
physical capacity. No second pool or hard syscall/destructor cleanup bound is added.

## Shared Chat manual observation session milestone

Chat implements the borrowed manual observation port over its existing kernel,
using application-frozen surface/workspace and actual permission identity. Its
optional broker dependency does not activate or replace application selection.
Idle binding preserves distinct current registries and original policy ceilings;
manual availability does not widen model profiles or maxima. Only the exact
human-origin observation proof bypasses model context preparation, and its
committed events remain available for ordinary surface delivery exactly once.
Manual pumping services the existing broker independently of inference polling.
Control results remain historical until explicit bounded model admission.

## Exact-invocation Linux journal milestone

The next Linux adapter milestone reads an explicitly permitted selected service's
default local system journal through adapter-private native libsystemd. This
requires a Linux adapter system development prerequisite, libsystemd >=233, chosen
for the trusted _SYSTEMD_INVOCATION_ID producer field introduced in that version.
A named CMake recipe and FindSystemdJournal module use find_package/pkg-config;
there is no system-manager source build, Meson, runtime dlopen or subprocess/JSON
fallback. Core-only and non-Linux configurations do not activate this dependency.
Client version/linkage does not prove producer visibility or current entry fields.

The existing selected boot/namespace/system-manager proof and canonical unit plus
InvocationID checks bracket collection on one original deadline and capture budget.
Only _BOOT_ID, _SYSTEMD_UNIT and _SYSTEMD_INVOCATION_ID exact conjunctive matches
are admitted; every retained entry's trusted identities are rechecked. Native
field enumeration detects ambiguous identity/message values within fixed bounds.
Library fields and diagnostics remain private; only bounded validated MESSAGE
lines with actual timestamps enter neutral evidence, subject to known credential
exclusions. Exact log consent remains owner-controlled and is rechecked by the
worker/broker before dispatch and durable publication. No model-provided journal
path, root, namespace, selector, format or credential source is introduced. Native
default discovery still uses system journal directories and machine-id filesystem
state in the proven mount namespace; fixed flags are not a no-ambient-I/O or
descriptor-pinned-directory guarantee. The adapter does not mutate process
environment, logging handlers or signal handlers.

This first reader samples the first bounded prefix of the selected recent window;
it does not claim to return the newest tail. Native order and blank lines are
preserved. Recognizable authorization schemes, credential assignments and private
key markers reject the whole sample through a bounded private heuristic; arbitrary
text is not guaranteed free of credentials. No secret store is consulted. Native
read paths may emit library diagnostics containing paths/offsets and fixed errors;
our neutral error conversion does not intercept stderr. Reviewed read paths do not
log MESSAGE payload bytes. No global logging/environment manipulation is added.
Malformed returned records are rejected; libsystemd may internally skip corrupt
items or inaccessible files, which remains part of unknown omissions. Nonempty readable results initially
remain partial with unknown omissions; observed local output limits are truncated.
Empty results without full visibility proof are unavailable. Malformed input, lost
identity, cancellation, capture exhaustion or expiry discards the attempted sample.
Library thresholds and application byte/iteration caps do not establish a hard
libsystemd mapping/decompression/RSS bound or interrupt a stalled syscall. Cursor
ownership/cleanup stays on the existing source worker; a stalled call or destructor
retains its physical slot under the established logical-cancellation contract.

Implementation and capability advertisement require deterministic admission,
identity, field, timing, bounds, visibility and dependency-consumer failure evidence.
No live host log read or successful host compatibility is implied by this milestone.


## Bounded source preparation (189 prerequisite)

Metadata listing does not construct a live source. An owned typed preparation
factory runs on the existing application-wide LocalSourceWorker, with the same
capacity, numeric request-ID high-water mark and session invalidation. Its token
binds session/epoch/request, selected target, independently reserved opaque
configuration revision and kind. No authority, log consent or observation is
created by preparation. Admission performs bounded metadata checks only.

The one original owner-established deadline is checked around producer work and
result validation. Existing Linux source creation retains its internal bound;
it does not inherit this outer deadline. Additional/stalled physical work and
cleanup can outlast logical expiry while retaining the occupied worker slot.

Prepared sources use the existing producer-held grant-result lifecycle. A ready
unclaimed/discarded result is destroyed outside the mutex on its producer before
retirement; owner cancellation and teardown never join it. Successful polling
transfers source ownership and accepted-resource cleanup responsibility to the
application. It does not establish an asynchronous retirement service for later
application-owned shared pointers. Current selection is checked before claim;
later native binding retains its separate atomic/current-authority gates.

## Owner target catalog milestone

`ops.targets` is a typed file-only catalog of at most 32 configured records.
Each record has a stable owner label (`id`), display name and one closed source:
`linux_local`, or `kubernetes_static` with an absolute `config_file`, explicit
`context` and `namespace`. Inline authentication, endpoint overrides, helper
programs, ambient current-context and unknown fields are not catalog options.
The reserved `local` record (`This environment`) is supplied by metadata
resolution; it does not assert host scope or system-bus permission.

Configured IDs are lower-case ASCII letters/digits with internal hyphens or
underscores, up to 64 bytes; display names are safe single-line UTF-8 up to 128
bytes, paths 4096, contexts 256, and namespaces DNS labels up to 63. Retained
configured text has a 64 KiB aggregate ceiling. The existing bounded config-file
reader still owns document framing and decoded duplicate-key rejection. No
referenced kubeconfig is opened by parsing, validation, formatting or catalog
resolution. Invalid Ops configuration refuses resolution even if another
candidate might otherwise provide a fallback.

Catalog IDs and labels are not live bindings, configuration revisions, broker
issuers, generations or log grants. Preparation reserves an opaque revision;
only a successful still-current prepared source can publish that revision with
its proven binding. Log access remains a separate explicit owner choice.
General config output summarizes record count; the later target-list view owns
safe metadata presentation. This milestone supplies catalog configuration only;
CLI/TUI collection and Kubernetes source availability are not implied.

## Standalone Admin command milestone

A separate closed Admin command port dispatches target listing and Linux health,
loaded services and exact service health. Product assembly creates a fresh durable
OpsSession with the genuine Observe launch policy and only the selected native
read registration. No provider/model, credentials, generic shell, child or memory
port is assembled. Metadata target listing opens neither sources nor session
storage. Invalid or incompatible selection never falls back to local collection.

One exclusively owned finite worker performs preparation and collection. Broker
activation follows successful durable creation. Preparation uses the worker's
monotonic ID and a single owner deadline; accepted result ownership transfers
before physical retirement, which is awaited within that original deadline before
capacity-one observation admission. This global-zero check is specific to the
standalone worker, not the future shared TUI graph. Observation timeout narrows
to the remaining allowance. Exact current committed evidence decides command
success; output failure does not recollect or erase durable evidence. Teardown
cancels the exact owned work and keeps storage/source graph alive past kernel
cleanup without claiming hard syscall/destructor interruption.

## Coordinated HTTPS dependency milestone

The private Kubernetes source requires the delivered Venice transport contract,
so AIForge consumes merged Venice source
`339729e945d0b3d6584702ecf49f013c1ad6779a` instead of tag 0.29.17. Installed
packages require at least 0.29.18 plus the exported
`httplib-0.51-openssl3-header-only-v1` capability marker and actual selected
header/API verification; version metadata alone is insufficient. Installed,
preexisting, sibling and fetched targets use the same canonical validation.
An incompatible existing target is refused rather than silently replaced.
The configure probe forwards only the exact selected Venice contract guard from
an isolated build directory. An HTTP header adjacent to an installed Venice
guard cannot substitute for the canonical HTTP target's selected header. The
reviewed upstream checker is reused, with an attributed exact copy for callers
that supply only preexisting exported targets.
Embedded acquisition temporarily promotes installed dependency targets to global
scope so the parent gate and adapters share the same canonical target. The
caller's normal and cached package-search preferences are preserved.

Exactly one header-only `httplib::httplib` target supplies OpenSSL 3, canonical
header limits and synchronous resolver behavior to Venice and AIForge. No second
HTTP header, private feature overrides, HTTP library or global resolver owner is added.
The new public c-ares link dependency is supplied by Venice; AIForge production
calls no c-ares/downloader API and introduces no initialization/cleanup pair.
The consumer fixture calls only the initialization-free c-ares version query
to verify the actual exported link dependency.
Future resolver use must separately establish Venice's startup-thread ownership
before all application threads and retain it through their complete shutdown.

This milestone updates dependency recipes/consumer proofs and the existing audio
multipart fixture. It enables no Kubernetes source, request or log capability.
Later TLS setup stays on the preparation/observation worker: metadata-only factory
construction must not initialize OpenSSL or load ambient crypto configuration.
Transport controls remain bounded rejection mechanisms, not hard process-memory,
DNS/syscall cancellation or destructor deadlines.

### Optional Admin view scheduling prerequisites

The shared source worker exposes exact preparation-token readiness and physical
outstanding metadata. This owner-thread query does not claim, reap, cancel,
perform IO, mint authority, or reserve capacity. A producer-held completion can
be ready before physical retirement, including failed or expired completions;
the explicit poll still checks the original deadline. An absent/reaped token
reports neither state and does not prove prior admission. Claimed/discarded work
remains outstanding until both producer and cancellation relay finish, using
the same retirement predicate as shared capacity accounting. Other jobs may
still consume capacity after this particular token retires.

Chat exposes an infallible ownership transfer of already-buffered committed
surface events, including ordinary and manual spans. It does not pump the
kernel, drain provider events, append history, or clear errors. This permits a
later optional Admin view to deliver explicit manual-pump output after the
manual run becomes terminal without accidentally advancing an ordinary model
run. Controller, toolbar, menu, rendering and explanation actions remain later
integration work; these metadata and delivery APIs enable no collection path.

## Bounded Admin controller milestone

The optional view controller owns only bounded catalog metadata, one exact
preparation/claimed-source retirement state, a current manual submission and
three typed historical snapshots. It borrows the application manual session and
native binding port while attached; detach clears those borrows even when
cancellation refuses. Configuration factories own their private inputs and run
preparation on the existing shared worker. Widget actions never carry endpoints,
credentials, policy replacements, callbacks or arbitrary commands.

A superseding choice cancels the previous exact preparation and may return busy
until physical retirement. It does not queue or retry the newer choice. A ready
result stays producer-owned while the session is busy, using its original
five-second deadline. Claiming and subsequent physical retirement are distinct;
only then does native binding atomically replace active selection. Pending B
never relabels active A, and ordinary refusal retains A. Fatal storage/history or
internal failures latch. Source cleanup after successful claim remains owned by
the application and does not acquire a new pool or cancellation guarantee.

Reads are limited to Linux health, loaded services and exact service health with
logs disabled. Explicit unit input remains name-only; inventory actions require
the original session, observation event, selection generation and row, matching
the full current binding. The controller caches only exact current committed
success and preserves original-target last-good evidence after failure. The
shared private target/unit validators serve CLI, controller and later slash
input. Manual pumping never drains a model; the application separately delivers
buffered events through194. This controller alone supplies no dialog, application
bootstrap, real Chat/SQLite integration proof or completed #226 user journey.

### Admin cached presentation and commands

The first Admin dialog borrows the typed cached controller action port. Widgets
receive no observation source, provider, worker, store or binding authority.
Opening, scrolling, drawing, resizing and switching views never collect data.
Explicit reads and target use dispatch the same typed actions as closed slash
commands. Toolbar visibility is application presentation state; hiding it does
not alter grants, selection, cancellation or keyboard access.

The dialog retains bounded formatted committed snapshots with their original
event and target identity. Service rows carry the displayed session, observation
event, selection generation and row index into the controller's exact cache
check. A changed cache cannot silently reinterpret a delayed displayed action.
Canonical observation formatting retains scope, timestamps, completeness and
unknown values. Historical presentation does not reconstruct current authority.

This presentation milestone does not attach the dialog to Chat, create a new
standalone loop or enable log collection, Explain or Kubernetes requests. Those
application integrations must retain the existing owner-thread session, policy,
approval and physical worker lifecycle contracts.
