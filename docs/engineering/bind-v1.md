# amber.bind.v1

Status: seed contract.

The binder produces a deterministic lexical scope graph after AST parsing and
before HIR lowering. The dump is intentionally semantic-light: it records what
names exist and which source references resolve to them, but it does not perform
call binding, default evaluation, object-model verification, or lowering.

Top-level shape:

- `format`: always `amber.bind.v1`;
- `module`: package name or `null`;
- `scopes`: lexical scopes in stable discovery order;
- `bindings`: bindings in stable declaration order;
- `signatures`: canonical signature descriptors for function/method scopes;
- `references`: source references in traversal order;
- `exports`: module export records;
- `source_hash`: SHA-256 of the source file.

Scope kinds currently emitted:

- `module`;
- `function`;
- `class_method`;
- `class`;
- `mixin`;
- `block`.

Clause-style `def` uses the same `function` scope kind as ordinary `def`. Each
`when` body and `else` body gets its own `block` scope under that function.
`case` / `case!` arms also get dedicated `block` scopes so pattern locals are
visible to both the arm guard and the arm body.

Binding kinds currently emitted:

- `local`;
- `import_alias`;
- `constant`;
- `ivar`;
- `cvar`;
- `placeholder`;
- `last_value`.

At module scope, the W13 pre-scan declares simple top-level assignment targets
as `local` bindings with role `module_cell` before export resolution and body
visitation. This lets `export value` resolve even when `value = ...` appears
later in the source while preserving source-order module init execution.

`$_` is represented as a read-only `last_value` binding per lexical scope.
Implicit block placeholders `_1.._N` are represented as read-only `placeholder`
bindings in the block suffix scope. Sparse placeholder numbering and mixing
implicit placeholders with explicit block params produce canonical diagnostics.

Binder negative corpus cases use `phase: "bind-diag"` and compare the canonical
`amber.diag.v1` JSON. The current binder emits the mandatory module/scope-level
diagnostics it can prove before HIR lowering, including unknown exports,
duplicate public exports, and writes to read-only import aliases.

Implementation-local binder diagnostics currently reserved by this workspace:

- `B0001`: duplicate lexical binding in one scope, including import alias
  collisions and duplicate parameters;
- `B0002`: wildcard `_` used as an ordinary binding or reference outside the
  pattern semantics where it is allowed to be non-binding.

Signature preflight currently covers the static part of the W2.2 default
pipeline:

- each `AstDefStmt` / `AstClassMethodDef` / `AstClauseDef` scope has one
  descriptor with source-order params;
- each param descriptor stores `external_name`, `local_name`, `kind`,
  `auto_assign_kind`, `auto_assign_target`, `type_expr`, `has_default`, and
  `default_kind`;

- `E1007`: a default expression reads its own parameter or a parameter to the
  right, before that local is available;
- `W1001`: a default expression reads `@field` / `@@field` while the same field
  is also targeted by delayed auto-assign in the signature.

The binder library also exposes a shape-only `bind_call_shape(...)` helper for
the explicit-arg part of `bind_call`. It is not serialized into
`amber.bind.v1`, because module binding cannot statically know dynamic dispatch
targets. The helper returns:

- one source-order slot per parameter with
  `source_kind = positional | keyword | missing`;
- `default_order`: slot indices that still need left-to-right default
  materialization;
- `pending_auto_assigns`: the delayed commit buffer in signature order, using
  slot indices and final field targets;
- canonical preflight diagnostics:

- `E2008`: duplicate keyword argument;
- `E2009`: unknown keyword argument;
- `E2010`: too many positional arguments;
- `E2011`: missing required parameter after explicit bind.

For parser integration, the same library exposes `extract_call_shape(...)`,
which reads `AstTailCall` / `AstTailSafeCall` or a postfix chain ending in one
of those tails and normalizes `AstKeywordArg` nodes into the same
`CallArgShape` sequence consumed by `bind_call_shape(...)`.

For the current pattern slice, binder parses pattern surface text for
clause-style `def`, `case` arms, explicit block params, and pattern assignment.
It extracts local binders so names such as `n`, `x`, `y`, `w`, `h`, `whole`,
map-rest binders such as `rest`, and sequence-rest binders such as `tail`
resolve in the appropriate lexical context. `PatAs(bind_name, inner)`
contributes the outer `bind_name` to the same local commit set as the inner
pattern locals.

Pin patterns and matcher expressions now also contribute read references to the
main scope graph. That means `^x`, `pattern(route(x))`, and case-only bare
matcher expressions such as `limit + 1` participate in ordinary binder
resolution and closure-capture planning, while still being validated against
the pattern-specific rules below.

The same pass now validates the current subset and emits:

- `E1001`: duplicate binding names in one pattern.
- `E1002`: different binding sets across OR-pattern alternatives.
- `E1003`: sequence/map rest pattern outside tail position.
- `E1008`: bare matcher expression outside `case` / `case!`.
- `E1009`: dynamic pattern object in block params or pattern assignment.
- `E1011`: `pattern(expr)` references a binding introduced by the same
  enclosing pattern.

Warnings are retained in `BindResult.diagnostics` but do not make the bind
result fail. The CLI writes warning diagnostics to stderr and still writes the
`amber.bind.v1` graph to stdout.
