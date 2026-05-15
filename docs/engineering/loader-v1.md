# amber.loader.v1

Status: `W8.1` dependency linking and module-init state machine are implemented.

The first loader slice is intentionally narrow: it consumes serialized
`.amberbc` bytes, routes every load through the bytecode verifier, links the
`DEPS` graph by logical module id supplied by the host/tooling caller, and runs
module init code through the current VM.

Implemented surface:

- public `RuntimeModuleLoader` in [runtime/module_loader.h](/Users/slowpilot/workspace/amber/runtime/module_loader.h:1);
- serialized module mapping via `add_serialized_module(name, bytes)`;
- verifier-gated load failure as `BytecodeVerificationError`;
- dependency linking over decoded `DepEntry.module_name_str_id` names;
- deterministic init traversal where dependencies initialize before the
  requesting module;
- single-run init guarantee for successful modules, exposed through
  `RuntimeModuleSnapshot::init_runs`;
- missing dependency reporting as `ImportError`;
- cycle-aware init failure as `ModuleInitError`;
- VM init failure propagation into failed module snapshots.

Current limits:

- module identity is supplied by the caller; package manifest/build graph
  identity belongs to later package work;
- import namespace/export cell materialization is deferred to `W8.2`;
- ABI/version mismatch diagnostics and debug-source loader diagnostics are
  deferred to `W8.2`;
- cyclic dependency graphs can link, but cyclic init access fails when the
  loader reaches the active cycle.
