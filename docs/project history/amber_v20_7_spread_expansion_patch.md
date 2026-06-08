# Amber v20.7 Draft Patch: Spread Expansion for Calls and Collection Literals

**Status:** proposed normative extension for the main Amber language specification and runtime collection layer  
**Target base:** Amber v20.6 value-keyed `Map` / `Set` + explicit `HashMap` / `HashSet`; Amber v20.5 optional bracket access; Amber v20.1.1 conditional collection elements  
**Patch scope:** positional argument spread, keyword argument spread, array/set/map literal spread, parser grammar, AST/HIR lowering, runtime expansion helpers, diagnostics and conformance corpus  
**Non-goals:** general prefix `*` / `**` operators, splat assignment/destructuring, rest parameters, Ruby-compatible `to_a`/`to_hash` aliases, implicit string-to-symbol keyword conversion, lazy infinite spread, macro/tagged literal syntax

---

# 0. Integration note

This patch introduces spread expansion in Amber.

The accepted surface forms are:

```amber
fn(1, *args, **kwargs)

[1, *items, 9]

{1, *items, 9}          # Set spread

{a: 1, **other, b: 2}   # Map spread
```

`*` and `**` are contextual spread markers. They are not general prefix operators.

Spread expansion is intentionally placed in v20.7, not v20.6. v20.6 defines value-keyed ordered `Map` / `Set`, explicit `HashMap` / `HashSet`, strict `Hashable`, and typed collection literals. v20.7 builds on those collection semantics without changing them.

---

# 1. Design principles

## 1.1. Spread markers are syntactic, not ordinary operators

The tokens `*` and `**` gain spread meaning only in these syntactic positions:

1. call argument lists;
2. array literal element lists;
3. set literal element lists;
4. map literal entry lists.

They are not valid as standalone prefix expressions:

```amber
*xs      # invalid outside spread position
**opts   # invalid outside spread position
```

Required diagnostic:

```text
E_SPREAD_POSITION
`*` spread is only valid in call arguments and collection literals
```

For `**`:

```text
E_KWARG_SPREAD_POSITION
`**` spread is only valid in call arguments and map literals
```

---

## 1.2. Evaluation is left-to-right

Spread expressions follow ordinary left-to-right evaluation order.

For calls:

```amber
fn(mark(1), *mark_args(), mode: mark(2), **mark_kwargs())
```

Evaluation order is:

```text
fn
mark(1)
mark_args()
mark(2)
mark_kwargs()
call
```

For literals:

```amber
[
  mark(1),
  *mark_items(),
  mark(2),
]
```

Evaluation order is:

```text
mark(1)
mark_items()
spread result
mark(2)
```

---

## 1.3. Conditional spread follows conditional collection element semantics

Spread entries may use the existing trailing conditional syntax:

```amber
[
  1,
  *extra if include_extra?,
  9,
]

{
  a: 1,
  **extra if include_extra?,
  b: 2,
}
```

If the condition is falsy, the spread expression is not evaluated.

This matches conditional collection elements: a suppressed element or entry is absent and its value expression is not evaluated.

---

## 1.4. Infinite spread is forbidden

Spread is eager. It must not silently exhaust or truncate infinite/open-ended collections.

Invalid:

```amber
[0, *(1..)]
fn(*(1..))
HashSet{*(1..)}
```

Required error:

```text
InfiniteCollectionError
cannot spread an infinite/open-ended collection
```

---

# 2. Positional call spread

## 2.1. Surface form

```amber
fn(1, *args, 4)
```

Example:

```amber
args = [2, 3]
fn(1, *args, 4)
```

is equivalent to:

```amber
fn(1, 2, 3, 4)
```

---

## 2.2. Accepted positional spread values

In v20.7, positional call spread accepts these finite values:

```text
Array
Tuple
finite Range
```

Future revisions may extend spreadability to a general finite iterable protocol. This patch deliberately keeps the v1 rule narrow so expansion is deterministic and diagnostics are clear.

Invalid:

