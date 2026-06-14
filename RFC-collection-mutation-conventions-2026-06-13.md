# RFC: Collection Mutation Conventions for Amber

**Status:** Draft / for discussion
**Author:** (expert review, synthesizing the A/B discussion doc)
**Date:** 2026-06-13
**Scope:** `Array`, `Map`, `Set`, `Tuple`, `Range`, `LazySeq`, and the language-wide meaning of the `!` suffix
**Supersedes discussion in:** `amber_collection_mutation_conventions.md` (Variant A vs Variant B)

---

## 1. Summary

This RFC adopts a refined version of **Variant B (strict bang mutation)**, but does not stop at the collection stdlib. The core decision the original document leaves implicit is what `!` means *for the language as a whole*. Adopting B for collections while leaving `!` free to mean something else elsewhere (e.g. Rails-style "raise on failure") would not deliver the simple mental model B promises — it would merely relocate the ambiguity.

The proposal in one sentence:

> **`!` is the visible, greppable marker of observable receiver mutation, everywhere in Amber. No bang, no in-place mutation.**

Everything else in this RFC is consequences of that rule plus the ergonomic decisions needed to make it pleasant.

---

## 2. Why this matters / motivation

Amber is a new language, not a Ruby dialect. Its positioning is "Ruby-level ergonomics without Ruby's unspecifiable history." Two of Amber's central traits make hidden mutation especially costly:

- **Expression-oriented, block-suffix, chain-heavy style.** Mutation buried mid-chain is invisible and dangerous.
- **A planned async / runtime-facing layer.** Anything touching concurrency benefits enormously from being able to *mechanically audit* where shared state is mutated.

Both of these reward a mutation contract that is (a) checkable at a single call site, and (b) searchable across a whole program. That is the bar this RFC optimizes for.

---

## 3. Correcting the premise: what `!` really means in Ruby

The original document frames the choice as "keep Ruby's `!` = mutation (A)" vs "make `!` strictly = mutation (B)." This mischaracterizes Ruby and the choice should be made honestly.

In Ruby, `!` does **not** mean "mutates." It means **"the surprising / more dangerous sibling of a safe method."** The surprise is *usually* mutation but not always:

- Many mutators have **no** bang: `push`, `<<`, `concat`, `shift`, `pop`, `store`, `update`, `delete`, `clear`, `[]=`.
- Some bangs do not mutate (`exit!`).
- Several mutating bangs carry a *different* surprise — `select!`, `reject!`, `flatten!`, `compact!` return `nil` when nothing changed.

Therefore:

- **Variant A is the genuinely Ruby-faithful option.** It reproduces Ruby's real rule: a `!` exists only when there is a pure twin to contrast against; structural verbs with no twin just mutate bare.
- **Variant B is a deliberate clean break**, not a "tightening" of Ruby.

This RFC chooses the clean break with eyes open. Amber should not inherit a convention whose defining property is "you must memorize which methods got historically blessed with a bang."

---

## 4. The decisive case against Variant A

One example settles it. Under Variant A:

```amber
xs.select: _1.active?    # pure — returns a new collection
xs.keep_if: _1.active?   # mutates xs in place
```

`select` and `keep_if` are near-synonyms with **opposite effects**, distinguished only by which one Ruby happened to give a bang to. Teaching that to a newcomer requires a lookup table, and the original document's §2.7 is exactly that table — a permanent fixture of the spec that reopens every time a collection method is added ("is this a structural verb, or does it get a twin?").

Under this RFC:

```amber
xs.select:  _1.active?   # pure
xs.select!: _1.active?   # mutates
xs.keep_if!: _1.active?  # mutates
# `keep_if` with no bang simply does not exist
```

No table. No adjudication. The rule *is* the spec.

---

## 5. The rule

### 5.1 Normative statement (Mutation Bang Law)

