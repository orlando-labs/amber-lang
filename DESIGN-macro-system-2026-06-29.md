# DESIGN: Macro system — compile-time AST metaprogramming

Date: 2026-06-29
Status: design / language-extension proposal — no code change in this doc.
Also a proposed **amendment to spec Q3**, which currently lists "DSL/macros"
as deliberately out of scope (`amber_unified_final_spec.md` §Q3, line ~3311).
Scope: a hygienic, compile-time macro system for Amber. Defines where macro
expansion sits in the pipeline, the first-class `Ast` value model, `quote`/
`unquote` (the kernel) and the `#{}` / `%`-control template surface, the single
authoring form (`macro def`, template-default body), the trigger surfaces
(call family: plain call and explicit dot-call / block-suffix / annotation /
`use`),
hygiene, the compile-time evaluation sandbox, module staging order,
diagnostics, and the interaction with the binder, the typed profile, the
frozen-world model, and the existing runtime MOP.
Follows: `amber_unified_final_spec.md` §1–§2 (pipeline + architectural
invariants), §5.1–5.3 (F0 lexer / F1 parser+AST / F2 binder), §8.13–§8.16
(open classes, reflective `define_method`, `send`, `method_missing`), Q3
(minimal MOP profile), Q4 (typed profile + reflective `Any`-boundary rule),
and the existing `frontend/pattern/` + `amber.pattern.v1` decision-program
infrastructure.

This doc is the *contract / shape* step. It deliberately stops short of the
expander VM's exact opcode surface, the `Ast.*` node-builder signatures for
every node kind, and the hygiene-renaming algorithm's data structures — those
are the implementation, gated on the contract here being agreed.

---

## 1. Where macros sit

Amber already ships three things a macro system normally has to
invent from scratch:

1. **A syntax-faithful, serialized, golden-tested AST** — `amber.ast.v1`
   (§1 pipeline, line ~21392; artifact table, line ~21422; invariant #1,
   line ~21408). This is a ready-made homoiconic data model. Elixir had to
   *design* `{form, meta, args}`; Amber already shipped an equivalent as a
   public contract with a JSON serializer and a fixed node schema.
2. **A bootstrapped bytecode VM** (`runtime/vm.cpp`, the `amber.bc.v1`
   execution path). Macro bodies can be ordinary Amber `def`s compiled to
   `.amberbc` and executed *on that same VM* at compile time — the Elixir
   hosting model (macros are host-language functions), with none of Rust's
   separate-proc-macro-crate ceremony.
