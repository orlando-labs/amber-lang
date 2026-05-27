# amber.build.v1

Status: implemented for `W14` build/bootstrap/conformance closure.

`amber.build.v1` is the repository-local build manifest consumed by
`amberc build`. It is intentionally small and deterministic: source paths are
manifest-relative, module output names are derived from module ids, profile
features are sorted/deduplicated, and all output `.amberbc` artifacts are
decoded and verified before the command reports success.

Implemented surface:

- manifest parser and stable JSON summary in [build/build.h](/Users/slowpilot/workspace/amber/build/build.h:1)
- build command in [tools/amberc/main.cpp](/Users/slowpilot/workspace/amber/tools/amberc/main.cpp:1)
- `PROF` bytecode metadata in [bytecode/format.h](/Users/slowpilot/workspace/amber/bytecode/format.h:1)
- unsupported-profile loader rejection in [runtime/module_loader.cpp](/Users/slowpilot/workspace/amber/runtime/module_loader.cpp:1)
- CLI smoke fixture in [tests/fixtures/w14_build/amber.build.json](/Users/slowpilot/workspace/amber/tests/fixtures/w14_build/amber.build.json:1)

## Manifest Shape

Minimal manifest:

```json
{
  "schema": "amber.build.v1",
  "name": "demo",
  "root": "demo.main",
  "profiles": {
    "required": ["core.v1"],
    "optional": ["typed.v1"],
    "forbidden": ["ffi.v1"]
  },
  "stdlib": [
    {"name": "amber.core", "path": "stdlib/core.am", "bootstrap": "B2"}
  ],
  "modules": [
    {"name": "demo.main", "path": "src/main.am"}
  ]
}
```

`stdlib` entries are compiled through the same source-to-bytecode pipeline as
ordinary modules. B2 stdlib modules receive `amber.bootstrap.layer` attributes
and ABI hashes. Ordinary modules receive dependency entries pinned to the
compiled stdlib ABI hashes.

## CLI

```sh
amberc build amber.build.json --out-dir build/amber --cache-dir build/amber/.cache
```

The command emits `amber.build.result.v1` JSON with per-module source hashes,
cache keys, artifact hashes, ABI hashes, output paths, and cache-hit state. A
second identical build reuses cache entries while preserving byte-identical
output artifacts.

## Conformance

`make test` runs the W14 fixture twice and verifies the emitted root module with
both `amberc amberbc-verify` and the public
`amberc verify <file.amberbc> --json` surface. It also smoke-checks
`amberc metadata <file.amberbc> --json` and a corrupted-bytecode verifier
failure. `make conformance` runs
`ambertest run corpus --bundle M11`, which adds a compile-all postpass over the
compile/disasm/run/load corpus.
