# amber.bc.v1

Status: implemented for `W4.1`-`W4.4` emission boundary. `W5.1` VM call/return
baseline is already consuming this container; dispatch caches, general unwind,
and loader-time object-model linking still live in later slices.

Implemented surface:

- typed schema in [bytecode/format.h](/Users/slowpilot/workspace/amber/bytecode/format.h:1)
- canonical serializer/deserializer + verifier skeleton in [bytecode/format.cpp](/Users/slowpilot/workspace/amber/bytecode/format.cpp:1)
- CLI inspection in [tools/amberc/main.cpp](/Users/slowpilot/workspace/amber/tools/amberc/main.cpp:1):
  - `amberc bc <file>`
  - `amberc bc-disasm <file>`
  - `amberc amberbc-dump <file>`
  - `amberc amberbc-verify <file>`
  - `amberc amberbc-disasm <file>`
- HIR-to-bytecode emission in [bytecode/emitter.cpp](/Users/slowpilot/workspace/amber/bytecode/emitter.cpp:1)

## Container

Header layout is fixed-size and little-endian:

- magic: `ABM1`
- `format_version.major/minor`
- `language_version.major/minor`
- `profile_flags`
- `section_count`
- `file_flags`
- `abi_hash[32]`

Section directory entries are:

- `kind` as 4-byte tag
- `offset` as `u64`
- `size` as `u32`
- `align` as `u32`
- `flags` as `u32`

Current canonical section tags:

- required: `STRS`, `SYMS`, `KONS`, `CODE`, `METH`, `CLAS`, `DEPS`, `EXPT`, `INIT`
- optional: `PATS`, `SPAN`, `LINE`, `LOCS`, `ATTR`, `HASH`

Writer policy is deterministic:

- section order is canonical enum order
- no host pointers are serialized
- integers are little-endian
- instruction immediates use `ULEB128` / `SLEB128`
- serialize -> deserialize -> serialize must produce byte-identical output for valid modules

## Record Encoding

Current `CODE` encoding stores:

- `code_id`, `kind`, `reg_count`, `flags`
- local/capture layout arrays
- instruction array of `opcode + operand_count + signed_flag + leb payload`
- handler table
- call-site / ivar-site tables
- safepoint table

`SPAN` is stored as a separate section and is reattached to owning `BcCode` by `code_id` during decode.

`PATS` currently stores deterministic pattern-program descriptors only:

- `pattern_id`
- `binding_count`
- `flags` where bit `0x1` means `requires_commit`

`module_to_json(...)` dumps the fully decoded module as `amber.bc.v1`.

`module_to_disasm(...)` prints deterministic text form:

- sections in stable order
- code ids as `cN`
- locals as `lN`
- captures as `uN`
- instructions as zero-padded `pc opcode operands...`

`CLAS` currently stores class-like runtime descriptors:

- `class_name_sym_id`
- optional `superclass_ref` as `KONS[path]`
- `ivar_schema_id`
- `method_range_start + method_range_count`
- direct `include` refs as source-order `KONS[path][]`
- direct `extend` refs as source-order `KONS[path][]`
- `flags` where bit `0x1` means `mixin`
- optional `class_init_code_id`

Current emitter notes:

- ordinary methods/classes/top-level init lower directly to `CODE` entries;
- clause-style methods currently serialize fallback `entry_code_id` plus
  `clause_table[]` with separate clause-pattern/guard/body code objects and
  `PATS` descriptors derived from HIR `match_program.binding_order`;
- defaulted params now emit separate `default_thunk` code objects referenced
  from `BcMethod.default_thunk_ids[]`, compiled in the same local/capture
  environment as the owning method;
- top-level `HClass` / `HMixin` now lower into `CLAS` records, preserving
  path-based superclass refs plus direct `include` / `extend` order for later
  loader/runtime linearization;
- static `case` / block-param-pattern / pattern-assignment lowering now emits
  inline `P_*` instructions in ordinary code objects plus `PATS` descriptors;
- matcher-expression patterns now lower to `P_TRIPLE_EQ` over lowered
  `matcher_expr` HIR subtrees;
- dynamic matcher objects now lower through ordinary `CALL` / `SEND`
  instructions (`route(...)`, `match`, `success`, `bindings`, `empty?`)
  plus existing `P_*` map-pattern ops and explicit `RAISE` paths for the
  currently enforced protocol checks.

## Verifier Skeleton

Current verifier is intentionally structural, not semantic/runtime-complete. It checks:

- header magic and supported version window
- duplicate/missing/unknown sections
- section alignment and bounds
- duplicate code ids / pattern ids
- string/symbol/code/class/method range references for the currently materialized descriptors
- path-typed superclass/include/extend refs inside `CLAS`
- handler ranges
- jump target bounds
- back-edge safepoint presence
- local debug ranges
- fixed 32-byte `HASH` digests

Representative current verifier codes:

- `BC1001` invalid magic
- `BC1102` missing required section
- `BC1204` unknown code id reference
- `BC1303` back-edge without safepoint

Not implemented yet in this layer:

- full protocol typing checks for every dynamic matcher contract edge are still
  deferred to the VM/runtime implementation
- loader-time ancestor linearization and world-mutation invalidation
- VM/runtime execution
- bytecode-level type or isolation proofs
- full instruction-arity verification against final VM ISA
