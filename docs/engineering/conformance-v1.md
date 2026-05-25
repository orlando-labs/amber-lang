# amber.conformance.v1

Status: `W8.4` full conformance runner gate is satisfied, `W9.1`
typed-profile fixtures are available behind the cumulative `M6` bundle,
`W9.2` open-world plus `W9.3` reflection mirror runtime behavior is covered by
focused VM tests, and `W9.4` package tooling is covered by focused package
tests. `W9.5` package hot-reload swap behavior, `W10.1` advanced concurrency
runtime behavior, and `W10.2` awaitable/native-readiness behavior are covered
by focused VM tests. `W10.3` MIR/SSA lowering, validation, dump stability, and
pass harness behavior are covered by focused MIR tests. `W10.4`
native/JIT-readiness metadata and frozen-world trampoline behavior are covered
by focused native tests. `W10.5` frozen image build, verification, runtime
load, and reload-barrier behavior are covered by focused image tests. `W11.1`
capability manifest parsing, bytecode metadata, policy resolution, and runtime
checks are covered by focused package/bytecode/VM tests. `W11.2` effect rows,
checker summaries, `EFCT` metadata, and runtime allowance checks are covered by
focused lexer/parser/checker/bytecode/VM tests. `W11.3` replay metadata,
`.ambertrace` serialization, runtime event recording, and replay divergence
checks are covered by focused bytecode/VM tests. `W11.4` schema/table metadata,
`SCMA`/`TABL` round-trips, query-plan fingerprints, and runtime mirror hooks are
covered by focused bytecode/VM tests. `W11.5` Wasm/accelerator metadata,
`WASM`/`ACCL` round-trips, component/kernel validation, and runtime mirror hooks
are covered by focused profile/bytecode/VM tests. `W11.6` AI-agent tooling,
contracts/property, privacy/lineage, and workflow metadata, `AGNT`/`CNTR`/
`PRIV`/`WFLW` round-trips, profile validation, CLI surfaces, and runtime mirror
hooks are covered by focused profile/bytecode/VM tests.

`ambertest run <path>` is the canonical corpus entrypoint. It discovers
`meta.json` fixtures deterministically, dispatches by fixture phase, compares
golden output byte-for-byte, and renders focused expected/actual mismatches.

Supported positive phases:

- `lex`
- `parse-expr`
- `parse`
- `bind`
- `check`
- `typed`
- `hir` / `lower`
- `bc` / `compile`
- `bc-disasm` / `disasm`
- `run`
- `load`

Supported negative phases:

- `bind-diag`
- `typed-diag`

Milestone bundles are selected with:

```sh
ambertest run corpus --bundle M5
```

Bundle levels are cumulative:

- `M1`: frontend, binder/check, HIR/lower, and diagnostic corpus;
- `M2`: `M1` plus bytecode compile/disasm corpus;
- `M3` and `M4`: `M2` plus VM run corpus;
- `M5`: full dynamic corpus, including loader load fixtures;
- `M6`: `M5` plus optional Amber/Typed checker fixtures.

The Makefile exposes the CI/mainline command:

```sh
make conformance
```

Current limits:

- `run` fixtures execute a zero-argument method named by `meta.json.entry`, or
  `main` when omitted;
- `load` fixtures compile one source module, serialize it to `.amberbc`, load it
  through `RuntimeModuleLoader`, and initialize all mapped modules;
- `typed` fixtures run the optional Amber/Typed profile without changing
  dynamic conformance or the `make conformance` M5 gate;
- `mir` / `mir-dump` / `mir-verify` are available through `amberc` and focused
  unit tests; corpus phases are not wired yet;
- `native` / `native-dump` / `native-verify` are available through `amberc`
  and focused unit tests; corpus phases are not wired yet;
- `image-build` / `image-inspect` / `image-verify` are available through
  `amberc` and focused unit tests; corpus phases are not wired yet;
- `capabilities-check` is available through `amberc` and focused unit tests;
  corpus profile fixtures are not wired yet;
- `effects-check` is available through `amberc` and focused unit tests; corpus
  profile fixtures are not wired yet;
- `replay-check` / `trace-inspect` are available through `amberc` and focused
  unit tests; corpus profile fixtures are not wired yet;
- `schema-check` / `table-explain` are available through `amberc` and focused
  unit tests; corpus profile fixtures are not wired yet;
- `wasm-build` / `accel-check` are available through `amberc` and focused unit
  tests; corpus profile fixtures are not wired yet;
- `symbols` / `explain` / `patch-check` / `provenance-audit` /
  `contract-check` / `privacy-check` / `workflow-check` are available through
  `amberc` and focused unit tests; corpus profile fixtures are not wired yet;
- W9.2 transaction/freeze, W9.3 reflection mirror, W9.4 package artifact,
  W9.5 package hot-reload, W10.1 advanced concurrency, W10.2 awaitable, and
  W10.3 MIR/SSA, W10.4 native/JIT metadata, W10.5 frozen-image, W11.1
  capability, W11.2 effect, W11.3 replay, W11.4 schema/table, W11.5
  Wasm/accelerator, and W11.6 modern-profile checks are focused API/tooling
  tests today;
- multi-source package/build-graph corpus can now target W9.4 package artifacts
  and W9.5 reload fixtures when the corpus runner grows package phases;
- W10.1-W10.5 `move`/`select`/supervisor/awaitable/MIR/native/image` and
  W11.1 capability / W11.2 effects / W11.3 replay / W11.4 schema-table /
  W11.5 Wasm-accelerator / W11.6 modern-profile
  language-surface fixtures can be added once those forms are exposed above the
  runtime API and corpus runner phases.
