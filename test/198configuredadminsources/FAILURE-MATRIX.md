# Configured Admin catalog failure matrix

Written before implementation. Pure make must reject rejected/shadowed ops
candidates, invalid typed records and duplicate/reserved IDs without returning a
fallback local catalog. A successful absent ops entry yields the reserved local
record; a valid configured record graph is owned independently of caller mutation
or destruction. Exact target and reserved configuration revision are retained by
the native metadata factory. Unknown targets, invalid revisions and configured
unsupported Kubernetes targets refuse without source construction.

The process loader must use the actual file-store outcome: missing file is a
legitimate empty layer, while malformed JSON, duplicate keys, invalid ops records,
non-file paths, symlinks or inaccessible configuration refuse with fixed errors.
Referenced kubeconfig paths are never opened. Output metadata contains only
ID/display/kind; no paths, context, namespace or raw diagnostics escape.

Tests use a task-owned temporary XDG configuration root and restore that variable
before fixture cleanup. They never change HOME or process permissions globally.
A strong no-IO Linux construction fixture counts any accidental native creation;
metadata creation/listing/factory lookup must leave it at zero. A final explicit
prepare invokes that fixed fixture once to prove the returned factory's exact
identity without reading host sources. Public boundary exceptions map to fixed
allocation-free failures. No GUI/kernel/model/provider execution is claimed.

Both compiler focused checks, changed-source tidy, format and independent review
precede grouped full application validation. Loader path-resolution failure is
guarded explicitly; the environment fixture does not alter HOME to force it.
