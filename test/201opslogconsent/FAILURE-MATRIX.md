# Current-session Ops log consent failure matrix

- Construction rejects enabled or source-bearing policy and is bound to one
  exact broker activation, so persisted/history data and a reused SessionId
  cannot restore live consent.
- A change must match the exact session, target binding, selection generation,
  policy revision and application source identity.
- Duplicate enable/disable and stale concurrent changes fail without advancing
  or broadening the current policy.
- Enabling distinct exact sources is bounded by the domain ceiling; disabling
  the last source returns to a disabled policy.
- Every effective policy change advances selection and policy revisions, so
  pending work under the prior snapshot cannot publish.
- Target replacement accepts only the same owner/session, newer generations and
  a disabled empty policy. A broker target switch not preceded by that exact
  replacement revokes the retained consent object before it can advance or
  rebind old target authority.
- Reactivation and revision exhaustion revoke the live activation, clear its
  sources and make broker-held enabled snapshots unusable.
- Explicit revocation is immediate, idempotent and permanently invalidates
  authority, changes and replacement through that activation.
- A happy-path smoke grants and revokes one exact Pod/container source.
