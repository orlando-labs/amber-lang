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
make fmt
make clean
build/amberc lex corpus/parse/lexer/basic/source.am
build/amberc parse corpus/parse/module/basic/source.am
build/amberc parse-expr corpus/parse/expr/postfix/source.am
build/amberc bind corpus/bind/module/basic/source.am
build/amberc hir corpus/hir/module/basic/source.am
build/amberc bc corpus/bc/module/basic/source.am
build/amberc bc-disasm corpus/bc/disasm/basic/source.am
build/amberc amberbc-dump /path/to/module.amberbc
build/amberc amberbc-verify /path/to/module.amberbc
build/amberc amberbc-disasm /path/to/module.amberbc
build/ambertest run corpus
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
| `W8.1` | not started | dependency linker and module-init state machine over serialized `.amberbc` are still absent |

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
- `amberc hir <file>` `amber.hir.v1` JSON dump with `HModule`, procedure
  table, declarative items, control-flow, structured `Pat*` source patterns
  plus `HCompiledPattern` matcher IR and deterministic `match_program` for
  clause-style `def`, `case`, block-param patterns, and pattern assignment,
  including `PatMatcherExpr`, `PatList`, sequence-rest hints, normalized
  `deconstruct` / `deconstruct_keys` hints, map-rest runtime hints, dynamic
  matcher explicit-binding metadata, canonical safe-nav null-guards,
  reflective `send(...)` lowering, inline block closures, and explicit closure
  captures;
- binder diagnostics for exports/import writes/duplicates, wildcard misuse,
  placeholder misuse, structural pattern validation including bare matcher
  misuse, forbidden dynamic-pattern contexts, dynamic matcher self-reference,
  and default-expression preflight;
- shape-only `frontend/binder` helpers for extracting parsed call-site shape and for ordinary signature call preflight, explicit positional/keyword bind with `MISSING` slots, default-order planning, and delayed auto-assign buffers;
- minimal `ambertest run <path>` corpus runner for lex, expression-parse, module-parse, bind, HIR, and bind-diagnostic fixtures;
- `amber.bc.v1` typed schema, canonical `.amberbc` serializer/deserializer, structural verifier skeleton, JSON dump, and deterministic text disassembly for the current bytecode subset;
- `amberc bc <file>` and `amberc bc-disasm <file>` HIR-to-bytecode emitter path for current supported methods/classes/control-flow subset, including clause-method metadata in `BcMethod.clause_table` with dedicated emitted clause pattern probe code, `default_thunk_ids[]`, `PATS` binding descriptors, matcher-expression and dynamic-matcher bridge lowering for `case`, static pattern-opcode lowering for block-param prologues and pattern assignment, and path-based `CLAS` descriptors for class/mixin owners with preserved superclass/include/extend metadata;
- `runtime/vm` execution baseline for verified `BcCode` in unit tests: frame stack, register file, `last_result`, branches, direct entry, closure capture materialization, closure `CALL`, constructor `CALL`, eager clause-table method dispatch, scalar and collection `SEND` / `SEND_DYN`, `RAISE` / handler-table unwinding, inline matcher execution without AST-walk fallback, slot-backed instance shapes, stable runtime method tables, W6.1 heap allocation boundary for objects/arrays/closures, W6.2 lifecycle tombstones, W6.3 non-moving GC boundary, and W6.4 pinning/native-handle boundary;
- `runtime/vm` W6.1-W6.4 memory baseline with `RuntimeHeap`, worker scopes, per-worker arena counters, object allocation ids, remote-free enqueue/drain semantics, safepoint drain hooks, allocation-heavy smoke coverage, lifecycle state transitions, tombstone payload release, destroyed/deallocated access guards, object generations, root scanning, write barriers, remembered sets, logical reclaim of unrooted cycles, parallel GC smoke coverage, active pin roots, stale-unpin guards, nested pin scopes, opaque handles, pinned value-buffer views, native wait cancellation polling, and pinned-object lifecycle guards;
- early token, bytecode section, opcode, and diagnostic registries plus engineering notes.