```amber
fn(*123)
fn(*object_without_spread_protocol)
```

Required error:

```text
TypeError
positional spread requires Array, Tuple, or finite Range
```

---

## 2.3. No implicit `to_array()` / `to_tuple()` dispatch in core spread

Core spread does not silently invoke user-defined conversion methods such as `to_array()` or `to_tuple()`.

Rationale:

* spread should not hide arbitrary user code execution before argument assembly;
* conversion methods are ordinary dynamic dispatch and may have side effects;
* the v20.7 core rule should remain predictable.

A future stdlib protocol may introduce explicit spread adapters.

---

# 3. Keyword call spread

## 3.1. Surface form

```amber
fn(1, *args, mode: :fast, **kwargs)
```

Example:

```amber
kwargs = {mode: :fast, debug: true}
fn("build", **kwargs)
```

is equivalent to:

```amber
fn("build", mode: :fast, debug: true)
```

---

## 3.2. Accepted keyword spread values

In v20.7, keyword spread accepts `Map` and `HashMap` values whose keys are all `Symbol` keys.

Valid:

```amber
fn(**{name: "Ada"})
fn(**{:name: "Ada"})
fn(**HashMap{name: "Ada"})
```

Invalid:

```amber
fn(**{"name": "Ada"})
fn(**{1: "Ada"})
fn(**{[1, 2]: "Ada"})
```

Required error:

```text
TypeError
keyword argument spread requires Map or HashMap with Symbol keys
```

Rationale:

* Amber preserves `{name: value}` as Symbol-key shorthand;
* `{"name": value}` is a real `Str` key, not a keyword-compatible key;
* keyword argument names are part of call syntax and should not be produced by arbitrary non-symbol map keys.

---

## 3.3. Duplicate keyword arguments are errors

Duplicate keyword arguments are rejected, regardless of whether they come from ordinary keyword syntax or spread.

Invalid:

```amber
fn(a: 1, a: 2)
fn(a: 1, **{a: 2})
fn(**{a: 1}, **{a: 2})
```

Required error:

```text
ArgumentError
duplicate keyword argument `a`
```

This differs from map literal merge semantics. In map literals, later entries overwrite earlier values. In calls, duplicate keywords are programmer errors.

---

## 3.4. Positional and keyword ordering restrictions

Valid:

```amber
fn(1, *args, mode: :fast, **opts)
```

Invalid:

```amber
fn(mode: :fast, *args)
fn(**opts, 1)
fn(**opts, mode: :fast)
```

Normative rules:

1. ordinary positional arguments and positional spreads must appear before keyword arguments;
2. ordinary keyword arguments and keyword spreads must appear after all positional arguments;
3. no ordinary keyword argument may appear after a keyword spread;
4. no positional argument may appear after any keyword argument or keyword spread.

Required diagnostic:

```text
E_ARGUMENT_ORDER
positional arguments and `*` spreads must appear before keyword arguments and `**` spreads
```

For ordinary keyword after keyword spread:

```text
E_ARGUMENT_ORDER
ordinary keyword arguments must appear before `**` keyword spread
```

---

# 4. Array literal spread

## 4.1. Surface form

```amber
[1, *items, 9]
```

Example:

```amber
items = [2, 3]
[1, *items, 4]
# [1, 2, 3, 4]
```

---

## 4.2. Lowering model

```amber
[1, *items, 4]
```

lowers conceptually to:

```amber
tmp = []
tmp.push(1)
tmp.extend_spread(items)
tmp.push(4)
tmp
```

The resulting array is fresh.

---

## 4.3. Accepted array spread values

Array literal spread accepts:

```text
Array
Tuple
finite Range
```

Invalid:

```amber
[1, *123]
[1, *(1..)]
```

Required errors:

```text
TypeError
array spread requires Array, Tuple, or finite Range
```

and:

```text
InfiniteCollectionError
cannot spread an infinite/open-ended collection
```

---

## 4.4. Conditional array spread

Valid:

```amber
[
  "base",
  *extra if include_extra?,
  "tail",
]
```