```text
Amber Mutation Bang Law

1. A method whose name ends in `!` MAY mutate its receiver in place,
   and is the ONLY way a stdlib method is permitted to do so.

2. A method whose name does NOT end in `!` MUST NOT mutate its receiver.
   It returns a new value, a derived value, or a read of existing state.

3. If an operation has both a pure and an in-place form:
       name(...)   returns a new value / new collection;
       name!(...)  mutates the receiver.

4. If an operation has only an in-place meaning, it is still written `name!`.
   No bare form is introduced.

5. Assignment and indexed assignment are a separate syntactic category and
   are exempt: `xs[i] = v`, `m[k] = v`, `@field = v`. These visibly assign.

6. `!` does not carry any OTHER meaning in Amber stdlib (see §6). It is not
   overloaded with Ruby/Rails "raise-on-failure" semantics.
```

### 5.2 Mental model given to users

```text
Has `!`   -> the receiver may change. Audit these.
No `!`    -> the receiver will not change. Safe to read, safe to chain.
```

That is the whole model. It holds for collections and, per §6, for the language at large.

### 5.3 Frozen receivers

If a receiver is frozen / immutable, invoking a `!` method on it is an error: a **compile-time** error where the receiver's mutability is statically known, and a runtime error otherwise. Non-`!` methods are always valid on frozen values, since they cannot mutate. Frozen-receiver + `!` is the one combination the bang law must reject — and it can reject it precisely *because* mutation is marked syntactically.

---

## 6. The decision the original document skips: `!` language-wide

This is the most important section of the RFC. The original A/B doc scopes everything to collections and never asks what `!` means elsewhere. If Amber adopts the Rails idiom where `!` means "raise instead of returning nil/false" (`save!`, `find!`, `create!`), then `!` means **mutation** in one place and **fallibility** in another, and B's "one simple model" quietly dies — the ambiguity just moves from "which methods mutate" to "which meaning of `!` is this."

**Decision:** `!` means *observable receiver mutation* **language-wide**. Amber does **not** adopt `!` for raise-on-failure.

**Fallibility is handled by `.or_raise`, not by `!`.** Pure, fallible operations return a result/optional value; escalating a failure to an exception is an explicit combinator *on that value*:

```amber
n = parse_int(s).or_raise              # raises on failure
n = parse_int(s).or_raise("bad int")   # raises with a message
n = parse_int(s).or(0)                 # non-raising fallback
```

`.or_raise` is an ordinary (non-`!`) method on the returned value, not a name suffix on the operation — so it never competes with the mutation meaning of `!`. There is no `parse!`/`save!`-style raising variant in stdlib; the raising path is always spelled `….or_raise`. This keeps `!` strictly mono-semantic.

**Consequence to enforce:** the stdlib must never ship a `!` method whose only effect is raising. A `!` is justified only by mutation. If an in-place mutator also throws on bad input, the bang is earned by the mutation, not the raise.

---

## 7. Naming conventions for the pure side

Pure forms use **past-participle / preposition naming**, which is independently informative (the tense alone signals "returns a copy"). This makes the bang and the tense mutually reinforcing — a reader can tell `appended` from `append!` by *either* signal.

```text
mutating (imperative + !)   pure (participle / preposition)
append!                     appended
prepend!                    prepended
insert!                     inserted
delete!                     deleted
delete_at!                  deleted_at      (see §8.2 — may be dropped)
sort!                       sorted
reverse!                    reversed
update! / store!  (Map)     with
delete!           (Map)     without
add!              (Set)     added
```

### 7.1 Embrace asymmetry (curate the pure vocabulary)

A naive reading of B provides a pure twin for *everything*, which recreates the memorization burden B was trying to kill — just on the pure side. **Not every mutator needs a pure twin, and not every pure op needs a mutator.**

Ship the pure forms that are genuinely common and read naturally; route the rare ones through `+`, spread, and comprehensions.

- **Ship:** `with` / `without` (Map), `appended`, `deleted`, `added` (Set), `sorted`, `reversed`, `uniq`, `compact`, `merge` (returns new), `select` / `reject` / `map` / `filter`, set algebra (`union`, `intersection`, `difference`, `symmetric_difference`).
- **Do not ship just for symmetry:** `prepended`, positional `replaced(start, count, …)`, pure `deleted_at`. These are rare and clunky; `+`/spread/comprehension covers them.

