# amber.full-native.implementation-plan.v1

Status: planning document for replacing the current bytecode-trampoline native
readiness layer with a real host-native execution path.

This plan assumes the current repository state:

- the bytecode VM is the semantic oracle;
- `amber.native.v1` already records native-readiness metadata, but its
  `machine_code_blob` is a trampoline descriptor;
- `runtime/native_bridge` validates frozen-world assumptions and then re-enters
  bytecode;
- `amberc build <amber.build.json>` emits `.amberbc` artifacts;
- `amberc build <file.am>` emits an executable shell wrapper with embedded
  bytecode.

The implementation should be incremental and testable. Every phase below must
preserve bytecode execution and conformance while adding native capability
behind explicit build/runtime switches until the native path is proven.

## Target Definition

Full native means:

- `amberc build` can produce a real host executable or native package artifact;
- eligible Amber code executes through host machine code, not through
  `RuntimeWorld::execute`;
- unsupported or reflective operations have explicit runtime helper slowpaths;
- heap values remain visible to GC through native root maps;
- language errors, exceptions, stack traces, and cancellation semantics remain
  Amber-level semantics, not host crashes;
- bytecode artifacts remain available as the canonical verification and fallback
  format during the transition;
- benchmarkable integer-heavy code, including `bench/polyglot`, no longer pays
  per-operation `SEND` VM dispatch overhead.

The first production backend should be AOT generated C++ compiled by the
configured `CXX`. This gives real native code without adding LLVM or a custom
object-file writer as an immediate dependency. A later direct object/JIT backend
can reuse the same MIR, eligibility, ABI, runtime helper, and metadata contracts.

## Non-Goals For The First Native Milestone

- No direct machine-code emitter in the first milestone.
- No speculative JIT patching before an AOT baseline is correct.
- No removal of `.amberbc` build outputs.
- No semantic divergence from bytecode for dynamic dispatch, pattern protocol,
  object lifetime, tasks, or exceptions.
- No silent native fallback. Every fallback must be represented in the native
  metadata and surfaced in dumps/tests.

## Guiding Invariants

- Bytecode remains the source-of-truth verifier until native verification is
  equally complete.
- Native code must never own runtime semantics that the VM cannot reproduce.
- Every native call boundary is a GC safepoint unless explicitly proven safe.
- Every native frame must have enough metadata for roots, source spans, and
  exception unwinding.
- Native eligibility must be conservative. Incorrect native execution is worse
  than bytecode fallback.
- Build output must be deterministic for identical sources, profiles, stdlib
  ABI hashes, and compiler options.

## Existing Assets To Reuse

- MIR schema and validator: `optimizer/mir.h`, `optimizer/mir.cpp`.
- Native metadata schema and validator: `optimizer/native.h`,
  `optimizer/native.cpp`.
- Runtime native assumption bridge: `runtime/native_bridge.h`,
  `runtime/native_bridge.cpp`.
- Runtime world, heap, values, dispatch, exceptions, scheduler, and GC:
  `runtime/vm.h`, `runtime/vm.cpp`.
- Build manifest and cache discipline: `buildsys/build.h`, `buildsys/build.cpp`,
  `tools/amberc/main.cpp`.
- Frozen image native metadata embedding: `frozen/image.cpp`,
  `runtime/frozen_image.cpp`.
- Current native tests: `tests/native_tests.cpp`,
  `tests/frozen_image_tests.cpp`.
- Polyglot performance target: `bench/polyglot`.

## Phase 0: Baseline Audit And Gates

Goal: establish the exact native-readiness baseline and lock the regression
gates before adding codegen.

Tasks:

- Add a native implementation status section to this plan after each completed
  phase.
- Record current polyglot benchmark numbers in `bench/polyglot/README.md` or a
  dedicated benchmark results file.
- Add `amberc native-dump bench/polyglot/amber/src/main.am` as an expected
  diagnostic reference or smoke test.
- Ensure these commands pass before native work starts:
  - `make test`
  - `make conformance`
  - `make spec-sync-check`
  - `python3 bench/polyglot/run_benchmark.py --repeats 3`

Exit criteria:

- Native work starts from a known green baseline.
- The benchmark target and correctness checksum are pinned.

## Phase 1: Native Build Surface

Goal: introduce explicit native build modes without changing existing build
behavior.

Tasks:

- Extend source build CLI:
  - `amberc build <file.am> --target bytecode-wrapper` (default, existing)
  - `amberc build <file.am> --target native`
  - `amberc build <file.am> --target native-debug`
