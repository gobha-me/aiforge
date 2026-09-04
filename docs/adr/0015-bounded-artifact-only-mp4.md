# ADR 0015: Bounded artifact-only MP4 validation

- Status: Accepted
- Date: 2026-09-04

## Context

The video epic needs a provider-neutral durable artifact before it selects a
provider job lifecycle, codec, decoder, player, renderer, or model-callable
tool. Provider or user-supplied media is an untrusted binary boundary. Merely
labeling bytes `video/mp4`, trusting a remote URL, or retaining a store-returned
digest would not establish either a bounded MP4 envelope or content identity.

The existing artifact store, events, SQLite codecs, and transcript projection
are media-neutral. This decision must reuse them rather than creating a video
event family or embedding encoded bytes in durable history.

## Decision

AIForge owns a small provider-neutral ISO-BMFF/MP4 structural validator. It
uses checked big-endian 32-bit and extended 64-bit box arithmetic and enforces
explicit limits for encoded bytes, visited boxes, nesting depth, tracks, and
compatible brands. The initial defaults are 32 MiB, 4,096 boxes, depth 16, 16
tracks, and 64 compatible brands.

The first top-level box is the only `ftyp`. Its exact, case-sensitive major
brand must be one of `isom`, `iso2` through `iso9`, `isoa`, `isob`, `isoc`,
`mp41`, or `mp42`. Compatible brands are bounded opaque FourCC values. They do
not authorize an excluded major brand.

There is exactly one top-level `moov`, at least one top-level `mdat` with a
nonempty payload, and at least one direct `moov/trak/mdia/hdlr` path whose
handler type is `vide`. The validator traverses only the box levels required
to prove that path. Every box at a traversed level has complete coverage;
unknown payloads remain opaque and inert. A size-zero box is accepted only for
a final top-level nonempty `mdat`. Cancellation is checked before and during
traversal.

Publication accepts owned bytes, validates before the store call, computes the
SHA-256 digest independently, fixes the media type to `video/mp4`, and verifies
the exact returned identity, size, digest, producer metadata, and absent
dimensions. Loading validates recorded metadata before a bounded store read,
requires the returned metadata to match exactly, recomputes the digest, and
structurally revalidates the bytes. Store-controlled diagnostic text is never
copied into the neutral error.

The generic `ArtifactMetadata`, `ArtifactCreated`, `ArtifactReferenced`,
artifact stores, SQLite encoding, and transcript projection remain unchanged.
Encoded bytes remain outside SQLite and events. Replay performs no network,
provider, decode, render, playback, or cleanup action.

## Consequences

- The result proves only a bounded MP4-family envelope and content-addressed
  integrity. It does not prove codec validity, decodability, duration,
  dimensions, seekability, playability, or safe rendering.
- Provider generation, upload, remote references, quote/job lifecycle,
  spending, commands, thumbnails, decoding, playback, and model-callable tools
  require later focused decisions.
- More formats or deeper ISO-BMFF interpretation require an explicit extension
  rather than treating opaque payload bytes as trusted structure.
