# amber.native.v1

Status: implemented for the `W10.4` native/JIT readiness baseline, consumed by
the `W10.5` frozen image layer, and hardened for `W15` native-readiness
metadata closure.

The native layer follows the compile-closure convention in
[amber_unified_final_spec.md](../../amber_unified_final_spec.md#8-canonical-mir-native-jit-frozen-image-profile):
the bytecode VM remains the reference execution engine, native compilation is a
frozen-world profile, and reflective sites are represented as runtime stubs
rather than unsafe host-code shortcuts.

Implemented surface:

- native/JIT metadata schema in [optimizer/native.h](../../optimizer/native.h:1)
- metadata generation, validation, JSON, and text dumps in
  [optimizer/native.cpp](../../optimizer/native.cpp:1)
- frozen-world runtime bridge in
  [runtime/native_bridge.h](../../runtime/native_bridge.h:1)
  and [runtime/native_bridge.cpp](../../runtime/native_bridge.cpp:1)
- CLI inspection in [tools/amberc/main.cpp](../../tools/amberc/main.cpp:1):
  - `amberc native <file>`
  - `amberc native-dump <file>`
  - `amberc native-verify <file>`
- frozen image embedding through
  [frozen/image.cpp](../../frozen/image.cpp:1),
  which stores deterministic `amber.native.v1` metadata summaries inside
  `.amberimg` artifacts.

## Code Objects

Top-level JSON format is `amber.native.v1`. The backend emits one
`NativeCodeObject` per bytecode `BcCode`.

Each object records:

- `source_bc_code_id` and code kind;
- optional linked MIR function identity for method entry code;
- a deterministic `machine_code_blob` in the current bytecode-trampoline
  profile;
- runtime call stubs for `CALL`, `SEND`, `SEND_DYN`, type hooks, pattern
  protocol calls, and `RAISE`;
- JIT patchpoint descriptors for call inline caches, reflective `SEND_DYN`, and
  ivar inline caches;
- explicit slowpath descriptors for runtime helpers, reflective stubs,
  patchpoint guard misses, and frozen-world invalidation fallback;
- conservative root maps, exception maps, and safepoint maps;
- frozen-world assumptions for `world_epoch` and owner `method_version`.
- focused conformance coverage for allocation, call, and back-edge safepoints
  that each carry matching root maps.

The `amber.native.v1` metadata layer still uses deterministic trampoline
payloads. Separately, `amberc build` now has a `cpp-bytecode-direct-v1`
executable backend for build artifacts: eligible bytecode functions are emitted
as direct C++ functions and compiled by the host C++ toolchain, while unsupported
bytecode remains available through an embedded verified VM fallback. This keeps
the metadata contract stable while allowing build outputs to execute hot integer
bytecode without VM dispatch. The direct subset includes captured closures with
shared mutable capture cells, closure calls, list literals, and read-only list
operations `[]`, `count`, and `first`. Generated functions keep register frames
in fixed-size stack storage; closure, capture-cell, and list objects live in a
process-lifetime native arena. Exception handlers, dynamic dispatch outside the
direct selector subset, watch operations, and object lifecycle operations still
use the VM fallback. Capture-bearing code is native when invoked through a
native closure; a direct entry that needs captures is reported as non-native
until the backend can materialize that entry closure.

## Runtime Bridge

`runtime/native_bridge` binds a native module to a `RuntimeWorldMirror`, copying
the frozen world epoch and owner method versions into every code object.

`execute_native_code(...)` enforces the W10.4 contract:

- open worlds are rejected with `WorldFrozenError`;
- unbound or stale epoch/method assumptions fail with native assumption errors;
- valid trampoline code re-enters `RuntimeWorld::execute(...)`;
- callers may request bytecode fallback for stale assumptions, modeling the
  specified no-deopt path where invalid native code is discarded at a safe call
  boundary.
- trampoline execution is covered with a requested GC at the bytecode
  safepoint, proving heap arguments remain rooted across the native boundary.

## Validation

`native-verify` checks that native code objects do not omit required metadata:

- unique native ids and source bytecode ids;
- non-empty machine-code payloads;
- frozen code carries world assumptions;
- every safepoint has a matching root map;
- call stubs name a runtime helper;
- patchpoints declare guard and action;
- call stubs have matching `slowpath_table` entries;
- frozen code declares an invalidation slowpath that can re-enter bytecode;
- slowpaths preserve language-level runtime errors instead of host crashes;
- exception handlers have matching root maps;
- optional source-bytecode validation checks source code ids, safepoint ranges,
  slowpath ranges, and exception map ranges.

This keeps W10.4 compatible with the spec rule that native/JIT output must not
drop root maps, exception maps, safepoints, or reflective slow paths.

## W15 Closure

W15 does not introduce executable host machine code. It closes the native
readiness contract so a future native/JIT backend can start without changing
bytecode or VM semantics:

- `NativeCodeObject.slowpath_table` is part of `amber.native.v1` JSON/dump
  output;
- reflective `send`, dynamic pattern protocol, `TypeTerm` hooks, and `RAISE`
  sites are represented as explicit slowpaths;
- stale frozen-world assumptions declare bytecode re-entry as the no-deopt
  invalidation path;
- exception edges are represented as safepoints with root maps;
- `.amberimg` verification now requires the embedded native JSON to advertise
  these readiness guards.