The test for shipping a pure verb: *would a competent user reach for it weekly, and does its name read cleanly?* If not, leave it out of v1.

### 7.2 The key-block convention (no `_by` family)

Methods that need "something to order, dedupe, or group by" take an **optional key block** — a block of arity 1 returning the value to compare by. There is no `_by` suffix family.

```amber
xs.sorted               # natural order (elements must be Comparable)
xs.sorted: _1.age       # ordered by key — replaces sort_by
xs.uniq                 # dedupe by identity
xs.uniq: _1.id          # dedupe by key
xs.max:  _1.score       # replaces max_by
xs.min:  _1.score       # replaces min_by
xs.group: _1.dept       # replaces group_by
```

One rule across the collection API: the optional block maps an element to the value the method operates by. `sort_by`, `sorted_by`, `min_by`, `max_by`, `group_by` are **not** provided — they are this block. (The *default*, no-block behavior differs per method by capability — `sorted` requires an ordering `<=>`, `uniq` requires equality/hash — but the block convention itself is uniform.)

Three deliberate constraints:

1. **The block is a key extractor, never a comparator.** Arity 1, returns a key. It is *not* overloaded to also mean a 2-arity comparator by inspecting arity. With implicit `_1`/`_2` params, arity-based dispatch would force the reader to count underscores to know whether the block returns a key or an ordering — two return contracts under one form, exactly the local ambiguity §4–5 exist to remove.

2. **Descending and multi-key live on the key side**, not in comparators:

   ```amber
   xs.sorted(reverse: true): _1.age      # descending, single key
   xs.sorted: [_1.dept, desc(_1.pay)]    # multi-key, mixed direction
   ```

   `reverse:` flips a single-key sort; `desc(...)` is a key wrapper for mixed-direction multi-key. Because the default sort is stable (§10.1), arbitrary multi-level orderings are also reachable by successive stable sorts.

3. **Raw comparators are an explicit escape hatch**, taken as a *value*, not a block, so there is nothing to guess:

   ```amber
   xs.sorted(using: comparator)
   ```

   This is the rare case — a domain ordering not expressible as a key. Mirrors Python's `functools.cmp_to_key`.

Rationale: this is Python's model (`sorted(key=…, reverse=…)`, with the comparator deliberately removed in Py3 as slower and muddier — a key is invoked O(n) times, a comparator O(n log n)). It collapses an entire family of names into one convention and removes Ruby's inconsistency, where `uniq` takes an optional key block but `sort` splits off a separate `sort_by`.

---

## 8. Per-collection specification

Only the deltas from the original Variant-B tables are called out; where this RFC agrees with B's §3.6, it is adopted as-is.

### 8.1 Array

```amber
# pure
xs.appended(value)
xs.inserted(index, *values)
xs.deleted(value)
xs.tail()              # all but first — pure twin of shift!
xs.init()              # all but last  — pure twin of pop!
xs.select: _1.good?
xs.reject: _1.bad?
xs.filter: _1.good?
xs.map: transform(_1)
xs.sorted()                       # natural order
xs.sorted: _1.key                 # by key (optional block, §7.2)
xs.sorted(reverse: true): _1.key  # descending
xs.sorted(using: comparator)      # explicit comparator value (rare)
xs.reversed()
xs.uniq()                         # by identity
xs.uniq: _1.key                   # by key
xs.compact()
xs.flatten(depth: null)
xs.rotated(count: 1)

# mutating
xs.push!(value)        # alias: append!(value)
xs.unshift!(value)     # alias: prepend!(value)
xs.insert!(index, *values)
xs.pop!()              # returns removed element
xs.shift!()            # returns removed element
xs.delete_at!(index)   # returns removed element
xs.delete!(value)
xs.delete_if!: _1.bad?
xs.select!: _1.good?
xs.reject!: _1.bad?
xs.keep_if!: _1.good?
xs.map!: transform(_1)
xs.sort!()                        # natural order
xs.sort!: _1.key                  # by key (optional block, §7.2)
xs.reverse!()
xs.uniq!()                        # by identity
xs.uniq!: _1.key                  # by key
xs.compact!()
xs.flatten!(depth: null)
xs.rotate!(count: 1)
xs.clear!()
xs.replace!(other)
```

