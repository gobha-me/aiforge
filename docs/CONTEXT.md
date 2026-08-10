# AIForge Context Architecture

## Repository Awareness, Derived Knowledge, and Inference Context

**Status:** Proposed design paper · subordinate to the north star and accepted ADRs
**Date:** 2026-08-10
**Scope:** Primarily the Code workspace; reusable by other workspaces where appropriate

## Authority

This document is subordinate to:

* `docs/ARCHITECTURE-NORTH-STAR.md`
* accepted ADRs under `docs/adr/`
* repository-level development instructions such as `AGENTS.md`

Where this document conflicts with the architecture north star or an accepted ADR, the north star or ADR governs.

This document is not an implementation specification and does not prescribe a final storage engine, parser library, database, file layout, or concrete C++ class structure.

Its purpose is to establish the architectural model from which focused implementation issues can be derived.

---

# 1. Purpose

AIForge needs repository-aware context construction for the Code workspace.

The problem is not simply that model context windows are finite.

Coding agents operate against repositories that may contain far more information than should be included in any single model inference. Sending large amounts of source indiscriminately causes:

* high token cost;
* attention dilution;
* repeated re-reading;
* loss of important information during compaction;
* stale summaries surviving source changes;
* unnecessary coupling between repository size and inference cost.

Aggressive summarization creates the opposite problem: information is discarded and then repeatedly rediscovered.

AIForge therefore needs a context architecture that keeps durable evidence, repository state, reusable derived knowledge, and disposable inference context distinct.

The design goal is:

> Provide each inference with the smallest sufficient, provenance-carrying working set while preserving exact source and durable evidence outside that working set.

This is not a proposal to create a second agent runtime or a special coding inference engine.

Code remains a workspace using the same run kernel as other AIForge workspaces.

---

# 2. Architectural Fit

AIForge retains its existing dependency direction:

```text
surfaces
(TUI / CLI / JSONL / future surfaces)
                |
                v
application runtime
(runs / tools / policy / projections / context construction)
                |
                v
domain types and ports
(events / content / artifacts / context / repository ports)
                ^
                |
adapters
(filesystem / VCS / build / language analysis / providers / UI)
```

Repository context intelligence is therefore not a new vertical subsystem below the runtime.

Its responsibilities are distributed according to existing boundaries.

## Application runtime responsibilities

The application runtime owns policy such as:

* what context is useful for the current inference;
* context admission and budgeting;
* context expansion;
* summarization selection;
* working-set retention;
* task-phase-aware selection;
* provider-capacity inputs;
* construction of the final provider-neutral inference request.

## Domain responsibilities

Provider-independent domain types and ports represent concepts such as:

* repository snapshots;
* repository evidence;
* source identity;
* derived knowledge;
* provenance;
* freshness;
* context parcels;
* context-selection requests;
* analyzer capabilities.

## Adapter responsibilities

Adapters observe or derive repository information through facilities such as:

* filesystem access;
* version-control systems;
* build systems;
* compilers;
* language analyzers;
* test runners.

## Provider responsibilities

Provider adapters receive the final provider-neutral inference request.

They do not own repository indexing, repository intelligence, summarization policy, or working-set selection.

---

# 3. Four Distinct Context States

The term "context" is overloaded.

AIForge must distinguish at least four kinds of state.

## 3.1 Durable run events

Durable run events are historical evidence.

They are append-only.

They may include:

* user content;
* assistant content;
* inference lifecycle;
* tool lifecycle;
* questions and answers;
* usage;
* artifact creation or reference;
* policy decisions;
* child-run relationships.

Persisted events are not evicted because a context window is full.

They are not rewritten by summarization.

A correction or later interpretation is represented as additional evidence rather than mutation of persisted history.

---

## 3.2 Repository snapshot

A repository snapshot identifies the source state against which repository observations are valid.

A snapshot may include or reference:

* VCS revision;
* branch or detached state where relevant;
* dirty-worktree status;
* tracked modifications;
* untracked files where relevant;
* source-content digests;
* repository root identity.

A clean VCS revision is one possible snapshot.

A dirty working tree is also a valid repository state and must be representable.

Repository identity must not depend solely on line numbers.

Line numbers are useful locations, but edits make them unstable.

A repository fact should ultimately be attributable to source identity such as:

```text
repository
snapshot
file
content digest
symbol or source range
```

