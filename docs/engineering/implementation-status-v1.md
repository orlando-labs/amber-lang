# amber.implementation-status.v1

Status: W12 implementation status dashboard for the current v20.1 reference
baseline.

This dashboard is the human-readable release index that ties the project-layer
work-package matrix to concrete repository artifacts. Use it with the generated
[spec anchor map](spec-anchor-map-v1.md), the
[v20.1 changelog](../../spec/changelog/v20.1.md), and the
[v20.1 migration notes](migration-notes-v20.1.md).

## Release Dashboard

| Work package | Status | Primary artifacts | Verification |
| --- | --- | --- | --- |
| `W0` repo/tooling | done | `Makefile`, `tools/amberc`, `tools/ambertest`, `spec/registries/*` | `make build`, `build/amberc --version` |
| `W1` lexer/parser/AST | done | `frontend/lexer`, `frontend/parser`, `frontend/ast`, `corpus/parse` | parser/lexer unit tests, parse corpus |
| `W2` binder/signatures/diagnostics | done | `frontend/binder`, `frontend/checker`, `spec/registries/diagnostics.yaml`, `corpus/bind`, `corpus/check` | binder/checker tests, negative diagnostic corpus |
| `W3` patterns/HIR/lowering | done | `frontend/pattern`, `frontend/hir`, `corpus/hir` | HIR and pattern tests, HIR golden corpus |
| `W4` bytecode artifacts | done | `bytecode/format.*`, `bytecode/emitter.*`, `docs/engineering/bc-v1.md` | bytecode/emitter tests, bc/disasm corpus |
| `W5` VM core/dispatch | done | `runtime/vm.*`, `docs/engineering/runtime-v1.md` | VM unit tests, run corpus |
| `W6` object/memory/lifetime/GC/pinning | done | `runtime/vm.*`, runtime error registry | VM lifetime, GC, and pinning coverage |
| `W7` scheduler/concurrency | done | `runtime/vm.*` scheduler/task/channel primitives | VM scheduler, task, channel, mutex, atomic coverage |
| `W8` loader/stdlib/full corpus | done | `runtime/module_loader.*`, `tools/ambertest`, `corpus/load`, `docs/engineering/conformance-v1.md` | `make conformance`, loader and collection tests |
| `W9` typed/open-world/packages | done | `frontend/checker`, `package/package.*`, `runtime/vm.*`, typed/package docs | typed, package, hot-reload, and reflection tests |
| `W10` advanced concurrency/native/frozen | done | `optimizer/mir.*`, `optimizer/native.*`, `frozen/image.*`, `runtime/native_bridge.*`, `runtime/frozen_image.*` | MIR, native, frozen-image, awaitable, and advanced concurrency tests |
| `W11` modern profile runtime | done | `profile/{capabilities,effects,replay,data,wasm_accel,modern}.*`, profile engineering docs | profile, bytecode, package, and VM tests |
| `W12` documentation/spec sync | done | `tools/spec_sync.py`, `docs/engineering/spec-anchor-map-v1.md`, this dashboard, changelog, migration notes | `make spec-sync-check` |
| `W13` compiler-contract closure | planned | source/literal rules, slot model, prelude registry, verifier dataflow registries | pending |
| `W14` build/bootstrap/conformance closure | planned | build graph, incremental cache, stdlib bootstrap, profile flags, conformance bundles | pending |
| `W15` native-readiness metadata | planned | MIR/native root maps, exception maps, slow stubs, frozen assumptions | pending |

## Release Gates

| Gate | Current state |
| --- | --- |
| M1-M5 dynamic baseline | satisfied through `make conformance` and focused tests |
| M6 typed/package/open-world baseline | satisfied through focused typed, package, reflection, and hot-reload tests |
| M7 advanced concurrency | satisfied at runtime API level through focused VM tests |
| M8 MIR/native/frozen | satisfied at metadata/runtime bridge level through focused MIR/native/image tests |
| M9 W0-W12 release baseline | satisfied for current repository scope after `make conformance` and `make spec-sync-check` |
| M10 modern profiles | satisfied at metadata-first profile level through focused profile/bytecode/VM tests |

## Sync Policy

Any work package status change must update this dashboard, the relevant
engineering note, the README matrix, and the changelog. If headings or local
links change, regenerate `docs/engineering/spec-anchor-map-v1.md` and run
`make spec-sync-check`.
