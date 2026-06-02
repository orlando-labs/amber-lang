# Amber Reference Implementation

This repository is the implementation workspace for `amber_spec_consolidated_v20_1_main.md`.

Current baseline:

- implementation language: C++17;
- compiler toolchain: `clang++` by default, override with `make CXX=/path/to/clang++`;
- supported host targets for now: Linux and macOS;
- no Python, npm, Cargo, or external package dependency is required for the compiler/runtime build and conformance slice;
- W12 documentation sync uses only the Python 3 standard library for generated anchor-map checks;
- bytecode tooling is pure C++17 and uses the same local `clang++` toolchain.

Useful commands:

```sh
make build
make test
make conformance
make spec-sync-check
make fmt
make clean
build/amberc tests/fixtures/run_script/main.am
build/amberc build tests/fixtures/w14_build/src/main.am -o build/w14-main-exe
build/amberc lex corpus/parse/lexer/basic/source.am
build/amberc parse corpus/parse/module/basic/source.am
build/amberc parse-expr corpus/parse/expr/postfix/source.am
build/amberc bind corpus/bind/module/basic/source.am
build/amberc hir corpus/hir/module/basic/source.am
build/amberc effects-check /path/to/source.am
build/amberc replay-check /path/to/run.ambertrace
build/amberc mir corpus/hir/module/basic/source.am
build/amberc mir-dump corpus/hir/module/basic/source.am
build/amberc mir-verify corpus/hir/module/basic/source.am
build/amberc native corpus/bc/module/basic/source.am
build/amberc native-dump corpus/bc/module/basic/source.am
build/amberc native-verify corpus/bc/module/basic/source.am
build/amberc bc corpus/bc/module/basic/source.am
build/amberc bc-disasm corpus/bc/disasm/basic/source.am
build/amberc build tests/fixtures/w14_build/amber.build.json --out-dir build/w14_build/out --cache-dir build/w14_build/cache
build/amberc metadata /path/to/module.amberbc --json
build/amberc verify /path/to/module.amberbc --json
build/amberc amberbc-dump /path/to/module.amberbc
build/amberc amberbc-verify /path/to/module.amberbc
build/amberc amberbc-disasm /path/to/module.amberbc
build/amberc wasm-build /path/to/plugin.amberwasm
build/amberc accel-check /path/to/kernels.amberaccel
build/ambertest run corpus
build/ambertest run corpus --bundle M11
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
| `W6.3` | done | non-moving GC boundary, object generations, root scanning, write barriers, remembered sets, safepoint-triggered collection, caller/back-edge safepoint roots, rooted/unrooted local and shared cycles, and parallel GC smoke coverage |
| `W6.4` | done | `PinToken` registry, active pin GC roots, stale-unpin guards, nested pin scopes, exception-unwind release, opaque native handles, pinned value-buffer views, native wait cancellation poll hooks, and pinned-object lifecycle guards |
| `W7.1` | done | scheduler core with bounded worker pool, strand TLS scopes, runnable global/local queues, timer wake queue, explicit wake coalescing, and parallel strand smoke coverage |
| `W7.2` | done | task lifecycle runtime with task TLS, join/rethrow, join timeouts without auto-cancel, cooperative cancellation safepoints, waiting parent scope exit, structured child sets, and first-failure sibling cancellation |
| `W7.3` | done | concurrency runtime primitives with rendezvous/buffered `RuntimeChannel`, FIFO send/recv wait queues, explicit close and `ChannelClosedError`, shareability-gated payloads, non-reentrant `RuntimeMutex`, and seq-cst `RuntimeAtomic` |
| `W8.1` | done | `RuntimeModuleLoader` for serialized `.amberbc`, verifier-gated module mapping, dependency linking, deterministic init order, single-run init, missing-dependency import failures, cycle-aware `ModuleInitError`, failed-init snapshots, and sticky failed-init retry behavior |
| `W8.2` | done | runtime export-cell/import-alias materialization, live alias readiness/failure snapshots, read-only alias readiness checks, missing-export `ImportError`, dependency format/language/ABI compatibility diagnostics, re-export chain resolution, and source-mapped loader diagnostics for init faults |
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
| `W10.4` | done | `amber.native.v1` native/JIT metadata backend over MIR+bytecode with frozen-world assumptions, runtime call stubs, JIT patchpoint descriptors, allocation/call/back-edge root maps, exception/safepoint maps, `native`/`native-dump`/`native-verify` CLI, and frozen runtime trampoline execution with stale-assumption bytecode fallback |
| `W10.5` | done | `amber.image.v1` frozen image builder with reproducible `.amberimg` artifacts over signed `.amberpkg` payloads, embedded native metadata summaries, freeze-analysis verification, `image-build`/`image-inspect`/`image-verify` CLI, and runtime load that installs the frozen-world/reload barrier |
| `W11.1` | done | `amber.capabilities.v1` manifest/profile baseline with `[capabilities]` parsing, canonical capability taxonomy and alias normalization, `CAPS` bytecode metadata, package/image propagation, host grant resolution, `CapabilityError` runtime checks, and `capabilities-check` CLI |
| `W11.2` | done | `amber.effects.v1` callable effect rows with `!{...}` parser/binder/HIR preservation, typed/effects summaries and subset diagnostics, `EFCT` bytecode metadata, package/image/reload propagation, `EffectViolationError` runtime checks, and `effects-check` CLI |
| `W11.3` | done | `amber.replay.v1` observability/replay baseline with canonical event families, `OBSV`/`RPLY` bytecode metadata, deterministic `.ambertrace` serialization, runtime trace recording and replay divergence checks, plus `replay-check` / `trace-inspect` CLI |
| `W11.4` | done | `amber.data/schema.v1` metadata-first schema/dataframe baseline with schema definitions, compatible migration validation, record codec checks, table/query-plan fingerprints, column dependency metadata, `SCMA`/`TABL` bytecode sections, runtime mirror/validation hooks, plus `schema-check` / `table-explain` CLI |
| `W11.5` | done | `amber.wasm/accelerator.v1` metadata-first Wasm component and accelerator baseline with frozen-world component interface validation, capability/effect boundary metadata, restricted accelerator kernel descriptors, `WASM`/`ACCL` bytecode sections, runtime mirror/validation hooks, plus `wasm-build` / `accel-check` CLI |
| `W11.6` | done | `amber.modern-profiles.v1` metadata-first AI-agent tooling/contracts/privacy/workflow baseline with semantic symbol graph and explain JSON, structured patch/provenance validation, contract/property descriptors, privacy label/policy/lineage checks, durable workflow step/history idempotency validation, `AGNT`/`CNTR`/`PRIV`/`WFLW` bytecode sections, runtime mirror/validation hooks, plus `symbols` / `explain` / `patch-check` / `provenance-audit` / `contract-check` / `privacy-check` / `workflow-check` CLI |
| `W12` | done | documentation/spec sync baseline with generated anchor map, v20.1 changelog, migration notes, implementation status dashboard, local Markdown link checks, and `make spec-sync-check` |
| `W13` | done | compiler-contract closure slice with W13 source/comment/numeric/range rules, prelude/runtime-error/token registry updates, top-level module-cell pre-scan, `in`/`and`/`or` lowering, bytecode verifier operand/range/dataflow checks, runtime UNINIT guards, unified `CALL` packet/object-call dispatch, structured source traces, and focused lexer/parser/binder/HIR/emitter/bytecode/VM tests |
| `W14` | done | build/bootstrap/conformance closure with `amber.build.v1` manifests, `amberc build`, deterministic `.amberbc` output, incremental cache keys, B2 stdlib bootstrap metadata/ABI hashes, `PROF` profile feature metadata, unsupported-profile loader rejection, CLI fixture smoke, and M11 compile/load/run conformance bundle |
| `W15` | done | native-readiness metadata closure with explicit `slowpath_table` entries for runtime/reflective stubs, invalidation-to-bytecode fallback metadata, exception-edge safepoint/root maps, hardened `native-verify` checks, and frozen-image readiness summary verification |

The current implemented frontend slices cover `W0.1`, `W0.3`, `W1.1`-`W1.4`,
`W2.1`, `W2.2`, the pattern/frontend contract for `W3.1`-`W3.4`, and the
artifact/container baseline for `W4.1`-`W4.4` from the implementation matrix:

- repo/build/test skeleton;
- deterministic lexer token stream with source spans;
- `amberc <file.am>` compiles and runs a single Amber source file;
- `amberc lex <file>` JSON token dump;
- Pratt expression parser for precedence, assignment, inline conditionals,
  conditional collection elements, syntax-faithful postfix chains, safe
  navigation, bare calls, and one-line block suffix chain boundaries;
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
- `amberc native <file>`, `amberc native-dump <file>`, and
  `amberc native-verify <file>` for `amber.native.v1` W10.4 native/JIT
  metadata over verified bytecode and MIR, including call stubs, patchpoints,
  conservative root maps, exception maps, safepoint maps, and frozen-world
  assumptions;
- `amberc image-build <amber.toml> <out.amberimg>`,
  `amberc image-inspect <file.amberimg>`, and
  `amberc image-verify <file.amberimg>` for `amber.image.v1` W10.5 frozen
  images that embed reproducible package bytes, native metadata summaries,
  freeze-analysis records, and package signature verification hooks;
- `amberc capabilities-check <amber.toml> [--grant <cap[=target]>...]` for
  `amber.capabilities.v1` W11.1 manifest preflight, host grant intersection,
  denied-capability JSON diagnostics, and path-scoped capability checks;
- `amberc effects-check <file>` for `amber.effects.v1` W11.2 callable effect
  summaries, pure-boundary validation, and deterministic `FX0001`/`FX0003`
  diagnostics;
- `amberc replay-check <file.ambertrace>` and
  `amberc trace-inspect <file.ambertrace>` for `amber.replay.v1` W11.3
  deterministic trace envelope validation and JSON inspection;
- `amberc schema-check <file.amberschema>` and
  `amberc table-explain <file.ambertable>` for `amber.data/schema.v1` W11.4
  schema/migration validation, record codec checks, and table query-plan
  fingerprints;
- `amberc wasm-build <file.amberwasm>` and
  `amberc accel-check <file.amberaccel>` for `amber.wasm/accelerator.v1`
  W11.5 frozen component boundary validation, capability/effect metadata, and
  restricted accelerator kernel checks;
- `amberc symbols <file>`, `amberc explain <file> --span <line>:<column>`,
  `amberc patch-check <file.ambermodern>`,
  `amberc provenance-audit <file.ambermodern>`,
  `amberc contract-check <file.ambermodern>`,
  `amberc privacy-check <file.ambermodern>`, and
  `amberc workflow-check <file.ambermodern>` for `amber.modern-profiles.v1`
  W11.6 semantic symbol/explain output, structured patch and provenance
  validation, contract/property descriptors, privacy/lineage policy checks, and
  durable workflow history/idempotency checks;
- `amberc build <amber.build.json>` for W14 multi-module build manifests,
  deterministic `.amberbc` emission into an output directory, cache-keyed
  incremental rebuilds, B2 stdlib bootstrap metadata, stdlib ABI dependency
  pinning, and required/optional/forbidden profile feature metadata in `PROF`;
- `amberc build <file.am>` for single-file executable wrappers that embed the
  compiled `.amberbc` payload and run through `amberc run-embedded`;
- `amberc metadata <file.amberbc> --json` and
  `amberc verify <file.amberbc> --json` as the public `.amberbc` artifact
  inspection/verification surface, with structured verifier JSON for corrupted
  bytecode files;
- `make spec-sync-check` for the W12 documentation/spec-sync gate, validating
  the generated [anchor map](docs/engineering/spec-anchor-map-v1.md), local
  Markdown links, the [v20.1 changelog](spec/changelog/v20.1.md),
  [migration notes](docs/engineering/migration-notes-v20.1.md), and the
  [implementation status dashboard](docs/engineering/implementation-status-v1.md);
- binder diagnostics for exports/import writes/duplicates, wildcard misuse,
  placeholder misuse, structural pattern validation including bare matcher
  misuse, forbidden dynamic-pattern contexts, dynamic matcher self-reference,
  and default-expression preflight;
- shape-only `frontend/binder` helpers for extracting parsed call-site shape and for ordinary signature call preflight, explicit positional/keyword bind with `MISSING` slots, default-order planning, and delayed auto-assign buffers;
- `ambertest run <path>` corpus runner for lex, expression-parse, module-parse, bind/check, optional typed checks, HIR/lower, bytecode compile/disasm, VM run, loader load, diagnostic fixtures, deterministic discovery, focused mismatch rendering, and `--bundle M1..M6/M11` milestone gates;
- `amber.bc.v1` typed schema, canonical `.amberbc` serializer/deserializer, structural verifier skeleton, JSON dump, and deterministic text disassembly for the current bytecode subset;
- `amberc bc <file>` and `amberc bc-disasm <file>` HIR-to-bytecode emitter path for current supported methods/classes/control-flow subset, including clause-method metadata in `BcMethod.clause_table` with dedicated emitted clause pattern probe code, `default_thunk_ids[]`, `type_hook_ids[]` metadata for annotated parameter/return boundaries, `PATS` binding descriptors, matcher-expression and dynamic-matcher bridge lowering for `case`, static pattern-opcode lowering for block-param prologues and pattern assignment, and path-based `CLAS` descriptors for class/mixin owners with preserved superclass/include/extend metadata;
- `runtime/vm` execution baseline for verified `BcCode` in unit tests: frame stack, register file, `last_result`, branches, direct entry, closure capture materialization, closure `CALL`, constructor `CALL`, eager clause-table method dispatch, scalar and collection `SEND` / `SEND_DYN`, `RAISE` / handler-table unwinding, inline matcher execution without AST-walk fallback, slot-backed instance shapes, stable runtime method tables, W6.1 heap allocation boundary for objects/arrays/closures, W6.2 lifecycle tombstones, W6.3 non-moving GC boundary, W6.4 pinning/native-handle boundary, W9.2 atomic open-world transactions/freeze guards, W9.3 immutable reflection mirror snapshots, W9.5 atomic package hot-reload swaps, W10.1 advanced concurrency primitives, and W10.2 awaitable readiness bridge;
- `runtime/vm` W6.1-W6.4 memory baseline with `RuntimeHeap`, worker scopes, per-worker arena counters, object allocation ids, remote-free enqueue/drain semantics, safepoint drain hooks, allocation-heavy smoke coverage, lifecycle state transitions, tombstone payload release, destroyed/deallocated access guards, object generations, root scanning, write barriers, remembered sets, caller/back-edge safepoint roots, rooted/unrooted local and shared cycles, parallel GC smoke coverage, active pin roots, stale-unpin guards, nested pin scopes, exception-unwind release, opaque handles, pinned value-buffer views, native wait cancellation polling, and pinned-object lifecycle guards;
- `runtime/vm` W7.1 scheduler core with `RuntimeScheduler`, `RuntimeStrandScope`, current strand/worker TLS introspection, runnable global/local queues, timer-backed sleeping strands, explicit wake coalescing, deterministic idle waits, and parallel strand smoke coverage;
- `runtime/vm` W7.2 task runtime with `spawn_task`, task TLS/cancellation polling, `join_task` failure propagation and timeout results, cooperative cancellation safepoints, waiting scope-exit parents, structured child snapshots, first-failure propagation, and sibling cancellation coverage;
- `runtime/vm` W7.3 concurrency base with `RuntimeChannel` rendezvous/buffered modes, FIFO blocking send/recv queues, explicit `close()`, `ChannelClosedError`, timeout/cancellation-aware blocking results, recursive shareability checks for channel payloads, non-reentrant `RuntimeMutex`, and seq-cst `RuntimeAtomic`;
- `runtime/vm` W10.1 advanced concurrency runtime with explicit `RuntimeMoveSlot` moved-from guards, move-aware `RuntimeChannel::send`, receiver-side adoption of moved confined graphs, `runtime_select` recv/send/moved-send arms with timeout and else semantics plus rotating ready-arm selection, and structured-task supervisor policies exposed through `RuntimeTaskOptions`;
- `runtime/vm` W10.2 `amber.io` awaitable bridge with `RuntimeAwaitable` pending/ready/failed/cancelled states, awaitable select arms, bounded await/poll behavior, scheduler-task cancellation propagation, and native wait completion/cancellation/failure paths backed by active W6.4 pin tokens;
- `optimizer/mir` W10.3 MIR/SSA baseline with one MIR function per HIR
  procedure, explicit basic blocks, `%vN` SSA values, phi nodes for `HIf`
  expression results, loop/control-flow terminators, structural SSA validation,
  deterministic dumps, and pass pipeline phase/invalidation records;
- `optimizer/native` W10.4 native/JIT readiness layer with one
  `NativeCodeObject` per `BcCode`, bytecode-trampoline machine-code payloads,
  reflective slow stubs for dynamic dispatch/type/pattern hooks, call/ivar IC
  patchpoint descriptors, allocation/call/back-edge root maps,
  exception/safepoint maps, and recorded world-epoch/method-table assumptions,
  plus `runtime/native_bridge` frozen execution, native trampoline heap-root
  preservation, and stale-assumption bytecode re-entry;
- `frozen/image` and `runtime/frozen_image` W10.5 frozen-image path with
  deterministic `.amberimg` serialization, package/native digest checks,
  bytecode/native verification, explicit freeze-analysis records, runtime
  `RuntimeWorld` load with an immediate freeze barrier, native binding to the
  frozen world mirror, and package hot-reload rejection after image load;
- `profile/capabilities`, `.amberbc` `CAPS`, and `runtime/vm` W11.1
  capability baseline with deny-by-default manifest requests, host-grant
  resolution, prefix-scoped resource targets, package/image propagation, and
  `CapabilityError` runtime preflight for host operations;
- `profile/effects`, callable `!{...}` rows, `.amberbc` `EFCT`, and
  `runtime/vm` W11.2 effects baseline with declared/observed row validation,
  HIR/checker summaries, package/image/reload propagation, and host-enforced
  `EffectViolationError` checks;
- `profile/replay`, `.amberbc` `OBSV`/`RPLY`, and `runtime/vm` W11.3
  observability/replay baseline with canonical event validation, deterministic
  event ids/virtual timestamps, `.ambertrace` digest serialization, and
  `ReplayDivergenceError` comparison for runtime replay;
- `profile/data`, `.amberbc` `SCMA`/`TABL`, and `runtime/vm` W11.4
  schema/dataframe baseline with schema definition and migration validation,
  record codec checks, stable query-plan fingerprints, column dependencies,
  and runtime mirror/validation exposure;
- `profile/wasm_accel`, `.amberbc` `WASM`/`ACCL`, and `runtime/vm` W11.5
  Wasm/accelerator baseline with frozen component interface validation,
  capability/effect boundary metadata, restricted kernel descriptors, and
  runtime mirror/validation exposure;
- `profile/modern`, `.amberbc` `AGNT`/`CNTR`/`PRIV`/`WFLW`, and `runtime/vm`
  W11.6 modern profile baseline with semantic symbols/explain, patch
  provenance, contracts/properties, privacy lineage, durable workflow history,
  and runtime mirror/validation exposure;
- `runtime/vm` W8.3 collections contract with closure-block execution for eager sequence `each/map/flat_map/select/reject/reduce/find/any?/all?/none?/first/count/group_by/to_a/lazy`, deterministic `EmptyCollectionError` for empty `reduce` without init, and ordered `Map#keys/#values/#entries/#map/#select/#reject/#transform_values/#each`;
- `runtime/module_loader` W8.1-W8.2 dependency loader with serialized `.amberbc` decode/verify on every load path, deterministic dependency linking, dependency-before-dependent module init, single-run init snapshots, sticky failed-init/cyclic-init retries, missing dependency/export `ImportError`, cycle-aware `ModuleInitError`, live export-cell/import-alias snapshots, read-only alias readiness checks, dependency ABI/version diagnostics, re-export chain resolution, and source-mapped VM fault propagation for failed module init;
- `runtime/module_loader` W14 profile gate for `PROF` required/optional/forbidden
  features, rejecting unsupported required host profiles before module init with
  `UnsupportedProfileError`;
- `package/package` W9.4 package tooling with restricted `amber.toml` parsing,
  deterministic `amber.lock` rendering, reproducible signed `.amberpkg`
  artifacts, artifact verify/inspect JSON, and filesystem registry
  install/publish smoke helpers exposed through `amberc package-*`;
- `runtime/vm` W9.5 dev-profile package hot reload through
  `RuntimeWorld::reload_package_artifact`, with whole-artifact predecode,
  manifest identity and public ABI/export/arity compatibility guards,
  frozen-world rejection, failed-swap rollback, and dispatch invalidation;
- `runtime/vm` W13 runtime closure with frame initialized-bit tracking,
  `NameError` for uninitialized local/module-cell reads, shared `CallPacket`
  decoding for `CALL`, closure/class/object-with-`call` dispatch, call-cache
  shape guards over positional count, canonical keyword shape, block presence,
  world epoch, and method version, plus structured trace frames carrying module
  id, byte/line/column spans, and generated-span kind;
- early token, prelude, runtime-error, bytecode section, opcode, and diagnostic
  registries plus engineering notes.