---

## 3.3 Derived repository knowledge

Derived knowledge is reusable understanding computed from repository evidence.

Examples include:

* symbol definitions;
* signatures;
* include/import relationships;
* call relationships;
* build-target relationships;
* test relationships;
* semantic summaries;
* ownership relationships;
* diagnostics;
* generated-file relationships;
* inferred behavioral descriptions.

Derived knowledge is:

* rebuildable;
* versioned;
* freshness-aware;
* provenance-carrying;
* invalidatable.

It is not durable historical truth.

It may be replaced when its source changes or its producer improves.

---

## 3.4 Per-inference working set

The working set is the bounded material actually selected for one model inference.

It may contain:

* instructions;
* recent conversation;
* selected run evidence;
* repository summaries;
* exact source excerpts;
* diagnostics;
* diffs;
* tool results;
* artifact excerpts;
* repository knowledge.

The working set may be:

* compressed;
* reordered;
* summarized;
* omitted;
* expanded;
* evicted completely after inference.

Eviction applies primarily here.

---

# 4. Core State Invariants

The four states have deliberately different lifetime rules.

```text
Durable run events
    append-only
    never silently rewritten

Repository snapshot
    exact identity of observed source state

Derived knowledge
    rebuildable
    invalidatable
    versioned

Inference working set
    disposable
    budgeted
    freely compacted or evicted
```

Therefore:

```text
Evict from inference context       allowed

Invalidate stale summary           allowed

Rebuild repository index           allowed

Replace derived interpretation     allowed

Delete source evidence because
a summary exists                    not allowed

Rewrite historical run events
during compaction                    not allowed
```

"Memory" should not be used ambiguously for all four concepts.

Where useful, AIForge should prefer explicit terms such as:

* run history;
* repository snapshot;
* repository knowledge;
* inference working set.

---

# 5. Repository Knowledge Is a Graph

The repository knowledge model is not fundamentally a directory tree.

Filesystem hierarchy remains useful for navigation, but software relationships are many-to-many.

Possible graph entities include:

* repository;
* snapshot;
* file;
* directory;
* source range;
* symbol;
* class/type;
* function/method;
* module/package;
* build target;
* test;
* generated artifact;
* diagnostic;
* configuration;
* commit/revision.

Possible relationships include:

```text
contains

defines

references

calls

imports

includes

inherits

implements

builds

depends-on

generates

generated-from

tests

diagnoses

changed-by
```

Example:

```text
                         +--> Symbol B
                         |      calls
                         |
Symbol A ----defined-in--+--> File X
    |
    +----built-by-----------> Target Y
    |
    +----tested-by----------> Test Z
    |
    +----diagnosed-by-------> Diagnostic D
```

This is a domain relationship model.

It does not imply use of a graph database.

Storage representation is an implementation decision.

---

# 6. Evidence and Trust

Repository evidence must not be confused with instruction authority.

A compiler or parser may be authoritative about an observation made against a particular repository snapshot.

For example:

```text
Observed:
    function F exists at source location X

Derived:
    function F references symbol G

Inferred:
    function F appears to implement retry behavior
```

Those confidence/provenance distinctions do not make repository contents trusted instructions.

Repository files, comments, generated source, build output, diagnostics, tool output, web content, and artifacts remain evidence subject to runtime instruction and capability policy.

Imperative text discovered in ordinary source evidence does not become a higher-precedence instruction merely because the model read it.

Instruction authority and evidence confidence are separate concerns.

---

# 7. Provenance Requirements

Every reusable derived record should be attributable to its origin.

Depending on record type, provenance should be capable of representing:

```text
repository identity

repository snapshot identity

source file identity

source content digest

source range or symbol identity

dirty-worktree state

producer

producer version

creation time

derivation inputs

confidence/type of derivation

invalidation rule
```

Semantic summaries require the same discipline.

For example:

```text
Symbol:
    Foo::bar

Source snapshot:
    <snapshot-id>

Source digest:
    <digest>

Producer:
    semantic-summary

Producer version:
    2

Created:
    <timestamp>

Purpose:
    validates request before dispatch

Validity:
    invalid when source digest or relevant dependencies change
```

A semantic summary must never silently survive a source change that invalidates its assumptions.

---

# 8. Freshness and Invalidation

Repository knowledge must expose whether it is current.

