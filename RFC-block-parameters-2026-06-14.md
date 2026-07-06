# RFC: User-Defined Block Parameters for Amber

**Status:** Draft / for discussion
**Author:** (expert review)
**Date:** 2026-06-14
**Scope:** `def` / `class_method def` signatures, the `&` sigil, block invocation and forwarding, `define_method` signatures
**Builds on:** §4.2 (block suffix), §4.6 (callable references + `.()`), §9.6 (block arity), §10 (multi-clause `def`), §12.7.4 (blocks as hidden final argument), §15.7 (parameter grammar), bare-nullary RFC (accepted 2026-06-12)

---

## 1. Summary

Today only builtin/stdlib methods (`map`, `select`, `reduce`, `each`, `times`…) can consume a block — §12.7.4 routes the block through the frame's hidden `block_slot`, and there is no surface syntax for a user `def` to name or invoke it. A user cannot write their own `map`.

This RFC closes that gap with the smallest possible surface, and it does so without inventing a `yield` keyword.

> **A method declares an incoming block by naming the block channel with `&name` as a trailing parameter. The bound block is an ordinary callable, invoked with the canonical call forms (`name(...)`, `name.(...)`) the language already has. Blocks are forwarded with `&name` in trailing argument position.**

Everything below is the consequences of that one decision plus the integration details. The defining property: **`&` is the block/callable channel in every grammar position** — declare it, forward it, reference it — and **block invocation is ordinary calling, not hidden control flow.**

---

## 2. Why no `yield`

Amber's reference point is Ruby, so the obvious move is Ruby's `def each; …; yield x; end`. This RFC deliberately rejects `yield` for blocks.

`yield` is an invisible jump to an unnamed slot. It is exactly the class of hidden control flow Amber has spent its other RFCs removing:

- The bare-nullary RFC (§8.13–8.17, accepted 2026-06-12) made implicit sends *explicit* rather than letting `foo` silently call.
- `$_` is an **explicit** frame slot, not magic.
- The collection-mutation RFC made effects **greppable** (`!`) rather than implicit.
- The parser is required to be syntax-faithful (§16.1): "Parser не имеет права прятать surface-формы в обычные вызовы."

A magic `yield` that calls an invisible block is the antithesis of all four. Naming the channel (`&blk`) and calling it like any other callable is:

- **more consistent** — it reuses `.()` / `callee(args…)` (§4.6) instead of adding a keyword and an opcode-visible-only construct;
- **strictly more expressive** — a named block can be tested (`blk == null`), stored (`@handler = blk`), forwarded (`other(&blk)`), and passed through `case`/clauses, none of which `yield` allows;
- **honestly typed** — the block has a name to hang a `Fn[…]` type and an effect row on (§7).

There is no `block_given?` either: the predicate is just `blk == null` (§5.3). One fewer piece of magic.

---

## 3. The unifying model: `&` is the callable channel

§4.6 already made `&` the callable-reference sigil. This RFC extends that single idea across all three grammar positions, each disambiguated by position — never by lookahead:

| Position | Form | Meaning | Status |
|---|---|---|---|
| expression | `&NameSpace.fn`, `&Class#m` | produce a callable-reference value | exists (§4.6) |
| **parameter** | `&blk` | **bind the incoming block channel to a name** | **new (§4)** |
| **argument (trailing)** | `each(&blk)` | **route a callable into the callee's block channel** | **new (§6)** |

One sentence for the language guide: *`&` always names the block/callable channel — at a definition it receives the block, at a call site it sends one, in an expression it lifts a named callable into a value.*

---

## 4. Declaration — the `&name` block parameter

### 4.1 Grammar delta (§15.7)

§15.7 is currently `Param ::= PosParam | KwParam`. Add a terminal, at-most-one block slot:

```ebnf
ParamListDef ::= [ Param { "," Param } ] [ "," BlockParam ] [ "," ]

BlockParam   ::= "&" LocalName [ "as" TypeTerm ]
               | "&" AutoName  [ "as" TypeTerm ]     # &@field capture — see §10, open
```

