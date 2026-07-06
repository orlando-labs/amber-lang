# Design closures for the bare-nullary + `.()` RFC

Status: **implemented and specced 2026-06-12.** The RFC lives at
`docs/engineering/rfc-bare-nullary-and-dotcall-v1.md`; the unified spec
carries the integrated section `Bare-nullary member access and dot-call
"expr.()"` plus the contradiction patch set below. Conformance:
`corpus/run/{bare_nullary_member,dot_call_member_result,prop_called_as_method,prop_non_suspendable,kwargs_spread_property_only}`,
plus parser/binder/vm unit tests.

Implementation deviations from the recommendations (deliberate, minimal):

- `AMB_BARE_NON_NULLARY` ships dynamic-only for member access on dynamic
  receivers (`ArgumentError` at runtime); the static binder twin lands with
  typed-receiver resolution. Module-level property misuse *is* diagnosed
  statically (`AMB_PROP_CALLED_AS_METHOD` in the binder).
- `W_MEMBER_KIND_OVERRIDE` (strict-profile kind-override warning) is not
  implemented; it remains an open follow-up.
- Block-suffix-on-property faults through the runtime `TypeError` path of
  `AMB_PROP_CALLED_AS_METHOD` semantics rather than a dedicated static
  `AMB_PROP_BLOCK_SUFFIX` code (member kinds are not statically known at
  bind time).
- Gap 3 (namespace access) is exercised on class-side receivers
  (`Build.version`). The current runtime has no first-class module-namespace
  receivers — cross-module access flows through import-alias bindings, which
  are identifier reads and correctly never invoke. The spec section states
  the dispatch rule so module values inherit it if/when they become
  receivers.

Companion to the `Computed property descriptors` section of
`amber_unified_final_spec.md`.

Each gap gets: the recommended ruling, the normative rule sketch, and the
registry/test consequences. Diagnostic names follow the registry convention
(`AMB_*` in `spec/registries/diagnostics.yaml`), not the spec-prose `E_*`
names — see Gap 8 for the naming normalization.

---

## Gap 1 — Refactor compatibility is one-directional (`obj.prop()`)

**Ruling: keep the hard error. Restate the compatibility contract precisely.
Formatter normalizes `?`-predicates only.**

`obj.prop()` invoking the getter (full symmetry) is rejected and recorded as a
rejected alternative in the RFC's §13: now that `.()` owns "call the result"
the form would be unambiguous, but it permanently doubles the read spelling
and erases the descriptor/method distinction for zero new capability.

Normative contract (replaces the RFC §2.1/§10.1 "call sites do not change"
claim):

> The bare read form `x.name` is stable across `prop` ↔ nullary-`def`
> refactors. The explicit form `x.name()` is method-call syntax only and is
> stable only while `name` is method-shaped. Public APIs should document
> query members in bare form.

Mechanisms:

1. Diagnostic `AMB_PROP_CALLED_AS_METHOD` (binder; dynamic twin `TypeError`)
   covers `obj.prop(...)` with **any** argument count including zero, with the
   fix-it "use `obj.prop` or `obj.prop.()` if the value is callable". This
   subsumes and retires the existing `AMB_PROP_CALL_ARGS_FORBIDDEN`.
2. Normative formatter rule: `x.name?()` with zero arguments is rewritten to
   `x.name?`. Predicates only — other explicit nullary calls are a legitimate
   style for effectful operations (RFC §13.4) and are left alone.
3. Style guide (§14 amendment): paren-calling a documented query member is a
   style deviation that a later `def → prop` migration is permitted to break.

---

## Gap 2 — Resolution order must be lookup-order, not kind-priority

**Ruling: one linearized lookup; nearest owner wins; the kind of the found
member governs both read and write; no cross-owner arm merging.**

Normative algorithm for `receiver.name` (read):

```
1. Resolve `name` through the standard member linearization of the
   receiver's class — identical owner order to ordinary method send.
2. The nearest owner declaring external member `name` fixes the member;
   farther owners are never consulted. Dispatch on its kind:
   a. readable property            -> property get (getter arm)
   b. write-only property          -> AMB_PROP_MISSING_GETTER /
                                      WriteOnlyPropertyError
   c. syntactically nullary method -> implicit zero-argument send
   d. any other method             -> AMB_BARE_NON_NULLARY / ArgumentError
   e. field accessor / readable binding -> ordinary read
3. No owner declares `name`: dynamic receivers take the zero-argument
   missing-member path (`method_missing(:name)`); otherwise NoMethodError.
   Statically known receivers diagnose before runtime.
```

