# amber.loader.v1

Status: `W8.1` dependency linking/module-init and `W8.2` export/import
diagnostics are implemented, with `W8.4` load conformance coverage through
`ambertest`.

The loader consumes serialized `.amberbc` bytes, routes every load through the
bytecode verifier, links the `DEPS` graph by logical module id supplied by the
host/tooling caller, materializes runtime exports/import aliases, and runs
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
- export-cell materialization from decoded `EXPT`, exposed through
  `RuntimeExportCellSnapshot`;
- read-only import aliases registered by host/tooling import tables via
  `add_import_alias(module, local, dependency, export)`;
- alias readiness checks through `read_import_alias(...)`, producing
  `ModuleInitError` for early reads of uninitialized exports;
- missing export reporting as `ImportError`;
- dependency bytecode format, language-version, and ABI-hash compatibility
  diagnostics at link time;
- re-export chain resolution using `ExportEntry::has_reexport_module_name`;
- structured `RuntimeLoaderDiagnostic` records with module/dependency/export
  identity and source locations when VM init faults include `SPAN` / `LINE`;
- cycle-aware init failure as `ModuleInitError`;
- VM init failure propagation into failed module snapshots;
- conformance `load` fixtures compile source, serialize to `.amberbc`, map the
  bytes through `RuntimeModuleLoader`, and assert deterministic init/module
  snapshots.

Current limits:

- module identity is supplied by the caller; package manifest/build graph
  identity belongs to later package work;
- the decoded `.amberbc` v1 format still stores dependency module ids in
  `DEPS`; the exact source import-name table is supplied to the runtime loader
  by host/tooling as import aliases;
- cyclic dependency graphs can link, but cyclic init access fails when the
  loader reaches the active cycle.