- Extend manifest build CLI:
  - `amberc build <amber.build.json> --target bytecode` (default, existing)
  - `amberc build <amber.build.json> --target native`
  - `amberc build <amber.build.json> --target both`
- Extend `BuildArtifactRecord` with:
  - `artifact_kind`
  - `native_output_path`
  - `native_cache_path`
  - `native_hash`
  - `native_backend`
  - `native_eligible`
  - `native_fallback_reason`
- Keep `.amberbc` output even in native mode.
- Add stable cache key material for native backend:
  - source hash
  - bytecode artifact hash
  - MIR/native backend version
  - compiler command and selected flags
  - target triple/host platform string
  - profile set
  - stdlib ABI material
- Add JSON summary fields without breaking existing consumers.

Files:

- `buildsys/build.h`
- `buildsys/build.cpp`
- `tools/amberc/main.cpp`
- `docs/engineering/build-v1.md`
- `tests/build_tests.cpp`

Exit criteria:

- Existing build commands are byte-for-byte compatible where expected.
- `--target native` can report "not implemented" or produce a placeholder
  native artifact through a controlled code path.
- Build JSON exposes native intent and cache fields.

## Phase 2: Native Artifact Model

Goal: represent generated native code as a first-class artifact while keeping
bytecode verification available.

Recommended artifact layout:

- `.amberbc`: existing verified bytecode module.
- `.amber.native.json`: deterministic native metadata and eligibility report.
- generated source directory under cache:
  - `<module>.native.cpp`
  - `<module>.native.h`
  - optional `<module>.native.map.json`
- executable or shared object:
  - source build: executable path requested by `-o` or default stem.
  - manifest build: `<out-dir>/<root-module>` for executable roots or
    `<out-dir>/<module>.amber.so` for module libraries if library mode is
    introduced.

Tasks:

- Split `machine_code_blob` into a real payload description:
  - keep existing string for JSON compatibility;
  - add a structured backend field if needed;
  - mark generated C++ backend as `bytecode_trampoline=false` only when a host
    entry is callable.
- Add native artifact verification that checks:
  - source bytecode hash matches;
  - native metadata source code ids match bytecode;
  - object/executable exists and hash matches summary;
  - unsupported functions have explicit fallback metadata.
- Add native dump output that distinguishes:
  - trampoline
  - generated-cpp
  - direct-object
  - fallback-only

Files:

- `optimizer/native.h`
- `optimizer/native.cpp`
- `tools/amberc/main.cpp`
- `docs/engineering/native-v1.md`
- `tests/native_tests.cpp`

Exit criteria:

- Native metadata can describe a non-trampoline backend.
- Native verification can reject missing or stale generated artifacts.

## Phase 3: Runtime Native ABI

Goal: define the stable C ABI between generated host code and Amber runtime.

Core ABI:

```cpp
extern "C" amber_native_result amber_native_entry(
    amber_runtime_world *world,
    amber_native_frame *frame,
    const amber_runtime_value *args,
    uint32_t arg_count);
```

The exact C++ types can be internal wrappers, but generated code must depend on
a narrow public ABI rather than the full `Vm` class.

Required ABI concepts:

- opaque world handle;
- opaque value handle or ABI-stable `Value` wrapper;
- native frame descriptor with:
  - code id/native id;
  - return pc/source span;
  - local root slots;
  - temporary root slots;
  - caller frame link;
- result union:
  - ok value;
  - Amber fault;
  - request bytecode fallback;
  - cancellation;
- helper calls for:
  - value construction and type tests;
  - integer arithmetic checked/unchecked as policy requires;
  - truthiness;
  - send/call slowpath;
  - local/capture/watch read/write if needed;
  - allocation;
  - safepoint polling;
  - exception raise/unwind;
  - trace frame registration.

Tasks:

- Add `runtime/native_abi.h` and `runtime/native_abi.cpp`.
- Keep generated C++ includes limited to `runtime/native_abi.h`.
- Add adapters from ABI values to current `runtime::Value`.
- Add native frame push/pop APIs to `RuntimeWorld`.
- Add root enumeration for active native frames into GC root collection.
- Add native trace frame rendering to `Fault.trace_text`.
- Add tests that trigger GC from a native safepoint while native locals hold
  heap objects.

Files:

