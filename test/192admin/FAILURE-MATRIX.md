# Standalone Admin CLI failure matrix

Parser/dispatch subsection written before implementation. These cases use a
recording AdminCommand port, with no adapter, store, source or provider calls.

- Missing/unknown subcommand, missing/extra unit, unknown/duplicate options and
  missing target value: usage failure and zero command execution.
- Target ID: nonempty, <=64 bytes, lower ASCII alphanumeric endpoints, only lower
  ASCII alphanumeric/underscore/hyphen inside. Never fall back to local on bad input.
- Service name: 9..255 bytes, .service suffix, first byte not '-', ASCII
  alphanumeric plus _-.@:. No shell syntax, path, backslash, Unicode or controls.
- Pod reads require an exact DNS-style name and opaque bounded UID. Events accept
  either neither (namespace scope) or both; half identities and extra values fail
  before catalog/source work. Linux/Kubernetes operation mismatch is a usage
  failure without preparation.
- --target belongs only to read leaves; --json belongs independently to each leaf.
  All argument IDs remain globally unique. Root model/session/repository options
  cannot accidentally become an Admin provider/session bootstrap.
- Absent adapter reports unavailable; usage/runtime/cancelled command failures
  preserve exit status, thrown command failures remain fixed internal diagnostics,
  and output stream failure cannot be a successful command.
- Help/version and metadata listing require no provider/model command. Verify
  all four closed request operations, default local target, explicit target,
  exact canonical service and text/JSON format through the real registry.

Root owns the production runner/store/preparation/kernel/output failure matrix
and will append its coverage here; parser tests do not claim those integrations.

Production integration coverage is implemented in test.cpp and failures.cpp:
real temporary SQLite, genuine standalone OpsSession/kernel/launch policy,
one physical worker slot shared by preparation and collection, and owning typed
fake sources. Includes zero-store/preparation listing and rejected selection;
create collision, preparation/source/cancellation failure; atomic admission and
publication refusal; output failure after commit; stalled source lifetime past
the real logical deadline with rejected late evidence; all three implemented
Linux reads plus Kubernetes workloads, exact Pod and namespace/exact-Pod events.
Terminal assertions are scoped to the manual run containing the human
observation request, while admission refusal proves that only the completed
target-selection control run survives. Source context/namespace and captured
timestamps remain in text/JSON output. No provider or generic process port is
present. Pending full runtime execution is tracked in the PR evidence.

Service and Pod log leaves require the explicit `--allow-log-text` flag; it is
invalid on every other leaf. The service leaf accepts only a canonical unit.
The Pod leaf also requires exact name, UID and bounded container name. Production
performs one bounded health proof read, refuses absent service invocation or
container runtime identity, then enables that exact source and performs one
finite log read. Cancellation, deadline, preparation, proof, bind, log, output
and teardown exits revoke consent and close the session/broker without retry.
The second durable selection append is refused after proof and consent mutation;
the command returns the fixed bind failure, performs no log read, persists no
enabled selection and tears down the temporary activation.
The standalone path contains no provider or Explain dependency.
