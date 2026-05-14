# amber.runtime.v1

Status: `W5.1` through `W6.4` acceptance is satisfied.

The runtime error taxonomy is frozen in `spec/registries/runtime_errors.yaml`.

The first runtime implementation must keep frontend, bytecode container, VM, loader, scheduler, and memory boundaries separately testable. No stdlib helper may silently redefine language semantics that belong in parser, binder, HIR, or VM contracts.

Current implemented slice:

- standalone `runtime/vm.{h,cpp}` execution core for verified `BcCode`;
- frame-local register file and `last_result` slot;
- direct code entry with positional args;
- opcode subset: `LOADK`, `LOADNULL`, `LOADBOOL`, `MOVE`, `LOADSELF`,
  `GETLAST`, `SETLAST`, `MAKE_LIST`, `MAKE_TUPLE`, `MAKE_MAP`, `FREEZE`,
  `LOADUPVAL`, `STOREUPVAL`, `LOAD_IVAR`, `STORE_IVAR`, `LOOKUP_CONST`,
  `MAKECLOSURE`, `OBJ_DESTROY`, `OBJ_DEALLOC`, `CALL`, `SEND`, `SEND_DYN`,
  `JUMP`, `JUMP_IF_TRUE`, `JUMP_IF_FALSE`, `JUMP_IF_NULL`, `RETURN`, `RAISE`,
  `CLOSE_UPVALUES`, `SAFEPOINT`, plus the current pattern-op family;
- builtin scalar dispatch for integer arithmetic/comparison selectors and
  dynamic selector decoding from `Symbol` / `String`;
- runtime container values for list/tuple/map plus builtin `empty?`,
  collection indexing, and minimal `deconstruct` / `deconstruct_keys`
  bridging for the currently emitted matcher paths;
- local class-object lookup for single- and multi-segment `KONS[path]`
  constants, with exact full-path matching before unique leaf fallback;
- class/instance method dispatch through `CLAS.method_range_*`, direct
  superclass lookup, instance-side `include` linearization, and class-side
  `extend` linearization for single- and multi-segment local refs;
- monomorphic runtime inline caches for `SEND` / `SEND_DYN` method hits and
  `LOAD_IVAR` / `STORE_IVAR` slot lookup, guarded by receiver class,
  selector/name, dispatch method version, `world_epoch`, and ivar
  `shape_id` / `shape_version` where applicable;
- persistent `RuntimeWorld` execution context with public instance-method
  replacement plus direct `include` / `extend` mutation hooks, dynamic method
  and ancestor overlays, and observable `world_epoch` / owner
  `method_version` invalidation;
- `method_missing` fallback on class/instance dispatch without recursive
  fallback on the `method_missing` selector itself;
- constructor `CALL` on class objects, allocating an instance and routing to
  instance-side `init` when present;
- tail positional default-thunk materialization for method dispatch and
  constructor `init` dispatch;
- runtime keyword shaping for positional/keyword method dispatch;
- forwarded block values for user-defined `SEND` / `SEND_DYN` / constructor
  `init` dispatch;
- eager runtime execution of `BcMethod.clause_table[]` through emitted clause
  pattern-probe code plus nested guard/body execution, with transactional
  pattern-local commit on successful clause match;
- inline matcher execution for `case` / `case!`, block-pattern prologues, and
  pattern assignment across scalar/class/sequence/map paths:
  `PPrepSeq`, `PPrepMap`, `PCheckEq`, `PCheckPin`, `PCheckLenEq`,
  `PCheckLenGte`, `PGetIndex`, `PHasKey`, `PGetKey`, `PTripleEq`, `PBind`,
  `PCommit`, `PFail`;
- deferred pattern-binding semantics for `PBind` / `PCommit`, including
  rest-slice materialization for sequence/map rest bindings and strict-map
  extra-key rejection on success boundary;
