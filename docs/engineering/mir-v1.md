# amber.mir.v1

Status: implemented for `W10.3` MIR/SSA baseline.

The MIR layer is a deterministic optimizer-facing IR between HIR and future
native/JIT backends. It does not replace the existing HIR-to-bytecode execution
path yet; it exposes a separately testable artifact for `ISS-067` and
`ISS-068`.

Implemented surface:

- typed MIR schema in [optimizer/mir.h](/Users/slowpilot/workspace/amber/optimizer/mir.h:1)
- HIR-to-MIR lowering, validator, deterministic JSON/text dumps, and pass
  harness in [optimizer/mir.cpp](/Users/slowpilot/workspace/amber/optimizer/mir.cpp:1)
- CLI inspection in [tools/amberc/main.cpp](/Users/slowpilot/workspace/amber/tools/amberc/main.cpp:1):
  - `amberc mir <file>`
  - `amberc mir-dump <file>`
  - `amberc mir-verify <file>`

## IR Shape

Top-level JSON format is `amber.mir.v1`:

- `module`: package name or `null`;
- `functions[]`: one MIR function per HIR procedure;
- `passes[]`: deterministic pass records appended by the pass harness;
- `source_hash`: source SHA-256 for dump stability.

Each function contains:

- stable procedure identity: `id`, `name`, `kind`, `owner`;
- `entry` block id;
- copied local/capture layout metadata from HIR;
- basic blocks with instruction arrays and one terminator.

Instruction results are SSA values named `%vN`. Local and capture slots remain
explicit storage operands (`local(lN)`, `capture(uN)`) so the initial MIR can
represent imperative Amber semantics without inventing incomplete memory-SSA
proofs. Expression values, branch conditions, send results, closure values, and
phi results are SSA.

Current node families:

- constants and lookup: `const`, `name.lookup`, `const.lookup`;
- storage: `local.load/store`, `capture.load/store`, `ivar.load/store`,
  `cvar.load/store`, `last.get/set`;
- calls and dispatch: `send`, `send.dynamic`, `call`, `keyword.arg`,
  `closure.make`;
- control flow: `branch_if`, `jump`, `return`, `phi`, `safepoint`;
- matcher bridge: `match.dispatch`, `pattern.assign`;
- fallback: `unsupported` with the original HIR node kind.

## SSA Validator

The validator checks the executable MIR contract:

- function entry block exists;
- block ids and SSA definitions are unique;
- every block has a terminator;
- SSA operands reference defined values;
- local/capture operands reference declared slots;
- `phi` nodes are at the top of a block and use block/value operand pairs;
- terminators have the expected operand and target shape;
- branch and phi targets reference existing blocks.

Representative verifier codes:

- `MIR1001` missing function entry block
- `MIR1003` block without terminator
- `MIR1004` duplicate SSA definition
- `MIR1005` undefined SSA operand
- `MIR1008` invalid phi shape
- `MIR1010` unknown block target

## Pass Harness

`run_pass_pipeline(...)` records each pass as `{name, phase_order,
invalidates}` and enforces:

- non-empty pass names;
- monotonic `phase_order`;
- optional validation before a pass that requires valid SSA;
- optional validation after a pass that promises to preserve SSA;
- explicit rejection of passes that both invalidate and preserve SSA.

Invalidation bits are:

- `kInvalidatesControlFlow`
- `kInvalidatesSsa`
- `kInvalidatesAnalyses`

The current harness is intentionally small. It provides the phase-ordering and
post-pass validation boundary needed before optimization passes can become
observable.
