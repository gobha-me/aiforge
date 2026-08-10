# ADR 0002: Prompt and context construction precedence

- Status: Accepted
- Date: 2026-08-10

## Context

AIForge must construct a bounded provider request from durable run history,
instructions, selected evidence, and tool results. These inputs have different
authority and trust. Treating them as an undifferentiated transcript would let
repository text, tool output, or a persona accidentally act as runtime policy.
Letting a provider adapter assemble prompts would also make precedence vary by
backend and hide which inputs entered an inference.

The architecture north star requires a provider-neutral context boundary before
personas, project-instruction discovery, or repository evidence depend on an
accidental prompt order. This ADR fixes that boundary without choosing a
retrieval algorithm, tokenizer, provider format, or persistence encoding.

## Decision

### Authority and trust

Instruction authority, from highest to lowest, is:

1. application/runtime contract;
2. workspace instructions;
3. project instructions;
4. persona;
5. session instructions;
6. task instructions.

Conversation content follows the instruction layers and retains its original
role and chronology. Selected repository/file/web/artifact evidence and tool
results are explicitly classified as untrusted context, not instructions.
Imperative text does not change that classification.

Tool declarations are runtime-owned interface descriptions, not another
instruction layer and not capability grants. They travel in the separate tool
field of the backend request in stable registry order. Their target-model size
is included in reserved input capacity. Tool results re-enter context only as
the untrusted, invocation-linked `tool_result` classification.

Runtime-owned safety, permission, capability, and approval policy is not prompt
content and remains authoritative over every layer. No context contribution can
grant or broaden authority.

Applicable project instructions are ordered from repository root toward the
target subtree. A more-specific file may narrow or specialize a broader file in
its scope, but it cannot alter a higher authority layer. Equal-specificity
inputs use explicit order and stable entry identity; container iteration order
is never a tie-breaker.

### Replacement, disabling, and conflicts

Each instruction contribution has stable identity, layer, provenance, explicit
order, and an add, replace, or disable operation. Replace and disable target an
earlier active contribution in the same layer. They cannot reach across layers,
and the application/runtime layer cannot be replaced or disabled. Missing
optional layers contribute nothing and are not errors. At least one
application/runtime contribution is mandatory for every constructed context.

The builder does not interpret prose to detect conflicts. It preserves the
authority and ordering contract so the higher layer, or the more-specific
project contribution within that layer, governs. Replacement and disabling are
recorded in the construction result rather than erasing their provenance.

### Runtime boundary

The application runtime constructs an ordered, provider-neutral context before
starting a backend. Every entry carries:

- stable entry and message identity;
- instruction layer or untrusted-content classification;
- source identity with optional location and digest;
- explicit order and project specificity;
- a target-model token estimate.

The result contains the admitted entries, decisions for superseded or disabled
instructions, the capacity input, and the total estimated input size. Backend
requests carry this constructed context. Evidence has its own neutral role;
adapters must map or wrap that classification explicitly and must not flatten it
into a provider system or user instruction. Adapters do not reorder or
reclassify entries.

Rejected per-entry inputs return a typed error carrying the stable entry ID.
Whole-request capacity failures have no fabricated entry identity.

Capacity is supplied for the actual target model as context-window size,
reserved output, and reserved input for material outside this builder. Arithmetic
is checked. The builder validates a preselected working set and fails if it does
not fit; it does not silently truncate, summarize, rank, or evict inputs. Those
selection policies belong to the later repository-context milestone.

Unknown future content kinds are rejected before a backend request rather than
being guessed into a provider role. Empty or inconsistent provenance and
ambiguous identities are also errors.

## Consequences

- Personas and project instructions can share one explainable precedence path.
- Evidence and tool results remain useful without acquiring instruction
  authority.
- Deterministic fakes can assert the complete context seen by a backend.
- Repository discovery, ranking, summarization, persistence, and provider wire
  formatting remain separate decisions.
- Adding a new instruction authority requires another architectural decision;
  adding a new evidence kind requires explicit builder and adapter support.
