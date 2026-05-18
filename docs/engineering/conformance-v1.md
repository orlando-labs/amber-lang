# amber.conformance.v1

Status: `W8.4` full conformance runner gate is satisfied, and `W9.1`
typed-profile fixtures are available behind the cumulative `M6` bundle.

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
- multi-source package/build-graph corpus belongs to later package work.
