# Admin presentation failure matrix

- Invalid, oversized, multiline and unknown Admin commands refuse as commands;
  no shell interpretation, quoted unit aliases or model fallback. Exact target
  and unit validation is shared with the existing noninteractive command.
- Opening, refresh, rendering, resizing, scrolling and target highlighting only
  inspect cached metadata. Explicit target use and reads send one typed action.
- Service row actions carry the rendered session, event, generation and row;
  replacement inventory cannot silently change the action identity.
- Failed actions preserve the last committed display and report a fixed error.
  Pending selection, active target and historical evidence remain distinct.
- Toolbar hiding is cosmetic. Keyboard view navigation and Escape remain usable
  with hidden menus and at 120x32, 80x24, 40x12, 20x5 and one/two-row sizes.
  Resize closes dropdowns and hidden controls leave the focus ring.
- Malformed injected snapshots refuse formatting; no unbounded text or terminal
  controls pass through metadata. Valid structured evidence uses the existing
  complete formatter, retaining scope, timestamps and unknown values.
- Happy-path buttons, menus and parsed commands dispatch identical actions.
  The adapter has no source, worker, kernel, provider or storage dependency.
