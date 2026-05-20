# amber.runtime.v1

Status: `W5.1` through `W8.3` runtime acceptance is satisfied, the `W8.4`
full conformance runner gate is satisfied through `ambertest`, `W9.2`
open-world transaction/freeze plus `W9.3` reflection mirror behavior is covered
in VM tests, `W9.4` package artifact tooling is covered by package tests, and
`W9.5` package hot-reload swaps, `W10.1` advanced concurrency runtime behavior,
`W10.2` awaitable/native-readiness behavior, and `W10.4` frozen native
trampoline behavior are covered by VM/native tests.

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
  collection indexing, `deconstruct` / `deconstruct_keys` bridging for matcher
  paths, and W8.3 eager collection SEND selectors for sequences and maps;
- local class-object lookup for single- and multi-segment `KONS[path]`
  constants, with exact full-path matching before unique leaf fallback;
- class/instance method dispatch through `CLAS.method_range_*`, direct
  superclass lookup, instance-side `include` linearization, and class-side
  `extend` linearization for single- and multi-segment local refs;
- monomorphic runtime inline caches for `SEND` / `SEND_DYN` method hits and
  `LOAD_IVAR` / `STORE_IVAR` slot lookup, guarded by receiver class,
  selector/name, dispatch method version, `world_epoch`, and ivar
  `shape_id` / `shape_version` where applicable;
- persistent `RuntimeWorld` execution context with atomic
  `RuntimeWorldTransaction` commits for class/mixin reopen, instance/class
  method replacement, direct `include`, and class-side `extend`, with
  pre-commit kind/superclass/include-cycle validation, rollback on failure,
  `open -> frozen` state, `WorldFrozenError` guards, and observable
  `world_epoch` / owner `method_version` invalidation;
- open-world dev-profile package hot reload in `RuntimeWorld`, using
  `reload_package_artifact` to predecode a whole `.amberpkg` module set,
  reject frozen worlds, enforce manifest identity plus ABI/profile/export
  surface and selector/arity compatibility, publish the root module atomically,
  and invalidate dispatch through a single `world_epoch` bump;
- runtime reflection mirror snapshots for package/world/class/mixin/method
  state through `RuntimeWorld::{package,world,class,mixin,owner}_mirror`,
  returning copied read-only views with deterministic method/export/dependency
  ordering, direct include/extend visibility for static and late dynamic edges,
  source locations from bytecode debug spans, and no mutable method-table
  backdoor;
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
- `W7.1` worker-pool scheduler with public `RuntimeScheduler`,
  `RuntimeWorkerScope` / `RuntimeStrandScope` TLS, global and worker-local
  runnable queues, timer-backed sleeping strands, explicit wake coalescing,
  deterministic idle waits, and parallel smoke coverage;
- `W7.2` task lifecycle with task TLS, join/rethrow results, timed joins
  without auto-cancel, cooperative cancellation polling, structured child sets,
  waiting parent scope exit, first-failure propagation, and sibling
  cancellation;
- `W7.3` concurrency primitives with `RuntimeChannel` rendezvous and buffered
  modes, FIFO blocking send/recv queues, explicit `close()`,
  `ChannelClosedError`, timeout/cancellation-aware blocking results,
  recursive shareability checks for channel payloads, non-reentrant
  `RuntimeMutex`, and seq-cst `RuntimeAtomic`;
- `W10.1` advanced concurrency runtime in `runtime/vm.{h,cpp}` with
  `RuntimeMoveSlot` ownership-transfer reservations, moved-from
  `MovedValueError` reads, move-aware `RuntimeChannel::send` for confined heap
  graphs, receiver-side adoption of moved payloads, `runtime_select` recv/send
  arms with timeout and immediate `else` behavior, rotating ready-arm selection
  to avoid fixed left bias, and structured-task supervisor policies
  `CancelScope`, `OneForOne`, `OneForAll`, and `RestForOne` through
  `RuntimeTaskOptions`;
- `W10.2` async-I/O awaitable bridge in `runtime/vm.{h,cpp}` with
  `RuntimeAwaitable` pending/ready/failed/cancelled states, awaitable-compatible
  `runtime_select` arms, bounded await/poll behavior, cooperative task
  cancellation propagation, and native wait readiness tokens backed by active
  W6.4 pin handles;
