# Bounded Ops explanation failure matrix

Written before implementation. All inputs are synthetic durable events; tests
perform no source, broker, cluster, host, artifact, provider, or paid request.

- Reject a missing or forward-referenced event as stale, and reject an existing
  event whose payload is not one typed Ops observation.
- Reject malformed or incomplete observation lineage before producing model
  evidence. A recorded observation without its exact successful tool result and
  successful source-run terminal is not explainable. Both must precede the
  selection; later completion cannot retroactively validate it.
- Bound the event scan, selection count, projected bytes, token estimate and
  cancellation. Never truncate a selected observation into a different fact.
- Require one selection in a conversation run before user/model content, with
  no invocation or parent-run attribution. Duplicate, late, control-run and
  unsupported-schema selections fail closed.
- Replay rejects empty current user messages and those carrying artifact
  references, unknown content, invocation attribution or tool calls; those
  shapes cannot be made valid by inserting them directly into durable storage.
- Admit exactly one derived evidence entry with the evidence role, stable event
  provenance and a digest, plus exactly one current user conversation entry.
  Changed content/provenance, prior conversation, tool results, contradictory
  decisions or additional evidence cannot satisfy kernel admission.
- Require the complete Explain context to be exactly reproducible by the
  ContextBuilder. Duplicate identities, invalid order/provenance/estimates,
  false totals or insufficient capacity fail before persistence or dispatch.
- When user-global instructions are present, require their deterministic
  entry/message identity, exact digest, positive target-model estimate and
  order, and exactly one unqualified admission decision.
- Explain runs expose no tools and carry no repository/local source admission,
  imported artifact or summary-generation work. Rejection commits nothing and
  starts no backend.
- SQLite replay reconstructs the same selection and evidence entirely from the
  committed event stream. Replay causes no backend or source activity and
  rejects effect-bearing events in a selected Explain run.

The happy path selects one completed unhealthy-service observation, commits its
selection before inference, and gives the backend only the bounded untrusted
snapshot plus ordinary runtime/user context.
