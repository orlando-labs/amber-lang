# amber.hir.v1

Status: seed contract.

The current HIR layer is a deterministic AST-to-HIR normalization pass that
introduces an explicit `HModule` root plus a flat procedure table.

Top-level shape:

- `format`: always `amber.hir.v1`;
- `module`: package name or `null`;
- `root`: `HModule`;
- `procedures`: module-init, method, and closure bodies in stable discovery
  order;
- `constants`: currently an empty deterministic array;
- `source_hash`: SHA-256 of the source file.

Current root form:

- `HModule(module_name, init, imports[], exports[], items[])`;
- declarative items are `HClass`, `HMixin`, and `HMethod`;
- executable top-level statements are lowered into the `init` procedure rather
  than kept inline in `items[]`.

Current procedure entries contain:

- `id`, `name`, `kind`, `owner`, and source `span`;
- `signature` as `HSignature(params[])`;
- `locals` with stable slot ids `l0..lN`: binder declaration order first, then
  lowering-generated synthetic temps in source order;
- `captures` with stable capture slot ids `u0..uN`, plus
  `source_kind = local | capture` and `source_slot` for closure materialization;
- `body` as `HSeq(items[])`.

Currently lowered node families:

- module/import/export/class/mixin/include/extend declarations;
- top-level `def` and `class_method def` as `HMethod` with `dispatch_side =
  module | instance | class`;
- clause-style `def` as `HMethod(signature, clauses[], else_body, procedure)`;
- literals as `HConst`;
- collection literals as `HListLiteral(elements[])`,
  `HTupleLiteral(elements[])`, `HSetLiteral(elements[])`, and
  `HMapLiteral(entries[])` with `HMapEntry(key_kind, key, value)`;
- conditional list/set elements as `HConditionalElement(condition_kind,
  condition, value)` inside the enclosing list/set literal, and conditional map
  entries as `HMapEntry(..., condition_kind, condition)`;
- local/import/placeholder reads as `HLoadLocal`;
- closure capture reads and writes as `HLoadCapture` / `HStoreCapture`;
- explicit safe-nav guards as `HIf(cond = HIsNull(...), ...)`;
- unresolved names as implementation-local `HLoadName` bridge nodes for late
  global lookup cases that binder cannot resolve statically;
- constant reads as `HLoadConst`;
- ivar/cvar reads and writes as `HLoadIvar` / `HStoreIvar` and `HLoadCvar` /
  `HStoreCvar`;
- `$_` as `HLastGet` and expression statements as `HLastSet(expr)`;
- assignments as `HStoreLocal` / `HStoreIvar` / `HStoreCvar`;
- binary operators as `HSend(receiver, selector = op, ...)`, except `and` /
  `or` as `HLogical`, `in` as `contains?`, and `..` as `Range.new(...,
  inclusive_end: true)`;
- unary operators as `HSend(receiver, selector = not | u+ | u-, ...)`;
- reflective builtin `send(recv, selector, ...)` as `HSend` for static string
  selectors or `HSendDyn(receiver, selector_expr, ...)` otherwise;
- `if` / `unless` block forms and inline `if ... then ... else ...` as `HIf`;
- `while` / `until` / `loop` / `do while` as `HLoop(kind, ...)`;
- `break` as `HBreak`;
- `case` / `case!` as `HMatchDispatch` with `HMatchArm`;
- `HCompiledPattern(pattern_ir)` bridge nodes attached to clause and case
  matching entries;
- simple many-def sugar normalized to one `HMethod` with multiple `HClause`
  entries;
- postfix lowering to `HSend`, `HCall`, and `HIndex`;
- one-line block suffix lowering to `HClosure` plus a separate closure
  procedure with explicit `captures[]`.

Current implementation notes:

- safe-navigation is lowered eagerly to `HIf + HIsNull + synthetic temp locals`,
  so equivalent safe member/call/index forms do not leave `HSafe*` nodes in the
  final dump;
- closure captures are materialized in source order, and nested closures may
  propagate outer captures transitively via `source_kind = capture`;
