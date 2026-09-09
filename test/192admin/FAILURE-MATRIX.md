# Standalone Admin CLI failure matrix

Parser/dispatch subsection written before implementation. These cases use a
recording AdminCommand port, with no adapter, store, source or provider calls.

- Missing/unknown subcommand, missing/extra unit, unknown/duplicate options and
  missing target value: usage failure and zero command execution.
- Target ID: nonempty, <=64 bytes, lower ASCII alphanumeric endpoints, only lower
  ASCII alphanumeric/underscore/hyphen inside. Never fall back to local on bad input.
- Service name: 9..255 bytes, .service suffix, first byte not '-', ASCII
  alphanumeric plus _-.@:. No shell syntax, path, backslash, Unicode or controls.
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
read operations and exact service identity. No provider or generic process port
is present. Pending full runtime execution is tracked in the PR evidence.