If `include_extra?` is falsy, `extra` is not evaluated.

---

# 5. Set literal spread

## 5.1. Surface form

```amber
{1, *items, 9}
```

For explicit set literals:

```amber
Set{1, *items, 9}
HashSet{1, *items, 9}
```

---

## 5.2. Lowering model

```amber
{1, *items, 9}
```

lowers conceptually to:

```amber
tmp = Set.new()
tmp.add(1)
tmp.extend_spread(items)
tmp.add(9)
tmp
```

For `HashSet{...}`, `tmp` is a `HashSet` and every inserted element must be hashable.

---

## 5.3. Accepted set spread values

Set spread accepts:

```text
Array
Tuple
Set
HashSet
finite Range
```

Future revisions may allow any finite iterable.

For `Set{...}`, elements use v20.6 value-key equality.

For `HashSet{...}`, every spread element must satisfy the strict `Hashable` protocol.

Invalid:

```amber
HashSet{*items_with_unhashable_object}
```

Required error:

```text
TypeError
HashSet element must be Hashable
```

---

## 5.4. Conditional set spread

Valid:

```amber
{
  :base,
  *extra_permissions if enabled?,
}
```

If `enabled?` is falsy, `extra_permissions` is not evaluated.

---

# 6. Map literal spread

## 6.1. Surface form

```amber
{a: 1, **other, b: 2}
```

For explicit typed map literals:

```amber
Map{a: 1, **other, b: 2}
HashMap{a: 1, **other, b: 2}
```

---

## 6.2. Lowering model

```amber
{a: 1, **other, b: 2}
```

lowers conceptually to:

```amber
tmp = {}
tmp[:a] = 1
tmp.extend_entries_from(other)
tmp[:b] = 2
tmp
```

For `HashMap{...}`, `tmp` is a `HashMap` and every inserted key must be hashable.

---

## 6.3. Accepted map spread values

Map literal spread accepts:

```text
Map
HashMap
```

Invalid:

```amber
{a: 1, **[[:b, 2]]}
```

Required error:

```text
TypeError
map spread requires Map or HashMap
```

No implicit pair-list-to-map conversion is performed by core spread.

---

## 6.4. Map spread accepts arbitrary map keys

Unlike keyword argument spread, map literal spread accepts arbitrary valid map keys.

Valid:

```amber
{**{1: "one", "name": "Ada", :age: 36}}
```

For `HashMap{...}`, all inserted keys must additionally be `Hashable`.

Valid only if every key is hashable:

```amber
HashMap{**{1: "one", "name": "Ada", :age: 36}}
```

---

## 6.5. Duplicate map keys overwrite

Map literal spread uses ordinary map insertion/update semantics.

```amber
m = {a: 1, **{a: 2}}
m[:a] # 2

m2 = {**{a: 1}, a: 2}
m2[:a] # 2
```

This is intentionally different from call keyword spread, where duplicate keyword arguments are errors.

---

## 6.6. Conditional map spread

Valid:

```amber
{
  a: 1,
  **extra if include_extra?,
  b: 2,
}
```

If `include_extra?` is falsy, `extra` is not evaluated.

---

# 7. Grammar additions

## 7.1. Call arguments

Reference grammar:

```ebnf
CallArg
  ::= Expr
   |  Identifier ":" Expr
   |  "*" Expr
   |  "**" Expr
```

Ordering restrictions are semantic/parser validation rules, not precedence rules.

---

## 7.2. Array and set elements

Reference grammar:

```ebnf
CollectionElement
  ::= Expr CollectionCondition?
   |  "*" Expr CollectionCondition?
```

Where:

```ebnf
CollectionCondition
  ::= "if" Expr
   |  "unless" Expr
```

---

## 7.3. Map entries

Reference grammar:

```ebnf
MapEntry
  ::= MapKey ":" Expr CollectionCondition?
   |  "**" Expr CollectionCondition?
```

`MapKey` follows the v20.6 expression-keyed map literal rules:

```amber
{name: v}         # Symbol key :name
{(name): v}       # expression key
{"name": v}       # Str key
{:name: v}        # explicit Symbol key
{(1..5): v}       # parenthesized Range key
```

---

# 8. AST and HIR representation

Parser output should remain syntax-faithful.

Recommended AST nodes:

```text
AstCallArgPositional(expr)
AstCallArgKeyword(name, expr)
AstCallArgSpread(expr)
AstCallArgKeywordSpread(expr)

AstArrayElement(expr, condition?)
AstArraySpread(expr, condition?)

AstSetElement(expr, condition?)
AstSetSpread(expr, condition?)

AstMapEntry(key, value, condition?)
AstMapSpread(expr, condition?)
```

Recommended HIR nodes:

```text
HCallArgSpread(expr)
HCallKwargSpread(expr)

HArraySpread(expr)
HSetSpread(expr)
HMapSpread(expr)
```

---

# 9. Runtime helpers

Recommended runtime helper boundaries:

```text
spread_positional_values(vm, frame, value) -> Value[] | Error
spread_array_values(vm, frame, value) -> Value[] | Error
spread_set_values(vm, frame, value) -> Iterable<Value> | Error
spread_map_entries(vm, frame, value) -> Iterable<(Value, Value)> | Error
spread_keyword_entries(vm, frame, value) -> Iterable<(Symbol, Value)> | Error
```

For `HashMap` / `HashSet` targets, insertion helpers must enforce the v20.6 `Hashable` protocol.

---

# 10. Bytecode strategy

This patch does not require one opcode per spread form.

Recommended lowering strategy:

1. For literals, compile to ordinary collection construction plus extend helpers:
   ```text
   ARRAY_NEW
   ARRAY_PUSH
   ARRAY_EXTEND_SPREAD

   SET_NEW
   SET_ADD
   SET_EXTEND_SPREAD

   MAP_NEW
   MAP_PUT
   MAP_EXTEND_SPREAD
   ```
2. For calls, compile spread calls through a packed-argument call path:
   ```text
   CALL_EXPANDED dst, callee, positional_vector, keyword_map
   ```
   or an equivalent VM helper.

The exact opcode names are implementation-defined, but verifier/disassembler/golden tests must preserve spread behavior deterministically.

---

# 11. Diagnostics

## 11.1. Spread outside valid position

```text
E_SPREAD_POSITION
`*` spread is only valid in call arguments and collection literals
```

```text
E_KWARG_SPREAD_POSITION
`**` spread is only valid in call arguments and map literals
```

## 11.2. Invalid positional spread value

```text
TypeError
positional spread requires Array, Tuple, or finite Range
```

## 11.3. Invalid array spread value

```text
TypeError
array spread requires Array, Tuple, or finite Range
```

## 11.4. Invalid set spread value

```text
TypeError
set spread requires Array, Tuple, Set, HashSet, or finite Range
```

## 11.5. Invalid map spread value

```text
TypeError
map spread requires Map or HashMap
```

## 11.6. Invalid keyword spread value

```text
TypeError
keyword argument spread requires Map or HashMap with Symbol keys
```

## 11.7. Duplicate keyword argument

```text
ArgumentError
duplicate keyword argument `name`
```

## 11.8. Infinite spread

```text
InfiniteCollectionError
cannot spread an infinite/open-ended collection
```

## 11.9. Hash target rejects non-hashable spread element/key

```text
TypeError
HashSet element must be Hashable
```

```text
TypeError
HashMap key must be Hashable
```

---

# 12. Formatter rules

Formatter should preserve spread markers without extra whitespace after `*` / `**`:

```amber
fn(1, *args, **kwargs)
[1, *items, 9]
{a: 1, **other, b: 2}
```

For multiline forms:

```amber
fn(
  1,
  *args,
  mode: :fast,
  **kwargs,
)
```

Conditional spread should format like conditional collection elements:

```amber
[
  base,
  *extra if include_extra?,
  tail,
]
```

---

# 13. Conformance corpus