Deliberately absent: bare `push`, `insert`, `delete_at`, `delete`, `delete_if`, `keep_if`, `clear`, `replace`, and `<<` (see §9). Pure `prepended`, positional `replaced`, pure `deleted_at` are intentionally not provided (§7.1). The `_by` family (`sort_by`, `sorted_by`, `uniq_by`, `min_by`, `max_by`) is absent — those are the optional key block (§7.2).

### 8.2 Map

```amber
# pure
m.with(key, value)
m.without(key)
m.merge(other)
m.merge(other) |key, old, new|: combine(old, new)
m.except(*keys)
m.slice(*keys)
m.compact()
m.select |k, v|: pred(k, v)
m.reject |k, v|: pred(k, v)
m.transform_keys |k, v|: new_key(k)
m.transform_values |v, k|: new_value(v)

# mutating
m.store!(key, value)
m.delete!(key)              # returns removed value
m.delete_if! |k, v|: pred(k, v)
m.merge!(other)
m.update!(other)
m.select! |k, v|: pred(k, v)
m.reject! |k, v|: pred(k, v)
m.keep_if! |k, v|: pred(k, v)
m.transform_keys! |k, v|: new_key(k)
m.transform_values! |v, k|: new_value(v)
m.compact!()
m.clear!()
m.replace!(other)
m.shift!()                  # returns (key, value)
```

Note: prefer a single insertion mutator. This RFC keeps `store!` and drops `put!` — two names for "insert/overwrite a key" is exactly the redundancy the rule is meant to remove. `m[key] = value` remains the idiomatic single-key write (assignment syntax, §5.1 rule 5).

### 8.3 Set

```amber
# pure
s.added(value)
s.deleted(value)
s.union(other)
s.intersection(other)
s.difference(other)
s.symmetric_difference(other)
s.select: pred(_1)
s.reject: pred(_1)
s.filter: pred(_1)

# mutating
s.add!(value)
s.delete!(value)
s.merge!(other)
s.subtract!(other)
s.delete_if!: pred(_1)
s.select!: pred(_1)
s.reject!: pred(_1)
s.keep_if!: pred(_1)
s.clear!()
s.replace!(other)
```

Note: the pure union is spelled `union`. There is **no** pure `Set#merge` — the only `merge` on Set is the mutating `merge!`. Providing a pure/mutating pair that differs only by a bang, on an operation many users won't realize mutates, is exactly the trap the bang law avoids; the distinct name `union` removes the ambiguity.

### 8.4 Tuple / Range / LazySeq

Immutable and lazy collections have **no** `!` methods at all — which under this RFC is not a special case, it is the rule working correctly (nothing here can mutate, so nothing is banged).

```amber
# Tuple — pure only
t.updated(index, value)
t.appended(value)
t.prepended(value)
t.deleted_at(index)

# Range / LazySeq — pure, lazy
r.lazy().map: _1 * 2
lz.map: _1 * 2
lz.take(10).to_a()
```

---

## 9. The `<<` question

**Decision: do not introduce `<<` for collection append/add in v1.**

The original doc justifies this as "it would break the Bang Law." The better, principled reason: **`<<` reads as pure.** It looks like a symmetric operator and in several languages is non-mutating stream insertion. Making it silently mutate is *precisely* the hidden-mutation hazard this whole RFC exists to prevent. Excluding it is principled, not merely legalistic.

(Note the asymmetry with `[]=`, which *is* exempted in §5.1: `[]=` visibly assigns, so it announces its effect; `<<` hides it.)

If append-sugar proves necessary later, revisit it with real usage data. Do not invent `<<!`. The idiomatic append is `xs.push!(x)`, and the idiomatic pure append is `xs.appended(x)` or `xs + [x]`.

---

## 10. Return values

```text
In-place mutators                -> return self (enables chaining of mutations)
Extracting mutators              -> return the removed value / entry
  pop! / shift! / delete_at!     -> removed element
  Map#delete!(key)               -> removed value
  Map#shift!                     -> (key, value)
```

### 10.1 Kill Ruby's nil-on-no-change footgun (normative)

