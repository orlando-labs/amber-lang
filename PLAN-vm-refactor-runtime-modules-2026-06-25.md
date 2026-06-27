# VM Refactor: Runtime Modules, Dispatch, and Error Registry

Status: active refactor; phase order clarified 2026-06-26. Phase 0 guardrail
baseline is recorded. Phase 1 registry foundation is mostly landed. The broad
mechanical file split is now Phase 2, after semantic registry seams exist.

This is a refactor path for shrinking `runtime/vm.h` / `runtime/vm.cpp` while
also removing the semantic coupling that currently makes core stdlib modules and
native package concepts leak directly into the VM.

## 1. Goal

The VM should own the language execution kernel:

- bytecode dispatch, frames, stack/register state, unwinding, and control flow;
- value representation and object/heap lifetime mechanics;
- capability/effect enforcement at the runtime boundary;
- stable hooks that let registered modules participate in calls, type checks,
  construction, errors, and native ABI interop.

The VM should not know that `net.http.Client`, `ArgParser`, `Uuid`, `Digest`, or
future third-party native package types exist. Those are runtime modules, not VM
intrinsics.

The immediate editability goal is still important: `runtime/vm.h` is about 2.7k
lines and `runtime/vm.cpp` is about 37k lines. The file split should happen, but
it should support the semantic redesign instead of just rearranging large
branches into smaller files.

Current implementation note: `RuntimeWorld` already exists. The first
implementation slice moves the legacy `NativeRegistry` used for migrated stdlib
path lookup and dispatch under `RuntimeWorld::Impl`; private VMs borrow that
registry, while direct VM entry points keep a local fallback for tests and
`execute_code`. The second slice introduces a `RuntimeModuleRegistry` adapter
that imports legacy native paths as `RuntimeBindingRef` values and makes
`Vm::lookup_native_prelude_constant` query it before falling back to
`NativeRegistry::kind_for_path`. The third slice introduces a
`RuntimeTypeRegistry` adapter for native type call metadata and makes bytecode
`CALL` ask the registry instead of keeping a constructor allowlist in the VM.
The next slices move legacy native type path exports such as `net.http.Client`,
`StrictHashMap`, and `sync.Channel`, plus core non-type prelude bindings such
as `print`, `Ok`, `task`, and `task.flow`, into `RuntimeModuleRegistry`. The VM
no longer falls back to `NativeRegistry::kind_for_path` for prelude lookup. The
current slice adds a `RuntimeDispatchRegistry` adapter for migrated native
handler dispatch; `Vm::try_apply_native_stdlib_send` now asks that adapter
instead of calling `NativeRegistry::handler_for` directly. The error-registry
slice adds `RuntimeErrorRegistry` as a world/direct-VM adapter for builtin error
id/name/inheritance lookup; VM prelude lookup, rescue matching, and native
error SEND selectors now ask that adapter while field/default-message metadata
remains on the legacy generated table. The first descriptor-backed module
slices add `RuntimeNativeModuleDescriptor` for path/handler exports and
register the current migrated stdlib set (`Math`, `Json`, codecs, `Digest`,
`SecureRandom`, `ArgParser`, `Uuid`, `Time`, and `Url`) directly into
`RuntimeModuleRegistry` and `RuntimeDispatchRegistry`, while still feeding the
legacy `NativeRegistry` for compatibility. `RuntimeWorld` and direct VM
fallback registries no longer seed module/dispatch bindings from
`NativeRegistry`; the import helpers remain only as compatibility APIs/tests.

## 2. Current Pressure Points

- `RuntimeNativeTypeKind` mixes true VM concepts with stdlib/module concepts.
  Core examples include `NetHttpClient`, `NetHttpRequest`, `ArgParser`, `Uuid`,
  `Digest`, `Time`, `Bytes`, `FsPath`, and similar names.
- `Vm::lookup_native_prelude_constant` maps module paths directly to
  `RuntimeNativeTypeKind` values, including nested paths such as
  `net.http.Client`. This direct map is now reduced to registry lookup.
- `Vm::try_apply_native_stdlib_send` still contains large module-specific
  dispatch chains, but migrated handler dispatch now enters through
  `RuntimeDispatchRegistry`; the remaining branches are the next descriptor
  migration targets.
- The bytecode `CALL` path contains an allowlist of native type kinds that may
  behave as constructors. Adding a module-owned type can require editing the VM
  dispatch loop. The first `RuntimeTypeRegistry` adapter removes this allowlist
  from the VM, but still registers legacy enum-backed call metadata.
- Typed rescue depends on a VM-global runtime error hierarchy. The
  `PoolTimeoutError` issue is the same smell in a different subsystem: if a
  module-owned error class is not registered in the VM's error table, then
  `rescue PoolTimeoutError` or `rescue HttpTimeoutError` cannot match it.
- Third-party native packages already have `RuntimeForeignHandle`,
  `NativeTagRegistry`, `NativeExtRegistry`, generated thunk registration, and
  `amber_ext` fault plumbing, but they do not yet have one unified route for
  registering package-owned types, constructors, dispatch, and rescue-able error
  classes into the same runtime namespace as first-party modules.
- The compatibility stdlib registry is now world-owned and still feeds legacy
  callers/tests. Runtime module and dispatch registries for migrated stdlib
  modules are seeded from descriptors instead of importing `NativeRegistry`
  path/handler tables. These adapters still point at `RuntimeNativeTypeKind`;
  the next pressure point is replacing the remaining enum-backed type,
  constructor, dispatch, and error entries with richer descriptors.

In the current checkout, `PoolTimeoutError` is already present in
`spec/registries/runtime_errors.yaml` and `spec/registries/runtime_errors.def`,
and `tests/vm_net_http_tests.cpp` has a typed rescue test for the active-slot
pool timeout path. If a parallel branch still says typed rescue does not catch
it, that branch is probably missing or failing to regenerate the runtime error
registry. The architectural pressure remains: module errors should not require
hardcoding in the VM.

## 3. Target Architecture

### Runtime world as composition root

`RuntimeWorld` becomes the owner of runtime registries:

- module/path registry: resolves `Math`, `net.http.Client`, package exports, and
  aliases to runtime bindings;
- type registry: stores VM intrinsic types plus module-defined runtime types;
- dispatch registry: routes static calls, constructors, methods, properties, and
  conversion hooks;
- error registry: stores error classes and parent links used by typed rescue;
- native package registry: exposes thunks, foreign-handle descriptors, ownership
  rules, and package-provided error classes.

The private `Vm` receives a `RuntimeWorld` and queries registries through narrow
interfaces. It does not switch on module-owned names.

During migration, the existing `NativeRegistry` should become a world-owned
compatibility adapter. New descriptor registries can then grow beside it without
requiring every `Vm` instance to rebuild module tables independently.

### Split intrinsic VM types from registered runtime types

Replace the overloaded meaning of `RuntimeNativeTypeKind` with two concepts:

- VM intrinsic kinds for values and language primitives that the VM truly owns
  (`Str`, `Int`, `Float`, `Bool`, `Array`, `Map`, `Null`, `Object`, and related
  conversion machinery);
- registered runtime type references for stdlib and package types
  (`net.http.Client`, `ArgParser`, `crypto.blake3.Hasher`, etc.).

During migration, keep `RuntimeNativeTypeKind` as an adapter so existing code can
move incrementally. The end state removes module-specific enum cases from VM
control flow.

