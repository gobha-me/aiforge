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
