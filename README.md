# Amber Reference Implementation

This repository is the implementation workspace for `amber_spec_consolidated_v20_1.md`.

Current baseline:

- implementation language: C++17;
- compiler toolchain: `clang++` by default, override with `make CXX=/path/to/clang++`;
- supported host targets for now: Linux and macOS;
- no Python, npm, Cargo, or external package dependency is required for the current frontend slice.
- bytecode tooling is still pure C++17 and uses the same local `clang++` toolchain.

Useful commands:

```sh
make build
make test
make conformance
make fmt
make clean
build/amberc lex corpus/parse/lexer/basic/source.am
build/amberc parse corpus/parse/module/basic/source.am
build/amberc parse-expr corpus/parse/expr/postfix/source.am
build/amberc bind corpus/bind/module/basic/source.am
build/amberc hir corpus/hir/module/basic/source.am
build/amberc mir corpus/hir/module/basic/source.am
build/amberc mir-dump corpus/hir/module/basic/source.am
build/amberc mir-verify corpus/hir/module/basic/source.am
build/amberc bc corpus/bc/module/basic/source.am
build/amberc bc-disasm corpus/bc/disasm/basic/source.am
build/amberc amberbc-dump /path/to/module.amberbc
build/amberc amberbc-verify /path/to/module.amberbc
build/amberc amberbc-disasm /path/to/module.amberbc
build/ambertest run corpus
build/ambertest run corpus --bundle M5
```

Current matrix status:

| Slice | Status | Notes |
| --- | --- | --- |
| `W0.1`, `W0.3` | done | repo/build/test skeleton, deterministic CLI/test workflow |
| `W1.1`-`W1.4` | done | lexer + syntax-faithful parser + postfix/control-flow/module surface |
| `W2.1`-`W2.3` | done | binder, signatures, canonical diagnostics, negative corpus |
| `W3.1`-`W3.4` | done | HIR, compiled patterns, match-program bridge, closure captures, safe-nav lowering |
| `W4.1`-`W4.3` | done | `amber.bc.v1` container, serializer/deserializer, verifier, dump/disasm tooling |
| `W4.4` | done | HIR->bytecode covers current methods/control-flow subset, closures, debug tables, clause-method metadata, default thunks, matcher-expression and dynamic-matcher lowering for `case`, block-param prologues, pattern assignment, and class-like `CLAS` descriptors for class/mixin/superclass/include/extend |
| `W5.1` | done | frame model, registers, direct code entry, locals/moves/constants/jumps, `CALL`, closure/block invocation, returns, default-thunk materialization, and VM-side execution for ordinary function/block paths |
| `W5.2` | done | `SEND` / `SEND_DYN`, local and multi-segment `LOOKUP_CONST`, `LOAD_IVAR` / `STORE_IVAR`, `LOAD_CVAR` / `STORE_CVAR`, constructor `CALL`, runtime auto-assign for `@field` / `@@field`, class/instance dispatch with superclass/include/extend, `method_missing`, `MAKE_LIST` / `MAKE_TUPLE` / `MAKE_MAP` / `FREEZE`, matcher-oriented collection bridging, eager clause-table dispatch, monomorphic `SEND` / ivar inline-cache baseline with version guards, public world-mutation paths, and object `deconstruct` / `deconstruct_keys` pattern protocol |
| `W5.3` | done | lexical captures, `GETLAST` / `SETLAST`, pattern-fail `MatchError`, inline matcher execution for scalar/class/sequence/map `case` / `case!`, block-param prologues, pattern assignment, deferred `P_COMMIT`, `RAISE`, handler-table unwinding across VM frames, and source-backed fault traces |
| `W5.4` | done | public heap object headers, shape descriptors, slot-backed instance ivars, stable shape transitions, dead-shape guards, runtime owner method-table descriptors, and method-table observability for class/instance dispatch |
| `W6.1` | done | runtime heap allocator boundary, per-worker arena stats, object header allocation metadata, remote-free queues with owner-drain semantics, and VM allocation paths for instances, collections, maps, and closures |
| `W6.2` | done | lifecycle state machine, `destroy!` dispatch, `OBJ_DESTROY` / `OBJ_DEALLOC`, tombstone header rewrite, payload clearing for instances/collections/closures, and destroyed/deallocated access guards |
| `W6.3` | done | non-moving GC boundary, object generations, root scanning, write barriers, remembered sets, safepoint-triggered collection, logical reclaim of unrooted cycles, and parallel GC smoke coverage |
| `W6.4` | done | `PinToken` registry, active pin GC roots, stale-unpin guards, nested pin scopes, opaque native handles, pinned value-buffer views, native wait cancellation poll hooks, and pinned-object lifecycle guards |
| `W7.1` | done | scheduler core with bounded worker pool, strand TLS scopes, runnable global/local queues, timer wake queue, explicit wake coalescing, and parallel strand smoke coverage |
| `W7.2` | done | task lifecycle runtime with task TLS, join/rethrow, join timeouts without auto-cancel, cooperative cancellation safepoints, waiting parent scope exit, structured child sets, and first-failure sibling cancellation |
| `W7.3` | done | concurrency runtime primitives with rendezvous/buffered `RuntimeChannel`, FIFO send/recv wait queues, explicit close and `ChannelClosedError`, shareability-gated payloads, non-reentrant `RuntimeMutex`, and seq-cst `RuntimeAtomic` |
| `W8.1` | done | `RuntimeModuleLoader` for serialized `.amberbc`, verifier-gated module mapping, dependency linking, deterministic init order, single-run init, missing-dependency import failures, cycle-aware `ModuleInitError`, and failed-init snapshots |
| `W8.2` | done | runtime export-cell/import-alias materialization, read-only alias readiness checks, missing-export `ImportError`, dependency format/language/ABI compatibility diagnostics, re-export chain resolution, and source-mapped loader diagnostics for init faults |
| `W8.3` | done | runtime collections contract for sequence `each/map/flat_map/select/reject/reduce/find/any?/all?/none?/first/count/group_by/to_a/lazy`, `EmptyCollectionError` reduce guard, and deterministic `Map` keys/values/entries/map/select/reject/transform_values/each |
| `W8.4` | done | deterministic full conformance runner for `parse/lower/check/compile/disasm/run/load`, M1-M5 gate bundles, failure rendering, and CI one-command `make conformance` |
| `W9.1` | done | optional Amber/Typed checker lane with `TypeTerm` parsing/canonicalization, exported callable annotation boundaries, parameter/default/return diagnostics, basic truthiness flow for `and`/`or`, strict `case!` exhaustiveness checks, runtime type-hook metadata, `amberc typed`, and M6 typed corpus |
| `W9.2` | done | open-world runtime transactions for class/mixin reopen, instance/class method replacement, direct `include` / class-side `extend`, pre-commit kind/superclass/include-cycle validation, `open -> frozen` world state with `WorldFrozenError`, atomic rollback, and dispatch invalidation via `world_epoch` / owner versions |
| `W9.3` | done | read-only runtime reflection mirrors for package/world/class/mixin/method state, deterministic method/export/dependency ordering, source-location exposure from bytecode debug spans, and stable snapshot semantics without mutation backdoors |
| `W9.4` | done | package manifest/lock/artifact tooling with restricted `amber.toml`, deterministic `amber.lock`, reproducible `.amberpkg` bundles over verified `.amberbc` bytes, SHA-256 dev signatures, and filesystem registry install/publish smoke |
| `W9.5` | done | open-world dev-profile hot reload as atomic package artifact swap, with package/module bytecode predecode, manifest identity checks, ABI/profile/export surface and selector/arity compatibility guards, frozen-world rejection, rollback on failed swap, and `world_epoch` dispatch invalidation |
| `W10.1` | done | advanced concurrency runtime with explicit `RuntimeMoveSlot` ownership transfer for channel/select boundaries, moved-from `MovedValueError` guards, fair-ish `runtime_select` recv/send arms with timeout/else behavior, and structured-task supervisor policies `cancel_scope`, `one_for_one`, `one_for_all`, and `rest_for_one` |
| `W10.2` | done | `amber.io` awaitable/readiness runtime bridge with `RuntimeAwaitable`, awaitable-compatible `runtime_select` arms, timeout/failure/cancellation states, and native wait integration through active W6.4 pin tokens |
| `W10.3` | done | `amber.mir.v1` optimizer IR with HIR-to-MIR lowering, SSA value/block validation, deterministic JSON/text dumps, `mir`/`mir-dump`/`mir-verify` CLI, and pass harness phase/invalidation checks |

