# amber.image.v1

Status: implemented for the `W10.5` frozen image baseline.

`amber.image.v1` is the reproducible frozen artifact layer above W9.4 packages
and W10.4 native metadata. The current reference profile keeps the verified
bytecode VM as the execution engine, but packages the frozen execution contract
into a deterministic `.amberimg` file that can be inspected, verified, and
loaded behind a runtime freeze barrier.

Implemented surface:

- image schema, builder, parser, verifier, JSON inspection, and diagnostics in
  [frozen/image.h](/Users/slowpilot/workspace/amber/frozen/image.h:1) and
  [frozen/image.cpp](/Users/slowpilot/workspace/amber/frozen/image.cpp:1)
- runtime loader in
  [runtime/frozen_image.h](/Users/slowpilot/workspace/amber/runtime/frozen_image.h:1)
  and [runtime/frozen_image.cpp](/Users/slowpilot/workspace/amber/runtime/frozen_image.cpp:1)
- CLI commands in [tools/amberc/main.cpp](/Users/slowpilot/workspace/amber/tools/amberc/main.cpp:1):
  - `amberc image-build <amber.toml> <out.amberimg>`
  - `amberc image-inspect <file.amberimg>`
  - `amberc image-verify <file.amberimg>`

## Artifact Layout

The serialized `.amberimg` starts with `amber.image.v1` and stores:

- the complete serialized `.amberpkg` payload as bytes;
- a package SHA-256 digest;
- a non-zero frozen world epoch seed;
- one native metadata summary per package module;
- deterministic freeze-analysis records for package verification, root module
  presence, bytecode verification, native verification, native frozen
  assumptions, world freeze, and reload rejection.

Native metadata is embedded as deterministic `amber.native.v1` JSON bytes with
its own digest. The verifier treats that metadata as an immutable native
readiness payload: every package module must have matching native metadata,
the metadata must declare `amber.native.v1`, and it must require a frozen
runtime world.

## Verification

`image-verify` checks:

- image structure and digest stability;
- embedded package verification, including package signature checks when a
  signing key is provided;
- package module bytecode decode/verification;
- root module presence;
- native metadata presence for every module;
- native metadata digest, format, code-object count, and frozen-world summary.

The build path also validates native modules against decoded bytecode before
serializing the image, so malformed root maps, safepoints, stubs, patchpoints,
or source code ids fail before `.amberimg` output is written.

## Runtime Load

`runtime/frozen_image` loads a verified image by constructing a `RuntimeWorld`
from the embedded package artifact and immediately calling `freeze_world()`.
When full native metadata objects are available in memory, the loader also
binds them to the frozen `RuntimeWorldMirror`, recording the active world epoch
and owner method versions for W10.4 native execution.

After image load:

- `RuntimeWorld::is_world_frozen()` is true;
- native trampoline execution can use bound frozen-world assumptions;
- `RuntimeWorld::reload_package_artifact(...)` is rejected with
  `WorldFrozenError`.

This keeps `.amberimg` loading compatible with the specification rule that
native/frozen artifacts are immutable deployment artifacts rather than
open-world development packages.