3. **An open-world → frozen-world lifecycle** (invariant #5, line ~21412) and
   a **runtime MOP** — open classes (§8.13), reflective `define_method`
   (§8.14), `send` (§8.15), `method_missing` (§8.16). Macros are the missing
   *compile-time* complement to that runtime MOP.

The thesis of this design: **a macro is the most compile-time member of the
open world.** It runs strictly before the binder, emits ordinary expanded
AST, and therefore produces code that is fully bound, type-checked
(under the typed profile), and devirtualizable — with **zero macro-dispatch
runtime cost**.
This is the precise contrast with the runtime MOP, which Q4 (line ~3329)
defines as an `Any`-boundary on the reflective slow path:

| Mechanism            | Phase        | Cost            | Typed?                  |
|----------------------|--------------|-----------------|-------------------------|
| Macro (this doc)     | compile-time | no macro dispatch at runtime | yes — output is checked |
| `method_missing`/`send` (§8.15–16) | runtime | reflective slow path | no — `Any`-boundary (Q4) |
| `define_method`/reopen (§8.14)     | runtime (pre-freeze) | dispatch invalidation | no — open-world `Any` |

Macros and the runtime MOP are therefore **complementary, not redundant**: a
DSL that can be resolved statically (routing tables, derived accessors,
capability mixins like `Comparable`) should expand via macros and pay no macro-dispatch cost at
runtime; a DSL that is genuinely dynamic (data-driven dispatch) stays on the
MOP.

## 2. Non-goals

- **Not** token-stream macros (the Rust proc-macro `TokenStream` model). Amber
  has a stable AST contract; macros operate on AST, not tokens. This keeps
  invariant #1 intact and means no macro ever re-parses.
- **Not** a second toolchain / plugin-crate model. Macros are Amber, run on the
  Amber VM. No native plugin loading, no FFI requirement.
- **Not** `instance_eval`/`class_eval`-style runtime `self`-rebinding (Q3 keeps
  those out; macros make them unnecessary for DSL bodies).
- **Not** unrestricted compile-time IO. The expander is sandboxed (§10).
- **Not** hot-reload of macros (Q3 keeps hot reload out).
- **Not** in the minimal/static core by default. Macros are an **opt-in
  profile** (§14), exactly like the typed profile is opt-in (Q4).

## 3. Pipeline placement: the expansion pass

The current pipeline (§1, line ~21389):

```text
source.am -> tokens -> amber.ast.v1 -> amber.diag.v1 -> amber.hir.v1
          -> pattern decision program -> amber.bc.v1 -> .amberbc -> VM
```

Macros add **one new pass, F1.5, between F1 (parser) and F2 (binder)**:

```text
... -> amber.ast.v1 --[F1.5 macro expansion (fixpoint)]--> amber.ast.expanded.v1
    -> binder(F2) -> ...
```

Hard requirements on F1.5:

- **Parsed surface AST in, expanded ordinary AST out.** Expansion consumes the
  syntax-faithful parser AST and produces ordinary expanded `amber.ast.v1` for
  downstream compilation. Dedicated macro surfaces (`macro def`, attributes,
  `use`) parse explicitly; ordinary/dot call syntax remains ordinary call AST
  until F1.5 resolves the callee in the compile-time macro namespace.
- **No macro opcodes downstream.** Binder, checker, HIR lowering, bytecode
  emission, verifier, and runtime see ordinary expanded Amber. The main
  downstream change is name-identity plumbing for hygiene (§9), not a second
  semantic pipeline.
- **Fixpoint.** A macro may expand into a call to another macro. F1.5 re-walks
  until no macro-invocation nodes remain or a configurable expansion-depth
  limit is hit (diagnostic `AMB_MACRO_EXPANSION_LIMIT`).
- **Deterministic** (invariant #7). Same input AST ⇒ byte-identical output AST.
  This constrains the expander environment (§10).
- **Origin-preserving** (§12). Output nodes keep user-facing call-site spans,
  while the expansion trace records macro-definition spans and backtrace edges.

Two artifacts mirror the existing dump convention:

- `amber.ast.expanded.v1`: post-expansion ordinary AST (`amberc expand --json`);
- `amber.macrotrace.v1`: deterministic expansion trace with macro name,
  provider module, call-site span, definition span, generated-node origin, and
  hygiene context ids.

If hygiene contexts or dual spans are serialized inline rather than sidecar,
that is an explicit `amber.ast.v1` schema extension / format bump, not an
implicit field added to every node.

## 4. The `Ast` value model

Macros manipulate AST as **first-class Amber values**. We expose the expanded
`amber.ast.v1` node schema, plus macro-profile surface nodes, as a stdlib module
`Ast` whose constructors and accessors mirror the serialized schema and
macrotrace identity model. Because the serializer already exists, this is
mostly a binding exercise; the new design work is deciding which macro metadata
lives inline versus in `amber.macrotrace.v1`.

- Each node kind is a value: `Ast.Call`, `Ast.If`, `Ast.Def`, `Ast.Ident`,
  `Ast.Lit`, `Ast.Block`, `Ast.ClassDecl`, etc. — one per `amber.ast.v1` node.
- Nodes are **immutable, shareable** values (like frozen `Str`), so they cross
  strands freely and never alias caller state.
- Nodes carry source `span` metadata. Macro-expanded nodes use the call-site
  span for primary diagnostics and link to definition/origin metadata through
  `amber.macrotrace.v1`.
- Hygienic identifiers carry name identity `(text, syntax_context)` (§9). The
  `syntax_context` is exposed through `Ast` for identifier nodes, but may be
  serialized as trace/sidecar metadata rather than as a field on every AST node.
- Introspection: `Ast.kind(node)`, field accessors, and source rendering through
  both `node.source` and `node.to_source` (matching Amber's coercion convention:
  `.int` / `.to_int`, etc.). These render back to surface text and are required
  by the `assert` example below.
- `Ast.gensym(:base)` mints a deterministic fresh hygienic identifier.

This module is the single integration point between "macro code" (ordinary
Amber) and "the compiler's AST." It is available **only** inside the macro
profile / expander environment; ordinary runtime code does not get `Ast.*`.

## 5. `quote` / `unquote`

Authoring AST by hand via `Ast.*` constructors is unbearable, so the primitive
is a quasiquote, identical in spirit to Elixir's `quote`/`unquote`:

```amber
quote:
  if not unquote(cond):
    raise AssertionError.new(unquote(msg))
```

- `quote: <amber>` parses its body with the *normal parser* and yields the
  corresponding `Ast` value. It is itself lowered by F1.5: the parser produces a
  `quote` node, and expansion turns it into the `Ast.*` builder calls that
  reconstruct the body.
- `unquote(expr)` evaluates `expr` (an `Ast` value) at expansion time and
  splices it into the surrounding quote at that position.
- `unquote_splice(list)` splices a `List[Ast]` as siblings (Elixir
  `unquote_splicing`), e.g. to emit N statements or N args.
- Identifiers written literally inside `quote` are **hygienic** by default
  (§9); identifiers spliced via `unquote` keep the context they already carry.

This `quote`/`unquote`/`Ast.*` form is the **kernel**. Almost no one writes it
directly: §6 gives a `#{}` / `%`-control template surface that lowers to it.
The kernel stays as the desugaring target and the escape hatch for AST surgery
that templating can't express cleanly.

## 6. Authoring a macro (`macro def`)

A macro is an ordinary `def` tagged `macro`. It runs at expansion time, receives
its arguments as **unevaluated `Ast` values**, and its body is a **template by
default** — it reads like the code it emits, with `#{}` punching the holes:

```amber
macro def assert(check):
  if not #{check}:
    raise AssertionError.new("assertion failed: " + #{check.source})
```

A use of this macro with argument `x > 5` expands (in F1.5) to:

```amber
if not (x > 5):
  raise AssertionError.new("assertion failed: " + "x > 5")
```

Two properties define it as a macro, not a function: `check` is **not
evaluated** before the macro runs, and `#{check.source}` recovers the
argument's *source text* — a function only ever sees the runtime value `false`
and can never reproduce `"x > 5"`.

**Splice forms (the `#{}` surface).** `#{}` reuses Amber's own string-
interpolation hole; a quote is just a template for code instead of text:

- `#{expr}` — splice an `Ast` value here (sugar for `unquote`, §5).
- `#{expr.source}` / `#{expr.to_source}` — evaluate the compile-time
  member/method expression and splice the resulting `Str` as a string literal
  AST. Both forms exist: `.source` is the ergonomic coercion property, and
  `.to_source` is the explicit conversion method.
- `#{*listexpr}` — splice a `List[Ast]` as siblings (sugar for
  `unquote_splice`), e.g. N statements or N call args.

**Template-default body + `%`-control lines.** A `macro def` body is emitted by
default. Compile-time code that runs but is **not** emitted goes on `%`-prefixed
lines — the same control/emit convention the multiline-string / html-tag design
already uses (`%`-line = control, `#{}` = emit). This recovers repetition and
compute-time locals with no further sigil:

```amber
macro def trace(*exprs):
  %for e in exprs:                         # runs at expansion, not emitted
    puts(#{e.source} + " = " + #{e})       # emitted once per element

macro def derive_eq(cls):
  %fields = Ast.fields(cls)                # compile-time binding
  def ==(other):
    all?(#{*fields.map |f|: field_eq(f)})  # every field compared, none exposed
```

`%` is a control line only at line start; mid-expression `%` stays modulo (the
disambiguation the html-tag design already fixes, §17).

Template lowering is mechanical: non-`%` body forms are wrapped in an implicit
`quote`, `%` lines are compile-time statements, and `#{...}` / `#{*...}` lower
to `unquote` / `unquote_splice`. An explicit `return Ast...` remains available
as the escape hatch for macro bodies that want to bypass the template-default
body and construct nodes directly.

> **Rest parameters.** Variadic macro params (`*exprs`) use the same
> definition-site rest parameter surface as ordinary Amber. The macro ABI still
> must pin the compile-time value shape: `*exprs` should bind an immutable
> `Tuple[Ast]`, and `**kwargs` should bind a deterministic, name-indifferent
> `Map[Str, Ast]`.

`macro def` is the full-power form: arbitrary Amber computation over `Ast`
values on `%`-lines, helper functions, the lot — bounded only by the sandbox
(§10). The §5 kernel is what `#{}` and `%`-lines lower to.

## 7. One authoring form, not two

An earlier draft of this design proposed a second, declarative rule layer
(a Rust `macro_rules!`-style `pattern => template` form). It is
**dropped.** The reasoning that justifies two forms in Rust does not transfer:

- **Rust needs `macro_rules!` because its procedural macros are heavyweight** —
  a separate `proc-macro` crate, `syn`/`quote`, slow builds. The declarative
  form is the lightweight escape from that ceremony. In Amber a `macro def` is
  an ordinary `def` on the already-bootstrapped VM (§1); there is no ceremony to
  escape, so the terseness argument nearly vanishes once `#{}` templating (§6)
  exists.
- **Elixir — the model Amber actually matches — has one form (`defmacro`)** and
  no declarative layer, for precisely this reason.
- **Amber already owns what the declarative rule layer was for.** Shape-matching
  over a macro's `Ast` args is done with `case`/`case!` (exhaustive under the
  typed profile, more capable than a bespoke `=>` grammar); multiple arities
  are done with multi-clause `def` (the existing many-def mechanism). No new
  pattern grammar is needed.

Dropping the declarative layer also drops its sigils: with no pattern grammar
there are no `$name:frag` captures and no `$(...)*` repetition. **The only
template sigil is `#{}`** (plus `%`-control lines). Net authoring surface: one
construct, one template hole.

Amber grammar, which is `:`-and-INDENT based with `;` as a mere separator. The
single-form `swap` is written in the §6 style:

```amber
macro def swap(a, b):
  tmp = #{a}        # `tmp` is hygienic (§9)
  #{a} = #{b}
  #{b} = tmp
```
)