The current implemented frontend slices cover `W0.1`, `W0.3`, `W1.1`-`W1.4`,
`W2.1`, `W2.2`, the pattern/frontend contract for `W3.1`-`W3.4`, and the
artifact/container baseline for `W4.1`-`W4.4` from the implementation matrix:

- repo/build/test skeleton;
- deterministic lexer token stream with source spans;
- `amberc lex <file>` JSON token dump;
- Pratt expression parser for precedence, assignment, syntax-faithful postfix chains, safe navigation, bare calls, and one-line block suffix chain boundaries;
- module parser for package/import/export, def/class/class_method/mixin,
  clause-style `def`, unambiguous simple many-def sugar, include/extend,
  pass/noop, expression statements, pattern assignment, and control-flow forms
  (`if`, `unless`, `case`, loops, `break`);
- `amberc parse <file>` `amber.ast.v1` JSON dump;
- `amberc parse-expr <file>` `amber.ast.v1` JSON dump;
- `amberc bind <file>` `amber.bind.v1` JSON dump for lexical scopes, bindings,
  signature descriptors, references, exports, clause/case pattern locals,
  block-param patterns, pattern assignment binders, pin references, and
  matcher-expression references;
- `amberc typed <file>` `amber.typed.v1` JSON dump for optional Amber/Typed
  boundary checks, normalized `TypeTerm` parameter/return hooks, inferred
  return summaries, exported callable annotation enforcement, and typed
  diagnostics;
- `amberc hir <file>` `amber.hir.v1` JSON dump with `HModule`, procedure
  table, declarative items, control-flow, structured `Pat*` source patterns
  plus `HCompiledPattern` matcher IR and deterministic `match_program` for
  clause-style `def`, `case`, block-param patterns, and pattern assignment,
  including `PatMatcherExpr`, `PatList`, sequence-rest hints, normalized
  `deconstruct` / `deconstruct_keys` hints, map-rest runtime hints, dynamic
  matcher explicit-binding metadata, canonical safe-nav null-guards,
  reflective `send(...)` lowering, inline block closures, and explicit closure
  captures;
- `amberc mir <file>`, `amberc mir-dump <file>`, and `amberc mir-verify <file>`
  for `amber.mir.v1` HIR-to-MIR lowering, SSA/block validation, deterministic
  JSON/text dumps, and pass harness metadata for future optimizer phases;
- binder diagnostics for exports/import writes/duplicates, wildcard misuse,
  placeholder misuse, structural pattern validation including bare matcher
  misuse, forbidden dynamic-pattern contexts, dynamic matcher self-reference,
  and default-expression preflight;
