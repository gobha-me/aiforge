# Ops target catalog failure matrix

- Reject non-file authority, unknown ops configuration keys, malformed typed
  values and rejected/shadowed malformed catalog candidates; never fall back to
  a local read after an explicitly invalid target configuration.
- Reject duplicate/reserved IDs, invalid/overlong IDs and display names, unsafe
  text, empty/relative kubeconfig paths, missing context/namespace, invalid DNS
  namespace, unsupported source kinds/fields and inline credentials.
- Bound 32 configured records, 64 KiB aggregate retained text, individual fields;
  exercise exact and one-over bounds without opening referenced kubeconfig files.
- JSON file load rejects duplicate decoded keys at every nesting level. Typed
  set/load round-trips both variants; invalid updates preserve existing content.
- Built-in local metadata requires no file; configured Linux labels remain
  distinct. Neither metadata resolution nor formatting creates a source, reads
  procfs/kubeconfig, activates a broker, or grants log access.
- Positive smoke last: two valid owner records resolve in stable order with the
  built-in local entry. Model/provider/credential ports are absent.