Ruby's `select!`, `reject!`, `compact!`, etc. return `nil` when nothing changed — a notorious trap in exactly the chains Amber relies on. **Amber `!` methods MUST NOT do this.** A mutator returns `self` (or the removed value for extractors) unconditionally; "nothing changed" is not signaled by `nil`. This is orthogonal to A vs B and must hold regardless.

---

## 11. Tooling: the rule teaches itself

A property B has and A structurally cannot: because bare `push` / `delete_at` / `keep_if` either don't exist or are pure, the compiler can emit precise, actionable diagnostics.

```text
error: `Array#push` does not exist; it would mutate the receiver.
       help: use `push!` to mutate in place, or `appended` for a copy.

error: `xs.keep_if` does not exist.
       help: use `keep_if!` to mutate in place.
```

This converts learnability from "read the docs and memorize" into "the call site corrects you." It is a first-class reason to prefer this design and should ship *with* it, not later. Variant A cannot offer this, because under A bare `push` legitimately mutates — there is nothing to correct.

**Lint, additionally:** flag a non-`!` collection method call whose result is discarded (`xs.appended(y)` as a statement) — it is almost always a bug where the user expected mutation. Pairs naturally with the bang law.

---

## 12. Performance & idiom guidance (tie-in to interpreter/native work)

The strict rule taxes imperative accumulation: builders and parsers will be `push!` everywhere. **This is the intended idiom, not a code smell** — document it explicitly. Two cautions for the stdlib guide:

- Bless `push!`-in-a-loop as the correct way to build a collection incrementally.
- Warn that the "no-bang aesthetic" must **not** push users toward `xs = xs + [x]` inside hot loops — that is O(n²) and would quietly erode the interpreter/native-backend gains already landed. Mutation is the right tool when you are building; the rule just asks you to *mark* it.

---

## 13. Rationale & alternatives considered

### 13.1 Variant A — Ruby-like structural verbs

Rejected. Its only real advantage is Ruby muscle memory, which is explicitly *not* an Amber goal. Its costs are permanent: a two-criteria mental model (twin? structural verb?), an exception table in the spec (§2.7), the indefensible `select`/`keep_if` split (§4), no clean teaching diagnostics, and degraded chain auditability.

### 13.2 Variant B verbatim — strict bang mutation

Adopted in spirit, refined in three ways: (1) the rule is lifted to **language-wide** scope and `!` is made mono-semantic (§6) — the original doc's biggest omission; (2) the pure vocabulary is **curated rather than mirrored** (§7.1) so B doesn't recreate its own memorization burden; (3) explicit nil-on-no-change ban (§10.1), tooling (§11), and perf idiom (§12) are made normative parts of the package rather than left implicit.

### 13.3 Swift-style — tense only, no bang

Considered and rejected as the primary mechanism. Swift distinguishes `sort` (mutating) from `sorted` (pure) using tense alone, no bang. It is elegant and proves the participle naming works. But for Amber's goals it loses on **auditability**: `!` is a single, unambiguous, *greppable* token — you can mechanically enumerate every mutation site in a program, which matters for the planned async layer. Tense distinctions are easy to misread (`reverse`/`reversed`, one letter) and impossible to grep reliably. This RFC keeps both signals (bang *and* participle); the redundancy is deliberate defense-in-depth, not waste.

### 13.4 Why not lean on the type system (Rust `&mut`)

Rust gets effect visibility from `&mut self` rather than naming. That requires a borrow/ownership model Amber does not have and should not adopt for this. Naming is the right layer for Amber.

---

## 14. Resolved decisions

These were open in earlier drafts; all are now decided.

1. **Fallibility (§6).** `.or_raise` combinator on a returned result/optional. `!`-as-name-suffix stays reserved for mutation; there is no `save!`/`parse!` raising variant. Companion `.or(default)` covers the non-raising fallback.
2. **Frozen receivers (§5.3).** `!` on a frozen receiver is a compile-time error when mutability is statically known, a runtime error otherwise. Non-`!` methods stay valid.
3. **`store!` vs `[]=` (§8.2).** Keep both. `[]=` is the idiomatic single-key write; `store!` exists for chaining. `put!` is dropped (one insertion verb, not two).
4. **`merge` on Set (§8.3).** Pure union is `union`; the only `merge` on Set is the mutating `merge!`. No pure `Set#merge`.
5. **Extractor pure twins (§8.1).** `init` (all but last) and `tail` (all but first) for `Array`, named functionally rather than as `popped`/`shifted`. The element reads `first` / `last` complement them.
6. **Sort/uniq/by-family shape (§7.2).** No `_by` suffix family. `sorted`, `uniq`, `min`, `max`, `group` take an optional arity-1 **key block**; descending/multi-key via `reverse:` and `desc()`; raw comparators via an explicit `using:` value. The block is never overloaded to be a comparator by arity.

