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