`AstParam.kind` (§16.7 AstParam node) gains the value `block`, alongside `positional` / `keyword`. This mirrors the spec's existing treatment of rest-params as "зарезервированы для будущего format bump" (§15.7) — the slot was already reserved conceptually; this surfaces it.

### 4.2 Static rules

1. **At most one** block parameter per signature. It binds the singular hidden `block_slot` (§12.7.4); there is no second channel.
2. The block parameter, if present, is **last** — after all positional and keyword params. Blocks are *the hidden final argument* (§12.7.4); the surface order matches the ABI order.
3. A block parameter is **always a single name**, never a pattern. You do not destructure a function. (Pattern-matching stays on the *block-literal* side at the call site — §4.2 / §9 — where it already lives.)
4. A block parameter does **not** participate in multi-clause dispatch. Per §15.7, all clauses of a rich multi-clause `def` share one base signature; `&blk` is part of that shared base and is bound identically in every clause, never matched on.
5. Violations: more than one block param, or a block param not in final position, is the parser diagnostic **`AMB_BLOCK_PARAM_NOT_LAST`** (see §9).

### 4.3 A method with a block parameter is not bare-callable

This already follows from existing text — **no new rule is required.** §8723 defines syntactically-nullary as "no positional, default, rest, keyword **or block parameters**," and §8.13 / line 7729 already states a method "with any declared parameters (including … block parameters)" diagnoses `AMB_BARE_NON_NULLARY`. The spec authors already wrote block parameters into the bare-nullary exclusion before the grammar surfaced them. `def each(&blk):` is therefore correctly non-bare-callable for free.

---

## 5. Invocation — ordinary calling, no new syntax

A bound block is a callable object. It is invoked with the canonical callable-call forms already normative in §4.6:

```amber
def each(xs, &blk):
  i = 0
  while i < xs.length:
    blk(xs[i])          # CALL on the block object
    i = i + 1           # blk.(xs[i]) is the identical HCall lowering
  xs
```

- `blk(args…)` and `blk.(args…)` both lower to `HCall` and, on a block value, to `CALL_BLOCK` / `CALL` exactly as stdlib `map`/`select` already do (§12.7.4). **No new opcode, no new surface form.**
- A **bare** `blk` is a *read* of the local (the callable value), **not** a call — §8.13 / line 7729: "Bare *identifiers* never become implicit call sites." To invoke with zero args, write `blk()`.
- Block **arity / shape** is governed by the existing rules (§9.6): the block's declared parameters (the `|…|` at the call site) must accept the arguments, with `MatchError` on a pattern mismatch at block-frame entry (§12.7.4) and `ArgumentError` on arity mismatch.

### 5.1 Optionality — `blk == null`, not `block_given?`

A bare `&blk` binds to the passed block **or `null`** when the caller supplied none.

```amber
def each(xs, &blk):
  when blk == null: xs                 # no block: nothing to do
  else:
    i = 0
    while i < xs.length: blk(xs[i]); i = i + 1
    xs
```

The safe-call form skips an absent block inline — `blk.?.(x)` returns `null` without calling when `blk` is `null` (§4.6). For a **required** block, annotate it non-null (`&blk as Fn[…]`, §7); a missing block then fails at the existing typecheck/arity step (§10.3 step 4) as `ArgumentError`, and "invalid block argument shape" is *already* a listed `ArgumentError` cause (§19 error table). Calling a `null` block via `blk(x)` raises the same `TypeError` as calling any non-callable (§4.6) — no new error needed.

---

## 6. Forwarding — `&name` in trailing argument position

To pass an already-captured block (or any callable held in a local) down to another call, write `&name` as the **last argument**:

```amber
def each_twice(xs, &blk):
  xs.each(&blk)        # forward the block into each's block channel
  xs.each(&blk)
  xs
```

### 6.1 Why this is conflict-free

`&` is both a prefix sigil and an infix bitwise-and operator (precedence 7). The block-pass therefore **must live inside the call parens**, where a leading `&` is unambiguously prefix — a bare `xs.each &blk` would parse as `xs.each & blk` and is *not* offered.