Derived records may become stale because of:

* file modification;
* branch/revision change;
* dirty-worktree change;
* generated-source regeneration;
* build configuration change;
* dependency change;
* analyzer-version change;
* summary-producer change.

Invalidation does not necessarily require eager recomputation.

A record may instead become:

```text
current

possibly stale

stale

unavailable
```

The runtime can then decide whether to:

* reuse it;
* refresh it;
* omit it;
* fall back to exact source.

No stale repository interpretation should silently present itself as current.

---

# 9. Code Workspace Instruction Discovery

Repository-aware work must discover applicable project instructions.

This includes repository and nested `AGENTS.md` files where applicable.

Discovery must preserve:

* source location;
* applicable repository subtree;
* provenance;
* precedence inputs.

The exact global instruction precedence remains governed by the architecture north star and the ADR that ultimately fixes the precedence contract.

Repository instruction discovery must not invent its own precedence model.

A nested project instruction may narrow behavior for its applicable scope but may not increase capabilities or override runtime-owned policy.

---

# 10. Task-Phase-Aware Context

Coding work changes information needs over time.

Context selection should therefore understand the current task phase.

Initial conceptual phases are:

## Orientation

Goal:

Understand where the work belongs.

Typical evidence:

* repository overview;
* module relationships;
* build topology;
* relevant project instructions;
* high-level symbols;
* architectural documents.

## Diagnosis

Goal:

Understand existing behavior or a failure.

Typical evidence:

* target symbols;
* callers/callees;
* exact diagnostics;
* tests;
* recent relevant changes;
* build relationships;
* source excerpts.

## Editing

Goal:

Perform a source modification safely.

Typical evidence:

* exact current source;
* directly affected interfaces;
* applicable project instructions;
* local dependencies;
* dirty-worktree state;
* baseline source digest.

## Verification

Goal:

Determine whether the modification works.

Typical evidence:

* current diff;
* compiler diagnostics;
* build results;
* tests;
* lint/static-analysis results;
* relevant runtime output.

## Review

Goal:

Evaluate the completed change independently.

Typical evidence:

* requirement/issue;
* resulting diff;
* affected interfaces;
* verification evidence;
* relevant project instructions;
* receipts from previous work.

Task phases are context-selection hints.

They are not separate run types or separate inference engines.

---

# 11. Context Acquisition

Repository access should increasingly express the knowledge required rather than defaulting to arbitrary bulk file reads.

A useful conceptual request is:

```text
Object:
    AuthenticationManager::validate

Purpose:
    diagnose failing authentication test

Phase:
    diagnosis

Requested representation:
    interface + implementation

Related evidence:
    callers
    failing diagnostics
    relevant tests
```

The runtime may satisfy this from:

* exact source;
* valid derived knowledge;
* repository graph relationships;
* artifacts;
* tool execution.

The original read remains traceable through provenance.

---

# 12. Purpose of Read

Repository reads should carry intent where practical.

At minimum, context acquisition should be capable of recording why evidence was requested.

Examples:

```text
orientation

understand interface

trace dependency

diagnose failure

prepare edit

verify edit

review change
```

This purpose is runtime/agent metadata.

It is not necessarily durable domain truth.

Purpose metadata can help with:

* context ranking;
* later compaction;
* debugging context-selection behavior;
* understanding repeated reads;
* evaluating context effectiveness.

The model may propose the purpose, but the runtime owns its representation and policy implications.

---

# 13. Context Parcels

A useful unit of context exchange is a bounded, provenance-carrying parcel.

Conceptually:

```text
ContextParcel

purpose:
    diagnose compiler failure

phase:
    diagnosis

repository snapshot:
    <snapshot>

contents:
    diagnostic artifact
    affected symbol summary
    exact target function
    related interface

provenance:
    compiler
    filesystem
    repository knowledge

freshness:
    current

estimated inference cost:
    model-dependent estimate
```

A parcel is not necessarily a persisted object or final C++ type.

It describes the architectural properties a selected context unit should retain.

---

# 14. Exact Source Before Editing

Derived summaries are navigation aids.

They are not sufficient authority for source modification.

Before editing a source region, the Code workspace must retrieve the exact current source relevant to that edit.

This protects against:

* stale summaries;
* incorrect semantic inference;
* line movement;
* intervening modifications;
* differences between declarations and implementation.