- `runtime/native_abi.h`
- `runtime/native_abi.cpp`
- `runtime/vm.h`
- `runtime/vm.cpp`
- `runtime/native_bridge.h`
- `runtime/native_bridge.cpp`
- `tests/native_tests.cpp`
- `tests/vm_tests.cpp`

Exit criteria:

- A hand-written native test function can be called through the ABI.
- GC sees native frame roots.
- Amber faults raised from native helpers preserve trace information.

## Phase 4: Native Eligibility Analysis

Goal: decide which MIR functions can be fully native, which require slowpaths,
and which must remain bytecode fallback.

Tasks:

- Add `optimizer/native_eligibility.h/.cpp`.
- Classify every MIR instruction and terminator:
  - `NativeDirect`
  - `NativeWithSlowpath`
  - `NativeFallbackOnly`
  - `Unsupported`
- Track reason codes:
  - `unsupported_mir_op`
  - `dynamic_selector`
  - `closure_capture`
  - `exception_handler`
  - `pattern_protocol`
  - `watch_binding`
  - `task_scheduler_boundary`
  - `unknown_value_type`
  - `requires_object_dispatch`
- Emit eligibility into native JSON and dump.
- Add an option:
  - `--native-strict`: fail build if any selected function is not native.
  - default: compile eligible functions and keep explicit fallback.
- Start with conservative function-level fallback, then move to block-level
  slowpaths.

Files:

- `optimizer/native_eligibility.h`
- `optimizer/native_eligibility.cpp`
- `optimizer/native.h`
- `optimizer/native.cpp`
- `tools/amberc/main.cpp`
- `tests/native_tests.cpp`

Exit criteria:

- Every function has a deterministic native eligibility report.
- `bench/polyglot` method is classified as eligible after Phase 5 lowering
  support is added.

## Phase 5: MIR Normalization For Native

Goal: make MIR suitable for codegen instead of only diagnostic dumps.

Tasks:

- Add typed/simple operations before generic `send` where safe:
  - `int.add`
  - `int.sub`
  - `int.mul`
  - `int.div`
  - `int.lt`
  - `int.gt`
  - `int.le`
  - `int.ge`
  - `int.eq`
  - `bool.not`
- Add constant canonicalization:
  - integer constants become typed MIR constants;
  - bool/null constants become typed values.
- Add local slot liveness metadata needed for roots.
- Add a pass pipeline:
  - constant fold simple literals;
  - remove redundant `last.set/get` where possible;
  - lower known scalar sends to typed ops;
  - normalize loops and branch conditions.
- Keep HIR-to-bytecode output unchanged until bytecode opcodes are separately
  improved.

Files:

- `optimizer/mir.h`
- `optimizer/mir.cpp`
- `optimizer/native_eligibility.*`
- `tests/mir_tests.cpp`
- `tests/native_tests.cpp`

Exit criteria:

- `x + y`, `x - y`, `x < y`, and `x > y` lower to typed MIR when operands are
  known or guarded integers.
- Polyglot loop has no generic `send` for scalar integer operations in native
  MIR.

## Phase 6: Generated C++ Backend Skeleton

Goal: generate compilable C++ for simple native-eligible functions.

Tasks:

- Add `optimizer/native_cpp_backend.h/.cpp`.
- Generate one C++ translation unit per module.
- Generate one exported entry per native code object.
- Generate a dispatch table:
  - native id -> function pointer;
  - source bytecode id -> native id;
  - method selector -> native entry where appropriate.
- Generate code for:
  - constants;
  - local load/store;
  - `last.get/set`;
  - integer typed ops;
  - `branch_if`;
  - `jump`;
  - `phi`;
  - `return`;
  - explicit safepoints.
- Generate fallback calls for unsupported functions:
  - `amber_native_fallback_to_bytecode(world, code_id, args...)`.
- Add deterministic source formatting.
- Add `amberc native-source <file>` for inspecting generated C++.

Files:

- `optimizer/native_cpp_backend.h`
- `optimizer/native_cpp_backend.cpp`
- `tools/amberc/main.cpp`
- `Makefile`
- `tests/native_tests.cpp`

Exit criteria:

- A hand-written small Amber function compiles to generated C++.
- Generated C++ compiles with the same `CXX` used by the repository.
- The result matches bytecode for constants, integer ops, branches, and returns.

## Phase 7: Native Compilation Driver

Goal: compile generated C++ into a host executable or shared object from
`amberc build --target native`.

Tasks:

- Add native compile options:
  - `--native-cxx <path>`
  - `--native-cxxflag <flag>`
  - `--native-linkflag <flag>`
  - `--native-keep-temp`
  - `--native-strict`