Assignment `receiver.name = v` uses the **same single lookup**: the nearest
member must be a writable property (setter arm runs); a read-only property is
`AMB_PROP_MISSING_SETTER` / `ReadOnlyPropertyError`; method/field kinds are
`AMB_PROP_SETTER_UNDEFINED`. Assignment to a missing member on a dynamic
receiver is `NoMethodError` — there is no `name=` selector family, so
`method_missing` does not participate in property set in v1 (a future
`property_missing` hook is a separate RFC).

Explicitly stated non-rules:

- No kind-priority: a distant ancestor's property never shadows a nearer
  owner's nullary method, and vice versa.
- No cross-owner arm merging: a getter-only `prop` in a subclass does not
  combine with an ancestor's setter arm (mirrors the same-owner-only merge
  rule in prop §9.2).

Optional, strict profile: warning `W_MEMBER_KIND_OVERRIDE` when an override
changes member kind across the chain (e.g. a method shadowing a writable
property silently removes assignability at the subtype).

---

## Gap 3 — Namespace/dotted access now invokes

**Ruling: adopt uniformly — "dot is a message; identifier is a value." Give it
its own normative subsection; do not leave it implied by the class-side
example.**

- `Owner.name` is member access regardless of whether `Owner` is an object,
  class, or module namespace, and follows the Gap-2 kind dispatch. For module
  namespaces: nullary module function → implicit call; non-nullary →
  `AMB_BARE_NON_NULLARY`; plain value export → binding read.
- `&Owner.name` remains the only extraction spelling. Consequence to state
  with an example: `fn = Math.answer` now binds the **result** of `answer`;
  extraction is `fn = &Math.answer`.
- Import-created local bindings (bare `c` after `import a.b.c`) are
  identifier reads and never invoke. Dotted path access in expression
  position is member access and follows the dispatch above.

Migration: audit the corpus for `= Namespace.fn`-shaped bindings when the
spec patch lands; each is either intended extraction (rewrite with `&`) or
already wants the new call semantics.

Conformance tests: nullary module fn (invokes), non-nullary module fn
(diagnoses), value export (plain read), `&` extraction (never invokes).

---

## Gap 4 — Dynamic-path runtime error for bare non-nullary access

**Ruling: no new error class. Static `AMB_BARE_NON_NULLARY` (binder) pairs
with dynamic `ArgumentError`,** following the existing static/dynamic pairing
pattern (`AMB_PROP_MISSING_SETTER` / `ReadOnlyPropertyError`). Deterministic
message, golden-testable:

```
ArgumentError: method `format` is not bare-callable: its signature is not
syntactically nullary; use format(...)
```

Related rulings in the same family:

- `.()` on a non-callable value raises exactly what `fn(args...)` on a
  non-callable raises — `TypeError`, identical class and message shape, since
  `expr.(args...)` ≡ `(expr)(args...)`. Static rejection where the value is
  known non-callable: `AMB_NOT_CALLABLE` (binder).
- `.()` without a preceding postfix expression: parser diagnostic
  `AMB_DOT_CALL_TARGET`.
- Registry fix (incidental, fold into this patch): `ReadOnlyPropertyError`
  and `WriteOnlyPropertyError` are referenced by the prop spec but missing
  from `spec/registries/runtime_errors.yaml` — add both.

---

## Gap 5 — Protocol positions stay property-only

**Ruling (revised 2026-06-12): protocol positions require a readable
property; nullary methods do NOT participate.** The earlier draft ruling
("extend to readable member") is withdrawn.

Principled line:

> The human call surface uses uniform reads (`obj.name` works for prop and
> nullary def alike). **Protocol positions are capability declarations, not
> call sites** — participation must be declared with `prop`, never inferred
> from a method name.

Rationale:

1. Prevents accidental protocol conformance: with member-based eligibility,
   any class that happens to define a nullary method named `kwargs` silently
   becomes keyword-spreadable. Property-only makes participation an explicit
   opt-in.
2. Protocol reads happen implicitly inside call/spread evaluation — exactly
   where field-like value semantics are wanted, not arbitrary operations.
3. Combined with Gap 9 (non-suspendable property arms), this yields the
   invariant that **protocol-driven implicit reads can never suspend**, so
   spread/kwargs-view construction lowers to straight-line code with no
   suspension state machine.

Consequences:

