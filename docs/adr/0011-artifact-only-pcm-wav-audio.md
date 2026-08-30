# ADR 0011: Artifact-only PCM WAV speech and transcription

- Status: Accepted
- Date: 2026-08-30

## Context

AIForge needs a first provider-neutral audio slice without selecting a codec or
device library. Speech results are paid binary media that must survive replay,
while transcription uploads are private user-selected bytes whose exact
identity must be durable without putting the bytes or source path in session
events. The existing content-addressed store is media-neutral, but generated
artifact events and run start did not yet cover non-image output or explicitly
imported user artifacts.

## Decision

The first slice supports buffered, uncompressed PCM in a little-endian
RIFF/WAVE container only. A bounded AIForge parser verifies the RIFF length,
chunk traversal and padding, one PCM format chunk, one non-empty data chunk,
channels, sample rate, sample width, byte rate, block alignment, and complete
sample frames. It does not decode, resample, play, or capture audio. Encoded
audio is limited to 32 MiB; synthesis and transcript text are limited to 1 MiB.

Provider-neutral audio ports accept typed model and voice identities, bounded
text, an optional bounded language tag, and owned bytes with their actual media
type. Venice maps these values to buffered speech and transcription calls,
always requests WAV speech, and uploads transcription input under the fixed
name `audio.wav`. Caller paths, credentials, multipart bodies, and raw provider
errors never enter events or renderable diagnostics.

The backend emits the generic `ArtifactProduced` event for synthesized speech.
The run kernel records its `ArtifactCreated`, `ArtifactReferenced`, and
assistant artifact-reference content before inference completion. A
transcription surface stores and validates the input first, then supplies its
metadata as an imported run-start artifact. The kernel atomically records the
create/reference facts with user content before launching inference. Invalid
or unreferenced imported metadata rejects the entire start.

The command contract is:

```text
aiforge audio synthesize --model ID --voice ID [--language TAG]
                         [--output PATH] TEXT...
aiforge audio transcribe --model ID [--language TAG] INPUT.wav
aiforge audio export --session ID [--artifact ID] --output PATH
```

Model selection uses catalogue types `tts` and `asr`. Synthesis and
transcription are explicit user actions, not model-callable tools, so this
slice introduces no ambient file or microphone authority. Transcription opens
only the explicitly named regular file without following symlinks and rejects
concurrent changes. Export creates a new file exclusively. Replay and export
never call a provider or repeat inference.

Transcription normalizes JSON or plain-text provider results into bounded UTF-8
text. Newline and tab are allowed; other control bytes are rejected before
they enter durable content or stdout.

## Consequences

- Original PCM WAV bytes deduplicate in the existing content-addressed store;
  SQLite retains only typed artifact metadata and references.
- A failed run can leave an unreferenced content-addressed blob. Garbage
  collection remains deferred until all durable-session reachability is known.
- Streaming synthesis, timestamp projections, playback, microphone capture,
  voice cloning, asynchronous music, decoding, resampling, non-WAV containers,
  and TUI audio controls remain separate decisions under issue #47.
- Growth beyond structural PCM WAV validation requires a later ADR and an
  explicit decision on whether the reusable codec/device boundary belongs in
  AIForge or a separate controlled dependency.