The source identity observed before editing should be usable for later change detection.

---

# 15. Dirty Worktrees and Concurrent Changes

The repository cannot be assumed static during an agent run.

The Code workspace must be capable of detecting that source has changed between:

```text
observation
    ->
reasoning
    ->
edit
    ->
verification
```

Relevant changes include:

* edits by the user;
* edits by another agent/run;
* formatter changes;
* build-generated changes;
* branch/revision changes.

Mutating operations should avoid overwriting unseen changes.

Exact mechanics are implementation-specific, but the architecture must retain enough source identity and digest information to detect stale assumptions.

---

# 16. Large Read Policy

Large reads are not prohibited.

They are exceptional.

The important rule is not:

> Never read large files.

The rule is:

> Do not place large amounts of evidence into an inference working set without an explicit reason and budget decision.

Preferred progression:

```text
repository orientation
        |
        v
relationship / symbol discovery
        |
        v
interface or focused evidence
        |
        v
exact implementation when required
```

A large result may instead become an artifact with:

* stable identity;
* digest;
* provenance;
* bounded excerpt;
* optional summary.

The complete evidence remains available outside the current model context.

---

# 17. Repository Evidence Beyond Source Code

Coding correctness depends on more than source text.

Repository context must be able to incorporate evidence from:

* compiler diagnostics;
* build systems;
* test results;
* static analyzers;
* diffs;
* VCS history;
* blame/history where useful;
* build graphs;
* generated-source relationships;
* configuration.

These are repository evidence sources, not separate truth systems.

Where results are large, artifact references and bounded excerpts should be preferred.

---

# 18. Generated, Vendor, and Binary Content

Not every repository file should receive identical treatment.

The runtime and adapters should be able to identify or classify where possible:

* generated source;
* vendored dependencies;
* binary files;
* large data files;
* build output;
* ordinary authored source.

Policies may prefer source-of-generation over generated output, or avoid indexing vendored content by default.

However, these policies must remain overridable when the task actually requires that evidence.

Binary or oversized content should generally travel through artifacts rather than raw prompt insertion.

---

# 19. Language Analysis

AIForge is implemented in C++, but repository context must remain language-independent.

Language-aware analysis should integrate through ports/adapters.

Possible providers may include:

```text
C/C++
    compiler/Clang-derived analysis

Python
    parser or language-service analysis

JavaScript/TypeScript
    parser/compiler-service analysis

Shell
    parser analysis

HTML/CSS
    structured document analysis
```

The architecture must not require one specific analyzer implementation.

Capabilities should be discoverable.

Examples:

```text
symbols

references

call relationships

diagnostics

type information

rename-safe locations
```

The application should request available capabilities rather than assume every language supports the same analysis.

---

# 20. Graceful Analyzer Fallback

Absence of a language analyzer must not make the Code workspace unusable.

Fallback may use:

* filesystem traversal;
* exact text search;
* bounded source reads;
* build/test evidence;
* VCS information;
* model reasoning over selected exact source.

Language analysis improves retrieval quality.

It is not a prerequisite for repository access.

---

# 21. Context Events and Telemetry

Not every context operation needs to become durable history.

The architecture must distinguish:

## Durable facts

Events that affect replay, auditability, user-visible run behavior, or important artifact provenance belong in the durable run event model.

Persisted events follow the accepted run-event contract:

* stable IDs;
* run association;
* ordering;
* schema version;
* timestamps;
* past-tense facts;
* relevant causation/parent/invocation IDs.

Large source excerpts should be referenced as artifacts rather than duplicated throughout events.

## Optional context telemetry

Implementation diagnostics such as:

* cache hit;
* ranking score;
* candidate rejected;
* context item evicted;
* summary cache reused;

may remain non-durable telemetry unless a later requirement makes persistence valuable.

Do not pollute the durable event vocabulary with every internal context-management decision.

---

# 22. Context Economics

Context selection is an optimization problem.

A conceptual decision may consider:

```text
expected reasoning value

freshness

relevance

phase

reload cost

token cost

provider capacity
```

Core policy should remain provider-neutral.

However, the actual model context limit and token estimation cannot be assumed universal.

Provider/model-specific capacity information should therefore enter context construction through an appropriate provider-neutral runtime port or capability input.

The context builder can remain independent of Venice or another specific provider while still making decisions using the actual target model's limits.