- kwargs §11.5 ("No implicit nullary method call") stays as written; add the
  rationale above so the question doesn't reopen.
- The prop↔def leak at protocol sites is accepted and documented: protocol
  hooks are declared for the protocol, so refactoring one to `def` is a
  deliberate capability removal, not an incidental rename.
- Migration/teaching diagnostic: when `fn(**obj)` fails with
  `E_KWARG_SPREAD_OPERAND` and the receiver's class defines a syntactically
  nullary method `kwargs`, the message appends a hint:
  "note: `Class` defines method `kwargs()`; keyword spread requires a
  readable property — declare `prop kwargs`".
- Future protocols must use the phrase "readable property" with a pointer to
  this ruling.

---

## Gap 6 — Implicit self is out: bare identifiers stay lexical

**Ruling: bare identifiers resolve lexically (locals → enclosing scopes →
module scope) and are never implicit self member sends.** Instance and class
members always require an explicit receiver inside bodies: `self.size`
(which, being member access, gets bare-nullary behavior and invokes) and
`@field` for storage.

This extends the language's existing explicitness rule for fields (mandatory
`@`) to methods, and eliminates Ruby's "local or self-send?" ambiguity class
entirely. One normative sentence plus an example in the member-access
section.

Caveat to verify during the spec patch: confirm the current binder/corpus
does not already resolve unqualified calls `size()` inside method bodies to
self methods. If it does, that existing rule must be stated explicitly
either way — bare-nullary must not silently change unqualified-call
resolution.

---

## Gap 7 — Block suffix interaction

**Ruling: block suffix is call syntax; the resolved member must be
method-kind.**

- Property + block suffix → `AMB_PROP_BLOCK_SUFFIX` (binder; dynamic twin
  `TypeError`).
- Nullary `def` + block suffix → ordinary send with block (existing bare-args
  rule already makes block suffix force the call interpretation).
- Update the parser note (spec line ~21842) when it is rewritten for
  bare-nullary so the property case is covered there too.

---

## Gap 8 — Registry and diagnostics consolidation

1. Drop the RFC's `E_AMBIGUOUS_MEMBER_KIND`; the situation is already covered
   by `AMB_PROP_NAME_CONFLICT` / `E_MEMBER_NAME_CONFLICT`. Those two overlap
   with each other as well — consolidate to one code (recommend keeping
   `E_MEMBER_NAME_CONFLICT` as the general owner-level rule and retiring the
   prop-specific one, or vice versa; one must go).
2. Naming normalization: the prop spec section uses `E_PROP_*` names that do
   not match the registry's `AMB_PROP_*` codes. The spec patch should align
   prose names to registry codes (or add an explicit mapping table once).
3. New diagnostics (registry-ready):

   | code | phase | condition |
   |---|---|---|
   | `AMB_BARE_NON_NULLARY` | binder | bare member access resolves to a method whose signature is not syntactically nullary |
   | `AMB_PROP_CALLED_AS_METHOD` | binder | call punctuation applied to a property member (any argc; replaces `AMB_PROP_CALL_ARGS_FORBIDDEN`) |
   | `AMB_PROP_BLOCK_SUFFIX` | binder | block suffix attached to a property member access |
   | `AMB_NOT_CALLABLE` | binder | dot-call target statically known not to be callable |
   | `AMB_DOT_CALL_TARGET` | parser | `.()` segment without a preceding postfix expression |
   | `W_BARE_BANG_CALL` | binder, warning | bare access invokes a `!`-suffixed method (default-on; strict profile may promote to error — answers RFC open Q6) |
   | `W_MEMBER_KIND_OVERRIDE` | binder, warning | optional, strict profile; override changes member kind across linearization |

4. Runtime registry additions: `ReadOnlyPropertyError`,
   `WriteOnlyPropertyError` (Gap 4 incidental). No new class for bare-call
   arity (`ArgumentError`) or dot-call (`TypeError`).

Open-question answers folded in: Q4 (formatter) → yes, predicates only
(Gap 1). Q6 (`!` bare-call) → warning, strict-promotable. Q7 (block after
`.()`) → defer; the grammar simply omits it, no reserved diagnostic needed.
Q1/Q2 (bound refs, property getter refs) → unchanged; property getter
references remain the top follow-up RFC, since `&User#name` is still the one
read site where prop ≠ def leaks.

---

## Gap 9 — Property arms are non-suspendable