Inside parens, `&LocalName` (a bare local) is **not currently a legal expression**: §4.6 restricts prefix `&` to static reference-targets (`&NS.fn`, `&Class.m`, `&Class#m`), explicitly rejecting `&local`. So `f(&blk)` has **no existing meaning** — this RFC claims an empty slot with zero collision.

### 6.2 Scope (v1)

- Block-pass in v1 is `& LocalName` in trailing argument position only. The named local must hold a callable; otherwise the call raises `TypeError` (non-callable in the block channel), surfaced statically as **`AMB_BLOCK_PASS_TARGET`** where provable.
- Generalizing block-pass to an arbitrary trailing `&Expr` (e.g. `each(&Class#m)`) is **deferred**. That spelling *does* currently mean "pass a callable-reference value positionally," so opening it would reinterpret existing programs; v1 sidesteps the collision entirely by only accepting bare locals. To forward a method reference today, bind it first: `r = &Class#m; xs.each(&r)`.

---

## 7. Typing and effects

A block parameter is typed with the existing callable type (§4.x, `Fn[Args -> Ret !{effects}]`):

```amber
def map(xs as Array, &blk as Fn[T -> U !{}]) -> Array:
  out = []
  xs.each |x|: out.push!(blk(x))
  out
```

Two consequences fall out of the existing effect system (§4.3) at no extra cost:

- **Effect transparency.** A method that invokes its block carries the **union** of its own effects and the block's effect row. A method generic over `&blk as Fn[… !{E}]` is *effect-polymorphic* in `E` — calling `blk()` is the only place those effects enter. This is strictly better than `yield`, which has no name to attach an effect row to, and it keeps the `!{…}` audit story (collection-mutation RFC) intact through higher-order calls.
- **Required vs optional** is expressed in the type, not a predicate: `&blk as Fn[…]` is a required, non-null block; bare `&blk` (or `&blk as Fn[…]?`) is optional and may bind `null` (§5.1).

---

## 8. `define_method` and uniform block signatures

`define_method(Target, :name) |params|: body` takes the installed method's signature *from the block's parameters* (§8.14, line 1177). For consistency, that block-parameter list accepts a trailing `&name` exactly as a `def` signature does:

```amber
define_method(Stream, :each) |&blk|:
  @items.each(&blk)
```

