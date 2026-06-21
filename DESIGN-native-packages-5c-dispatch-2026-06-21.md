# Native Packages 5c-ii: ABI Implementation + Dispatch Routing

Status: focused-session handoff, 2026-06-21. Branch: `native-packages`.
Parent design: `DESIGN-native-extension-packages-2026-06-20.md`.

This is the last and deepest piece of native packages: wiring a runtime call from
Amber through `amber_ext.h` to a linked C thunk. Everything else is committed and
verified (see "Already landed"). Read this cold; it carries the two architectural
decisions and the integration points.

## Already landed (branch `native-packages`, 7 commits)

- Surface + enforcement: `native def` / `native class … from "binding"` with
  `owned`/`borrowed`/`collected` markers; `E_NATIVE_*` diagnostics; accelerated
  `native def` runs its Amber body as fallback; native-only leaf raises
  `NativeRequiredError` in bytecode.
- Manifest: `[[native]]` / `[native.symbols]` / `[[native.types]]` in both the
  package manifest (`package/package.{h,cpp}`) and the build manifest
  (`buildsys/build.{h,cpp}`, reusing `amber::pkg::PackageNativeExtension`), with
  `ffi` gating.
- ABI contract: `runtime/amber_ext.h` (opaque `AmberCtx`/`AmberValue`,
  predicates/readers incl. `amber_bytes_view`, builders incl.
  `amber_make_handle`, `amber_handle_ptr`, `amber_fault`, `amber_call_block`,
  thunk/destructor/reclaim typedefs, `amber_ext_abi_version`). Compiles C99+C++17.
- Lifetime: `RuntimeForeignHandle` (`runtime/vm.h`) — `owned`/`borrowed`/
  `collected`, deterministic `destroy()`, GC reclaim only for `collected`, never
  `owned`. Wired as a `Value` tail kind on both reps (variant + tagged).
- Tag registry: `NativeTypeDescriptor` + `NativeTagRegistry` (`runtime/vm.h`).
- Build: `build_native_executable` compiles/links declared native sources
  (`-I`/`-D`/cxxflags-guarded/`-l`) and runs a `nm` symbol-presence check.
  Verified end to end on `/tmp/natpkg` (a C source links in and the binary runs).

## The goal of 5c-ii

When a native build invokes a `native def`/`native class` binding, call its C
thunk (the physical `[native.symbols]` symbol) instead of running the Amber
fallback body, marshalling arguments and results across the ABI; and build
foreign-handle instances whose lifetime obeys the manifest ownership.

## Decision 1 — AmberValue ↔ runtime Value; dispatch in the VM lane

There are three value types: runtime `Value` (variant/tagged), the native
backend's emitted-lane `NativeValue`, and the ABI `AmberValue`. **Route all
native-extension calls through the VM lane on runtime `Value`, not the
emitted `NativeValue` lane.**

- `AmberValue` is an opaque handle into a per-call arena of runtime `Value`s
  owned by `AmberCtx`. Builders push a `Value` and return its handle; readers
  read the `Value`.
- `AmberCtx` wraps the active VM frame + the `StdlibHost` facade
  (`Vm : public StdlibHost`, `runtime/vm.cpp:9857`) + the `NativeTagRegistry`.
  The ABI functions are thin shims over `StdlibHost`
  (`runtime/stdlib_registry.h`): `amber_make_str` →
  `stdlib_string_value_from_text`, `amber_make_bytes` →
  `stdlib_bytes_value_from_bytes`, `amber_bytes_view` → a new narrow
  zero-copy accessor (StdlibHost currently only has copying `stdlib_bytes_of`;
  add a borrowed-view method backed by `RuntimePinViewKind::ValueBuffer`),
  `amber_fault` → `stdlib_set_fault`, `amber_call_block` → `stdlib_call_block`.
- The emitted native lane, when it reaches a `native def`/method SEND, bridges to
  the VM (the per-function VM bridge already converts `NativeValue` ↔ `Value` at
  that boundary) and the VM-side SEND dispatches to the thunk. No foreign-handle
  support is needed in `NativeValue`.

Rationale: this is the design's intended "amber_ext.h is the C face of
StdlibHost," reuses the mature facade, keeps `NativeValue` untouched, and the
native→VM bridge cost is acceptable because an extension call is already a
foreign-call boundary, not hot Amber code. Effectful native-only leaves remain
sound: they are direct thunk calls at the bridge boundary, with no
bail-and-restart after the effect.