---

## 15. Spec patch (replaces STD-007)

```text
STD-007  Mutation naming and the bang law

1. `!` is the language-wide marker of observable receiver mutation.
   - A stdlib method ending in `!` MAY mutate its receiver and is the only
     permitted way for a stdlib method to do so.
   - A stdlib method NOT ending in `!` MUST NOT mutate its receiver.
   - `!` carries no other meaning in stdlib; it is NOT used for
     raise-on-failure. Fallibility uses `.or_raise` on a returned
     result/optional value, never a `!` suffix.

2. Operations with both forms: `name(...)` is pure, `name!(...)` mutates.
   Operations with only an in-place meaning are written `name!`; no bare
   form is introduced.

3. Bare in-place mutators are not part of v1 stdlib:
   `push`, `insert`, `delete_at`, `delete`, `delete_if`, `keep_if`, `clear`,
   `replace`, `store`, `add`, `subtract`, `update`, `merge` (when mutating).

4. `keep_if` / `delete_if` exist only as `keep_if!` / `delete_if!`.

5. `[]=` and assignment syntax remain valid and exempt (they visibly assign).

6. `<<` is NOT introduced for collection append/add in v1.

7. `!` mutators return `self`; extracting mutators return the removed
   value/entry. `!` methods MUST NOT return nil to signal "no change".

8. Pure copy-edit APIs are provided where ergonomic and common, NOT mirrored
   exhaustively: `with`, `without`, `appended`, `deleted`, `added`,
   `sorted`, `reversed`, `uniq`, `compact`, `merge`, `init`, `tail`,
   set algebra. Pure twins of extractors are `init` / `tail`, not
   `popped` / `shifted`.

8a. Methods that compare/dedupe/group take an optional arity-1 key block, not
    a `_by` variant. `sort_by`, `sorted_by`, `uniq_by`, `min_by`, `max_by`,
    `group_by` are not provided. The key block is never overloaded to be a
    2-arity comparator. Descending uses `reverse:`; mixed-direction multi-key
    uses a `desc()` key wrapper; raw comparators use `using:` taking a
    comparator value.

9. Immutable/lazy collections (Tuple, Range, LazySeq) expose no `!` methods.

10. Invoking a `!` method on a frozen receiver is a compile-time error when
    mutability is statically known, otherwise a runtime error.

11. The compiler SHOULD emit a corrective diagnostic when a bare mutator name
    is used (e.g. `push` -> "use `push!` to mutate, or `appended` for a copy").
```

---

## 16. Recommendation

Adopt this RFC: **Variant B, lifted to a language-wide mono-semantic `!`, with a curated pure vocabulary, a hard nil-on-no-change ban, self-teaching diagnostics, and blessed in-place building.**

The gating decision (§6) is now resolved: `!` means mutation and nothing else, and fallibility lives in `.or_raise` on a returned value. With that settled, Amber earns a property neither Ruby nor Swift has: *every mutation in a program is both locally visible and globally greppable.* That is worth more to Amber's expression-oriented, async-bound future than Ruby muscle memory ever could be.

Compromise framing for the language guide:

```text
Amber borrows Ruby's `?` and `!` suffixes but gives `!` a single, strict
meaning Ruby never committed to: `!` marks receiver mutation, everywhere.
No bang, no mutation. If you can grep for `!`, you can find every place
your program changes state in place.
```