### Module descriptors

First-party stdlib modules and third-party native packages register through the
same descriptor shape:

- exported paths and aliases;
- type descriptors, including constructor/call behavior;
- static and instance dispatch entries;
- module-owned error classes with parent references;
- capability/effect requirements;
- optional native ABI hooks for thunk calls and foreign-handle lifecycle.

The descriptor API should cover current `NativeRegistry` use cases and the
legacy inline branches in `try_apply_native_stdlib_send`.

### Error registry and typed rescue

Keep the existing generated runtime error list as the bootstrap source for VM and
core errors while the registry is introduced.

Then make error lookup world-scoped:

- `RuntimeErrorClassId` identifies registered error classes, not just static
  indexes from `runtime_errors.def`;
- `ErrorInstanceValue` stores the resolved error class id;
- typed rescue uses `RuntimeWorld::errors().is_a(actual, matcher)`;
- module descriptors register errors such as
  `net.http.PoolTimeoutError <: net.http.HttpTimeoutError <: net.http.HttpError
  <: Exception`;
- frontend binding imports known error exports so `rescue SomePackage.Error`
  resolves like other exported names;
- `amber_fault` and VM-side `raise_runtime_error` resolve error names through the
  active world registry.

This lets third-party native code raise rescue-able package errors without
adding every package error class to `vm.cpp` or the global generated table.

## 4. Implementation Path

Phases are dependency order, not commit size. Implementation slices may be
smaller than a phase, but every slice must say which phase item it advances.

### Phase 0: Guardrails

Status: baseline recorded before Phase 2.

- Add/confirm focused tests around current behavior before moving code:
  registered stdlib dispatch, native prelude path lookup, constructor `CALL`,
  typed rescue inheritance, net.http `PoolTimeoutError`, and native extension
  fault translation.
- Run and record the relevant gates before and after each phase:
  `vm_tests`, `stdlib_registry_tests`, `vm_net_http_tests`,
  `amber_ext_tests`, `module_loader_tests`, `native_tests`,
  `make test`, `make backend-equivalence`, and the tagged `VALUE_REPR` build
  where value layout is touched.
- If a gate is intentionally skipped, record the reason in the phase commit or
  plan update. Do not start a later code phase with an unexplained missing
  guardrail.

Baseline record, 2026-06-26 on `codex/vm-runtime-world-registry`:

- Passed focused gates: `build/stdlib_registry_tests`, `build/vm_tests`,
  `build/amber_ext_tests`, `build/native_tests`, `build/module_loader_tests`,
  and elevated `build/vm_net_http_tests` (`566 checks`).
- Passed full gate: elevated `make test`, including unit binaries, native
  backend smoke checks, net.http TCP checks, and `ambertest run corpus`
  (`139 passed, 0 failed`).
- Passed backend gate: `make backend-equivalence` (`80 passed, 0 failed`).
- Passed conformance gate: elevated `make conformance`
  (`140 passed, 0 failed, 0 skipped for M11`). A sandboxed attempt failed first
  on loopback socket corpus cases with `PermissionDeniedError`, so future
  conformance runs that include socket cases should use elevated loopback
  access.
- Tagged `VALUE_REPR=tagged` build was not run for this baseline because the
  next planned phase is mechanical file split and should not touch `Value`
  layout. If Phase 2 touches `Value` storage or ABI, run a fresh tagged build in
  a separate build directory before continuing.

### Phase 1: Registry foundation and composition seams

Status: mostly landed. This phase intentionally comes before the broad file
split so that later file movement follows semantic ownership boundaries instead
of preserving VM-local module branches in smaller files.

- Move builtin `NativeRegistry` construction from `Vm` into
  `RuntimeWorld::Impl`; private VMs borrow world registries, while direct VM
  entry points keep local fallback registries.
- Introduce `RuntimeModuleRegistry`, `RuntimeTypeRegistry`,
  `RuntimeDispatchRegistry`, and `RuntimeErrorRegistry` under `RuntimeWorld`.
  The first versions are compatibility adapters over legacy native paths,
  native type calls, migrated native handlers, and builtin runtime error
  lookup.
- Teach `lookup_native_prelude_constant` to ask the module registry. This is
  landed for core prelude bindings, migrated stdlib paths, and the VM's former
  native-type path map; the `NativeRegistry::kind_for_path` fallback has been
  removed from the VM.
- Teach `CALL` on a registered type to use registry constructor/call metadata
  instead of the VM hardcoded allowlist. This is landed for legacy native-type
  call selectors.
- Teach migrated native SEND dispatch to use the dispatch registry instead of
  calling `NativeRegistry::handler_for` from the VM.
- Teach prelude error lookup, native error matching, and native error class
  selectors to use the error registry adapter. This is landed for id/name/is-a
  lookup; error field/default metadata still comes from the generated legacy
  table until module error descriptors land.
- Add the first descriptor shape for exported paths and native handlers:
  `RuntimeNativeModuleDescriptor`. Runtime module/dispatch wiring for migrated
  stdlib now uses descriptors instead of importing `NativeRegistry`
  path/handler tables.
- Keep `NativeRegistry` as a compatibility facade until all current stdlib
  modules have moved to the richer descriptor API and legacy tests no longer
  need it.

### Phase 2: Mechanical file split for editability

Status: in progress. The first mechanical slice starts with
`runtime/text.h` because text buffers/logger declarations already have a
separate `runtime/text.cpp` implementation and do not require moving VM
dispatch behavior.

Slice record, 2026-06-26:

- Split text writer/logger declarations from `runtime/vm.h` into
  `runtime/text.h`; `runtime/vm.h` remains the compatibility umbrella.
- Verified with focused build targets: `build/vm_tests`,
  `build/stdlib_registry_tests`, `build/amber_ext_tests`,
  `build/module_loader_tests`, `build/native_tests`,
  `build/stdlib_argparser_tests`, and `build/iamber_tests`.
- Verified focused binaries: `build/vm_tests`, `build/stdlib_registry_tests`,
  `build/amber_ext_tests`, `build/module_loader_tests`, `build/native_tests`,
  `build/stdlib_argparser_tests`, and `build/iamber_tests`.

Slice record, 2026-06-26:

- Split the `Value` public declaration surface from `runtime/vm.h` into
  `runtime/value.h`: `IntrusivePtr`, small scalar/native payload wrappers,
  `RuntimeNativeTypeKind`, BigInt/time/uuid payload structs, and both variant
  and tagged `Value` declarations.
- Left `Value` method implementations in `runtime/vm.cpp`; this slice is a
  declaration split and does not change `Value` storage or ABI.
- Verified with forced focused build targets: `make -B build/vm_tests
  build/stdlib_registry_tests build/amber_ext_tests build/module_loader_tests
  build/native_tests build/stdlib_argparser_tests build/stdlib_time_tests
  build/stdlib_uuid_tests`.
- Verified focused binaries: `build/vm_tests`, `build/stdlib_registry_tests`,
  `build/amber_ext_tests`, `build/module_loader_tests`, `build/native_tests`,
  `build/stdlib_argparser_tests`, `build/stdlib_time_tests`, and
  `build/stdlib_uuid_tests`.
- Verified tagged representation smoke in a separate build directory:
  `make -B BUILD_DIR=build-tagged VALUE_REPR=tagged build-tagged/vm_tests`
  and `build-tagged/vm_tests`.

