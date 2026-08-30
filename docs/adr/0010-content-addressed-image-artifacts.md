# ADR 0010: Content-addressed generated-image artifacts

- Status: Accepted
- Date: 2026-08-30

## Context

Generated images are too large and too binary to become event payloads, but a
durable session must still explain which exact media a provider returned. A
display replay must neither rerun a paid generation request nor trust a remote
URL. The storage boundary must also fail closed when metadata, file identity,
permissions, or content no longer agree.

## Decision

AIForge stores original encoded generated-image bytes beneath the process XDG
state directory. Blobs use the path
`artifacts/sha256/<first-two-hex>/<sha256-hex>` and owner-only directory and
file permissions. Publication writes and synchronizes a new owner-only
temporary file, then hard-links it into its digest path without overwriting an
existing blob. Equal content deduplicates while each run retains its own stable
`ArtifactId` in event metadata.

Reads resolve only a validated lowercase SHA-256 digest. They refuse symlinked
or permissive managed directories, non-regular or permissive blobs, size
mismatches, configured-limit violations, cancellation, and digest mismatches.
Export is a separate explicit `--output` operation and uses exclusive creation;
the artifact store never silently writes a user-selected path.

An image inference returns provider-neutral owned encoded bytes and an actual
media type. RasterForge performs bounded signature detection and static
PNG/JPEG/WebP decode before storage becomes visible to the run. The detected
format, declared media type, positive dimensions, encoded-byte bound, decoded
byte bound, temporary byte bound, and cancellation state must agree.

After storage succeeds, the backend emits a neutral artifact observation. The
run kernel appends `ArtifactCreated`, `ArtifactReferenced`, and an assistant
artifact-reference content block before inference completion. Events retain
only metadata: artifact identity, media type, byte size, SHA-256 digest,
producing inference, and dimensions. Encoded bytes never enter SQLite or a
renderable error.

Terminal replay reads the artifact by its recorded metadata and validates it
again. TermForge receives a validated PNG encoding only when the selected
driver supports that exact path; otherwise AIForge passes bounded decoded RGBA.
A terminal viewer owns TermForge screen setup and teardown. Nonterminal output
is one control-free metadata line with the same session and artifact identity.

The command contract is:

```text
aiforge image generate --model ID [--format auto|png|jpeg|webp]
                       [--output PATH] PROMPT...
aiforge image show --session ID [--artifact ID] [--output PATH]
```

`show` selects the latest generated image by default. It replays local events
and content only; it performs no provider request, generation, or network
fetch.

## Consequences

- Correcting media metadata appends new events; stored session history is not
  rewritten.
- Missing, forged, corrupted, or insecure blobs are explicit display failures.
- Blob garbage collection is deferred because reachability must account for
  every durable session before deletion can be safe.
- Animation, image editing, arbitrary placement, and remote-link fetching are
  outside this decision.
