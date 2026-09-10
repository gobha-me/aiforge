# Linux journal logs failure matrix (188)

Saved before source/dependency implementation. Public factory uses only fixed
local system journal selection; native libsystemd remains adapter-private.

- Admission: disabled/missing exact log grant, stale revision, wrong owner/session
  through worker/broker; malformed request, foreign target/boot, absent or malformed
  service invocation through source. Zero journal opens, no successful evidence.
- Identity: wrong GetUnit canonical object path/Id, missing/changed invocation,
  unavailable namespaces/peer/manager, restart during collection and failed final
  proof. Zero journal reads before initial proof; no prefix after final failure.
- Fields: missing/duplicate/foreign boot/unit/invocation; missing/duplicate MESSAGE;
  trusted/untrusted-name substitution; invalid binary/UTF-8/controls; malformed returned
  field envelope; too many fields; ignored oversized field; native error.
- Threshold: below/exact/above threshold, including library-returned oversize despite
  hint. No application copy before charging; no claim about library allocation.
- Bounds: 0/1/200/201 lines, 32768/32769 bytes, 600/601-second request, narrowed
  maximum_entries/maximum_bytes, cumulative handshake + field + final-proof budget;
  full lookahead validation, multiline/empty/trailing blank preservation.
- Time: actual microsecond conversion/overflow, future/old data, equal and backward
  timestamps in native order, final window crossing, backward/forward clock jump,
  stop/expiry in open/match/seek/next/field/final-proof phases.
- Visibility: permission denial, no files/EOF/no matching trusted invocation are
  unavailable when empty. Nonempty EOF is partial with unknown omission count;
  proved local output limit is truncated, also unknown total omissions.
- Text exclusions: known credential forms/PEM markers fail the whole attempt;
  unknown native fields and raw library diagnostics never enter neutral values.
- Ownership: per-observe independent cursor, no handle reused across worker threads,
  cleanup exception/stall retains bounded physical slot, no detached reader.
- Existing health/list/service-health remain unchanged; health opens no journal/bus.
  History/replay/inspection do not perform reads. Manual/model use existing kernel.
- Dependency: installed/preexisting actual headers+symbols, absent/obsolete/malformed
  package and header/symbol failure, core-only/non-Linux nonactivation, no fallback.

Happy smoke comes last: deterministic cursor + actual common decoder, then optional
synthetic native journal fixture if supported without live paths. No host logs or
host bus, elevation, services/configuration changes or paid calls in this slice.

Sampling is a bounded window prefix, never a newest-tail claim. Internal native
corrupt-item skipping remains an unknown omission; fixed errors do not intercept
residual native stderr diagnostics.