Slice record, 2026-06-26:

- Split runtime watch/debug declarations from `runtime/vm.h` into
  `runtime/watch.h`; `runtime/vm.h` remains the compatibility umbrella.
- Left watch implementations in `runtime/vm.cpp`; this is still a declaration
  split, not a behavioral move.
- Verified with forced focused build targets:
  `make -B build/vm_tests build/module_loader_tests`.
- Verified focused binaries: `build/vm_tests` and `build/module_loader_tests`.

Slice record, 2026-06-26:

- Split heap object layout declarations from `runtime/vm.h` into
  `runtime/objects.h`: object ownership/lifetime metadata, `ObjHeader`,
  `ShapeDescriptor`, and `Instance/List/Tuple/Set/Map/Closure` payload structs.
- Left heap allocation, pinning, GC stats/results, and `RuntimeHeap` APIs in
  `runtime/vm.h`; those remain for a later `runtime/heap.h` slice.
- Verified with forced focused build targets: `make -B build/vm_tests
  build/stdlib_collections_tests build/stdlib_json_tests
  build/stdlib_task_tests build/native_tests build/amber_ext_tests`.
- Verified focused binaries: `build/vm_tests`, `build/stdlib_collections_tests`,
  `build/stdlib_json_tests`, elevated `build/stdlib_task_tests` for loopback
  socket coverage, `build/native_tests`, and `build/amber_ext_tests`.
- Verified tagged representation smoke in a separate build directory:
  `make -B BUILD_DIR=build-tagged VALUE_REPR=tagged build-tagged/vm_tests`
  and `build-tagged/vm_tests`.

Slice record, 2026-06-26:

- Split heap public declarations from `runtime/vm.h` into `runtime/heap.h`:
  GC stats/results, write-barrier results, pin/opaque/buffer/native-wait
  handles, `RuntimeHeap`, `RuntimePinScope`, default heap, and collection
  allocation helpers.
- Left heap implementation in `runtime/vm.cpp`; this remains a declaration
  split and does not move GC/pinning behavior.
- Verified with forced focused build targets: `make -B build/vm_tests
  build/stdlib_collections_tests build/stdlib_task_tests build/native_tests
  build/amber_ext_tests`.
- Verified focused binaries: `build/vm_tests`, `build/stdlib_collections_tests`,
  elevated `build/stdlib_task_tests` for loopback socket coverage,
  `build/native_tests`, and `build/amber_ext_tests`.
- Verified tagged representation smoke in a separate build directory:
  `make -B BUILD_DIR=build-tagged VALUE_REPR=tagged build-tagged/vm_tests`
  and `build-tagged/vm_tests`.

Slice record, 2026-06-26:

- Split concurrency/task/sync public declarations from `runtime/vm.h` into
  `runtime/concurrency.h`: IO wait observation records, scheduler/task handle
  APIs, awaitable/move-slot/channel/select, mutex/atomic/barrier, flow, threaded
  collections, shareability helper, and cooperative park observability hook.
- Left implementations in `runtime/vm.cpp`; this remains a declaration split
  and does not move scheduler/channel/flow behavior.
- Updated `runtime/context.h` to include `runtime/concurrency.h` and
  `runtime/text.h` directly instead of depending on the full `runtime/vm.h`
  umbrella.
- Verified with forced focused build targets: `make -B build/vm_tests
  build/stdlib_task_tests build/native_tests build/amber_ext_tests`.
- Verified focused binaries: `build/vm_tests`, elevated
  `build/stdlib_task_tests` for loopback socket coverage, `build/native_tests`,
  and `build/amber_ext_tests`. A sandboxed `build/stdlib_task_tests` attempt
  failed first at `cooperative socket read`, matching the known loopback
  sandbox limitation.
- Verified include smoke: standalone `runtime/concurrency.h` compile and
  `runtime/context.cpp` compile.

Slice record, 2026-06-26:

- Split `RuntimeWorld` public declarations from `runtime/vm.h` into
  `runtime/world.h`: world transactions, mirrors, capability/effect/replay
  aliases, IO provider options, `Fault`, `ExecutionLocal`, `ExecutionResult`,
  and the `RuntimeWorld` API.
- Left `RuntimeWorld` implementation in `runtime/vm.cpp`; this remains a
  declaration split and does not move world execution or registry ownership
  behavior.
- Updated `runtime/native_bridge.h` and `runtime/module_loader.h` to include
  `runtime/world.h` directly. Updated `tests/module_loader_tests.cpp` to include
  `runtime/vm.h` explicitly for `value_to_debug_string` after the production
  header stopped providing that helper transitively.
- Verified with standalone `runtime/world.h` compile smoke.
- Verified with forced focused build targets: `make -B build/vm_tests
  build/module_loader_tests build/native_tests build/frozen_image_tests
  build/amber_ext_tests`. The first attempt exposed the missing explicit
  `runtime/vm.h` include in `tests/module_loader_tests.cpp`; after fixing that,
  the listed targets rebuilt successfully.
- Verified focused binaries: `build/vm_tests`, `build/module_loader_tests`,
  `build/native_tests`, `build/frozen_image_tests`, and
  `build/amber_ext_tests`.

Slice record, 2026-06-26:

- Started the private `runtime/vm_internal.h` split by moving interpreter-only
  state declarations out of `runtime/vm.cpp`: prepared pattern state, lazy
  sequence state, quickened opcode/instruction caches, `PendingThrow`, frame
  register maps, `Frame`, method/class runtime tables, and `RuntimeState`.
- Left the `Vm` class body and implementations in `runtime/vm.cpp`; this slice
  only establishes the private internal header and avoids converting the large
  inline `Vm` method body set in one risky step.
- Verified include/compile smoke: standalone `runtime/vm_internal.h` compile
  and standalone `runtime/vm.cpp` compile.
- Verified with forced focused build targets: `make -B build/vm_tests
  build/module_loader_tests build/stdlib_task_tests build/native_tests
  build/amber_ext_tests`.
- Verified focused binaries: `build/vm_tests`, `build/module_loader_tests`,
  `build/native_tests`, and `build/amber_ext_tests`. A sandboxed
  `build/stdlib_task_tests` attempt failed first at `cooperative socket read`,
  matching the known loopback sandbox limitation; the elevated retry could not
  be run in this session because the approval request was rejected by the
  automatic reviewer.

Slice record, 2026-06-26:

- Moved watch implementation island from `runtime/vm.cpp` into
  `runtime/watch.cpp`: `RuntimeWatchCell`, `RuntimeWatchObjectState`, and
  `RuntimeWatchHandle` implementations now live beside `runtime/watch.h`.
- Kept VM-local helpers such as `watch_cell_from_value` in `runtime/vm.cpp`
  because they are tied to value unwrapping and dependency recording.
- Added `runtime/watch.cpp` to `RUNTIME_SRCS`.
- Verified compile smoke: standalone `runtime/watch.cpp` compile and
  standalone `runtime/vm.cpp` compile.
- Verified with forced focused build targets: `make -B build/vm_tests
  build/module_loader_tests`.
- Verified focused binaries: `build/vm_tests` and
  `build/module_loader_tests`.

Slice record, 2026-06-26:

- Moved the variant and tagged `Value` method bodies from `runtime/vm.cpp` into
  `runtime/value.cpp`, beside the `runtime/value.h` declaration split.