This is the **only** place a block-suffix parameter list may contain `&name`. Ordinary block literals passed to a call (`xs.map |x|: …`) may **not** declare a block parameter in v1 — a block does not itself receive a block. This keeps the hidden-final-argument channel singular (§12.7.4) and avoids nested block channels. (`define_method`'s block is a method body, not a call-site block, so it is a definition site, not a consumer site.)

---

## 9. Diagnostics

House-style `AMB_` codes with dynamic twins (cf. §8 error table):

| Code | Stage | Condition | Dynamic twin |
|---|---|---|---|
| `AMB_BLOCK_PARAM_NOT_LAST` | parser | block param not in final position, or more than one block param | — (parse error) |
| `AMB_BLOCK_PARAM_PATTERN` | parser | `&` applied to a pattern rather than a single name | — (parse error) |
| `AMB_BLOCK_PASS_TARGET` | binder / runtime | `&name` block-pass where `name` is not a callable | `TypeError` |
| `AMB_BLOCK_REQUIRED` | runtime (static where provable) | a non-null `&blk as Fn[…]` param received no block | `ArgumentError` ("invalid block argument shape", §19) |

Calling a `null` optional block reuses the existing non-callable `TypeError` (§4.6) — intentionally **not** a new code, so the failure mode matches every other "called a non-callable" site.

---

## 10. Open questions (for decision)

### 10.1 Default block parameter — `&blk = …`

**Recommendation: exclude from v1.**

A default would let `def each(&blk = &identity):` supply a fallback callable. But the `blk == null` idiom (§5.1) and `blk.?.()` already cover "no block" cleanly, and a default callable expression drags in the default-thunk machinery (§12.7.5) for marginal ergonomic gain. Leave `&blk` defaultless in v1; revisit if real usage shows a pattern that `null`-checking handles poorly.

### 10.2 `&@field` auto-capture — `def init(&@handler):`

**Recommendation: include in v1.**

This reuses the existing `AutoName` machinery (§15.7) with no new concept: the incoming block is stored directly into field `@handler`, exactly as `def init(@x):` stores a positional. It is genuinely useful — event handlers, strategy objects, callbacks stored at construction — and it is *more* ergonomic than any alternative (`def init(&h): @handler = h`). The cost is one production (`BlockParam ::= "&" AutoName …`, already in §4.1) and binding it through the same auto-assign commit step (§10.3 step 6). The only constraint worth stating: like other auto-assign params, `&@handler` commits **after** clause selection, so it does not leak on a `MatchError`.

If excluded, drop the `AutoName` alternative from the §4.1 grammar and require the explicit `@handler = blk` body.

---

## 11. ABI and lowering — zero runtime change

This RFC is almost entirely surface. The runtime already does the work:

- The block already travels in `block_reg` / `block_slot` (§12.7.4); `&blk` simply *names* that slot as an ordinary local at frame entry.
- `blk(args…)` lowers through `HCall` → `CALL_BLOCK` / `CALL` — the same path stdlib `map`/`select`/`reduce` already take (§12.7.4). No new opcode.
- `other(&blk)` lowers by moving the local's value into the callee's `block_reg` of the `CallPacket` (§16.x `SEND … block_reg`), instead of compiling a block-suffix closure into it.
- Block-param pattern matching at the call site, `MatchError` on mismatch, and bare-nullary exclusion are **already specified** (§12.7.4, §8723).

The compiler changes are confined to: the parser (one grammar production + diagnostics), the binder (bind `block_slot` to a name; route trailing `&name` to `block_reg`), and the AST/HIR serializer (the reserved `block` param kind). The VM, ABI, and pattern engine are untouched.

---

## 12. Worked example — your own collection methods

```amber
def each(xs, &blk):
  when blk == null: xs
  else:
    i = 0
    while i < xs.length: blk(xs[i]); i = i + 1
    xs

def map(xs, &blk as Fn[T -> U !{}]) -> Array:
  out = []
  each(xs) |x|: out.push!(blk(x))     # call your own each, with a block
  out

def reduce(xs, acc, &blk):
  each(xs) |x|: acc = blk(acc, x)
  acc

# call them exactly like builtins — including pattern-matched block vars (§4.2):
pairs.map |((0, x) | (x, 0))|: x
total = reduce([1, 2, 3], 0) |a, x|: a + x
```

---

## 13. Rationale & alternatives considered

### 13.1 Ruby `yield`
Rejected (§2). Invisible control flow, no name to type/test/store/forward, contradicts the syntax-faithful and effect-greppable posture of the existing RFCs. Naming the channel costs three characters and buys all four capabilities back.

### 13.2 `block_given?` predicate
Rejected. `blk == null` is the predicate. A named optional block makes a magic predicate redundant.

### 13.3 Expression block-suffix `xs.each &blk`
Rejected. `&` is also infix bitwise-and (prec 7), so `xs.each &blk` collides with `xs.each & blk` (§6.1). The in-parens trailing `&name` is unambiguous and reuses an empty slot.

### 13.4 Generalized trailing `&Expr` block-pass
Deferred, not rejected (§6.2). `each(&Class#m)` already means "positional callable-ref value"; opening it would reinterpret existing code. v1 accepts only bare locals (`&blk`), which currently have no meaning, then revisits generalization with a migration note.

### 13.5 A dedicated callable type vs reusing `Fn[…]`
Reused `Fn[…]` (§7). The typed profile already has it; a block is just a callable, so it needs no distinct type. Effect-polymorphism over the block's `!{…}` row falls out for free.

---

## 14. Spec patch (new STD entry)

```text
STD-0NN  User-defined block parameters

1. A def / class_method def signature MAY end in one block parameter,
   written `&name` (or `&@field` for capture, if §10.2 is adopted),
   optionally typed `as Fn[...]`. At most one; it must be the last
   parameter. It binds the frame's block_slot to `name`.

2. A method with a block parameter is NOT syntactically nullary and is not
   bare-callable (already required by §8723 / bare-nullary RFC).

3. A bound block is an ordinary callable. It is invoked via the canonical
   callable-call forms `name(args...)` / `name.(args...)` (§4.6). There is
   NO `yield` keyword and NO `block_given?` predicate; absence is tested as
   `name == null`, optional invocation as `name.?.(args...)`.

4. A bare `&name` binds the passed block or `null` when none was passed.
   A `&name as Fn[...]` (non-null) block is required; a missing one is
   `ArgumentError` (invalid block argument shape). Calling a `null` block is
   the ordinary non-callable `TypeError`.

5. Forwarding: a trailing call argument written `&LocalName` routes that
   local's callable into the callee's block channel. v1 accepts only a bare
   local in this position; `&NS.fn` / `&Class#m` as a trailing block-pass is
   deferred. Bind first to forward a reference: `r = &Class#m; f(&r)`.

6. The block parameter does not participate in multi-clause dispatch; it is
   part of the shared base signature and is bound identically in all clauses.

7. `define_method`'s block signature accepts a trailing `&name` (it defines a
   method body). Ordinary call-site block literals do NOT declare block
   parameters in v1.

