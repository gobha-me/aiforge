# Configured Kubernetes preparation failure matrix

Written before implementation. Tests use only task-owned files and synthetic
static credentials; they make no network, cluster, helper or provider call.

- Metadata creation rejects malformed, relative, aliased and overbound source
  paths plus unsafe context/namespace values without opening a referenced file.
- Worker preparation rejects missing, symlinked (leaf and ancestor), directory,
  FIFO, non-owner-private and over-256-KiB sources. Ambient `HOME` and
  `KUBECONFIG` never substitute for the exact configured path.
- A deterministic wrapped-read gate replaces the configured path while its
  original descriptor is pinned. The worker discards the result as
  `source_changed`; neither the old nor replacement bytes become observable.
- The same gate proves cancellation and the original deadline remain effective
  while physical preparation retains its shared worker slot.
- Unsupported authentication helpers, file-indirected credentials, proxies and
  insecure TLS fail as `unsupported`; a named helper marker remains absent.
- Invalid syntax and selection fail with fixed enums that contain no path,
  credential or parser diagnostics.
- Happy-path token and client-certificate sources preserve the configured
  context, explicit namespace, HTTPS endpoint, non-secret trust identity,
  target and caller-assigned revision in the prepared source binding. Source
  construction itself performs no observation or network request.

The configured catalog tests separately prove Kubernetes factory lookup owns a
copy of the selected record and performs no referenced-file I/O. Existing
static-parser and HTTPS-source suites retain their detailed authentication,
projection, TLS and request-boundary coverage.