- Added `runtime/value.cpp` to `RUNTIME_SRCS` and `FORMAT_FILES`, and updated
  `runtime/value.h` comments to point at the new implementation file.
- Kept `MapEntry` constructors, default-heap collection helpers, and
  `make_result_value` in `runtime/vm.cpp`; those still belong to the later
  object/heap split rather than the pure `Value` representation slice.
- Verified compile smoke: standalone `runtime/value.cpp` compile, standalone
  `runtime/vm.cpp` compile, and standalone tagged `runtime/value.cpp` compile.
- Verified with forced focused build targets: `make -B build/vm_tests
  build/module_loader_tests build/native_tests build/amber_ext_tests`, `make -B
  build/stdlib_registry_tests build/stdlib_time_tests build/stdlib_uuid_tests
  build/stdlib_argparser_tests`, and `make -B BUILD_DIR=build-tagged
  VALUE_REPR=tagged build-tagged/vm_tests`.
- Verified focused binaries: `build/vm_tests`, `build/module_loader_tests`,
  `build/native_tests`, `build/amber_ext_tests`, `build/stdlib_registry_tests`,
  `build/stdlib_time_tests`, `build/stdlib_uuid_tests`,
  `build/stdlib_argparser_tests`, and `build-tagged/vm_tests`.

Slice record, 2026-06-26:

- Moved the low-coupling `MapEntry` constructors from `runtime/vm.cpp` into
  `runtime/objects.cpp`, beside the `runtime/objects.h` declarations.
- Added `runtime/objects.cpp` to `RUNTIME_SRCS` and `FORMAT_FILES`.
- Verified compile smoke: standalone `runtime/objects.cpp` compile, standalone
  tagged `runtime/objects.cpp` compile, and standalone `runtime/vm.cpp`
  compile.
- Verified with forced focused build targets: `make -B build/vm_tests
  build/stdlib_collections_tests build/module_loader_tests`.
- Verified focused binaries: `build/vm_tests`, `build/stdlib_collections_tests`,
  and `build/module_loader_tests`.

Slice record, 2026-06-26:

- Started `runtime/heap.cpp` with the low-coupling default heap facade:
  `default_runtime_heap`, `make_list_value`, `make_tuple_value`,
  `make_set_value`, and `make_symbol_map_value`.
- Kept `make_result_value` in `runtime/vm.cpp`; it depends on the VM-level
  `ResultValue` helper surface rather than the collection heap facade.
- Added `runtime/heap.cpp` to `RUNTIME_SRCS` and `FORMAT_FILES`.
- Verified compile smoke: standalone `runtime/heap.cpp` compile, standalone
  tagged `runtime/heap.cpp` compile, and standalone `runtime/vm.cpp` compile.
- Verified with forced focused build targets: `make -B build/vm_tests
  build/stdlib_collections_tests build/module_loader_tests`.
- Verified focused binaries: `build/vm_tests`, `build/stdlib_collections_tests`,
  and `build/module_loader_tests`.

Slice record, 2026-06-26:

- Moved shared object lifecycle/header helpers from `runtime/vm.cpp` into
  `runtime/objects.cpp`: heap-payload detection, `Value` to `ObjHeader`
  accessors, lifecycle predicates, access error messages, and debug labels.
- Declared those helpers in `runtime/objects.h` so later `runtime/heap.cpp` and
  `runtime/concurrency.cpp` slices do not need to depend on `vm.cpp` anonymous
  namespace helpers.
- Verified compile smoke: standalone `runtime/objects.cpp` compile, standalone
  tagged `runtime/objects.cpp` compile, and standalone `runtime/vm.cpp`
  compile.
- Verified with forced focused build targets: `make -B build/vm_tests
  build/stdlib_collections_tests build/native_tests build/module_loader_tests`
  and `make -B BUILD_DIR=build-tagged VALUE_REPR=tagged
  build-tagged/vm_tests`.
- Verified focused binaries: `build/vm_tests`, `build/stdlib_collections_tests`,
  `build/native_tests`, `build/module_loader_tests`, and
  `build-tagged/vm_tests`.

Slice record, 2026-06-26:

- Moved `RuntimePinScope` method bodies from `runtime/vm.cpp` into
  `runtime/heap.cpp`, beside the low-coupling default heap facade.
- Verified compile smoke: standalone `runtime/heap.cpp` compile, standalone
  tagged `runtime/heap.cpp` compile, and standalone `runtime/vm.cpp` compile.
- Verified with forced focused build targets: `make -B build/vm_tests
  build/native_tests build/module_loader_tests`.
- Verified focused binaries: `build/vm_tests`, `build/native_tests`, and
  `build/module_loader_tests`.

Slice record, 2026-06-26:

- Moved intrusive refcount implementation from `runtime/vm.cpp` into
  `runtime/heap.cpp`: `runtime_heap_add_ref`, `runtime_heap_release`,
  `make_intrusive`, the heap object deleter, and the six explicit
  instantiations for ObjHeader-bearing runtime objects.
- Updated `runtime/value.h` comments to point at `runtime/heap.cpp` for those
  out-of-line definitions.
- Kept `RuntimeHeap::drop_object` in `runtime/vm.cpp` for now because it still
  names private `RuntimeHeap::Impl`; this can move with the full heap
  implementation slice.
- Verified compile smoke: standalone `runtime/heap.cpp` compile, standalone
  tagged `runtime/heap.cpp` compile, standalone `runtime/vm.cpp` compile,
  standalone `runtime/value.cpp` compile, and standalone tagged
  `runtime/value.cpp` compile.
- Verified with forced focused build targets: `make -B build/vm_tests
  build/stdlib_collections_tests build/native_tests build/module_loader_tests`
  and `make -B BUILD_DIR=build-tagged VALUE_REPR=tagged
  build-tagged/vm_tests`.
- Verified focused binaries: `build/vm_tests`, `build/stdlib_collections_tests`,
  `build/native_tests`, `build/module_loader_tests`, and
  `build-tagged/vm_tests`.

Slice record, 2026-06-26:

- Moved the public BigInt decimal string helper from `runtime/vm.cpp` into
  `runtime/value.cpp`, with its declaration beside `BigIntValue` in
  `runtime/value.h`.
- Kept BigInt arithmetic/parsing helpers in `runtime/vm.cpp` for now because
  they are still tightly coupled to VM SEND/conversion branches; this slice only
  moves the low-coupling value display helper.
- Verified compile smoke: standalone `runtime/value.cpp`, standalone tagged
  `runtime/value.cpp`, and standalone `runtime/vm.cpp` compile.
- Verified with forced focused build targets: `make -B build/vm_tests
  build/native_tests build/module_loader_tests` and `make -B
  BUILD_DIR=build-tagged VALUE_REPR=tagged build-tagged/vm_tests`.
- Verified focused binaries: `build/vm_tests`, `build/native_tests`,
  `build/module_loader_tests`, and `build-tagged/vm_tests`.

Slice record, 2026-06-26:

- Moved `native_type_name` and `native_function_name` from the `runtime/vm.cpp`
  anonymous namespace into `runtime/value.cpp`, with declarations in
  `runtime/value.h` beside the corresponding runtime native value enums.
- Kept all native type enum cases and VM dispatch branches unchanged; these
  helpers are only the transitional display/name surface while module-owned
  type descriptors are still being migrated in later phases.
