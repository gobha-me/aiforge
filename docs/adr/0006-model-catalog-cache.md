# ADR 0006: Derived model catalog cache

- Status: Accepted
- Date: 2026-08-25

## Context

The model-list command, command-line validation, context budgeting, and the
interactive picker need the same provider-neutral catalog. Requiring a network
request for every lookup makes offline selection impossible, while storing the
provider payload directly would leak Venice and JSON shapes into runtime APIs.
ADRs 0004 and 0005 explicitly leave caches undecided, and the AF-13 milestone
makes this cache and its format necessary.

## Decision

Use a neutral catalog snapshot containing the retrieval time and bounded model
records. Provider adapters map their typed metadata into model identity,
modality, context/output limits, availability, capabilities, and pricing before
the snapshot crosses the adapter boundary. Provider payloads, credentials, and
JSON-library objects are not retained.

Cache snapshots as schema-versioned strict UTF-8 JSON at
`$XDG_CACHE_HOME/aiforge/model-catalog.json`, falling back to
`$HOME/.cache/aiforge/model-catalog.json`. This is replaceable derived data, not
configuration or durable run truth. The application directory and file are
owner-only, symlinked or non-regular targets are rejected, input is bounded,
duplicate keys and unsupported schemas fail closed, and publication uses a
same-directory 0600 temporary file followed by sync, rename, and directory
sync. Concurrent successful writers may publish in either order because each
snapshot is complete and replaceable.

A snapshot is fresh for 24 hours. A fresh cache avoids the network. An expired
cache triggers one live refresh per catalog-service lifetime; a successful
refresh replaces it. An unavailable live source may fall back to an otherwise
valid stale snapshot with an explicit warning. Cancellation never becomes a
stale-cache success. Invalid or insecure cache data is ignored with a warning
only when a live refresh succeeds; if neither source is usable, the operation
fails through a typed error.

## Consequences

- CLI listing, validation, model context lookup, and the TUI picker share one
  in-memory snapshot and deterministic source policy.
- Replay does not fetch or consult the cache; runs continue to record the model
  actually used through existing provenance and inference events.
- The format is intentionally limited to model catalog snapshots. It does not
  select an artifact encoding, a general cache database, or session storage.
