# Ops UI state prerequisites failure matrix

- Exact preparation token: absent, foreign session/epoch/request/target/revision/kind report neither ready nor outstanding; queries neither consume nor admit work.
- A producer-held completion is ready before its producer retires; repeated inspection preserves one-shot poll ownership, including failures and expired results.
- Claimed expired preparation retains physical capacity during producer-side source destruction; unrelated stalled work never changes the exact token result.
- Cancellation/invalidation suppress readiness while blocked physical work remains outstanding; releasing owned gates retires it.
- Buffered Chat delivery is an exact-once ownership transfer, including ordinary/manual interleaving and committed events retained after pump failure.
- Taking an empty or populated buffer never polls a model stream, observes a source, appends history, or clears failure/approval state.
- Happy smoke: explicit manual pumping followed by buffered delivery returns the completed observation without any provider calls.

All blocking fixtures own shared gates and unwind release guards. Deadline tests use the real original deadline; no reservation or hard cancellation is claimed.