- Verified compile smoke: standalone `runtime/value.cpp`, standalone tagged
  `runtime/value.cpp`, and standalone `runtime/vm.cpp` compile.
- Verified with forced focused build targets: `make -B build/vm_tests
  build/stdlib_registry_tests build/module_loader_tests` and `make -B
  BUILD_DIR=build-tagged VALUE_REPR=tagged build-tagged/vm_tests`.
- Verified focused binaries: `build/vm_tests`, `build/stdlib_registry_tests`,
  `build/module_loader_tests`, and `build-tagged/vm_tests`.

Slice record, 2026-06-26:

- Split builtin runtime error metadata helpers from `runtime/vm.cpp` into
  `runtime/errors.h` / `runtime/errors.cpp`: generated error name/id/is-a
  lookup, inherited default messages/exit codes, and structured error field
  masks.
- Kept `RuntimeErrorRegistry` as the existing world/direct-VM adapter in
  `runtime/stdlib_registry.*`; this slice is only a mechanical home for the
  bootstrap generated error table, not the Phase 4 descriptor migration.
- Added `runtime/errors.cpp` to `RUNTIME_SRCS` and both error files to
  `FORMAT_FILES`. `runtime/vm.h` includes `runtime/errors.h` so existing
  umbrella includes keep working.
- Verified compile smoke: standalone `runtime/errors.cpp`, `runtime/vm.cpp`,
  `runtime/stdlib_registry.cpp`, and `runtime/stdlib_argparser.cpp` compile.
- Verified with forced focused build targets: `make -B build/vm_tests
  build/stdlib_registry_tests build/stdlib_argparser_tests
  build/amber_ext_tests`.
- Verified focused binaries: `build/vm_tests`, `build/stdlib_registry_tests`,
  `build/stdlib_argparser_tests`, and `build/amber_ext_tests`.
- Tagged `VALUE_REPR=tagged` was not rerun for this slice because it does not
  touch `Value` layout, object layout, or representation-specific code.

Slice record, 2026-06-27:

- Moved the value-based `ResultValue` payload struct and `make_result_value`
  helper from `runtime/vm.h` / `runtime/vm.cpp` into `runtime/value.h` /
  `runtime/value.cpp`, beside the `Value::result` tail-kind API.
- Kept VM `Ok`/`Err` call handling unchanged; this slice only moves the shared
  result payload/value construction helper used by both VM and stdlib code.
- Verified compile smoke: standalone `runtime/value.cpp`, standalone tagged
  `runtime/value.cpp`, standalone `runtime/vm.cpp`, and standalone
  `runtime/stdlib_argparser.cpp` compile.
- Verified with forced focused build targets: `make -B build/vm_tests
  build/stdlib_argparser_tests` and `make -B BUILD_DIR=build-tagged
  VALUE_REPR=tagged build-tagged/vm_tests`.
- Verified focused binaries: `build/vm_tests`, `build/stdlib_argparser_tests`,
  and `build-tagged/vm_tests`.

Slice record, 2026-06-27:

- Split fixed-width numeric profile helpers from `runtime/vm.h` /
  `runtime/vm.cpp` into `runtime/numeric.h` / `runtime/numeric.cpp`:
  `NumericPolicy`, profile lookup, floor division/modulo, bit operations, and
  checked/wrapping/saturating Int arithmetic helpers.
- Kept BigInt arithmetic/parsing in `runtime/vm.cpp` for now because it remains
  coupled to VM conversion and SEND branches; this slice only moves the
  VM-state-free fixed-width numeric helper layer.
- Added `runtime/numeric.cpp` to `RUNTIME_SRCS` and both numeric files to
  `FORMAT_FILES`. `runtime/vm.h` includes `runtime/numeric.h` so existing
  umbrella includes keep working.
- Verified compile smoke: standalone `runtime/numeric.cpp`, `runtime/vm.cpp`,
  `runtime/value.cpp`, and standalone tagged `runtime/value.cpp` compile.
- Verified with forced focused build targets: `make -B build/vm_tests
  build/native_tests build/stdlib_argparser_tests` and `make -B
  BUILD_DIR=build-tagged VALUE_REPR=tagged build-tagged/vm_tests`.
- Verified focused binaries: `build/vm_tests`, `build/native_tests`,
  `build/stdlib_argparser_tests`, and `build-tagged/vm_tests`.

Slice record, 2026-06-27:

- Split value display/debug declarations from `runtime/vm.h` into
  `runtime/value_display.h`: `RuntimeStringifyMode`,
  `RuntimePrettyPrintOptions`, and the public `value_to_debug_string` helper.
- Kept value display implementations in `runtime/vm.cpp` for this slice; the
  new header is the narrow declaration seam for a later implementation move.
- Added `runtime/value_display.h` to `FORMAT_FILES`. `runtime/vm.h` includes it
  so existing umbrella includes keep working.
- Verified compile smoke: standalone `runtime/value_display.h` header compile
  and standalone `runtime/vm.cpp` compile.
- Verified with forced focused build targets: `make -B build/vm_tests
  build/module_loader_tests`.
- Verified focused binaries: `build/vm_tests` and `build/module_loader_tests`.
- Tagged `VALUE_REPR=tagged` was not rerun for this declaration-only slice
  because it does not touch `Value` layout or representation-specific code.

Slice record, 2026-06-27:

- Prepared the value display implementation move by relocating shared payload
  and object helpers out of `runtime/vm.h` / `runtime/vm.cpp`: moved
  `RuntimeForeignHandle` and `ErrorInstanceValue` into `runtime/value.h`,
  moved native range detection (`kNativeSyntheticClassIndex`,
  `kNativeRangeMarker`, `instance_is_native_range`) into `runtime/objects.h` /
  `runtime/objects.cpp`, and moved uuid/time display helper declarations into
  `runtime/value_display.h`.
- Kept VM native descriptor and ArgParser surfaces in `runtime/vm.h`; this
  slice only removed the direct dependencies that would otherwise force
  `runtime/value_display.cpp` to include the full VM umbrella.
- Verified compile smoke: standalone `runtime/value_display.cpp`,
  `runtime/vm.cpp`, and `runtime/value.cpp` compile.
- Verified with forced focused build targets: `make -B build/vm_tests
  build/amber_ext_tests` and `make -B BUILD_DIR=build-tagged
  VALUE_REPR=tagged build-tagged/vm_tests`.
- Verified focused binaries: `build/vm_tests`, `build/amber_ext_tests`, and
  `build-tagged/vm_tests`.

Slice record, 2026-06-27:

- Moved value display/debug implementation from `runtime/vm.cpp` into
  `runtime/value_display.cpp`: private stringification helpers,
  `runtime_stringify_value`, and `value_to_debug_string` now live beside
  `runtime/value_display.h`.
- Added `runtime/value_display.cpp` to `RUNTIME_SRCS` and `FORMAT_FILES`.
  `runtime/vm.cpp` now calls the public `runtime_stringify_value` declaration
  from `runtime/value_display.h` instead of an anonymous-namespace helper.
- Verified compile smoke: standalone `runtime/value_display.cpp` and
  `runtime/vm.cpp` compile after formatting.
- Verified with forced focused build targets: `make -B build/vm_tests
  build/module_loader_tests` and `make -B BUILD_DIR=build-tagged
  VALUE_REPR=tagged build-tagged/vm_tests`.