## Decision 2 — `destroy(ctx)`; teardown takes the ctx

The `owned` destructor needs the `destroy!`-time ctx (not construction-time).
Change `RuntimeForeignHandle`:

- `bool destroy(void *ctx = nullptr);` (was no-arg)
- `std::function<void(void *ctx, void *ptr)> teardown;` (was `void(void*)`)
- deterministic `destroy(ctx)` calls `teardown(ctx, ptr)` for owning handles;
- the GC reclaim path (`~RuntimeForeignHandle`) calls `teardown(nullptr, ptr)` —
  only for `collected`, whose reclaim is context-free and ignores ctx.

Update `test_foreign_handle_lifetime` accordingly. `amber_make_handle` builds the
teardown closure from the `NativeTypeDescriptor`: owned → `[d](void* c, void* p){
d(c, p); }`; collected → `[r](void*, void* p){ r(p); }`; borrowed → empty.

## Implementation steps

1. `runtime/amber_ext.cpp` (new TU; add to Makefile `RUNTIME_SRCS` and the test
   src lists). Define `struct AmberCtx { void *frame; StdlibHost *host;
   NativeTagRegistry *tags; std::vector<Value> arena; };`, `AmberValueObj` as an
   arena index, and implement every `amber_ext.h` function over `StdlibHost` +
   `Value` factories + the tag registry.
2. Add the StdlibHost zero-copy bytes-view method (Decision 1) and implement it in
   `Vm`.
3. `RuntimeForeignHandle::destroy(ctx)` + teardown-signature change (Decision 2).
4. Dispatch: in the VM SEND path, detect a native-bound selector (free function
   or handle method), build an `AmberCtx`, marshal args `Value`→`AmberValue`,
   call the thunk fn pointer, marshal `out`→`Value`, translate `AMBER_ERR` into
   the recorded fault. The thunk fn pointers + `NativeTypeDescriptor`s come from a
   generated registration table the native binary populates at startup — the
   emitter (`build_native_cpp_plan`, `tools/amberc/main.cpp:2984`) emits it from
   the build manifest's `native_extensions` (thread them into the plan; the
   `run_build_command` native call at `main.cpp:6839` already has them).
5. `amber_make_handle` → look up tag in the registry, build a
   `RuntimeForeignHandle` with the right ownership + teardown, wrap as
   `Value::foreign_handle`.
6. Route `destroy!` / `memory.dealloc` on a foreign-handle `Value` to
   `RuntimeForeignHandle::destroy(ctx)` and the tombstone state.

## Test plan

- Unit (`vm_tests` or a new `amber_ext_tests`): build an `AmberCtx` over a test
  `Vm`, round-trip int/float/bool/bytes/str/handle through builders+readers,
  `amber_handle_ptr` tombstone check, `amber_fault` maps to the right class.
- End-to-end native build: a package whose `native def hash` Amber body returns
  one value and whose C thunk returns a *different* value; assert the native
  binary uses the thunk while the bytecode build uses the body (proves routing).
- `native class` round-trip in a native build: `new → update! → finalize →
  destroy!`; use-after-`destroy!` raises `LifetimeError`; an owned handle dropped
  without `destroy!` trips the leak backstop, not the destructor.
- `make backend-equivalence` for the accelerated def (Amber body vs. thunk must
  match on the package corpus).
- `make test` green on `VALUE_REPR=variant` and `VALUE_REPR=tagged`.

## Integration points (file:line)

- `tools/amberc/main.cpp`: `build_native_cpp_plan` (2984), `native_cpp_function_name`
  (1094), `build_native_executable` (~6020, now extension-aware),
  `run_build_command` native call (~6839).
- `runtime/vm.cpp`: `Vm : public StdlibHost` (9857), the SEND dispatch, `destroy!`
  / `memory.dealloc` handling.
- `runtime/stdlib_registry.h`: the `StdlibHost` facade to shim over.
- `runtime/vm.h`: `RuntimeForeignHandle`, `NativeTagRegistry`, `Value::foreign_handle`.

## Deferred to 5d (separate again)

Whole-graph native (drop the root-only restriction): `run_build_command` builds
native only for the root module; merge the linked module graph into one
`BcModule` for the emitter. Independent of 5c-ii.