8. Diagnostics: AMB_BLOCK_PARAM_NOT_LAST, AMB_BLOCK_PARAM_PATTERN,
   AMB_BLOCK_PASS_TARGET (TypeError), AMB_BLOCK_REQUIRED (ArgumentError).

9. ABI is unchanged: the block already rides in block_reg/block_slot
   (§12.7.4); this entry only names and forwards it.
```

---

## 15. Conformance suite additions (§18)

Positive:
- `def` with a trailing `&blk`; invoke via `blk(x)` and `blk.(x)`; identical observable result.
- optional block: `blk == null` branch taken when no block passed; `blk.?.(x)` no-ops on absent block.
- required block: `&blk as Fn[…]` with no block → `ArgumentError`.
- forwarding: `f(&blk)` routes into a callee's block channel; nested forwarding two levels deep.
- pattern-matched block var against a user method: `my_map |((0,x)|(x,0))|: x`.
- `define_method(T,:each) |&blk|:` installs a block-consuming method.
- multi-clause `def` sharing a `&blk` base signature across clauses.
- (if §10.2) `def init(&@handler):` stores the block into a field; no leak on `MatchError`.

Negative:
- two block params / non-final block param → `AMB_BLOCK_PARAM_NOT_LAST`.
- `&(a, b)` block param → `AMB_BLOCK_PARAM_PATTERN`.
- `f(&local)` where `local` is not callable → `AMB_BLOCK_PASS_TARGET` / `TypeError`.
- bare member with a block param treated as bare-nullary → `AMB_BARE_NON_NULLARY`.
- block literal at a call site declaring `&name` → parse error (v1 restriction, §8).

---

## 16. Recommendation

Adopt this RFC. It opens block consumption to user code with **one new grammar production** (`BlockParam`), **one new argument form** (trailing `&LocalName`), and a reserved AST enum value — the VM, ABI, and pattern engine are untouched because §12.7.4 already carries the block. It does so while *strengthening* Amber's existing commitments: no hidden control flow (no `yield`), greppable effects (the block's `!{…}` row flows through `Fn[…]`), and a single coherent meaning for `&` across declaration, forwarding, and reference.

Decisions requested: **§10.1** (default block param — recommend *exclude*) and **§10.2** (`&@field` capture — recommend *include*).

Compromise framing for the language guide:

```text
Amber methods take a block by naming it: `def each(&blk):`. The block is a
plain callable — call it `blk(x)`, test it `blk == null`, forward it
`other(&blk)`, store it `@h = blk`. There is no `yield` and no
`block_given?`: a block you can name is a block you can see.
```