- Default to `CXX` env var, then `clang++`, `g++`, `c++`.
- Use deterministic temp/cache directories under the native cache key.
- Compile generated C++ plus runtime native ABI object.
- Link executable for source builds.
- For manifest builds, start with root executable linking all module generated
  sources; defer shared library packaging until needed.
- Capture compiler stdout/stderr in structured diagnostics.
- Never shell-concatenate untrusted source paths; pass argv vectors directly.

Files:

- `tools/amberc/main.cpp`
- `buildsys/build.h`
- `buildsys/build.cpp`
- `Makefile`
- `tests/build_tests.cpp`

Exit criteria:

- `amberc build bench/polyglot/amber/src/main.am --target native -o <path>`
  produces a native executable.
- Running the executable returns the same checksum as bytecode.

## Phase 8: Runtime Dispatch To Native

Goal: allow runtime package/frozen worlds to call native entries instead of
always executing bytecode.

Tasks:

- Extend `RuntimeWorld` with a native registry:
  - module name -> native module;
  - code id -> native entry;
  - method entry code id -> native entry.
- Extend `execute_native_code`:
  - use a real host entry when `bytecode_trampoline=false`;
  - keep current bytecode trampoline path for old metadata;
  - enforce frozen-world assumptions before host entry.
- Extend method invocation:
  - if receiver dispatch resolves to a method with native entry, call native;
  - otherwise use bytecode.
- Add policy flags:
  - prefer native;
  - force bytecode;
  - strict native;
  - allow fallback on invalidation.
- Preserve call cache invalidation and method version checks.

Files:

- `runtime/vm.h`
- `runtime/vm.cpp`
- `runtime/native_bridge.h`
- `runtime/native_bridge.cpp`
- `runtime/frozen_image.cpp`
- `tests/native_tests.cpp`
- `tests/frozen_image_tests.cpp`

Exit criteria:

- Frozen image native entries execute host code.
- Stale assumptions reject or fallback according to policy.
- Existing bytecode execution is unaffected.

## Phase 9: Slowpath Helper Coverage

Goal: make native execution correct for dynamic Amber semantics by routing
complex operations through VM-compatible helpers.

Slowpaths to implement:

- generic `send`;
- dynamic `send`;
- `call`;
- constructor call;
- method missing;
- ivar load/store cache miss;
- cvar load/store;
- allocation and freeze;
- closure creation;
- watch local/upvalue/ivar;
- pattern protocol;
- matcher protocol;
- `raise`;
- exception handler enter/resume;
- task cancellation and scheduler safepoints;
- object lifetime destroy/dealloc;
- type conversion hooks.

Tasks:

- Add one helper at a time.
- Each helper must:
  - accept native frame state and roots;
  - report language faults;
  - update world/cache state consistently with VM;
  - return control to native when possible;
  - request bytecode fallback when native continuation is not yet supported.
- Add native dump entries for every helper slowpath.

Files:

- `runtime/native_abi.*`
- `runtime/vm.cpp`
- `optimizer/native.cpp`
- `optimizer/native_cpp_backend.cpp`
- `tests/native_tests.cpp`
- `tests/vm_tests.cpp`

Exit criteria:

- Native code can call into dynamic dispatch and resume correctly.
- Slowpath behavior matches VM tests for dispatch, ivars, class methods,
  constructors, method_missing, patterns, and exceptions.

## Phase 10: Native Exception And Unwind Support

Goal: support Amber exceptions across native and bytecode frames.

Tasks:

- Represent native protected ranges in generated C++.
- Map native program points back to source spans.
- Add native exception map lookup:
  - current native pc -> handler;
  - handler resume point -> generated block label or bytecode fallback.
- Preserve `Fault.trace` with mixed native/bytecode frames.
- Add tests for:
  - raise in native caught in native;
  - raise in native caught in bytecode caller;
  - raise in bytecode slowpath caught in native caller;
  - unhandled native raise trace formatting.

Files:

- `runtime/native_abi.*`
- `runtime/native_bridge.*`
- `optimizer/native_cpp_backend.cpp`
- `tests/native_tests.cpp`
- `tests/vm_tests.cpp`

Exit criteria:

- Existing `RAISE` VM tests have native equivalents.
- Mixed traces are source-stable.

## Phase 11: Heap, GC, Pinning, And Lifetime Hardening