---

# 23. Compaction

Compaction should preferentially remove representation detail rather than destroy addressability.

Instead of retaining:

```text
<large source implementation>
```

the working set might retain:

```text
Symbol:
    Foo::bar

Repository snapshot:
    <snapshot>

Source:
    src/foo.cpp

Source identity:
    <digest/range identity>

Purpose:
    validates request before dispatch

Relationships:
    calls Baz::dispatch
    tested by FooTests

Expandable:
    yes
```

The full source remains in the repository or artifact system.

Derived knowledge remains rebuildable.

Durable run history remains unchanged.

This creates a context hierarchy analogous to working memory backed by addressable external evidence.

---

# 24. Child Runs

A child run should receive a deliberately bounded context.

It should not inherit an entire parent transcript or repository working set by default.

A child context parcel should preserve:

* task;
* relevant instructions;
* capability subset;
* selected repository evidence;
* snapshot identity;
* artifact references;
* required provenance.

Fresh context is a feature.

The child can request additional evidence through the same repository/context mechanisms.

---

# 25. Persistence

Different context states have different persistence needs.

## Persist

Durable run events and artifact references according to the run/session architecture.

## Persist or cache where useful

Repository snapshots and derived knowledge.

Their persistence is an optimization and must preserve provenance/freshness.

## Do not require persistence

Per-inference working sets and ranking decisions.

They may be retained for debugging or evaluation, but correctness must not depend on them surviving.

---

# 26. Storage and Implementation Techniques

This architecture intentionally does not choose:

* SQLite versus another database;
* files versus content-addressed storage;
* mmap versus ordinary reads;
* Tree-sitter versus compiler-native parsers;
* one universal symbol-index implementation;
* one graph-storage engine.

For example, a C++ filesystem adapter may eventually use `mmap` as an efficient source-access implementation technique.

That optimization must remain behind the repository/filesystem boundary.

The domain model should not depend on mmap semantics.

---

# 27. Non-Goals

This design does not require AIForge to:

* ingest an entire repository into every model request;
* build a full IDE;
* implement every language analyzer;
* create a graph database;
* persist every context-selection decision;
* create a second Code-specific run kernel;
* trust LLM-generated summaries as source truth;
* use embeddings as the universal retrieval mechanism;
* forbid large reads;
* settle the final storage format.

---

# 28. Success Criteria

The architecture is successful when the Code workspace can eventually:

1. orient itself within an unfamiliar repository without reading everything;
2. identify which repository state its knowledge describes;
3. reuse valid derived knowledge without repeatedly reading source;
4. detect stale derived knowledge;
5. retrieve exact source before modification;
6. avoid overwriting unseen concurrent changes;
7. incorporate diagnostics, tests, diffs, and build information;
8. preserve applicable project instructions and provenance;
9. operate when rich language analysis is unavailable;
10. provide each inference with a bounded context appropriate to the task phase;
11. retain durable evidence independently of model-context compaction;
12. send providers only the final provider-neutral request.

The optimization target is:

> Maximum useful and current understanding per inference token, without sacrificing exact source access, provenance, replayability, or repository correctness.

---

# 29. Relationship to the Issue Tracker

This section records the tracker reconciliation completed on 2026-08-10. It is a dated audit trail, not a permanent architectural contract. Live issue bodies are the source of current implementation scope and acceptance criteria.

The audit applied the following changes:

* rewrote #3, #4, #7 through #12, #14, #16, #17, #19, and #20 where their assumptions conflicted with the north star, ADR 0001, this proposal, or current repository reality;
* closed obsolete failure-scaffolding issue #15 because focused tests and CI now exist;
* closed monolithic "agent mode" issue #18 because tools, Code, process execution, and derived knowledge are independent boundaries;
* added shared context/tool prerequisites #22 through #25;
* added repository-context issues #26 through #33;
* split the quality pipeline into deterministic TUI replay #34, review receipts #35, and later reviewer child-run orchestration #36.

Issues #5, #6, and #13 were reviewed and retained without architectural changes. Future tracker edits should remain similarly narrow: an issue changes only when it conflicts with the north star, an accepted ADR, this proposal, or current repository reality.


# 30. Tracked Issue Boundaries Derived From This Design

These engineering boundaries now exist as live issues. The issue bodies govern
their current implementation scope; the summaries here preserve the design
reason for each boundary.

