# ADR 0007: Credential sources and local storage

- Status: Accepted
- Date: 2026-08-25

## Context

AIForge needs a Venice credential for authenticated inference while model
discovery remains usable without one. ADR 0004 deliberately excludes secrets
from `config.json`, and ADRs 0001 and 0005 prohibit credentials and unredacted
provider failures from run events and durable session storage. AF-14 is the
milestone that owns credential-source precedence and the first local storage
format.

A platform keychain is not yet selected. The initial implementation therefore
needs a narrowly scoped local file without turning credentials into ordinary
configuration or exposing them through provider-neutral request types.

## Decision

The explicit `VENICE_API_KEY` environment variable has precedence over a stored
credential. If the variable is present, including as an empty or malformed
value, it is authoritative and validation failure does not fall through to the
file. When it is absent, AIForge reads
`$XDG_CONFIG_HOME/aiforge/credentials`, falling back to
`$HOME/.config/aiforge/credentials` when the XDG value is unset or relative.

A credential is a nonempty printable-ASCII token of at most 64 KiB. Space,
DEL, and ASCII control bytes are rejected so a credential cannot inject an HTTP
header or a line into the file format. The stored representation is the token
followed by one newline; readers also accept an otherwise valid token without
the final newline. Additional lines or trailing whitespace are invalid.

The AIForge configuration directory and credential target cannot be symlinks.
The directory must be mode 0700 and the credential must be a regular mode-0600
file. An insecure or malformed stored source is diagnosed and ignored rather
than sent to a provider. Login refuses to overwrite an insecure target.

`aiforge login` is an explicit terminal-only action. It disables terminal echo,
reads one bounded token, restores the terminal on every exit, validates the
complete value, and publishes it under an advisory lock. New directories use
0700; lock, temporary, and credential files use 0600. Publication writes and
syncs a same-directory temporary file, renames it over the target, and syncs
the directory. Concurrent successful writers may finish in either order, but a
reader observes a complete old or new credential.

Credential bytes travel through a move-only value used only by bootstrap and
production adapter construction. The Venice adapter consumes that value and
does not retain a second copy in general backend options. Best-effort clearing
reduces the lifetime of AIForge-owned buffers but is not claimed as protection
against swap, core dumps, allocator copies, or the provider client's required
authentication storage.

Only a bounded source locator is durable: `VENICE_API_KEY` for the environment
or `aiforge/credentials` for the local file. Backend requests, events,
artifacts, fake captures, diagnostics, rendered frames, and provider error
messages never contain credential bytes.

## Consequences

- Missing credentials remain compatible with public/cached model discovery and
  an interactive offline surface, while one-shot inference fails explicitly.
- The plaintext file is a limited bootstrap mechanism, not a claim of keychain
  equivalence; a future credential backend can implement the same resolution
  boundary.
- Invalid explicit environment configuration fails loudly instead of silently
  selecting a lower-precedence secret.
- Provider errors are mapped to fixed redacted adapter errors before they cross
  into runtime or presentation code.
