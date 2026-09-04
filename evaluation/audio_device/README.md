# Audio-device boundary evaluator

This directory contains AIForge's opt-in, noninstalled Linux evaluator for the
bounded PCM playback and capture boundary accepted in ADR 0014. It compares
fixed RtAudio and miniaudio builds through synthetic backends and exercises the
neutral lifecycle contract with hermetic fakes. It does not enumerate or open
physical devices, request microphone permission, ship a production adapter,
or make captured samples available to a provider.

Configure and build it explicitly:

```sh
cmake -B build-audio-evidence -Daiforge_AUDIO_DEVICE_EVALUATION=ON
cmake --build build-audio-evidence --parallel 2
```

Explicit enablement on a non-Linux target is a configuration error. Evaluator
tests are built and registered only when `aiforge_TESTS=ON`.

Capture a report for the exact source revision:

```sh
timeout 30s \
  build-audio-evidence/evaluation/audio_device/aiforge_audio_device_evaluation \
  --source-sha "$(git rev-parse HEAD)" \
  --output build-audio-evidence/audio-device-evidence.json
```

The parent launches only its three fixed sibling probes. Each child is bounded
to five seconds and 16 KiB of output, and the report is bounded to 64 KiB.
Callers must impose the documented 30-second outer deadline because native
teardown in an uninterruptible kernel state cannot be bounded in process.
Missing, malformed, reordered, unknown, timed-out, signaled, oversized, or
cleanup-indeterminate output fails closed. Reports contain fixed dependency
identities, synthetic backend results, contract results, architecture, and the
exact source revision. They contain no device metadata, paths, environment
values, host identity, credentials, raw diagnostics, or sample bytes.

CI evidence proves only the synthetic lifecycle and build contracts on that
exact Ubuntu Linux x86-64 run. It is not a production capability cache and
makes no claim about physical hardware, user permissions, or other platforms.
