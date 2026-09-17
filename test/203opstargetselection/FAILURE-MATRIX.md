# Durable Ops target-selection failure matrix

| Boundary | Required result |
| --- | --- |
| Missing, reordered, duplicated, unknown, or mixed control events | Reject replay |
| Unrelated future event before selection | Skip it without weakening selection validation |
| Invalid target identity or start/selection/completion lineage metadata | Reject replay |
| Duplicate, stale, skipped, or overflowing generation | Reject before selection |
| Same target ID with the next generation | Record a new selection |
| Broker selection failure | Append no selection event |
| Selection while another run is unfinished | Reject replay before stale evidence can publish |
| Event append failure after broker selection | Revoke the live issuer and expose no ready selection |
| Reopen after an unrelated interrupted run | Show the last exact target as historical and unverified |
| Legacy observation generation without selection event | Restore no target; seed the next selection generation |
| Reopen with committed evidence | Atomically hydrate the final eight-slot catalog with exact provenance |
| Replay | Perform no target collection and restore no source, authority, or consent |
