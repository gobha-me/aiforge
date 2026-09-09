# Actual Chat Admin integration failure matrix

The tests exercise InteractiveChatApp with an actual ChatSession, real SQLite,
real launch policy, and an owning bounded fake catalog. All collection is fake;
no infrastructure, credentials, provider network, or live source is used.

- Missing/explicitly failed catalog: opening, malformed commands, resize and
  redraw never prepare a default source or dispatch a model.
- No selection: health is refused without a run; inspecting and hiding toolbars
  remain available through normal keyboard/paste/Enter paths.
- Manual health/services/exact cached service: typed observation and result are
  durable once; model metadata unavailable after open, backend unavailable,
  model tool support disabled and an off tool profile do not become dependencies.
- Disabled tick polling: Admin actions and ticks pump manual work without
  consuming an ordinary queued provider update. An ordinary runtime wake still
  drains that run according to the existing bridge contract. Admin inspection, close,
  Escape, invalid commands, and busy reads do not cancel/relaunch that run.
- Real policy approval: approval overlays Admin; approval, denial and cancellation
  deliver their terminal events without a model continuation or duplicate result;
  returning to Admin preserves selection/focus.
- Failed candidate target: source preparation failure preserves usable last-good
  target/history; cancelled or superseded held preparation cannot later bind.
- Candidate session open/replay refusal preserves old selection; successful
  replacement clears current authority and labels retained evidence truthfully.
- Store append refusal: no partial observation/result pair or duplicate surface
  delivery; subsequent ticks do not silently restart work.
- App teardown with source/preparation work in flight: shared owned state survives
  until physical retirement; assertion unwinding always releases all test gates.
- Menu/command parity and narrow displays use existing widget events; redraw,
  navigation and toolbar visibility do not themselves collect evidence.

Coverage is recorded per test as it is implemented. This matrix is a target,
not a claim that all paths have executed. Full adapter/Chat runtime verification
belongs to the serialized GCC/Clang matrix; no test-only production callback or
public authority bypass is introduced.

## Current implemented coverage and limits

The fourteen application cases and one pure registry discovery case cover the rows above with real SQLite, owning
fake sources, actual launch policy, normal input and rendered-menu clicks. The
storage-refusal case runs with tick polling off/on and the explicit runtime wake
path. Assertions inspect durable/application event history, current rendered
view and failure/status state. `InteractiveChatApp::events()` is the session log;
it is not an independent count of private `apply_events` calls.

An arbitrary delayed private approval callback cannot be invoked through the
public application input API. The tests exercise the real overlay decision,
restored underlying view, and session replacement; direct stale callback
injection is not claimed. Runtime wake tests deliver the existing typed event
marker through `on_event`; they do not claim an operating-system terminal loop
or physical terminal screenshot. Full runtime execution is pending Captain.
