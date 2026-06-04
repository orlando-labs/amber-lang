# amber.ast.v1

Status: seed contract.

The parser must produce a syntax-faithful AST before any HIR lowering. Every node carries:

- stable `kind`;
- source `span` with file, start, and end positions;
- semantic fields in source order;
- child nodes in source order.

The dump format follows the spec-level `amber.ast.v1` JSON envelope. `CHAIN_DOT` is a lexer/parser boundary marker and does not have to survive as a standalone AST node if the postfix chain plus block suffix preserves the source boundary.

## Postfix chains

Postfix syntax is represented as a surface-faithful chain instead of being
lowered into ordinary call/member nodes:

- `AstPostfixChain` has a `base` node and ordered `tails`;
- member tails are `AstTailDotMember` and `AstTailSafeMember`;
- call tails are `AstTailCall` and `AstTailSafeCall`, with `call_style` set to
  `paren` or `bare` where applicable;
- index tails are `AstTailIndex` and `AstTailSafeIndex`;
- block suffixes are preserved as `AstTailBlockSuffix` immediately after the
  call/member segment they bind to;
- inline block suffixes store `AstBlock.body` as a single expression node, while
  indented block suffixes store `AstBlock.body` as a statement list;
- `AstTailDotMember.chain_boundary = true` marks a `CHAIN_DOT` continuation
  after a one-line block suffix.

## Collection Literals

- `AstListLiteral(elements[])` represents `[expr, ...]`;
- `AstTupleLiteral(elements[])` represents `()` and parenthesized expressions
  with a comma, while a single parenthesized expression without a comma remains
  `AstGroup(expr)`;
- `AstSetLiteral(elements[])` represents non-empty `{expr, ...}` forms that do
  not parse as map entries;
- `AstMapLiteral(entries[])` represents `{key: value, ...}` in expression
  context, with each `AstMapEntry(key_kind, key, value)` preserving whether the
  key was written as an identifier/symbol or string key;
- symbol literals are `AstLiteral(token = SYMBOL, value = name)`.

Conditional collection elements preserve their surface condition in the parser
AST:

- list elements with trailing `if` / `unless` are wrapped as
  `AstArrayElement(expr, condition)`;
- set elements with trailing `if` / `unless` are wrapped as
  `AstSetElement(expr, condition)`;
- map entries keep the same `AstMapEntry` shape and add optional `condition`;
- `condition` is `AstCollectionCondition(kind = if | unless, expr)`.

Unconditional list/set elements remain the expression node itself for backward
compatibility with existing `elements[]` consumers.

## Inline conditionals

Inline conditional expressions use `if condition then consequent else
alternative` and parse as `AstInlineIfExpr(form = inline, condition,
consequent, alternative)`. The parser deliberately rejects C-style
`cond ? a : b` and missing-`else` inline forms with dedicated diagnostics.

## Clause defs

- canonical `def ...:` bodies that begin with `when` / `else` are parsed as
  `AstClauseDef(name, base_signature, clauses[], else_body[])`;
- each clause is `AstClause(pattern, guard_expr?, body[])`;
- case-arm and clause `pattern` fields keep the surface text instead of
  lowering into matcher IR at parse time;
- one-line block suffix params are now `AstPatternParam(pattern)` entries, so
  destructuring block params and pin/list/map/head patterns survive parsing as
  syntax-faithful surface text;
- destructuring assignment is a dedicated `AstPatternAssign(pattern, right)`
  statement-form rather than a special case of ordinary `AstAssign`;
- simple many-def sugar is normalized eagerly when it is syntactically
  unambiguous:
  - `def f(0): ...`
  - `def f(x) if guard: ...`
- consecutive same-name simple many-def entries merge into one `AstClauseDef`;
- the normalized many-def `base_signature` is synthetic positional
  `__arg0..__argN`, so clause-local names remain part of the clause pattern
  rather than the method signature.