- Verified focused binaries: `build/vm_tests`, `build/module_loader_tests`, and
  `build-tagged/vm_tests`.

Slice record, 2026-06-27:

- Moved shared value equality and collection-key normalization helpers from
  `runtime/vm.cpp` into `runtime/objects.cpp` / `runtime/objects.h`:
  `value_equals`, `CollectionKeyError`, map/set key normalization,
  name-indifferent map entry comparison, and normalized map-entry upsert.
- Kept VM-owned canonical map-key id construction in `runtime/vm.cpp`, because
  it depends on the active module's string/symbol interning tables. This slice
  removes the collection helper dependency that blocked a later
  `RuntimeHeap` implementation move.
- Verified compile smoke: standalone `runtime/objects.cpp` and
  `runtime/vm.cpp` compile after formatting.
- Verified with forced focused build targets: `make -B build/vm_tests
  build/stdlib_collections_tests build/module_loader_tests` and `make -B
  BUILD_DIR=build-tagged VALUE_REPR=tagged build-tagged/vm_tests`.
- Verified focused binaries: `build/vm_tests`, `build/stdlib_collections_tests`,
  `build/module_loader_tests`, and `build-tagged/vm_tests`.

Slice record, 2026-06-27:

- Moved `RuntimeHeap::Impl` and the public `RuntimeHeap` method bodies from
  `runtime/vm.cpp` into `runtime/heap.cpp`: allocation, cross-strand remote
  frees, write barriers, GC, pin/opaque-handle/value-buffer/native-wait APIs,
  stats, and the intrusive `drop_object` path.
- Kept the default heap facade and intrusive-refcount templates in
  `runtime/heap.cpp`, now beside the full heap implementation. `runtime/vm.cpp`
  still owns VM/interpreter heap call sites and world adapter forwarding.
- Verified compile smoke: standalone `runtime/heap.cpp`, standalone tagged
  `runtime/heap.cpp`, and standalone `runtime/vm.cpp` compile after formatting.
- Verified with forced focused build targets: `make -B build/vm_tests
  build/stdlib_collections_tests build/native_tests build/module_loader_tests`
  and `make -B BUILD_DIR=build-tagged VALUE_REPR=tagged
  build-tagged/vm_tests`.
- Verified focused binaries: `build/vm_tests`, `build/stdlib_collections_tests`,
  `build/native_tests`, `build/module_loader_tests`, and
  `build-tagged/vm_tests`.

Slice record, 2026-06-27:

- Moved concurrency/task/sync implementation islands from `runtime/vm.cpp` into
  `runtime/concurrency.cpp`: shareability/move-boundary helpers,
  `RuntimeAwaitable`, `RuntimeMoveSlot`, channels/select, mutexes, atomics,
  barriers, the scheduler, task handles/modules, flow, and threaded
  collections.
- Kept VM-specific cooperative park request plumbing in `runtime/vm.cpp`, but
  routed the shared task parked-state through small `runtime/concurrency.h`
  helpers so TaskModule state lives beside the scheduler implementation.
- Added `runtime/concurrency.cpp` to `RUNTIME_SRCS` and `FORMAT_FILES`.
- Verified compile smoke: standalone `runtime/concurrency.cpp`, standalone
  tagged `runtime/concurrency.cpp`, and standalone `runtime/vm.cpp` compile
  after formatting.
- Verified with forced focused build targets: `make -B build/vm_tests
  build/stdlib_task_tests build/native_tests build/amber_ext_tests
  build/module_loader_tests` and `make -B BUILD_DIR=build-tagged
  VALUE_REPR=tagged build-tagged/vm_tests`.
- Verified focused binaries: `build/vm_tests`, elevated
  `build/stdlib_task_tests` for loopback cooperative socket coverage,
  `build/native_tests`, `build/amber_ext_tests`, `build/module_loader_tests`,
  and `build-tagged/vm_tests`. A sandboxed `build/stdlib_task_tests` attempt
  failed first at `cooperative socket read`, matching the known loopback
  sandbox limitation.

Slice record, 2026-06-27:

- Started `runtime/world.cpp` with the low-coupling `RuntimeIoProvider` default
  method bodies from `runtime/vm.cpp`, beside the `runtime/world.h` declaration
  split.
- Kept `RuntimeWorld` and `execute_code` implementations in `runtime/vm.cpp`
  for now because `RuntimeWorld::execute` still constructs the private `Vm`
  implementation directly. The full world move needs a VM-internal seam first.
- Added `runtime/world.cpp` to `RUNTIME_SRCS` and `FORMAT_FILES`.
- Verified compile smoke: standalone `runtime/world.cpp` and standalone
  `runtime/vm.cpp` compile after formatting.
- Verified with forced focused build targets: `make -B build/vm_tests
  build/module_loader_tests build/frozen_image_tests`.
- Verified focused binaries: `build/vm_tests`, `build/module_loader_tests`,
  and `build/frozen_image_tests`.
- Tagged `VALUE_REPR=tagged` was not rerun for this slice because it only moves
  `RuntimeIoProvider` default methods and does not touch `Value` layout,
  object layout, or representation-specific code.

Slice record, 2026-06-27:

- Added a private VM execution seam in `runtime/vm_internal.h`:
  `RuntimeVmExecutionContext` and `execute_runtime_vm(...)`.
- Kept the `Vm` class body private to `runtime/vm.cpp`, but routed both
  `execute_code` and `RuntimeWorld::execute` through the new seam so a later
  `RuntimeWorld` implementation move no longer needs to construct `Vm`
  directly.
- Verified compile smoke: standalone `runtime/vm_internal.h` and standalone
  `runtime/vm.cpp` compile after formatting.
- Verified with forced focused build targets: `make -B build/vm_tests
  build/module_loader_tests build/frozen_image_tests` and `make -B
  BUILD_DIR=build-tagged VALUE_REPR=tagged build-tagged/vm_tests`.
- Verified focused binaries: `build/vm_tests`, `build/module_loader_tests`,
  `build/frozen_image_tests`, and `build-tagged/vm_tests`.

Slice record, 2026-06-27:

- Moved the remaining `RuntimeWorld` implementation from `runtime/vm.cpp` into
  `runtime/world.cpp`: package image decode/reload helpers, `RuntimeWorld::Impl`,
  execution, world mutation/freeze/reload, capability/effect/replay accessors,
  mirror generation, dispatch stats, and heap/GC/pinning facade methods.
- Kept direct `execute_code(...)` in `runtime/vm.cpp`; `RuntimeWorld::execute`
  now reaches the private `Vm` only through the `execute_runtime_vm(...)` seam
  from the previous slice.
- Added local `runtime/world.cpp` helper copies for code-object lookup and
  watch-cell root unwrapping so `world.cpp` does not depend on the interpreter
  anonymous namespace in `runtime/vm.cpp`.
- Verified compile smoke: standalone `runtime/world.cpp` and standalone
  `runtime/vm.cpp` compile after formatting; `git diff --check` is clean.
- Verified with forced focused build targets: `make -B build/vm_tests
  build/module_loader_tests build/frozen_image_tests`.
- Verified focused binaries: `build/vm_tests`, `build/module_loader_tests`,
  and `build/frozen_image_tests`.
