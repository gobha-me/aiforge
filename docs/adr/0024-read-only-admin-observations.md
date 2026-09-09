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