## 8. Trigger surfaces

Where a macro invocation is recognized in source. Five families, all built from
syntax Amber already has:

1. **Function-like calls** — ordinary calls resolve to macros when the callee is
   a compile-time macro binding:

   ```amber
   assert(x > 5)
   ```

   The parser keeps this as a syntax-faithful ordinary call. F1.5 consults the
   compile-time macro namespace before the binder runs: if `assert` resolves to
   a macro, the call expands; otherwise the call remains ordinary runtime Amber.
   This keeps macro calls familiar while avoiding a new sigil.

   Explicit dot-call remains available for cases where the callee is already a
   macro/callable value or where the author wants the call-channel to be
   visually explicit:

   ```amber
   assert.(x > 5)
   ```

   The earlier `name!(args...)` candidate is rejected for v1: `!` already
   belongs to Amber's bang-method culture (`destroy!`, mutating collection
   methods) and effect-row syntax, so it is the wrong default marker for macros.
2. **Block-suffix** — `name: <block>` / `name |args|: <block>`, reusing the
   existing block-suffix surface. This is the most Amber-idiomatic DSL entry and
   what the web framework (§ web-DSL sketch) leans on.
3. **Annotation** — a declaration is annotated by one or more *bare* macro calls
   on their own lines immediately above it, with **no sigil**:

   ```amber
   test 'rejects empty input'
   desc 'validates and stores a record'
   route '/api/v1/users'
   def create(params): ...
   ```

   An *attribute macro* receives the annotated declaration's `Ast` (plus its own
   call arguments) and returns replacement / augmented declarations — `route`
   on a `def`, `memoize` / `deprecated` on a method, per-field `skip` / `rename`
   inside a class body. This is the Phoenix/Rails-style decorator surface, and
   the sigil-less spelling is deliberate: Elixir's `@doc` / `@spec` attach to the
   following `def` exactly this way, but Amber cannot spend `@` — it is already
   the instance-variable sigil (`@field` / `@@field`). Every language that spells
   decorators `@` (Python, Elixir, Kotlin) is one *without* an `@ivar`; Amber is
   not, so the sigil is dropped rather than replaced.

   **Attachment rule.** The parser keeps each such line as an ordinary call node.
   F1.5 attaches a *contiguous run* of lines that resolve in the macro namespace
   to *attribute macros*, appearing immediately above a declaration with no blank
   line between, to that declaration in **source (top-to-bottom) order**. A blank
   line or an intervening non-annotation statement breaks the run; an
   attribute-macro call with no declaration under it is a diagnostic
   (`AMB_MACRO_DANGLING_ANNOTATION`). Non-macro lines stay ordinary statements.

   **Cost of no sigil.** A sigil advertises "metadata about the next decl, not
   executable prelude." Without one, `route '/api'` is locally indistinguishable
   from a side-effecting call; whether it annotates depends on non-local macro
   resolution, so a reader — or a naive highlighter — cannot tell from the line
   alone. This is the accepted trade for not spending a scarce sigil (`@`/`@@`
   are ivar/cvar, `#`/`#{` are comment/splice, `!`/`?` are bang/predicate); it
   matches Elixir/RSpec/Sinatra practice, where bareword DSL is already idiomatic.
