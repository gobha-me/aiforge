# Private Kubernetes HTTPS source failures

1. Metadata factory: invalid/moved-from binding or config, wrong identity/kind,
   credential-bearing public identity, allocation failure. Valid creation and
   metadata access invoke no SSL context creation, PEM decoder, native file
   reader, socket, DNS, thread or provider constructor. Parsed but invalid X.509
   material may create the metadata owner; observe must refuse it before connect.
2. Admission: pre-cancel, malformed limits/resource, foreign target/revision,
   namespace/endpoint/trust mismatch and unsupported logs/Linux operations issue
   zero TLS/network calls. Valid lists require monostate. Exact Pod health
   retains exact UID/container constraints. Events attest the exact
   Pod namespace/name/UID, with no container runtime attestation.
3. TLS: valid unrelated root, wrong DNS/IP SAN, expired server cert, missing/invalid
   CA block among valid blocks, unsupported/encrypted/mismatched client key,
   bad/reordered/omitted intermediate, and valid full mTLS chain. Every provided
   certificate is decoded, checked and installed; no silent first-cert-only path.
   Use independent synthetic server/client trust roots so missing intermediates
   cannot be rescued by the server-trust store.
4. Request: exact GET path/namespace/limit/field selector, fixed Host/Accept and
   one private bearer header or mTLS; no other path/verb, discovery or helper.
   UID values containing comma, equals and backslash remain one exact selector
   value. DNS, IPv4 and IPv6 endpoint forms preserve verification identity.
5. HTTP: 3xx with a second local listener gets no redirected request; explicit
   proxy/ambient proxy and system-CA configurations cannot replace selection.
   Refuse 401/403/404/429/5xx as fixed errors, non-JSON content types, compressed
   responses, ambiguous essential headers and dependency-exposed malformed framing. Error response
   bodies never become evidence, diagnostics or fake generic request captures.
6. Bounds: exact/one-over captured header+body budget, chunked body, declared and
   absent length, canonical header count/line limits, bounded trailer accounting,
   valid body prefix followed by overflow/malformed tail. No successful prefix
   after failure. Continue token is retained as partial with unknown omissions;
   no next-page request. Existing191 decoder/row bounds remain enforced.
7. One deadline: expiry during TLS setup, handshake, headers, body, projection and
   credential exclusion; no fresh stage deadline. Stream cancellation and server
   close during TLS teardown preserve the caller's SIGPIPE disposition/mask and
   pre-existing pending signal. Watcher startup failure issues no request.
   Stop callback only notifies private bounded state; watcher joins before client
   destruction. Test the caller's request_stop operation returns without joining
   the retained worker read; observe itself joins its watcher before return.
8. Source result: wrong API kind/group/namespace/UID/runtime and invalid projected
   fields refuse; token/private-key recognizable material in retained metadata
   refuses the whole result. Unknown free-form response fields remain dropped.
   Independent concurrent calls share only immutable config; each has its own
   TLS client, body, control state and watcher.
9. Physical retention: deterministic private blocking-transport seam, if required,
   proves cancelled/deadline work still occupies its LocalSourceWorker slot until
   the retained operation returns. No real hanging DNS or kernel fault is needed;
   real loopback TLS proves the actual streaming/cancellation path separately.
10. Happy path last: Pod inventory, exact Pod/container health, namespace events
    and exact-Pod events produce191-validated neutral observations with the exact
    original request and one real list/object resourceVersion. Empty validated
    lists are actual successful empty evidence; missing/failed HTTP is never an
    empty-success fallback.


The pinned HTTP parser discards undeclared/prohibited trailers and accepts its
documented missing final CRLF case. Tests and source promise no stricter framing
policy than that dependency exposes; maximum_bytes covers retained normalized
status/headers/body/trailers, not total wire or discarded trailer framing.

Executed evidence is tracked in the task handoff. Native TLS/context and watcher
startup interposition is test-owned and exercises the actual source/broker/worker
path; it simulates the blocked native call instead of reproducing a stalled OS.
The real TLS peers in this slice bind IPv4 loopback and use synthetic credentials.
No live cluster, DNS/IPv6 connection, forced allocator failure, or deterministic
mid-projection/wall-clock-reversal reproduction is claimed by this test target.
The existing pure projector/domain tests and final source deadline/validation gates
cover those structural checks without introducing a production fault framework.