- Verified conformance gate: elevated `make conformance`
  (`140 passed, 0 failed, 0 skipped for M11`).
- Tagged `VALUE_REPR=tagged` was not rerun for this slice because it is a
  translation-unit move of `RuntimeWorld` implementation and does not touch
  `Value` storage, object layout, or representation-specific code.

Slice record, 2026-06-27:

- Moved BigInt arithmetic/parsing helpers from `runtime/vm.cpp` into
  `runtime/numeric.cpp`, beside the existing numeric-profile fixed-width Int
  helpers.
- Added `runtime/numeric.h` declarations for the VM-facing BigInt helpers:
  Int64 conversion, signed compare/add/negate/multiply/division/modulo/power,
  and decimal parsing. Magnitude limb helpers remain file-local in
  `runtime/numeric.cpp`.
- Verified compile smoke: standalone `runtime/numeric.cpp`, standalone
  `runtime/vm.cpp`, standalone tagged `runtime/numeric.cpp`, and
  `git diff --check`.
- Verified with forced focused build target: `make -B build/vm_tests`.
- Verified focused binaries: `build/vm_tests` and `build-tagged/vm_tests`.
- Verified tagged representation smoke: `make -B BUILD_DIR=build-tagged
  VALUE_REPR=tagged build-tagged/vm_tests`.

Keep `runtime/vm.h` as a compatibility umbrella during the split, but move
declarations and low-coupling implementation islands into smaller files:

- `runtime/value.h` / `runtime/value.cpp`: `Value`, value helpers, intrinsic
  type naming;
- `runtime/value_display.h` / `runtime/value_display.cpp`: public value
  display/debug declarations and implementations;
- `runtime/errors.h` / `runtime/errors.cpp`: generated builtin runtime error
  metadata and field/default lookup helpers;
- `runtime/numeric.h` / `runtime/numeric.cpp`: numeric profile lookup and
  fixed-width Int arithmetic helpers;
- `runtime/objects.h`: heap object structs and runtime object handles;
- `runtime/heap.h` / `runtime/heap.cpp`: heap allocation, string table, pinning,
  and lifecycle helpers;
- `runtime/watch.h` / `runtime/watch.cpp`: runtime watch/debug state;
- `runtime/concurrency.h` / `runtime/concurrency.cpp`: task/channel/mutex/atomic
  runtime objects;
- `runtime/text.h` / `runtime/text.cpp`: text buffers, logger, text writer
  declarations already implemented outside `vm.cpp`;
- `runtime/world.h` / `runtime/world.cpp`: `RuntimeWorld` public surface and
  registry ownership;
- private `runtime/vm_internal.h`: the `Vm` class and interpreter-only helpers.

Do not move module-specific SEND/constructor branches unchanged just to shrink
`vm.cpp`. Move a module branch only when the move is part of descriptor
registration or when the branch is already behind a registry interface.

### Phase 3: Move first-party modules behind descriptors

Migrate modules in slices, each with tests:

- modules already using `NativeRegistry`: Math, Json, codecs, Digest,
  SecureRandom, ArgParser, Uuid, Time, Url. This slice is landed for path and
  handler descriptors; richer type/constructor/error descriptors remain future
  work where applicable;
- IO/data types currently in `vm.cpp`: Bytes, ByteBuffer, ByteSlice, IoPipe,
  TextBuffer, Logger;
- filesystem/network modules: fs, net, tcp, udp, net.http;
- task/sync modules: Task, Channel, Mutex, Atomic, Barrier, Flow,
  ThreadedCollection.

Acceptance criterion for each migrated slice: no path lookup, constructor
allowlist, or selector dispatch for that module remains in `Vm` except through
the registry interface.

Slice record, 2026-06-27:

- Extended `RuntimeNativeModuleDescriptor` with descriptor-owned type-call
  metadata and registration through `RuntimeTypeRegistry`, beside the existing
  path and dispatch-handler descriptor registration.
- Updated builtin runtime module registration to pass `RuntimeTypeRegistry`
  through each descriptor registration function for both `RuntimeWorld` and
  direct-VM fallback registries.
- Migrated `ArgParser.new` type-call metadata out of
  `register_legacy_native_type_calls` and into the `ArgParser` module
  descriptor. Other constructor/call metadata remains in the legacy list until
  the owning module descriptors move in later Phase 3 slices.
- Verified compile smoke: standalone `runtime/stdlib_registry.cpp`,
  `runtime/stdlib_argparser.cpp`, `runtime/world.cpp`, and `runtime/vm.cpp`
  compile after formatting; `git diff --check` is clean.
- Verified with forced focused build targets: `make -B
  build/stdlib_registry_tests build/vm_tests build/stdlib_argparser_tests`.
- Verified focused binaries: `build/stdlib_registry_tests`, `build/vm_tests`,
  and `build/stdlib_argparser_tests`.

Slice record, 2026-06-27:

- Added descriptor-only `runtime/stdlib_io.cpp` for IO/data native type module
  metadata.
- Moved IO/data path metadata for `io`, `io.Buffer`, `io.Logger`, `Bytes`,
  `io.ByteBuffer`, `io.ByteSlice`, and `io.Pipe` out of
  `register_legacy_native_type_paths` and into the IO descriptor.
- Moved constructor/call metadata for `Bytes.new`, `io.ByteBuffer.new`, and
  `io.Pipe()` out of `register_legacy_native_type_calls` and into the IO
  descriptor. Large VM SEND/constructor bodies remain in `runtime/vm.cpp` for a
  later selector-dispatch descriptor slice.
- Added `runtime/stdlib_io.cpp` to `STDLIB_SRCS` and `FORMAT_FILES`.
- Verified compile smoke: standalone `runtime/stdlib_io.cpp`,
  standalone `runtime/stdlib_registry.cpp`, and `git diff --check`.
- Verified with forced focused build targets: `make -B
  build/stdlib_registry_tests build/vm_tests build/stdlib_collections_tests`.
- Verified focused binaries: `build/stdlib_registry_tests`, `build/vm_tests`,
  and `build/stdlib_collections_tests`.

Slice record, 2026-06-27:

- Added descriptor-only `runtime/stdlib_fs.cpp` for filesystem native type
  module metadata.
- Moved filesystem path metadata for `fs`, `fs.Path`, and `fs.File` out of
  `register_legacy_native_type_paths` and into the filesystem descriptor.
- Moved constructor metadata for `fs.Path.new` out of
  `register_legacy_native_type_calls` and into the filesystem descriptor. Large
  VM filesystem selector bodies remain in `runtime/vm.cpp` for a later
  selector-dispatch descriptor slice.
- Added `runtime/stdlib_fs.cpp` to `STDLIB_SRCS` and `FORMAT_FILES`.
- Verified compile smoke: standalone `runtime/stdlib_fs.cpp`, standalone
  `runtime/stdlib_registry.cpp`, and `git diff --check`.
- Verified with forced focused build targets: `make -B
  build/stdlib_registry_tests build/vm_tests`.
- Verified focused binaries: `build/stdlib_registry_tests` and
  `build/vm_tests`.
- Verified conformance gate: elevated `make conformance`
  (`140 passed, 0 failed, 0 skipped for M11`).

Slice record, 2026-06-27:

- Added descriptor-only `runtime/stdlib_net.cpp` for core network native type
  module metadata.
