# ADR 0004: Configuration resolution and JSON file adapter

- Status: Accepted
- Date: 2026-08-17

## Context

AIForge needs typed application configuration before model selection and the
one-shot surface can share command-line, environment, file, and compiled
defaults. The parser already preserves whether an argument was supplied, but it
does not own environment or file inputs. Session restore, prompt instructions,
credentials, and provider objects are separate sources of state and must not be
folded into this configuration boundary.

The architecture north star defers persisted encodings and JSON-library choices
until an owning milestone records a decision. AF-04 is that milestone for the
small user configuration file. It requires unknown-field preservation and
atomic updates without selecting the encoding for session events or artifacts.

The timeboxed candidates were strict JSON with nlohmann/json, TOML with
toml++, and a small bespoke key/value format. JSON matches the documented
`config.json` path and the existing Venice ecosystem, represents booleans,
integers, strings, lists, and nested dotted keys without another conversion
grammar, and permits an adapter to retain fields unknown to the current
registry. TOML is more convenient to edit but introduces a second parser and
different value semantics without a current requirement. A bespoke format
would duplicate mature parsing and Unicode handling while making structured
unknown-field preservation harder.

## Decision

Configuration resolution is provider-neutral and independent of the file
adapter. A registry declares stable dotted key IDs, value kinds, environment
bindings, optional compiled defaults, constraints, and sensitive/file-write
policy. Candidates retain source provenance. Resolution uses the fixed order:

1. command line;
2. environment;
3. configuration file;
4. compiled default.

An explicit default-like value such as `false`, zero, empty text, or an empty
list remains present. Invalid command-line candidates fail resolution. Invalid
environment or file candidates are diagnosed and permit a valid lower layer to
win. Selected, shadowed, and rejected candidates remain inspectable. Session
restore is not a configuration source and cannot replace an explicit
current-run value.

The AF-04 file adapter uses strict UTF-8 JSON through nlohmann/json 3.11.3. The
dependency is private, is found as a package first, and otherwise uses a pinned
FetchContent fallback. JSON types do not appear in public domain or runtime
interfaces. This pin matches venice-cpp's compatible baseline; changes require
a compatibility reason.

The document root is an object. Dotted registry keys traverse nested objects.
Comments, trailing syntax, duplicate object keys, invalid Unicode, incompatible
types, non-object roots, and files over 1 MiB are rejected. Unknown fields are
retained structurally when a known field is changed; their whitespace and
original key ordering are not contractual.

The location is `$XDG_CONFIG_HOME/aiforge/config.json` when the XDG value is
absolute, otherwise `$HOME/.config/aiforge/config.json`. The AIForge directory
and file targets cannot be symlinks. New directories use mode 0700 and new
configuration, lock, and temporary files use 0600. Read-only resolution may
warn about a loose existing file, but mutation refuses to overwrite it.

Mutations take an advisory lock, read and validate the current document under
that lock, change only the selected registered field, write a same-directory
temporary file, sync it, rename it over the destination, and sync the parent
directory. Malformed or insecure existing files are never silently repaired or
overwritten.

Credentials and other raw secrets are excluded from this file. A sensitive key
may be represented by the neutral registry for provenance and redaction, but it
cannot be file-writable. Credential storage belongs to AF-14.

## Consequences

- Parser, environment, file, and default inputs share one deterministic
  resolver without CLI11 or JSON types in its public contract.
- `config show/get/set/unset` can derive validation and display behavior from
  the same key registry.
- Unknown JSON fields survive recognized updates, but byte-for-byte formatting
  does not.
- The decision applies only to the small user configuration file. Session event
  persistence, artifacts, caches, and databases remain undecided.
- JSON and POSIX filesystem behavior stay behind adapters and deterministic
  fakes may replace the file-store boundary.
