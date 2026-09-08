# Context control JSONL v1

`aiforge context --session SESSION_ID --model MODEL_ID --jsonl` opens an existing
session and holds a persistent, noninteractive command loop. Each request is
one newline-terminated JSON object; stdin can remain open between commands.
This protocol is separate from `aiforge agent` and does not submit chat prompts.
Unfinished existing runs require recovery before opening context control.

Every request requires `version: 1`, a positive integer `id`, and `op`.
Replies include `version`, the correlated `id`, current durable `sequence`, and
`type`. Protocol parse errors use `id: 0`. Unknown fields, duplicate keys,
floating-point integer fields, unknown versions, and oversized input are rejected.
The input limit is 1 MiB per line; output records are bounded to 2 MiB.

| Operation | Additional required fields | Optional fields |
|---|---|---|
| `inspect` | — | `draft`, `offset`, `limit` |
| `policy` | `expected_revision`, `mode`, `pins` | — |
| `summary.generate` | `expected_sequence`, `runs` | `maximum_output_bytes` |
| `summary.publish` | `summary_id` | — |
| `summary.edit` | `expected_sequence`, `parent`, `text` | — |
| `summary.preview` | `candidate`, `replacements`, `draft` | — |
| `summary.preview.page` | `handle` | `offset`, `limit` |
| `summary.apply` | `handle`, `draft` | — |
| `summary.disable` | `expected_revision`, `candidate`, `activation_event_id` | — |
| `close` | — | — |

`mode` is `full` or `rolling`. `pins` and `runs` are arrays of exact run IDs.
A candidate reference (also used for `parent` and every `replacements` entry)
contains `summary_id`, positive `revision`, and `candidate_digest` with
`algorithm: "sha256"`, its lowercase hexadecimal `value`, and integer `byte_size`.
Use the exact references returned by inspection or publication.

Inspection returns policy, model capacity, selected/omitted group decisions,
summary text and producer/source/edit provenance, active coverage, and prospective
context entry metadata. Source messages are not exported. Collections have
`NAME_count` and `NAME_next_offset`; a null next offset means the collection is
complete. Send another inspection with that offset to continue that collection.
Each collection may stop early to fit its encoded byte budget. The requested
limit is 1–16 (inspection defaults to 8). Pagination does not freeze the session;
compare reply sequences when assembling pages.

Generation returns `generation.started`, then asynchronously emits
`generation.finished` as the loop drains the producer. Completion does not
publish or activate anything. Inspect to recover an unpublished draft, explicitly
publish it, and optionally edit its exact immutable parent revision. Unpublishable
outputs remain visible with a refusal reason and are never automatically retried.

Preview preserves the supplied composer draft and performs no provider call or
activation. It returns a process-local handle and the prospective context budget.
Only one review exists. Each new preview replaces the previous handle; changing
policy, generating, publishing, editing, disabling, or closing invalidates it.
Inspection preserves the review. `summary.preview.page` pages the same immutable
review using its handle, `offset`, and `limit` (default 16). It returns the original
`review_sequence` and never changes the proof or handle. A changed session sequence
invalidates paging; external model, memory and source changes are checked on apply.
Apply sends
only the current handle and current draft, and revalidates the reviewed context.
Every apply attempt consumes its handle, including a stale or rejected attempt.
Restarting the process always requires a fresh preview.

EOF, close, and cancellation stop only a producer started by this loop and drain
its cleanup with a bounded deadline, preserving late accounting. A failed command
may report `effect_may_have_applied`; inspect durable state before retrying it.
No operation automatically generates, publishes, edits, or activates a summary.