- Moved network path metadata for `net`, `net.Endpoint`, `net.tcp`, and
  `net.udp` out of `register_legacy_native_type_paths` and into the network
  descriptor.
- Moved constructor metadata for `net.Endpoint.new` out of
  `register_legacy_native_type_calls` and into the network descriptor. The
  `net.http*` metadata remains legacy for a dedicated net.http descriptor
  slice.
- Added `runtime/stdlib_net.cpp` to `STDLIB_SRCS` and `FORMAT_FILES`.
- Verified compile smoke: standalone `runtime/stdlib_net.cpp`, standalone
  `runtime/stdlib_registry.cpp`, and `git diff --check`.
- Verified with forced focused build targets: `make -B
  build/stdlib_registry_tests build/vm_tests build/vm_net_http_tests`.
- Verified focused binaries: `build/stdlib_registry_tests`, `build/vm_tests`,
  and elevated `build/vm_net_http_tests` (`566 checks`).
- Verified conformance gate: elevated `make conformance`
  (`140 passed, 0 failed, 0 skipped for M11`).

Slice record, 2026-06-27:

- Added descriptor-only `runtime/stdlib_net_http.cpp` for `net.http*` native
  type module metadata.
- Moved `net.http`, client/request/body/headers/server/server request/server
  response, json helper, and form helper path metadata out of
  `register_legacy_native_type_paths` and into the net.http descriptor.
- Moved constructor/call metadata for net.http client/request/body/headers/
  server/server response/json helpers/form body out of
  `register_legacy_native_type_calls` and into the net.http descriptor. Large VM
  net.http selector bodies and module error handling remain in `runtime/vm.cpp`
  for later selector-dispatch and error-descriptor slices.
- Added `runtime/stdlib_net_http.cpp` to `STDLIB_SRCS` and `FORMAT_FILES`.
- Verified compile smoke: standalone `runtime/stdlib_net_http.cpp`,
  standalone `runtime/stdlib_registry.cpp`, and `git diff --check`.
- Verified with forced focused build targets: `make -B
  build/stdlib_registry_tests build/vm_tests build/vm_net_http_tests`.
- Verified focused binaries: `build/stdlib_registry_tests`, `build/vm_tests`,
  and elevated `build/vm_net_http_tests` (`566 checks`).
- Verified conformance gate: elevated `make conformance`
  (`140 passed, 0 failed, 0 skipped for M11`).

Slice record, 2026-06-27:

- Added descriptor-only `runtime/stdlib_task.cpp` for task/sync native type
  module metadata.
- Moved task/sync path metadata for `Flow`, `task.flow.Flow`, `Channel`,
  `sync.Channel`, `Mutex`, `sync.Mutex`, `Atomic`, `sync.Atomic`, `Barrier`,
  `sync.Barrier`, `ThreadedCollection`, and `task.flow.ThreadedCollection`
  out of `register_legacy_native_type_paths` and into the task/sync
  descriptor.
- Added `runtime/stdlib_task.cpp` to `STDLIB_SRCS` and `FORMAT_FILES`.
- Large task/sync behavior remains in `runtime/concurrency.cpp` and the VM
  selector/type-call paths for later dispatch descriptor slices.
- Verified compile smoke: standalone `runtime/stdlib_task.cpp`, standalone
  `runtime/stdlib_registry.cpp`, and `git diff --check`.
- Verified with forced focused build targets: `make -B
  build/stdlib_registry_tests build/vm_tests build/stdlib_task_tests`.
- Verified focused binaries: `build/stdlib_registry_tests`, `build/vm_tests`,
  and elevated `build/stdlib_task_tests`.
- Verified conformance gate: elevated `make conformance`
  (`140 passed, 0 failed, 0 skipped for M11`).

### Phase 4: Move errors behind descriptors

- Add error descriptors to core module registration.
- Register net.http errors from the net.http module descriptor, including
  `PoolTimeoutError`.
- Make typed rescue use the world-scoped error registry for both core and module
  errors.
- Keep `runtime_errors.def` for VM/kernel bootstrap errors and for generated
  frontend knowledge until package/module error exports are loaded through the
  compiler/module loader.
- Add a native package test where a thunk raises a package-defined error and
  Amber catches it by exact class and by parent class.

### Phase 5: Unify third-party native packages

- Extend generated native package registration so it contributes a module
  descriptor, not only `NativeExtRegistry` thunks and `NativeTagRegistry` types.
- Register package-owned foreign-handle types, constructors, methods, ownership
  rules, and errors through the same world registries used by first-party
  modules.
- Route `amber_fault(ctx, "...", ...)` through the active world's error registry.
- Preserve current native package semantics: pure Amber fallback in bytecode,
  native thunk in native builds, native-only leaves fail closed without native
  support, and foreign-handle lifetime rules remain deterministic.

### Phase 6: Remove legacy coupling

- Retire module-specific `RuntimeNativeTypeKind` enum values once all callers use
  runtime type references.
- Delete the VM hardcoded module path map and constructor allowlist.
- Reduce `try_apply_native_stdlib_send` to registry dispatch plus true VM
  intrinsic behavior.
- Keep `vm.cpp` focused on interpreter control flow and VM-owned primitives.

## 5. Public Interface Changes

The following new or revised interfaces should be treated as the stable refactor
surface:

- `RuntimeModuleDescriptor`: exported paths, type descriptors, dispatch entries,
  errors, capability/effect metadata, and optional native hooks.
- `RuntimeTypeRef` / `RuntimeBindingRef`: opaque references returned by the
  world registries for registered types and path exports.
- `RuntimeErrorClassId` and `RuntimeErrorRegistry`: dynamic error identity and
  inheritance checks for typed rescue.
- `StdlibHost`: remains the narrow VM facade for stdlib/native ABI helpers, but
  should lose dependencies on module-specific enum cases over time.
- `NativeExtRegistry`: becomes a contributor to `RuntimeWorld` registration
  rather than a parallel global namespace.

No Amber source-level behavior should change as part of this refactor.

## 6. Test Plan

- Unit tests for module registry path lookup, alias lookup, descriptor dispatch,
  constructor/call routing, and legacy enum-backed compatibility descriptors
  during migration.
- Unit tests for error registry inheritance: exact class, parent class,
  `Exception`, unknown error names, and duplicate registration diagnostics.
- net.http tests: active-slot pool timeout raises `PoolTimeoutError`; rescue by
  `PoolTimeoutError`, `HttpTimeoutError`, `HttpError`, and `Exception` all work.
- Native extension tests: a generated native package registers a type, method,
  constructor, and package error; its thunk raises that error; Amber catches it
  by exact class and parent class.
- Regression gates: `make test`, `make conformance`, `make backend-equivalence`,
  plus tagged value representation coverage for phases that touch `Value` or
  runtime object layout.

## 7. Assumptions and Defaults

- Prefer incremental adapters over a big-bang rewrite. The current enum and
  generated error table can remain as compatibility layers until each module is
  migrated.
- First-party stdlib modules should use the same registration machinery as
  third-party modules. Core status may affect trust and distribution, but not VM
  dispatch shape.
- The VM may keep intrinsic knowledge for language primitives and representation
  details. It should not keep semantic knowledge of optional libraries.
- Package/module descriptors are the source of truth for module-owned errors.
  The generated `runtime_errors.def` file remains the source of truth only for
  VM/kernel bootstrap errors during the transition.