**Ruling (added 2026-06-12): getter and setter arms must not suspend.** A
suspension attempt (task sleep, channel/flow wait, suspending IO, or any
other scheduler yield point) inside a property arm raises
`EffectViolationError` with a deterministic message, e.g.:

```
EffectViolationError: property getter `full_name` attempted to suspend
```

Scope and enforcement:

1. The restriction attaches to the **declaration kind** (property arms), not
   to access syntax. A nullary `def` invoked bare via `obj.member` may still
   suspend — `x.f` and `x.f()` must not differ in suspendability for the
   same method. Once effect annotations exist, bare reads of suspending
   nullary defs fall under the `W_EXPENSIVE_BARE_CALL` lint family
   (RFC §9.2).
2. The no-suspend region is a **dynamic extent**: it covers the arm body and
   everything it calls transitively (a frame flag checked at suspension
   points — negligible cost, since suspension already involves the
   scheduler). A lexical rule cannot catch transitive calls. The extent ends
   when the arm's frames unwind; exceptions propagating out of the arm are
   unaffected.
3. Both arms: a suspending setter would put a scheduling point inside
   assignment evaluation, which has strict left-to-right ordering and
   returns the RHS — forbidden for the same reason as getters.
4. Spawning a task from an arm is not suspension and is permitted (style
   discourages it); raising is permitted; blocking-as-suspension (green
   IO) is forbidden by consequence — properties cannot do runtime IO, which
   is the intended contract.
5. Static enforcement where an effect profile can prove it; the dynamic
   guard is the v1 baseline. Typed/effect profiles may promote to
   compile-time diagnostics later.
6. `class_prop`, module-level props, and mixin props follow the same rule;
   `attr`-generated descriptors are trivially compliant.

What this buys:

- Member reads backed by `prop` are guaranteed synchronous — no invisible
  scheduling points behind field-like syntax, which matters double under
  bare-nullary where reads are everywhere.
- With Gap 5, all protocol-driven implicit reads are non-suspending, so
  call-argument and spread evaluation never suspends except inside syntax
  the user explicitly wrote.
- Gives `prop` vs `def` a real semantic contract beyond assignability:
  props are synchronous value access; defs are operations that may suspend.
  This is the enforceable core of the style guidance ("expensive/effectful
  → explicit call").
- Helps determinism/replay machinery: props cannot introduce scheduling
  nondeterminism.

Future escape hatch: none in v1. If async properties are ever wanted, they
require explicit use-site marking (Swift SE-0310-style `await`), which Amber
deliberately does not have — so the prohibition is structurally
future-proof, revisitable only alongside an explicit-await RFC.

---

## Gap 10 — Spec patch set (land as one change)

The RFC text plus all of the above must land together with these
contradiction fixes in `amber_unified_final_spec.md`:

| Location (line) | Current statement | Change |
|---|---|---|
| ~480 | `fn.()` "в язык не вводится" | replace with `.()` postfix segment definition (note: safe variant `fn.?.(args)` already exists at ~463 — cite the symmetry) |
| ~6771–6778 | preserved-decisions items 1, 2, 8 | rewrite for member-level bare-nullary |
| ~6830–6841, ~6862–6883 | "intentionally different from implicit nullary calls" | restate: holds for bare identifiers, not member access |
| ~7151–7158 (§5.3) | `obj.size()` = getter then call result | replace with `AMB_PROP_CALLED_AS_METHOD` rule |
| ~7704, ~7946 | "no ordinary method becomes implicitly callable" | rewrite |
| ~21842 | "no call-tail, no block suffix ⇒ field access" | becomes "member read or implicit nullary send"; add property/block-suffix note (Gap 7) |
| ~15283–15301, ~531 | kwargs spread: property only | **keep as written**; add the capability-declaration rationale and the `E_KWARG_SPREAD_OPERAND` hint (Gap 5) |

Plus new subsections: namespace access semantics (Gap 3), lexical-identifier
rule (Gap 6), resolution algorithm (Gap 2), compatibility contract (Gap 1),
non-suspendable property arms (Gap 9 — extends prop §5/§6 semantics).

Conformance additions: RFC §16 tests, plus Gap 2 (cross-hierarchy
kind-dispatch, no arm merging), Gap 3 (module namespace ×4), Gap 5 (negative:
`**obj` with nullary `def kwargs()` fails with the hint message), Gap 7
(property + block suffix), Gap 9 (suspension attempt in getter and in
setter, direct and transitive, raises `EffectViolationError`), and golden
messages for every new diagnostic. All gated through the existing
backend-equivalence gate.