4. **`use Module`** — an injection macro (`__using__`-style) that emits members
   into the enclosing class/module body. Resolved at compile time, so the result
   is statically typed, in contrast to runtime `include` (open-world `Any`).
   This is the home for capability mixins that derive a family of methods:
   `use Comparable` synthesizes `<`, `<=`, `>`, `>=`, `==` from a single
   `compare`; `use Enumerable` synthesizes `map` / `select` / `reduce` from
   `each`. These are legitimately whole-object — they build on one method the
   author defines and read no fields. Field-reading serialization (`use Json`)
   lives here too, but is **not** a naive whole-ivar dump: a model carries fields
   that must never cross the boundary (secrets, cache/dirty flags, derived
   state), so serialization composes `use Json` for the capability with per-field
   `skip` / `rename` attribute macros (item 3) for policy — mirroring serde's
   `#[serde(skip)]` and Jason's `only:`. Serialization is therefore a
   two-surface feature, not the one-liner "killer app" it is sometimes sold as.
5. **String tag** — `sql"""…"""` / `html"""…"""`: an identifier immediately
   followed (no whitespace) by a text-block opener invokes a **string-tag
   macro** on the literal (JS tagged templates / Scala interpolators). The
   macro receives a dedicated `Ast.StringTemplate` node — post-dedent static
   chunks plus unevaluated interpolant `Ast`s and the text/raw flag — not a
   call's argument list, so the surface is declared at the definition site:

   ```amber
   string_tag macro def sql(t as Ast.StringTemplate) -> Ast:
     Sql.expand(t, dialect: Sql.Postgres)
   ```

   The tag head in v1 is a plain (possibly import-renamed) macro identifier,
   not an arbitrary expression or dotted path — static resolution, and import
   aliases already solve dialect/provider selection. Payload shape, dedent
   interaction, and the SQL/HTML sanitization contracts live in the multiline
   text-block design (`DESIGN-multiline-string-literals` §7); this doc owns
   the trigger, the `string_tag` definition modifier, and resolution.

   **Surface kinds are declared, not inferred.** `string_tag` occupies a
   definition-modifier slot before `macro def` (the same contextual-modifier
   grammar as `native`). A tag macro *must* be declared (nothing at the use
   site could classify it); the slot is also the growth path for making the
   other special surfaces explicit (`annotation macro def …`) if the v1
   inference below proves too implicit.

