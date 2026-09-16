# Standalone manual Ops session failure matrix

Recorded before implementation from the accepted186 API checkpoint.

- Invalid UTF-8 within an admitted domain ID, absent generator, policy, broker
  and kernel limits fail before
  durable create. Duplicate create never invokes resume/replay or changes an
  existing selection. Surface allocation precedes the kernel's create call.
  A generator throwing during submit appends/launches nothing.
- Unbound/stale/foreign intent, active or retiring run, reused generated ID and
  wrong run/invocation approval cannot launch an additional observation.
- Real Observe allow_all allows repeated reads. Prompt approve/deny retains exact
  scopes. Automatic rules retain their existing match ceiling; accepted immediate
  policy denial is terminal failure, never observation success.
- Failed source, saturation, broker closure/selection change, cancellation and
  append refusal cannot publish uncommitted evidence. Last successful original-
  target evidence remains separate from failed or running work. Broker/source
  disconnection is typed separately from generic availability, including an
  asynchronous native-source disconnect reported once by broker owner service;
  ordinary source unavailability, explicit close and storage/history/internal
  failures cannot masquerade as disconnection.
- Pure projection rejects missing/substituted request, observation/result pair,
  invocation, canonical content or completion, and cancellation resurrection.
  Valid mixed conversation/manual history exposes only committed human evidence.
  Replay retains the latest successful terminal observation in each of the final
  eight operation slots under a fixed 512 KiB aggregate, preserves exact event
  provenance and earlier success across later failure, and rejects cancellation,
  malformed history or bounds before returning a partial catalog. Superseded
  evidence does not count against the final retained aggregate. A target change
  cannot cross an in-flight observation or its result publication, and the
  latest catalog entry follows successful completion order when runs interleave.
- Owner pumping advances the real manual kernel independently of inference.
  Cached inspection and unchanged history never perform IO or implicit refresh.
- First and repeated explicit close preserve the original fatal cancellation
  persistence error even after broker cleanup retires the run; repeated close
  makes no additional append attempt. Destructor stops only
  its own captured kernel; a replacement broker issuer remains usable. Physical
  retired source slots and stalled cleanup retain their existing contract.
- No provider/model/credential port exists in standalone dependencies. Committed
  history contains no InferenceStarted, and the private backend fails closed if
  called. Tests use actual durable kernel plus bounded fake source/store.

Pure projector runs and runtime source/test syntax checks are local. Actual
standalone kernel runtime tests and grouped session integration use root's
serialized full Captain compiler matrix; do not conflate syntax with execution.
