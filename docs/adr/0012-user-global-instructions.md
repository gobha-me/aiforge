# ADR 0012: User-global instruction authority and run-pinned sources

- Status: Accepted
- Date: 2026-09-04
- Supersedes: ADR 0002 instruction-authority ordering

## Context

AIForge users need one persistent instruction document that applies without
selecting a persona. ADR 0002 makes the application/runtime contract mandatory
and non-replaceable, while personas are explicitly selected and session
history is not a configuration source. Treating editable user text as either
the runtime contract or an implicit persona would obscure its authority and
make provenance and disabling behavior ambiguous.

The document may change while a run is awaiting a tool, question, or approval.
AIForge records provenance once at run start and currently retains one context
snapshot across automatic tool continuations. Reloading an instruction source
inside that run would change behavior without a corresponding run-start fact
and would make replay depend on filesystem timing.

## Decision

### Authority and identity

Instruction authority, from highest to lowest, is:

1. application/runtime contract;
2. user-global instructions;
3. workspace instructions;
4. project instructions;
5. persona;
6. session instructions;
7. task instructions.

The new layer does not change the remaining trust and replacement rules from
ADR 0002. A user-global contribution is an `add` operation at its own layer. It
cannot replace or disable the application/runtime contract or any contribution
at another layer. Its prose cannot grant tools, capabilities, permission,
approval bypasses, or safety-policy changes. Evidence, memory, artifacts, tool
results, and conversation content remain non-authoritative context.

There is one canonical document:

```text
$XDG_CONFIG_HOME/aiforge/instructions/global.md
```

When `XDG_CONFIG_HOME` is unset or relative, the ordinary configuration-home
fallback is `$HOME/.config/aiforge/instructions/global.md`. Its portable source
location is `instructions/global.md` and its fixed source identity is
`aiforge:user-global-instructions`. Source provenance combines that identity
and location with the exact SHA-256 digest and byte size of admitted content.
Absolute user paths and document text do not enter durable provenance.

### Activation, replacement, and disabling

The user-global layer has an explicit enabled setting whose compiled default is
enabled. A missing canonical document is optional and contributes nothing.
Disabling the layer retains the file and omits it from context; it does not
create a cross-layer disable instruction. Enabling a present document requires
a bounded, stable, validated read before a new run may start. No recursive
includes, environment interpolation, credential import, or alternate path is
supported.

The in-application editor creates the missing canonical document or replaces
one exact observed digest. Writes use restrictive permissions and atomic
publication. A stale precondition fails without overwriting concurrent work.
An indeterminate durability result requires a reload before another write or
live-state change. Editing and activation changes are idle-only and affect the
next newly started run.

### Run pinning and resume

Each newly started run loads at most one enabled user-global document and pins
that exact validated snapshot. Every automatic inference continuation in the
same run retains its original text and provenance and does not reload the
filesystem. Run-start validation requires the recorded reference and the
constructed user-global context entry to match bidirectionally.

Completed historical runs retain their exact reference without requiring the
source file to remain available. Recovering a pending run after process restart
may reload the canonical file only when its identity and digest match the
recorded run reference. Changed, removed, malformed, or unavailable content
blocks continuation rather than substituting a new instruction into the old
run. After the pending run is cancelled or completed, a new run may load and
record the current document.

Resume never restores user-global content as session state. It preserves old
run provenance while resolving the current enabled document for the next new
run. Existing persona files and selections are not migrated, copied, or
implicitly promoted into the user-global layer.

### Validation, capacity, and presentation

The source accepts non-empty bounded UTF-8 text and rejects unsafe controls,
symlinks, path escape, non-regular entries, unstable reads, and insecure
mutation targets. Context construction accounts for the document with the
actual target-model estimate and fails before provider transport when required
content exceeds capacity. It does not silently truncate or demote the source.

Presentation reports that the admitted contribution is at the user-global
layer, along with its fixed portable source, digest, byte size, activation
state, and context-budget estimate. AIForge does not interpret prose conflicts
or claim that one instruction text semantically "won"; authority ordering is
the explainable result.

Venice provider system-prompt inclusion remains an independent provider request
setting. It neither enables nor disables this layer and has no instruction
authority semantics.

## Consequences

- User defaults are independent from selectable personas and protected runtime
  policy.
- A run is deterministic with respect to user-global instructions across tool,
  question, and approval continuations.
- Source changes become visible at a new run boundary and remain auditable by
  exact digest without persisting instruction text.
- Recovering a pending run may require restoring the recorded source bytes or
  cancelling that run; silently continuing with newer instructions is unsafe.
- Workspace and project instruction discovery can retain their own ownership
  while sharing one explicit precedence path.