They are not an instruction to implement them all at once.

Each should be suitable for a focused Codex session.

---

## Prerequisite issues

The tracker also contains prerequisites that are shared with work beyond the
Code workspace:

* #22 fixes prompt/context precedence through an ADR;
* #23 establishes the shared tool registry and invocation lifecycle;
* #24 establishes effects, capabilities, and approval policy;
* #25 establishes bounded argv-based process execution.

---

## #26 — Repository snapshot identity

### Purpose

Establish a provider-independent identity for repository state.

### Design concerns

* repository root identity;
* VCS revision where available;
* dirty-worktree representation;
* source digests;
* change detection.

### Explicitly not in scope

* semantic indexing;
* model summaries;
* language-specific analysis.

---

## #27 — Code workspace project-instruction discovery

### Purpose

Discover applicable repository and nested project instructions with provenance and scope.

### Design concerns

* `AGENTS.md` discovery;
* nested applicability;
* repository source identity;
* handoff to future instruction-precedence policy.

### Explicitly not in scope

* inventing a new precedence rule;
* increasing capabilities based on repository content.

---

## #28 — Repository evidence and context parcel domain types

### Purpose

Establish provider-independent types for selecting bounded repository evidence for an inference.

### Design concerns

* source identity;
* provenance;
* freshness;
* purpose;
* task phase;
* artifact references.

### Explicitly not in scope

* retrieval ranking algorithm;
* semantic database;
* provider request implementation.

---

## #29 — Code workspace context-selection policy

### Purpose

Integrate repository evidence into the existing application-runtime context builder.

### Design concerns

* orientation/diagnosis/editing/verification/review phases;
* budgeted selection;
* exact versus derived evidence;
* provider/model capacity inputs.

### Explicitly not in scope

* language analyzer implementation;
* persistent repository index.

---

## #30 — Derived repository knowledge records

### Purpose

Define rebuildable repository knowledge with provenance, versioning, freshness, and invalidation.

### Design concerns

* symbol/relationship records;
* semantic summaries;
* source digests;
* producer identity/version;
* invalidation.

### Explicitly not in scope

* one universal storage engine;
* graph database selection.

---

## #31 — Language-analysis capability port

### Purpose

Allow language-specific adapters to provide repository intelligence without leaking parser/compiler types into the domain.

### Design concerns

* capability discovery;
* symbols;
* references;
* relationships;
* diagnostics;
* graceful absence of capabilities.

### Explicitly not in scope

* implementing every supported language.

---

## #32 — Exact-source edit guard

### Purpose

Ensure source is current before mutation and detect intervening repository changes.

### Design concerns

* exact source retrieval;
* baseline source digest;
* dirty-worktree awareness;
* concurrent-change detection.

### Explicitly not in scope

* generalized merge-conflict resolution;
* language-aware refactoring.

---

## #33 — Code verification evidence

### Purpose

Represent build, test, diagnostic, and diff results as bounded provenance-carrying evidence usable during verification and review.

### Design concerns

* artifacts;
* excerpts;
* source snapshot relationship;
* verification phase context.

### Explicitly not in scope

* implementing every build/test system.

---

# 31. Development Workflow

This architecture paper defines direction.

It is not itself an implementation issue.

The intended development workflow is:

```text
Architecture / design session
        |
        v
Update governing documents or proposal
        |
        v
Reconcile live issue tracker
        |
        v
Select one focused issue
        |
        v
One Codex implementation session
        |
        v
Tests / review / merge
        |
        v
Periodic architecture review
```

Future Codex sessions should not interpret this document as permission to implement adjacent architectural pieces that are not required by the selected issue.

If work reveals a missing architectural concern, capture it as:

* a design question;
* an ADR candidate;
* or a new/follow-up issue.

Do not silently broaden the active issue.

---

# Final Principle

AIForge should not optimize for placing the maximum amount of repository text into a model context window.

It should maintain clear separation between:

```text
what happened,
what source state exists,
what has been derived from that state,
and what this particular inference needs to see.
```

The Code workspace can then behave less like:

```text
cat repository | model
```

and more like a software engineer operating against an addressable, current, provenance-aware body of evidence.

The objective is not maximum context.

The objective is sufficient context with known identity, known freshness, known provenance, and predictable cost.
