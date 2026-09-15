# Private Kubernetes projection failures (#226, slice191)

Write this matrix before implementation. Pure synthetic bytes only: no network,
provider, credentials, cluster, subprocess, new source advertisement or grants.

- Input/operation/identity: malformed request, wrong kind/operation/namespace,
  missing or replaced UID, resource version, duplicate row identity, qualified
  container absent/changed, negative/reversed timestamps, public pre-cancellation
  and the callback stop gate after earlier progress.
- Strict JSON: bad/trailing/truncated input, type/null confusion, duplicate
  decoded keys including skipped objects, invalid Unicode/numbers, unknown
  structural fields skipped without copying their values into evidence.
- Budgets: input1MiB/request maximum; callbacks32768; nesting32; scalar128KiB;
  aggregate active decoded keys128KiB; retained typed strings256KiB;
  intermediate typed records1024; each bound
  and one over, plus final neutral64KiB/256-row budgets. Parser allocation and
  OS cancellation are not claimed formally bounded by these counters.
- Pod inventory: valid list identity/version before empty success; exact Pod
  identities and server order; explicit readiness only; no invented replica
  counts/healthy state; continuation means unknown omissions, never fetch or
  trust remainingItemCount as exact; excessive rows fail rather than disappear.
- Exact Pod: declared regular/init/ephemeral names are unique and category-bound
  to status; flattened names preserve no invented category label; no status means
  unknown/absent; state variants, readiness, restart/exit type/range failures;
  lastState/message/spec secrets never become current evidence.
- Events: finite core/v1 list; supported closed workload kinds in namespace
  scope; strict selected Pod UID/name/namespace in exact scope; validate omitted
  row identities; unsupported kinds/count omissions remain partial; no default
  occurrence1; series precedence, numeric/timestamp bounds, unique event UIDs;
  no arbitrary messages/reasons/reporting data retained.
- TLS custody: selected token or complete decoded chain/key retained with CA;
  exact existing identity/JSON behavior; move ownership and namespace override;
  decoded aggregate256KiB boundary; bogus DER remains only envelope-admitted,
  never claimed a usable TLS certificate/key. No secret-bearing public API/error.
- Happy cases last: strict JSON output passes existing pure historical/domain
  validation; all omitted known-secret sentinels stay absent from neutral fields.
  Future source still owns known-secret matching and current authority checks.

Cancellation coverage combines actual SAX callback-budget wiring and the stop
gate after earlier callbacks. It does not claim a timed concurrent mid-parse
reproduction or a hard interruption deadline for library scanning.