- shape-only `frontend/binder` helpers for extracting parsed call-site shape and for ordinary signature call preflight, explicit positional/keyword bind with `MISSING` slots, default-order planning, and delayed auto-assign buffers;
- `ambertest run <path>` corpus runner for lex, expression-parse, module-parse, bind/check, optional typed checks, HIR/lower, bytecode compile/disasm, VM run, loader load, diagnostic fixtures, deterministic discovery, focused mismatch rendering, and `--bundle M1..M6` milestone gates;
- `amber.bc.v1` typed schema, canonical `.amberbc` serializer/deserializer, structural verifier skeleton, JSON dump, and deterministic text disassembly for the current bytecode subset;
- `amberc bc <file>` and `amberc bc-disasm <file>` HIR-to-bytecode emitter path for current supported methods/classes/control-flow subset, including clause-method metadata in `BcMethod.clause_table` with dedicated emitted clause pattern probe code, `default_thunk_ids[]`, `type_hook_ids[]` metadata for annotated parameter/return boundaries, `PATS` binding descriptors, matcher-expression and dynamic-matcher bridge lowering for `case`, static pattern-opcode lowering for block-param prologues and pattern assignment, and path-based `CLAS` descriptors for class/mixin owners with preserved superclass/include/extend metadata;
- `runtime/vm` execution baseline for verified `BcCode` in unit tests: frame stack, register file, `last_result`, branches, direct entry, closure capture materialization, closure `CALL`, constructor `CALL`, eager clause-table method dispatch, scalar and collection `SEND` / `SEND_DYN`, `RAISE` / handler-table unwinding, inline matcher execution without AST-walk fallback, slot-backed instance shapes, stable runtime method tables, W6.1 heap allocation boundary for objects/arrays/closures, W6.2 lifecycle tombstones, W6.3 non-moving GC boundary, W6.4 pinning/native-handle boundary, W9.2 atomic open-world transactions/freeze guards, W9.3 immutable reflection mirror snapshots, W9.5 atomic package hot-reload swaps, W10.1 advanced concurrency primitives, and W10.2 awaitable readiness bridge;
- `runtime/vm` W6.1-W6.4 memory baseline with `RuntimeHeap`, worker scopes, per-worker arena counters, object allocation ids, remote-free enqueue/drain semantics, safepoint drain hooks, allocation-heavy smoke coverage, lifecycle state transitions, tombstone payload release, destroyed/deallocated access guards, object generations, root scanning, write barriers, remembered sets, logical reclaim of unrooted cycles, parallel GC smoke coverage, active pin roots, stale-unpin guards, nested pin scopes, opaque handles, pinned value-buffer views, native wait cancellation polling, and pinned-object lifecycle guards;
- `runtime/vm` W7.1 scheduler core with `RuntimeScheduler`, `RuntimeStrandScope`, current strand/worker TLS introspection, runnable global/local queues, timer-backed sleeping strands, explicit wake coalescing, deterministic idle waits, and parallel strand smoke coverage;
- `runtime/vm` W7.2 task runtime with `spawn_task`, task TLS/cancellation polling, `join_task` failure propagation and timeout results, cooperative cancellation safepoints, waiting scope-exit parents, structured child snapshots, first-failure propagation, and sibling cancellation coverage;
- `runtime/vm` W7.3 concurrency base with `RuntimeChannel` rendezvous/buffered modes, FIFO blocking send/recv queues, explicit `close()`, `ChannelClosedError`, timeout/cancellation-aware blocking results, recursive shareability checks for channel payloads, non-reentrant `RuntimeMutex`, and seq-cst `RuntimeAtomic`;
- `runtime/vm` W10.1 advanced concurrency runtime with explicit `RuntimeMoveSlot` moved-from guards, move-aware `RuntimeChannel::send`, receiver-side adoption of moved confined graphs, `runtime_select` recv/send/moved-send arms with timeout and else semantics plus rotating ready-arm selection, and structured-task supervisor policies exposed through `RuntimeTaskOptions`;
- `runtime/vm` W10.2 `amber.io` awaitable bridge with `RuntimeAwaitable` pending/ready/failed/cancelled states, awaitable select arms, bounded await/poll behavior, scheduler-task cancellation propagation, and native wait completion/cancellation/failure paths backed by active W6.4 pin tokens;
- `optimizer/mir` W10.3 MIR/SSA baseline with one MIR function per HIR
  procedure, explicit basic blocks, `%vN` SSA values, phi nodes for `HIf`
  expression results, loop/control-flow terminators, structural SSA validation,
  deterministic dumps, and pass pipeline phase/invalidation records;
- `runtime/vm` W8.3 collections contract with closure-block execution for eager sequence `each/map/flat_map/select/reject/reduce/find/any?/all?/none?/first/count/group_by/to_a/lazy`, deterministic `EmptyCollectionError` for empty `reduce` without init, and ordered `Map#keys/#values/#entries/#map/#select/#reject/#transform_values/#each`;
- `runtime/module_loader` W8.1-W8.2 dependency loader with serialized `.amberbc` decode/verify on every load path, deterministic dependency linking, dependency-before-dependent module init, single-run init snapshots, missing dependency/export `ImportError`, cycle-aware `ModuleInitError`, export-cell/import-alias snapshots, read-only alias readiness checks, dependency ABI/version diagnostics, re-export chain resolution, and source-mapped VM fault propagation for failed module init;
- `package/package` W9.4 package tooling with restricted `amber.toml` parsing,
  deterministic `amber.lock` rendering, reproducible signed `.amberpkg`
  artifacts, artifact verify/inspect JSON, and filesystem registry
  install/publish smoke helpers exposed through `amberc package-*`;
- `runtime/vm` W9.5 dev-profile package hot reload through
  `RuntimeWorld::reload_package_artifact`, with whole-artifact predecode,
  manifest identity and public ABI/export/arity compatibility guards,
  frozen-world rejection, failed-swap rollback, and dispatch invalidation;
- early token, bytecode section, opcode, and diagnostic registries plus engineering notes.
