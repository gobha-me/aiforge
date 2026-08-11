# ADR 0003: Command-line parser boundary

- Status: Accepted
- Date: 2026-08-11

## Context

AIForge needs a command-line grammar for the default surface, nested
subcommands, positionals, typed options, repeated values, and the `--`
positional delimiter. The parser must preserve whether a value was absent or
explicitly supplied so the configuration resolver can apply command-line,
environment, file, and compiled-default precedence with provenance.

The command registry remains the source of command identity, help, availability,
completion, and dispatch. A parser library is only an adapter implementation;
it must not become a second registry or leak its types and failure conventions
into the application runtime.

The candidates considered were CLI11, p-ranav argparse, cxxopts, Lyra, and a
small bespoke parser provisionally called `argforge`. The mature libraries use
exceptions for at least some parse or construction failures, while AIForge's
public fallible operations return `std::expected`.

## Decision

Use CLI11, initially compatible with version 2.7 and with a v2.7.2 pinned
`FetchContent` fallback, behind an AIForge-owned adapter.

The adapter contract is provider- and library-neutral:

- it accepts bounded argument strings and returns a neutral invocation or a
  typed diagnostic through `std::expected`;
- it catches CLI11 parse and construction exceptions before they cross the
  adapter boundary;
- it never prints diagnostics, calls `exit`, dispatches a handler, or mutates
  application state;
- it preserves absence separately from explicit default-like values such as
  zero and false;
- it applies explicit cardinality to container-valued options and rejects or
  normalizes ambiguous greedy forms rather than silently consuming a positional;
- it preflights argument-count and argument-size limits before invoking CLI11;
- it does not expose CLI11 objects in public domain, runtime, or surface APIs.

Environment variables and configuration files are not parsed through CLI11.
They remain independent AF-04 inputs so their provenance is preserved. The
future command registry may carry environment-binding metadata, but the
configuration resolver performs the binding and precedence decision.

Help, completion, command listing, availability, and dispatch derive from the
AF-06 registry. The adapter may consume registry metadata to configure CLI11;
CLI11 introspection is not an authoritative parallel table.

The dependency recipe will use `find_package(CLI11 2.7 CONFIG QUIET)` first.
When no compatible package is available, it will fetch the v2.7.2 release with
upstream tests, examples, and install rules disabled and link the canonical
`CLI11::CLI11` target. A future pin change requires a compatibility reason.

## Spike evidence

An isolated two-command probe modeled `chat` and nested `config show` paths. It
demonstrated typed optional values, explicit zero and false, repeated values,
the positional delimiter, nested subcommands, and conversion of unknown
commands/options, missing subcommands, invalid numbers, and oversized input into
typed errors without output or process termination. The probe configured,
compiled, and passed with GCC 14.2 and Clang 20.1 in C++23 mode.

The probe also exposed that an unconstrained vector option greedily consumes
following positional values. The production registry/adapter must declare
cardinality and test repeated options adjacent to positionals; callers must not
depend on CLI11's unconstrained container default.

## Alternatives considered

- **p-ranav argparse:** familiar syntax, but no advantage over CLI11 for the
  required adapter boundary and less alignment with environment and command
  metadata needs.
- **cxxopts:** suitable for flat option parsing, but native subcommand support is
  not strong enough for the planned command hierarchy.
- **Lyra:** composable and lightweight, but offers less of the required command
  and metadata surface than CLI11 without eliminating exception containment.
- **`argforge`:** would provide native `std::expected` semantics, but a new
  generic parser library is not justified while a thin CLI11 adapter satisfies
  the current milestone. Reconsider only if concrete adapter limitations emerge,
  and obtain maintainer agreement before creating or extracting a project.

## Consequences

- AF-06 can implement one AIForge registry while delegating tokenization and
  typed conversion to the adapter.
- AF-04 remains solely responsible for configuration-source precedence and
  provenance.
- AF-07 can share the same parse result without inheriting CLI11 output or exit
  behavior.
- Parser exceptions remain an adapter implementation detail; exception-free
  compilation is not required.
- Production dependency integration and adapter failure tests are a focused
  follow-on and are not part of this decision spike.