- structural matching currently lowers `pattern` as nested `Pat*` nodes:
  `PatLiteral`, `PatBind`, `PatIgnore`, `PatConst`, `PatPin`,
  `PatMatcherExpr`, `PatTuple`, `PatList`, `PatMap`, `PatMapField`,
  `PatHead`, `PatKwField`, `PatAs`, `PatOr`, and `PatDynamic`;
- current `PatTuple.rest_mode` / `PatList.rest_mode` values are `none`,
  `bind_rest(name)`, and `ignore_rest`;
- current `PatMap.rest_mode` values are `extra_ok`, `bind_rest(name)`,
  `ignore_rest`, and `strict_null`;
- each clause and case arm also carries `compiled_pattern =
  HCompiledPattern(pattern_ir, match_program)` as a deterministic matcher
  bridge;
- current `pattern_ir` forms are `PIrLiteral`, `PIrBind`, `PIrIgnore`,
  `PIrConst`, `PIrPin`, `PIrMatcherExpr`, `PIrTuple`, `PIrList`, `PIrMap`,
  `PIrMapField`, `PIrHead`, `PIrKwField`, `PIrAs`, `PIrOr`, and `PIrDynamic`;
- `PIrTuple` / `PIrList` now carry normalized sequence-rest hints:
  `rest_mode`, `rest_kind`, `capture_rest`, optional `rest_binding`,
  `min_arity`, and `exact_arity`;
- `PIrMap` now carries runtime-facing rest hints in addition to `rest_mode`:
  normalized `rest_kind`, `strict_map`, `capture_rest`, `ignore_rest`,
  `needs_full_map`, optional `rest_binding`, and `requested_keys[]` as
  `PIrRequestedKey(name)` entries;
- `PIrHead` now carries `destructure_mode = POSITIONAL | KEYS | MIXED |
  HEAD_ONLY`, plus `requires_deconstruct`, `requires_deconstruct_keys`,
  `needs_full_map`, and optional `requested_keys[]`;
- dynamic matcher patterns lower as `PatDynamic(matcher_text,
  export_map_pattern?)`, and the compiled layer keeps the same optional
  `export_map_pattern` bridge under `PIrDynamic`;
- compiled matcher-expression nodes now also carry lowered `matcher_expr`
  subtrees, so later bytecode/runtime layers do not need to re-parse
  `expr_text` / `matcher_text`;
- `PIrDynamic` now also carries `protocol = DynamicMatchResult`,
  `binding_mode = forbid_bindings | map_pattern`, and
  `requires_empty_bindings` so later runtime lowering can enforce the
  explicit-binding profile without re-deriving it from surface syntax;
- `match_program` is a second deterministic lowering view under
  `HCompiledPattern`: `PProgram(root, binding_order[], requires_commit)` plus
  structured nodes such as `PWildcard`, `PBind`, `PPin`, `PLiteral`,
  `PConstMatch`, `PMatcherExpr`, `PSeqTuple`, `PSeqList`, `PMap`, `PHead`,
  `PAs`, `POr`, and `PDynamic`;
- explicit block-param patterns lower through `HClosure(signature,
  param_patterns[])`, where each `HParamPattern(param_slot, pattern,
  compiled_pattern)` bridges incoming raw args to the same compiled matcher
  pipeline used by `case` and clause dispatch; when a block param needs a
  synthetic raw input slot such as `__arg0`, the owning closure procedure also
  reserves that synthetic param local explicitly in `procedures[].locals`;
- pattern assignment lowers as `HPatternAssign(pattern, compiled_pattern,
  value, fail_mode = match_error)`;
- method signatures retain lowered `default_expr` nodes on `HParam` entries,
  so later bytecode lowering can compile canonical `default_thunk` code
  objects without re-reading parser AST;
- clause bodies are lowered inline as `HClause(subject_kind, pattern, guard?,
  body)`, and `case` arms use the same `pattern` node family via `HMatchArm`,
  while the owning method still keeps a normal procedure entry for local slot
  layout and nested closures;
- `subject_kind` uses spec-aligned names:
  `single_positional | positional_tuple | named_args_map`;
- async intrinsics and typed/runtime hooks are not part of this slice yet, but
  executable matcher planning no longer depends on ad hoc surface re-analysis.
