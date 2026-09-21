# Admin presentation failure matrix

- Invalid, oversized, multiline and unknown Admin commands refuse as commands;
  no shell interpretation, quoted unit aliases or model fallback. Exact target
  and unit validation is shared with the existing noninteractive command.
- Opening, refresh, rendering, resizing, scrolling and target highlighting only
  inspect cached metadata. Explicit target use and reads send one typed action.
- Service row actions carry the rendered session, event, generation and row;
  replacement inventory cannot silently change the action identity.
- Kubernetes menus/keys expose workloads, exact cached Pod health and namespace
  or selected-Pod events. Pod rows retain session/event/generation/UID identity;
  non-Pod workload rows cannot dispatch Pod actions. Context, namespace,
  endpoint, captured time, age and last-success/refreshing/failed/disconnected
  labels are cached presentation only and cause no collection.
- Every displayed refresh retains the committed session, full target,
  generation, event, operation and resource scope. Selecting B cannot alter an
  A service, Pod, exact-Pod-event or namespace-event action. Cross-navigation
  cannot substitute the opposite inventory kind or broaden exact-Pod events
  into namespace events; malformed displayed evidence has no refresh action.
- Failed actions preserve the last committed display and report a fixed error.
  Pending selection, active target and historical evidence remain distinct. A
  compact active-B view may retain last-success evidence captured from A without
  presenting it as disconnected.
- Toolbar hiding is cosmetic. Keyboard view navigation and Escape remain usable
  with hidden menus and at 120x32, 80x24, 40x12, 20x5 and one/two-row sizes.
  At 20x5, target, Kubernetes context/namespace, freshness and fixed status or
  error feedback remain visible. Detached and typed source-disconnected states
  stay visibly distinct. Resize closes dropdowns and hidden controls leave the
  focus ring.
- Malformed injected snapshots refuse formatting; no unbounded text or terminal
  controls pass through metadata. Valid structured evidence uses the existing
  complete formatter, retaining scope, timestamps and unknown values.
- Happy-path buttons, menus and parsed commands dispatch identical actions.
  The adapter has no source, worker, kernel, provider or storage dependency.

- Log enable, disable and read controls hydrate only the exact displayed
  service-invocation or Pod/container-runtime proof. Toolbar, menu, hidden-menu
  keys and slash commands share those typed actions. After enable advances the
  policy counters, the same displayed source/evidence uses the retained current
  proof, while a different candidate remains separate. Compact output keeps
  consent and selected source visible; drawing and navigation never enable or
  read logs, and log actions never invoke Explain.
- `/admin logs enable <container>` resolves exactly one name only from the
  currently displayed committed Pod-health proof, then dispatches the ordinary
  hydrated enable action. Unknown, stale, duplicate and missing-runtime
  container identities fail before source access.
