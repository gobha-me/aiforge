# ADR 0014: Bounded PCM audio-device boundary

- Status: Accepted
- Date: 2026-09-04

## Context

ADR 0011 established artifact-only PCM WAV speech and transcription. Local
playback and microphone capture add a different authority boundary: native
device callbacks outlive ordinary function calls, may race teardown, and must
not expose private samples or device identity through events, logs, evidence,
or provider requests. A library that compiles or opens a synthetic backend is
not evidence that physical hardware, permissions, or a particular format will
work on a user's machine.

The first device seam must keep callback and library types out of AIForge's
domain and run kernel, preserve bounded ownership, and fail closed through
open, start, streaming, stop, and close. It must also remain useful when no
audio device exists, as in CI.

## Decision

AIForge owns a small, whole-operation audio-device port. The initial format is
signed 16-bit little-endian PCM with one or two interleaved channels. Playback
accepts an owned, validated frame buffer. Capture accepts a bounded request,
preallocates its destination, and publishes bytes only after successful stop
and close. Unsupported formats, incomplete frames, arithmetic overflow, and
requests outside the configured frame and byte ceilings are rejected before a
device is touched.

Only one playback or capture operation may run at a time. Realtime duplex,
mixing, resampling, non-WAV decoding, and streaming public APIs are excluded.
The public contract is synchronous and cooperative: its lifecycle is
`opening`, `starting`, `streaming`, `stopping`, `closing`, then one terminal
result. Cancellation never skips stop or close. A cleanup failure outranks a
cancellation result because the port cannot claim a quiescent device when
cleanup is uncertain.

Native callbacks are private adapter details. They are `noexcept`, bounded,
and perform no allocation, locking, logging, filesystem access, provider work,
or UI work. Only the controller thread may call device lifecycle operations.
Playback owns its samples until callbacks are proven quiescent. Capture writes
only within its preallocated buffer. `stream()` returns only after its callback
interval is quiescent. The callback object remains alive through `close()` so
that a broken adapter can be rejected safely, but any callback after `stream()`
returns, including during stop or close, is late. Any callback after a terminal
callback decision is a duplicate. Both are contract violations and cannot
publish captured data; `close()` must prove that none can arrive afterward.

Playback and capture are explicit user actions and are never model-callable
tools. Starting capture requires a visible active state. Captured bytes remain
in process memory unless the user separately approves a persistence or
transport action. Session replay, artifact export, and context reconstruction
never access an audio device.

The first private Linux adapter will use RtAudio. RtAudio's small static C++
surface and explicit callback/lifecycle model fit this boundary with less
adapter-owned platform code than miniaudio. Miniaudio remains the comparison
candidate: its single-header implementation and null backend are useful, but
its wider optional codec, engine, graph, and backend surface increases compile
configuration and review cost for this narrow device-only seam. Neither
library type becomes a public AIForge type.

The evaluation pins RtAudio 6.0.1 and miniaudio 0.11.25, builds static targets,
and disables examples, tests, tools, codecs, engines, node graphs, resource
management, and generation features that are outside the comparison. RtAudio
is compiled with its dummy API available and only the Linux ALSA production
backend configured. Miniaudio is compiled with its null backend plus the Linux
ALSA and PulseAudio candidates. RtAudio uses its MIT-style license and links
`libasound`; its fallback builds two C++ implementation units and therefore
requires the Linux ALSA development package. Miniaudio is consumed under its
MIT No Attribution alternative; its fallback builds one C implementation unit
and links threads, `dl`, and `m`, while ALSA and PulseAudio are runtime-loaded.
The evaluator adds no codec package. Both libraries remain evaluation-only,
static, noninstalled, and unexported. An exact installed package is usable only
when it advertises AIForge's matching, reviewable evidence-profile property;
otherwise configuration fails closed and the controlled fallback must be
selected explicitly.

An opt-in, noninstalled Linux evaluator compares the two candidates through
forced synthetic backends only. It never selects a default backend, enumerates
physical devices, records device names, or requests microphone permission.
Hermetic lifecycle fakes cover absent devices, permission denial, device loss,
unsupported formats, underrun and overrun, concurrent use, cancellation at
every lifecycle boundary, teardown races, and callback quiescence. The
candidate probes cover only library identity, fixed build configuration,
synthetic open/start/callback/stop/close behavior, and bounded cleanup.

Evidence is a canonical, closed-schema JSON report no larger than 64 KiB. Each
candidate runs in a disposable child bounded to five seconds and 16 KiB of
output. Callers impose a 30-second outer deadline because a native process in
an uninterruptible kernel state cannot be given an honest in-process wall-time
guarantee. Missing, reordered,
duplicated, unknown, malformed, oversized, timed-out, signaled, or
cleanup-indeterminate results are `probe_error`, never positive evidence. The
report contains the exact 40-character lowercase source revision, fixed
candidate/version/source/linkage/backend identities, closed reason codes, and
the Linux architecture. It contains no paths, environment values, device
metadata, usernames, hostnames, credentials, raw diagnostics, or sample bytes.

The evaluator is enabled only by `aiforge_AUDIO_DEVICE_EVALUATION=ON`; enabling
it on a non-Linux target is a configuration error. Ubuntu 24.04 Linux x86-64
CI publishes separate GCC and Clang reports for the exact source revision.
Those reports validate the evaluator and synthetic contracts only. They make
no physical-device, other-distribution, macOS, Windows, or production support
claim.

RtAudio is preferred for the first future Linux adapter, not selected as an
already-shipped adapter. Playback and capture each require a focused follow-up
issue with the public port, explicit UI/CLI activation, artifact fallback,
adversarial lifecycle tests, and exact-SHA release evidence. A production
adapter must fail closed when its requested backend, device, permission,
format, or cleanup contract is unavailable. Cooperative cancellation does not
authorize hard-killing native teardown; an outer evaluator deadline invalidates
the report rather than approving cleanup.

The boundary stays in AIForge until a second real consumer demonstrates a
generic library contract. No separate project or plugin ABI is created by this
decision.

## Consequences

- Domain and run-kernel APIs remain independent of RtAudio, miniaudio, native
  handles, and callback types.
- No-device CI can prove ownership, bounds, lifecycle ordering, cancellation,
  quiescence, and report integrity without claiming hardware support.
- Capture persistence and any network transport require separate explicit
  authority; replay remains side-effect free.
- A failed or cancelled capture produces no publishable buffer. Playback can
  fall back to the already durable WAV artifact in a later surface.
- Future platform support needs its own backend evidence and accepted scope.
- A future need for codecs, resampling, duplex, mixing, or reusable extraction
  requires a separate decision.
