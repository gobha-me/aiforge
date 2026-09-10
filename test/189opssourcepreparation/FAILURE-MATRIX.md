# Ops source preparation failure matrix

Written before implementation. No host/source/provider IO in this test suite.

- Invalid token, identity, kind, deadline and unclaimed/null factory: zero calls.
- Exceptions, invalid error enums, null/unclaimed/foreign/malformed prepared
  sources: fixed failures, producer cleanup and no observation/authority.
- One absolute deadline checked before/after producer work; no late success.
- Shared monotonic request IDs, full-token poll/cancel isolation, capacity and
  session invalidation preserve existing worker semantics.
- Blocked preparation, cancellation callbacks, factory cleanup and discarded
  prepared-source destruction retain physical capacity without owner joins.
- Ready resource result stays producer-owned until claim/discard, avoids the
  reader_done readiness deadlock, and is delivered once. Successful claim
  transfers lifetime responsibility to its caller.
- Thread-start failure and mixed ordinary/grant/preparation jobs preserve the
  common admission/lifetime implementation; existing160/173/174 regressions run.
- Linux factory construction is metadata only, malformed input refuses without
  create, and a pre-cancelled/expired prepare never opens procfs.
- Happy prepared-source smoke runs last and makes zero observe calls.