- public `ObjHeader` / `ShapeDescriptor` metadata on heap values, with
  slot-backed `InstanceValue` ivar storage and a legacy map mirror for tests and
  slow-path compatibility;
- stable shape interning for root class shapes and ivar growth transitions,
  guarded by `shape_id` / `shape_version` in ivar inline caches;
- dead-shape compatibility checks for instance ivar access before ordinary
  shape-cache lookup, returning `UseAfterFreeError` for tombstone-like headers;
- runtime owner method-table descriptors populated from `CLAS.method_range_*`
  and dynamic method replacement, with class/instance table observability through
  `RuntimeWorld::method_table_size(...)`;
- per-execution class storage semantics for `LOAD_CVAR` / `STORE_CVAR`;
- runtime-side ivar auto-assign commit for `@field` params before method body
  execution, plus class-variable auto-assign for `@@field`;
- `W6.1` heap allocation boundary for runtime-owned instances, lists, tuples,
  maps, and closures, with allocation ids in `ObjHeader`, per-worker arena
  counters, remote-free queues, owner-side drain semantics, and safepoint/final
  execution drain hooks;
- `W6.2` lifecycle baseline with `ObjectLifetimeState`, special `destroy!`
  dispatch, `OBJ_DESTROY` / `OBJ_DEALLOC`, idempotent destroy/dealloc results,
  tombstone `DeadShape` rewrite, payload release for instances, lists, tuples,
  maps, and closures, ownership/lifetime precondition errors, and
  `DestroyedAccessError` /
  `UseAfterFreeError` guards for ordinary access paths;
- `W6.3` non-moving GC boundary with `ObjectGeneration`, public
  `RuntimeGcCycle` collection hooks, logical mark/sweep over heap allocation
  records, root scanning for VM frames and runtime class cvars, write barriers
  for heap-reference writes, mature-to-young remembered sets, shared-to-confined
  isolation rejection, safepoint-triggered collection requests, unrooted cycle
  logical reclaim, and parallel smoke coverage;
- `W6.4` pinning/native boundary with public `RuntimePinToken`,
  active-pin registry roots for GC, `RuntimePinScope` nesting, stale/double
  unpin guards, `PinnedObjectError` lifecycle rejection, opaque managed handles,
  pinned list/tuple value-buffer views, and native wait cancellation poll hooks;
- direct `P_PREP_SEQ` / `P_PREP_MAP` coercion through object-level
  `deconstruct` / `deconstruct_keys(keys)` when native list/tuple/map
  matching is not available, with `null` as no-match and wrong protocol return
  types as `TypeError`;
- `RAISE` execution with handler-table protected-range lookup, unwinding across
  active method and closure frames, out-of-line rescue handler code execution,
  and unhandled fault traces backed by `SPAN` / `LINE` metadata where present;
- unit coverage for emitted methods, branching/falsey semantics, and closure
  capture/call baseline, plus emitted and dynamic send scenarios, class-side
  lookup/send, constructor call, keyword shaping, block forwarding, instance
  dispatch, unicode auto-assign, ivar/cvar load/store, multi-segment class
  refs, cache guard miss paths, dynamic method replacement invalidation, late
  include invalidation, object `deconstruct*` pattern protocol paths, and
  handled/unhandled `RAISE` paths, plus W5.4 slot-shape transition stability,
  dead-shape guard, method-table descriptor coverage, W6.1 allocator stress /
  remote-free / VM allocation-path coverage, W6.2 lifecycle destroy, dealloc,
  tombstone, and dead-access coverage, W6.3 non-moving GC, barrier,
  remembered-set, safepoint-root, and parallel smoke coverage, and W6.4 pin
  roots, stale-unpin, nested-scope, opaque-handle, buffer-view,
  dealloc-after-pin, native-wait-cancel, and parallel pin/unpin coverage.

Still intentionally missing in later layers:

- `W8.1`: dependency linker and module-init state machine over serialized
  `.amberbc`;
- `W7+`: scheduler, concurrency, and deeper memory/runtime integration.
