# Idle native Ops binding failure matrix

Write these checks before enabling the owner operation:

- Missing broker, foreign/stale endpoint, wrong session/owner/source binding,
  stale generation and unchanged log revision reject before collection or mutation.
- Active manual/model run, pending approval, active child or unusable kernel
  rejects; old bindings and approvals retain their original authority.
- A custom policy with copied provenance cannot impersonate the final launch
  policy. An ordinary or altered registration named observe_target cannot be
  replaced as if it were the native executor.
- Registration/policy preparation failures preserve the old selection; a broker
  cleanup exception closes admission instead of leaving mixed live authority.
- Kernel and policy each preserve all their unrelated registrations. In
  particular current memory capture narrowing must not overwrite the original
  policy ceiling needed when capture is enabled later.
- The launch context, approval mode and actual automatic matcher/counters remain
  unchanged; a new target does not acquire old invocation grants or broader rules.
- Initial unavailable registration can become a native binding. Target and log
  changes update its exact declarations/limits/contracts in both snapshots.
- Owner-facing copies are prepared before selection. Final state transfers are
  proven no-throw. No policy evaluation, inference, source collection, durable
  append, or reclaimed physical-worker claim occurs during binding.
- Success smoke: bind a second target, collect through manual kernel admission,
  and verify its committed observation matches the selected target with zero
  provider calls. Existing registry-only memory rebinding remains unchanged.