- `W10.4` native/JIT runtime bridge in `runtime/native_bridge.{h,cpp}` with
  `amber.native.v1` world-epoch and method-version assumption checks,
  frozen-world rejection, deterministic bytecode-trampoline execution through
  `RuntimeWorld::execute`, and explicit bytecode fallback for stale native
  assumptions when the caller discards invalid native code;
- `W8.1`-`W8.2` loader baseline in `runtime/module_loader.{h,cpp}` with
  serialized `.amberbc` decode/verify on every load path, dependency graph
  linking, deterministic dependency-before-dependent module init, single-run
  init snapshots, missing-dependency/export `ImportError`, dependency
  format/language/ABI compatibility diagnostics, export-cell/import-alias
  snapshots, read-only alias readiness checks, re-export chain resolution,
  cycle-aware `ModuleInitError`, and source-mapped VM fault propagation for
  failed module init;
- `W8.3` collections baseline in `runtime/vm.{h,cpp}` with closure-block
  execution for eager sequence `each`, `map`, `flat_map`, `select`, `reject`,
  `reduce`, `find`, `any?`, `all?`, `none?`, `first`, `count`, `group_by`,
  `to_a`, and eager-compatible `lazy`, plus `Map#each`, `Map#map`,
  `Map#select`, `Map#reject`, `transform_values`, `keys`, `values`, and
  `entries` with insertion-order-preserving results;
- `W8.4` conformance gate in `tools/ambertest` with deterministic fixture
  discovery, focused mismatch rendering, phase aliases for
  `lower`/`compile`/`disasm`, positive `check`/`run`/`load` lanes, and
  cumulative `M1`-`M5` bundle filtering exposed through `make conformance`;
- `W9.4` package tooling in `package/package.{h,cpp}` and `amberc package-*`
  commands with restricted manifest parsing, deterministic lock rendering,
  reproducible `.amberpkg` serialization, SHA-256 dev signatures, artifact
  verification, and filesystem registry install/publish smoke;
- `W9.5` package hot-reload runtime path in `runtime/vm.{h,cpp}` with
  compatible body-swap success, incompatible export/arity rejection,
  frozen-world rejection, and failed-decode rollback coverage;
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
  dealloc-after-pin, native-wait-cancel, and parallel pin/unpin coverage, plus
  W7.1 worker-pool scheduler, strand TLS scope, timer wake, explicit wake
  coalescing, and parallel strand smoke coverage, and W7.2 task lifecycle,
  join/rethrow, timed join, cooperative cancellation, waiting scope-exit parent,
  structured child, and first-failure sibling-cancellation coverage, plus W7.3
  rendezvous/buffered channel close/FIFO/isolation coverage, mutex
  reentrancy/contention coverage, and atomic compare-and-set contention
  coverage, plus W8.1-W8.2 verifier-gated module load, missing dependency,
  dependency init order, single-run init, cycle detection, failed-init, export
  alias, missing export, version/ABI mismatch, re-export, and source-mapped
  loader diagnostic coverage, plus W8.3 eager sequence chaining, `reduce`
  empty-error, `flat_map`, `count`, `find`, `group_by`, ordered `Map`
  projection/transform coverage, and W9.2 open-world transaction coverage for
  rollback, `WorldFrozenError`, `SuperclassMismatchError`, include-cycle
  rejection, mixin method replacement, and late class-side `extend`
  invalidation, plus W9.3 mirror coverage for read-only snapshots,
  deterministic ordering, source locations, and post-mutation stability, plus
  W9.4 package manifest/lock determinism, signed artifact reproducibility,
  signature rejection, and registry publish/install smoke coverage, plus W9.5
  package hot-reload coverage for compatible swaps, incompatible export/arity
  guards, frozen-world rejection, and failed-decode rollback, plus W10.1
  runtime coverage for moved channel payloads and moved-from reads,
  `select` fairness/else/timeout paths, moved send-arm commit behavior, and
  supervisor `one_for_one`, `one_for_all`, and `rest_for_one` propagation, plus
  W10.2 awaitable coverage for select readiness/timeout/failure, scheduler wake
  after native wait completion, stale-pin failure, and cancellation finishing
  native waits, plus W10.4 native metadata/trampoline coverage for root maps,
  reflective `SEND_DYN` stubs, frozen-world execution, and stale-assumption
  bytecode fallback.

Still intentionally missing in later layers:

- stdlib/language surfacing for the W10.1-W10.2 runtime primitives.
