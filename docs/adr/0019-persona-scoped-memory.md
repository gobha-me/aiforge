# ADR 0019: Persona-owned memory

- Status: Accepted
- Date: 2026-09-04

## Context

ADR 0009 defines global and repository-owned memory as untrusted, inspectable
evidence. A selected persona can also benefit from durable preferences and
workflows, but persona memory must not become an instruction layer or a route
to capabilities.

## Decision

Memory proposals and records have exactly one `MemoryOwner`: global,
repository, or persona. Persona ownership uses the immutable generated
`PersonaId` recorded by the producing run. The identity survives accepted
renames and content edits. Deletion makes it dormant; recreating a persona gets
a new identity unless the user explicitly rebinds the dormant identity.

The model chooses only the `persona` scope; it cannot supply or override the
identity. Persona capture defaults to `review` through
`memory.persona.capture` and also accepts `off` or `auto`.

Global, current-repository, and active-persona records compete within one
shared relevance budget. Equal-relevance ties prefer persona, then repository,
then global. Contradictory records remain independently visible with their
provenance; selection does not silently resolve or rewrite them. Selecting
another persona or disabling personas omits the previous persona owner from
the next request.

All persona memory uses the existing untrusted memory kinds and evidence
classification. It does not grant tools, permissions, capabilities, prompt
precedence, or any other authority. It does not introduce a persona resource
root for images, history, or related files; that broader storage design is
deferred.

## Consequences

- Memory journals and event codecs persist explicit owners and continue to
  decode schema-v1 global and project memory.
- Proposed, Saved, and History views include the active persona owner and
  mutation commands can target an exact owner explicitly.
- Rename, accepted content change, deletion, and explicit rebinding are persona
  lifecycle operations; memory never infers identity from display text or a
  path.
- Cross-persona reuse requires an explicit new proposal. Similar names and
  content never imply ownership.