Goal: prove native code is safe around heap objects and object lifetime.

Tasks:

- Register native frame roots with the heap root collector.
- Add root maps for all generated safepoints.
- Add tests with forced GC:
  - before native call;
  - during allocation slowpath;
  - at loop backedge;
  - during dynamic send;
  - while native temps hold lists/maps/instances.
- Add pinning helpers:
  - pin value;
  - unpin value;
  - native wait pin bridge where needed.
- Preserve lifecycle errors:
  - destroyed object access;
  - deallocated object access;
  - pinned object mutation/dealloc restrictions.

Files:

- `runtime/vm.*`
- `runtime/native_abi.*`
- `optimizer/native.cpp`
- `optimizer/native_cpp_backend.cpp`
- `tests/native_tests.cpp`
- `tests/vm_tests.cpp`

Exit criteria:

- Native GC tests match VM behavior.
- No native helper can hide a lifetime error behind a host crash.

## Phase 12: Classes, Mixins, Open World, And Invalidation

Goal: support native dispatch in the presence of Amber's mutable world model.

Tasks:

- Use existing owner method versions and world epoch assumptions.
- Emit dispatch guards for:
  - receiver class;
  - method version;
  - world epoch;
  - ivar shape id/version.
- Implement invalidation policy:
  - frozen world: host native code remains valid;
  - open world: native execution rejected unless explicitly allowed;
  - stale native code: fallback or error according to policy.
- Add native-aware package reload tests.
- Add native-aware include/extend/mixin dispatch tests.

Files:

- `runtime/vm.*`
- `runtime/native_bridge.*`
- `optimizer/native.cpp`
- `optimizer/native_cpp_backend.cpp`
- `tests/native_tests.cpp`
- `tests/vm_tests.cpp`

Exit criteria:

- Native dispatch respects method table changes.
- Package reload cannot execute stale native code silently.

## Phase 13: Full Language Coverage

Goal: move from scalar/function native to complete language-native support.

Feature groups:

- scalar primitives:
  - int, bool, null, float, string, symbol;
  - arithmetic, comparison, conversion, string interpolation.
- collections:
  - list, tuple, set, map;
  - indexing, iteration, higher-order methods through slowpaths/native loops.
- object model:
  - class construction;
  - instance methods;
  - class methods;
  - ivars/cvars;
  - mixins, include, extend, superclass dispatch.
- closures:
  - capture layout;
  - upvalue read/write;
  - block calls;
  - closure lifetime roots.
- patterns:
  - literal/pin/list/map patterns;
  - matcher protocol slowpath;
  - clause dispatch.
- concurrency:
  - task/channel/mutex/atomic/barrier/flow calls through runtime helpers;
  - cancellation safepoints.
- profiles:
  - capabilities;
  - effects;
  - replay observability;
  - data/schema/table plans;
  - wasm accelerator metadata;
  - modern profile metadata.

Tasks:

- For each group:
  - add eligibility support;
  - add direct codegen where beneficial;
  - add slowpath helper where semantics are dynamic;
  - add native tests parallel to existing VM tests;
  - add conformance corpus native run mode where applicable.

Exit criteria:

- Every current conformance run can execute in native-preferred mode.
- Unsupported native sites are expected, explicit, and shrinking.

## Phase 14: Native Conformance Mode

Goal: make native correctness observable at the same level as bytecode.

Tasks:

- Add `ambertest run corpus --mode native`.
- Add `ambertest run corpus --mode native-strict` for native-eligible corpus
  items.
- Add corpus metadata for expected native fallback where needed.
- Add build matrix:
  - bytecode;
  - native;
  - native-strict;
  - frozen image with native metadata.
- Add benchmark suite:
  - `bench/polyglot`;
  - dispatch-heavy;
  - allocation-heavy;
  - collection-heavy;
  - exception-heavy.

Files:

- `tools/ambertest`
- `corpus`
- `bench`
- `Makefile`
- `docs/engineering/conformance-v1.md`

Exit criteria:

- Native mode is part of routine CI/local verification.
- Polyglot native performance demonstrates the expected elimination of VM
  `SEND` overhead for integer loops.

## Phase 15: Make Native The Default Build Target

Goal: switch `amber build` to native output only after correctness and
compatibility gates are satisfied.

Prerequisites:

- native build mode passes `make test`;
- native conformance mode passes required corpus bundles;
- bytecode fallback remains available;
- native artifact format and cache are deterministic;
- source build and manifest build both produce native executables where a root
  entry is defined;
