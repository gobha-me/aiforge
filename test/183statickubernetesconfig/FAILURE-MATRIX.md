# Static configuration failure matrix

Tests use synthetic strings only. No file, credential helper, environment or
network seam exists in the parser API.

- Closed schema: missing/unknown/duplicate fields at each mapping, duplicate
  decoded keys and named entries, unresolved selection/references, wrong types.
- Selection: explicit namespace mandatory and validated; a differing context
  default is overridden; current-context cannot select; unselected entries are
  validated and are absent from generated output.
- Credentials: empty/mixed/incomplete auth, all unsupported fields (including
  unselected), malformed base64/padding/PEM, unsafe text, URL authority/path/TLS.
- Frontends: strict JSON malformed/trailing/comments/native scalar types; YAML
  anchors/aliases/merge/complex keys/null/tags/multiple documents/plain ambiguous
  scalars; quoted or explicit-string counterparts.
- Bounds: input, scalar, aggregate, event, depth, list, name, namespace, token
  and output guards at N/N+1; overflow-safe accounting. Some global ceilings
  cannot be reached by the closed schema and are tested through its private
  budget guard rather than by inventing additional admitted schema fields.
- Cancellation: pre-parse and deterministic callback boundary; failures remain
  fixed enums even when malformed input contains synthetic secret sentinels.
- Ownership: move-only result, immutable borrowed output, neutral identity has
  no auth bytes, trust digest depends on CA only. Existing target binding
  validation remains equivalent after extracting its identity helper.
- Happy smoke last: token YAML and strict JSON agree; certificate mode yields
  a minimal generated document that parses under the same static subset.

Dependency evidence separately covers pinned fallback and upstream installed
consumer paths, preexisting targets/options and invalid/obsolete packages.