All five families feed the same expansion mechanism. Dedicated macro surfaces
produce parser AST nodes that F1.5 recognizes directly; ordinary and explicit
dot-calls are syntax-faithful call nodes that F1.5 classifies by resolving the
callee in the compile-time macro namespace. In every case, expansion replaces
the invocation with `Ast` returned by the macro body.

**Macro ABI and expansion cardinality.**

- A function-like macro receives unevaluated call arguments as source-order
  `Ast` values plus call-shape metadata (plain call vs explicit dot-call,
  positional, keyword, spread, block channel if present). In expression
  position it must return exactly one expression AST. In statement/declaration
  position it may return one node or a `List[Ast]` only where the surrounding
  grammar accepts sibling statements or declarations.
- A block-suffix macro receives the callee surface, arguments, and an
  `Ast.Block` whose params/body are still unevaluated AST. v1 spelling: the
  block arrives as the macro's trailing `Ast` argument (kind `AstBlock`);
  splicing it at statement position flattens it into its statements.
- An attribute macro receives the annotated declaration AST (and its own call
  arguments) and returns a replacement declaration or a deterministic list of
  declarations. When several stack on one declaration they compose in source
  (top-to-bottom) order per the §8 attachment rule, each macro seeing the
  previous one's output (§17.5, resolved); only the last annotation in a stack
  may expand to multiple declarations. v1 classification: a call is
  annotation-shaped when the macro's declared arity is exactly one more than
  the arguments passed — the extra (final) parameter receives the declaration.
  Macros with rest/keyword/defaulted params are not annotation-capable in v1
  (the +1 test would be ambiguous); an explicit `annotation` definition
  modifier (item 5's slot) is the successor if inference proves too implicit.
- A string-tag macro receives one `Ast.StringTemplate` (see item 5 and the
  multiline text-block design §7) and must return exactly one expression AST.
- A `use` macro receives the enclosing class/mixin/module context and returns
  member declarations. Generated `macro def` declarations are disallowed in v1;
  macro providers must be known before expansion staging starts (§11).
- Macro diagnostics should be emitted through `Macro.error(...)` /
  `Macro.warn(...)` with explicit spans. Throwing from a macro body is reserved
  for macro implementation failure and is reported as a compiler diagnostic,
  not as an Amber runtime exception.

## 9. Hygiene

Hygiene is automatic and on by default (both Rust and Elixir agree here); the
one invasive change lands in the binder.

- Every identifier carries a `(name, syntax_context)` pair. Identifiers written
  literally inside a `quote` get a **fresh `syntax_context` mark** per macro
  expansion, so a macro-introduced `tmp` can never capture or be captured by a
  caller's `tmp`.
- A fresh context mark is deterministic and collision-free by construction:
  `(macro_provider_id, macro_def_id, call_site_id, expansion_depth,
  local_counter)` is sufficient as the conceptual identity. The rendered name
  may still be `tmp`; the binder key is the identity pair.
- **Binder change (F2, §5.3):** the scope-graph key becomes
  `(name, syntax_context)` rather than `name` alone. This is the single most
  invasive change in the design and where most of the implementation risk and
  review attention belongs. Everything else in F1.5 is local.
- **Escape hatch** (Elixir `var!`): `unhygienic(name)` deliberately binds into
  the caller's context — needed when a DSL *intends* to introduce a visible name
  (e.g. a `result` the block can reference). Use is explicit and greppable.
- Identifiers arriving via `unquote` retain their original context — a value the
  caller passed in resolves in the caller's scope, as expected.

## 10. Compile-time evaluation environment (the expander VM)

Macro bodies execute on a dedicated **expander VM** instance — the same
`runtime/vm.cpp` engine, configured for compile-time use:

- **Pure / sandboxed by default.** No capability grants: no filesystem, no net,
  no clock, no RNG, no environment. This both satisfies the determinism
  invariant (#7) and closes the supply-chain hole that unrestricted Rust
  proc-macros and Elixir compile-time side effects leave open. Amber's existing
  **capability model is exactly the right tool** — the expander VM simply runs
  with an empty grant set.
- **Deterministic surface.** Even otherwise-pure operations that could leak
  nondeterminism are constrained: map iteration must use the deterministic order
  the spec already mandates (invariant #7); `Ast.gensym` draws from the
  deterministic hygiene identity above, not from a global mutable counter, so
  golden output is stable and independent of unrelated expansion order.
- **Bounded.** Expansion depth, step count, and allocation are capped; overruns
  are diagnostics (`AMB_MACRO_EXPANSION_LIMIT`, `AMB_MACRO_BUDGET`), never
  hangs or panics (mirrors the "malformed input ⇒ diagnostic, not panic" rule
  in §5.1).
- **Opt-in capability escalation** (future): a package may request specific
  compile-time capabilities (e.g. read a sibling schema file for codegen) via an
  explicit, audited manifest grant. Off by default; out of scope for v1. Any
  future compile-time filesystem read must be declared as a build input and its
  digest folded into the expansion cache key. Net, clock, RNG, environment, and
  process capabilities remain out of v1.

## 11. Staging and module ordering

Macros must exist before the code that uses them, so the build/compiler graph
gains a compile-time edge class (the Elixir model). The runtime loader remains
unchanged: it consumes verified post-expansion `.amberbc`.

- All modules are parsed first so macro definitions and macro-use edges can be
  discovered from syntax-faithful AST.
- Macro names live in a compile-time namespace parallel to ordinary runtime
  bindings. A plain call is considered for expansion only if its callee resolves
  in that macro namespace; otherwise it remains an ordinary call for F2.
  Ambiguous macro imports are expansion diagnostics, not runtime dispatch
  choices.
- A module's `macro def` declarations and ordinary helper functions reachable
  from them are compiled (parsed → bound → lowered → `.amberbc`) **before** any
  module that invokes those macros is expanded.
- Macro definitions may not (transitively) use macros defined in modules that
  depend on them — **cyclic macro use is illegal** (`AMB_MACRO_CYCLE`).
- A macro body may call ordinary (non-macro) functions, but only ones reachable
  in the already-compiled macro-definition graph; it cannot call into the
  not-yet-compiled user program.
- Generated macro definitions are rejected in v1. Expansion may generate
  ordinary functions/classes/mixins, but not new compile-time providers.

**Exports and imports (resolves §17.4).** Macros ride the existing module
surfaces — there is no `import macro` statement:

```amber
package db.postgres
export macro sql

string_tag macro def sql(t as Ast.StringTemplate) -> Ast:
  Sql.expand(t, dialect: Sql.Postgres)
```

```amber
from db.postgres import sql as psql

rows = conn.execute(psql"""
  SELECT id, name FROM users WHERE active = #{flag}
  """)
```

- **Export marking is explicit and load-bearing.** `export macro name [as
  public]` is the existing export statement with a contextual `macro` marker
  (mirroring `macro def`). The importer's F1.5 must classify imported names as
  macros *without compiling the provider's runtime*, so the macro-marked export
  table is what makes §11's "providers known before staging starts" statically
  true. An unexported `macro def` is module-private — exactly the current
  same-module behavior.
- **Imports are unified.** `from m import sql as psql` and `import m as pg`
  bring macro names through the same statements and alias table as runtime
  names. A module's compile-time namespace = its own `macro def`s plus its
  macro-marked imports. Ambiguous imports (two providers exporting `sql`) are
  expansion diagnostics; `as`-renaming is the resolution tool.
- **Macros are not values.** F1.5 consumes every macro use before the binder
  runs; a leftover reference to a macro name in value position (e.g. passing
  `psql` as an argument) is a compile-time diagnostic, so the compile-time and
  runtime namespaces sharing one alias never leaks into runtime semantics.
- **The artifact carries a macro section.** A provider's `.amberbc` gains a
  table of exported macros: public name, surface kind (call / string-tag /
  annotation), declared signature/arity, and the compiled expander bytecode
  together with its reachable helper graph (the §11 closure). The importer's
  build cache key includes the provider macro-section hash — expansion
  *output*, not just linkage, depends on it.
- **Supply-chain consequence.** An imported macro is third-party code running
  at compile time. The §10 sandbox therefore gates this feature: expander
  capability lockdown ships before cross-module macro imports are enabled.

## 12. Diagnostics, spans, expansion backtraces

- Every emitted node has a primary **call-site span** in the expanded AST, plus
  a macrotrace origin edge to the macro-definition span where the
  `quote`/builder emitted it — the Rust `Span` / Elixir `__CALLER__` story
  without overloading the parser AST node shape.
- Type errors and binder errors in expanded code report against the call site by
  default, with an **expansion backtrace** ("expanded from macro `assert` at
  …") so the user is not shown synthetic code with no source.
- `amberc expand --json` dumps post-expansion `amber.ast.expanded.v1` for
  golden tests; `amberc expand --trace-json` dumps `amber.macrotrace.v1`;
  `amberc expand --source` renders the expanded AST back through the same
  source-rendering surface exposed as `node.source` / `node.to_source` for
  human review.
- All of this rides the existing deterministic-span machinery (invariant #7,
  §5.1 spans); no new span model is introduced.

## 13. Interaction with existing subsystems

- **Binder (F2):** the `(name, syntax_context)` scope key (§9), with macrotrace
  origin data available for diagnostics. Otherwise the binder sees ordinary
  expanded AST.
- **Typed profile (Q4):** expansion happens *before* the checker, so generated
  code is fully type-checked. Crucially, a macro-built DSL is **not** an
  `Any`-boundary — unlike `send`/`method_missing`, which Q4 (line ~3329) pins to
  `Any` outside frozen builds. This is the headline typing advantage of macros.
- **Frozen-world (invariant #5):** macros run before the runtime world exists,
  so they are orthogonal to freeze. A macro that *emits* `define_method`/reopen
  is emitting open-world operations that still obey the freeze barrier at
  runtime — the macro does not bypass it.
- **Runtime MOP (§8.13–16):** unchanged. Macros are the compile-time sibling;
  the table in §1 is the guidance on which to reach for.
- **HIR (invariant #2):** receives ordinary post-expansion AST and eliminates
  surface sugar as today. There are no macro HIR nodes or bytecode opcodes.

## 14. Profile gating and the Q3 amendment

Q3 (line ~3294) fixes a **minimal MOP profile** and explicitly excludes
"DSL/macros" (line ~3311). This design proposes adding macros as an **opt-in
build profile**, parallel to how Q4 makes the typed profile opt-in:

- A package opts in through the existing build-profile mechanism, e.g.
  `profiles.required = ["core.v1", "macro.v1"]`. The feature is recorded in
  `.amberbc` `PROF` metadata like other profile features; the minimal/static
  core is unchanged for packages that do not require `macro.v1`.
- `macro def`, `quote`/`unquote`, `Ast.*`, and the plain-call / explicit
  dot-call / block-suffix / annotation / `use` macro surfaces are available
  **only** under the macro profile.
- The amendment to Q3 is narrow: move "DSL/macros" from "consciously excluded"
  to "available as an opt-in profile (see DESIGN-macro-system)", keeping the
  compiler-friendly minimal core as the default.

## 15. Definition of done

1. `Ast.*` value model mirrors expanded `amber.ast.v1` plus macro-surface
   nodes, round-trips through `node.source` / `node.to_source` and the existing
   serializer (golden corpus).
2. `quote`/`unquote`/`unquote_splice` produce correct `Ast` values (golden).
3. F1.5 fixpoint pass: `amberc expand --json` stable; expands `macro def`;
   depth/budget limits are diagnostics, not hangs.
4. Hygiene corpus: macro-introduced names never capture/are-captured; the
   `swap`/`tmp` and `unhygienic` cases pass; binder scope-key change verified.
5. Macrotrace corpus: call-site spans, definition spans, expansion backtraces,
   and generated-node origins are deterministic and useful.
6. Macro ABI/cardinality: expression macros return one expression; statement /
   declaration macros can return legal sibling lists; invalid cardinality is a
   diagnostic.
7. All trigger surfaces (plain call, explicit dot-call, block-suffix,
   annotation, `use`).
8. Sandbox: expander VM has no capabilities; a macro attempting IO is a
   diagnostic; expansion is deterministic across runs (byte-identical dumps).
9. Staging: cross-module macro use works; cyclic macro use is `AMB_MACRO_CYCLE`.
10. Diagnostics: expansion backtraces on errors in generated code.
11. Typed-profile interop: expanded DSL is type-checked with no `Any`-boundary.
12. The web-DSL sketch (companion doc) compiles end-to-end on this substrate.

## 16. Phased implementation plan

- **M0 — surface/profile contract.** *(done)* Reserve `macro.v1`, add parsed
  macro-surface AST nodes, define `amber.ast.expanded.v1` /
  `amber.macrotrace.v1`, and define plain-call / explicit-dot-call macro
  resolution (explicitly not `!`).
- **M1 — AST as data.** *(done, minus `Ast.gensym`)* `Ast.*` model +
  `node.source` / `node.to_source`.
  Foundation; no expansion yet.
- **M2 — quote/unquote + `macro def` + F1.5 (no hygiene).** *(done)*
  Procedural macros expand to a fixpoint.
- **M3 — hygiene.** *(done)* Binder `(name, syntax_context)` key +
  `unhygienic`. Closes the M2 hole. This is the deep milestone.
- **M4 — `#{}` + `%`-control template surface.** *(template-default bodies +
  `#{expr}` / `#{expr.source}` / `#{expr.to_source}` / `#{*list}` splices
  done; `%`-control lines pending)* All lowering to the §5 kernel.
- **M5 — trigger surfaces.** *(done except `use`, which moves to M6 with the
  staging it depends on)* Plain-call and explicit-dot-call macros,
  block-suffix macros (incl. the paren-less `name:` statement form),
  annotation (attribute) macros with the §8 attachment rule (contiguous run
  above a declaration, source order, `AMB_MACRO_DANGLING_ANNOTATION`), and
  statement-position expansion cardinality (AstBlock / `List[Ast]` sibling
  splicing).
- **M6 — sandbox + staging + cross-module macros.** *(sandbox + budgets,
  Ast introspection accessors, call-site diagnostic spans, the export/import
  surface (parse-only pre-pass staging in manifest builds), `use`, and the
  string-tag trigger (2026-07-06: `string_tag macro def` modifier; the tag
  invocation is classified in F1.5 — an identifier bare-called with an
  ADJACENT block string literal, `sql """…"""` with a space stays an ordinary
  call; the literal is re-kinded `Ast.StringTemplate` at hand-off, same parts
  model; misuse through any ordinary call channel, `use`, or annotation
  position is a located diagnostic; exports/imports carry the surface kind)
  are done; pending: persisted artifact macro section, module-alias macro
  calls, full macrotrace backtraces)* Expander capability lockdown and budgets
  (gates everything below); build-graph ordering + cycle detection
  (`AMB_MACRO_CYCLE`); the §11 export/import surface (`export macro`, unified
  imports, artifact macro section); `use Module`; expansion backtraces.

## 17. Open questions

1. **`quote` block delimiter.** *Resolved:* reuse the `:`-block, gated on a
   newline after the colon (`quote:` + NEWLINE + INDENT), which leaves map keys
   and inline `name:` labels untouched. The same gate powers the paren-less
   block-suffix statement form (§8.2).
2. **`#{` vs `#` comment lexing.** *Resolved:* the lexer tracks the `macro def`
   body as a template region (armed by `macro` + `def`, opened at the header
   colon / body INDENT, closed at the matching DEDENT); inside it `#{` opens a
   splice hole — including at line start — and a bare `#` still starts a
   comment. Accepted cost: a comment beginning exactly with `#{` inside a
   macro body is now code.
3. **`%`-control vs modulo lexing.** Line-start `%` opens a control line;
   mid-expression `%` stays modulo. Reuse the multiline-string / html-tag
   design's existing rule verbatim, or restate it for macro bodies? (Open —
   pending the M4 `%`-control implementation.)
4. **Plain-call macro resolution.** *Resolved — see §11 "Exports and
   imports":* `export macro name` marks macro exports on the existing export
   statement; imports are unified (`from m import sql as psql`, `import m as
   pg`) with no `import macro` form; the artifact carries a macro section; the
   sandbox gates cross-module enablement. The invariant stands: `name!(args…)`
   is rejected; plain calls and explicit dot-calls are the call forms.
5. **Attribute-macro ordering.** *Resolved:* source (top-to-bottom) order,
   each macro seeing the previous one's output; only the last annotation in a
   stack may expand to multiple declarations (§8 ABI).
6. **`Ast` introspection surface.** *Accessors resolved & landed:* a nullary
   selector on an `Ast` value reads the amber.ast.v1 field of that name —
   Str for string fields, Bool for bool fields, `Ast` for child nodes
   (aliasing the shared immutable root, `.source` preserved on subtrees),
   `List[Ast]` for node lists (`bin.op`, `bin.left`, `blk.params`,
   `blk.body`, and `tpl.parts` once templates exist). Field names are the
   schema names — no renamed conveniences. Still open: `case`/`case!`
   shape-matching over `Ast` values (§7) as the ergonomic layer on top.
6. **`gensym` rendering.** Hygiene identity is deterministic (§9), but
   `node.source` / `node.to_source` / `amberc expand --source` must choose how
   much of that identity to reveal for generated names (`tmp`, `tmp#1`, or
   trace-only metadata).
7. **Compile-time capability escalation** (§10) — defer to a later RFC, or
   reserve manifest syntax now?