## 13.1. Positional call spread

```amber
args = [2, 3]
assert(fn(1, *args, 4) == fn(1, 2, 3, 4))
```

## 13.2. Tuple call spread

```amber
args = (2, 3)
assert(fn(1, *args) == fn(1, 2, 3))
```

## 13.3. Range call spread

```amber
assert(fn(*(1..3)) == fn(1, 2, 3))
```

## 13.4. Open-ended range spread rejected

```amber
assert_raises(InfiniteCollectionError):
  fn(*(1..))
```

## 13.5. Keyword spread with symbol keys

```amber
opts = {mode: :fast, debug: true}
assert(fn(**opts) == fn(mode: :fast, debug: true))
```

## 13.6. Keyword spread rejects string key

```amber
assert_raises(TypeError):
  fn(**{"mode": :fast})
```

## 13.7. Duplicate keyword rejected

```amber
assert_raises(ArgumentError):
  fn(mode: :slow, **{mode: :fast})
```

## 13.8. Array spread

```amber
assert([1, *[2, 3], 4] == [1, 2, 3, 4])
assert([1, *(2..4), 5] == [1, 2, 3, 4, 5])
```

## 13.9. Conditional array spread suppresses expression

```amber
called = false

def extra():
  called = true
  [2, 3]

xs = [1, *extra() if false, 4]
assert(xs == [1, 4])
assert(called == false)
```

## 13.10. Set spread collapse

```amber
s = {1, *[1, 2, 3]}
assert(s.count() == 3)
```

## 13.11. HashSet spread enforces Hashable

```amber
class NoHash:
  def ==(other):
    true

assert_raises(TypeError):
  HashSet{* [NoHash()]}
```

## 13.12. Map spread with arbitrary keys

```amber
m = {a: 1, **{1: "one", "name": "Ada"}}
assert(m[:a] == 1)
assert(m[1] == "one")
assert(m["name"] == "Ada")
```

## 13.13. Map spread duplicate overwrite

```amber
m = {a: 1, **{a: 2}}
assert(m[:a] == 2)
```

## 13.14. HashMap spread enforces Hashable keys

```amber
class EqOnly:
  def ==(other):
    true

assert_raises(TypeError):
  HashMap{**{EqOnly(): 1}}
```

## 13.15. Argument order diagnostics

```amber
assert_syntax_error:
  fn(mode: :fast, *args)

assert_syntax_error:
  fn(**opts, 1)
```

---

# 14. Rejected alternatives

## 14.1. General prefix spread operators

Rejected:

```amber
x = *items
x = **opts
```

Reason: spread has meaning only when a receiver context defines how expanded values are consumed. A general prefix operator would require standalone runtime values for “spread packs”, complicating evaluation and diagnostics.

---

## 14.2. Implicit conversion through `to_array()` / `to_map()`

Rejected for v20.7 core spread.

Reason: spread should not silently call user-defined conversion methods with arbitrary side effects. Future stdlib protocols may add explicit adapters.

---

## 14.3. String-key keyword spread

Rejected:

```amber
fn(**{"name": "Ada"})
```

Reason: `{"name": value}` is a real `Str` key in v20.6. Keyword argument names are `Symbol`-like call names, not strings.

---

# 15. Normative decision summary

v20.7 fixes the following decisions:

1. `*` and `**` are contextual spread markers, not general prefix operators.
2. Calls support positional spread and keyword spread.
3. Keyword spread accepts only `Map` / `HashMap` values with `Symbol` keys.
4. Duplicate keyword arguments are errors.
5. Array literals support `*` spread.
6. Set and `HashSet` literals support `*` spread.
7. Map and `HashMap` literals support `**` spread.
8. Map literal spread accepts arbitrary valid map keys; `HashMap` spread additionally requires hashable keys.
9. Conditional spread is supported and suppresses evaluation when the condition is falsy.
10. Infinite/open-ended values cannot be spread.
11. Spread does not implicitly dispatch to `to_array()`, `to_tuple()` or `to_map()` in the core language.