- package/frozen image integration can bind and execute native entries;
- user-facing docs explain target selection and fallback policy.

Tasks:

- Change default source build target from wrapper to native executable.
- Keep explicit `--target bytecode-wrapper`.
- Decide manifest default:
  - either `--target both` for compatibility;
  - or native executable plus `.amberbc` sidecar.
- Update docs:
  - `README.md`;
  - `docs/engineering/build-v1.md`;
  - `docs/engineering/native-v1.md`;
  - `docs/engineering/implementation-status-v1.md`;
  - changelog.
- Add migration notes for scripts expecting `.amberbc` only.

Exit criteria:

- `amberc build src/main.am` produces native output by default.
- Existing bytecode workflows remain one flag away.

## Recommended First Implementation Slice

Start with the smallest slice that produces a measurable win:

1. Add `--target native` plumbing and native build JSON fields.
2. Add native eligibility reporting.
3. Add typed MIR lowering for integer scalar sends.
4. Add generated C++ backend for:
   - constants;
   - integer locals;
   - integer arithmetic/comparison;
   - loops and branches;
   - return.
5. Add runtime ABI for returning an integer value and reporting faults.
6. Build `bench/polyglot` as native.
7. Compare checksum and benchmark time.

This slice does not need object model, closures, collections, or exceptions.
It proves the architecture by eliminating the benchmark's current bottleneck:
integer operations lowered through generic `SEND`.

## Test Matrix

Every native phase should add tests in the narrowest existing suite first.

| Area | Tests |
| --- | --- |
| MIR transforms | `tests/mir_tests.cpp` |
| Native metadata and eligibility | `tests/native_tests.cpp` |
| Runtime ABI and GC roots | `tests/native_tests.cpp`, `tests/vm_tests.cpp` |
| Build target/cache/output | `tests/build_tests.cpp`, W14 fixture |
| Frozen image integration | `tests/frozen_image_tests.cpp` |
| CLI smoke | `Makefile test` commands |
| Corpus/native mode | `tools/ambertest`, `corpus` |
| Performance | `bench/polyglot/run_benchmark.py` |

## Risk Register

| Risk | Mitigation |
| --- | --- |
| Native code diverges from VM semantics | Bytecode oracle tests for every native feature |
| GC misses native roots | Conservative root maps first, forced-GC tests at every native safepoint |
| Dynamic dispatch invalidation is unsound | Frozen-world requirement first, method-version/world-epoch guards |
| Generated C++ depends on private VM internals | Narrow `runtime/native_abi.h` facade |
| Build cache becomes non-deterministic | Include backend/compiler/options in cache key and hash outputs |
| Native slowpath cannot resume | Function-level bytecode fallback first, then block-level continuation |
| Native traces are poor | Add native frame source-span metadata before exception-heavy features |
| Performance is hidden by helper overhead | Keep scalar typed ops direct; benchmark after each slice |

## Documentation Updates Per Phase

- Native ABI introduced: update `docs/engineering/native-v1.md`.
- Build target introduced: update `docs/engineering/build-v1.md`.
- Conformance native mode introduced: update `docs/engineering/conformance-v1.md`.
- Default target changes: update `README.md`, implementation status, changelog,
  and migration notes.
- Any heading/link changes: regenerate `docs/engineering/spec-anchor-map-v1.md`
  and run `make spec-sync-check`.

## Open Decisions

- Whether generated C++ native artifacts should be embedded in `.amberimg` or
  stored beside it.
- Whether manifest builds should produce one executable, one shared object per
  module, or both.
- Whether native strictness is function-level, module-level, or package-level.
- Whether direct bytecode fallback remains available in release native builds.
- Whether a future direct object/JIT backend should preserve `amber.native.v1`
  or introduce `amber.native.v2`.
- How much type inference is required before scalar native code is considered
  production-ready.

## Completion Checklist

- `amberc build --target native <file.am>` emits a real native executable.
- `amberc build --target native <amber.build.json>` emits native root output
  plus verified bytecode sidecars.
- Native code can call runtime helpers and resume.
- Native frame roots are visible to GC.
- Native exceptions preserve Amber fault semantics and traces.
- Frozen images can bind and execute non-trampoline native code.
- Package reload and open-world invalidation cannot execute stale native code.
- `ambertest run corpus --mode native` passes.
- `bench/polyglot` native result matches checksum and materially improves over
  bytecode VM execution.
- Documentation and migration notes are updated.
