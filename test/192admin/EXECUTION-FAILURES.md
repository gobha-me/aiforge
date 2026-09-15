# Standalone Admin execution failure matrix

Before implementation: exercise invalid operation/unit/target and incompatible
catalog selection with zero store/factory calls; malformed catalog refusal;
metadata listing with zero store/preparation/observation calls; store open/create
and publication refusal; preparation failure, invalid identity, cancellation and
expiry; capacity-one preparation retirement before observation; source refusal
and stalled source cancellation; exact committed current success only; output
failure after durable success without recollection. The hermetic runner uses
real OpsSession, broker/worker, native registry/policy and temporary SQLite.
No provider, generic process executor, model catalog or credential port exists.
