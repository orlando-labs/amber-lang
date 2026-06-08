# Amber v20.6 Draft Patch: Value-Keyed `Map` / `Set`, `HashMap` / `HashSet`, and Expression Map Keys

**Status:** proposed normative extension for the main Amber language specification and runtime collection layer  
**Target base:** Amber v20.5 optional bracket access + Amber v20.4 range/indexing layer + Amber core collections stdlib plan  
**Patch scope:** `Map` and `Set` key semantics, explicit `HashMap` / `HashSet`, strict `Hashable` protocol, typed collection literal constructors, map literal syntax, parser/HIR lowering, bytecode, runtime value equality, diagnostics and conformance corpus  
**Non-goals:** `<=>`/ordering protocol, deep graph-key equality, mutable key tracking, pattern matching redesign, Ruby `Hash` compatibility aliases, Python dict compatibility, weak maps/identity maps, sorted maps, identity maps, persistent maps, spread expansion (`*` / `**`)

---

# 0. Integration note

This patch changes Amber `Map` and `Set` from symbol-only / identity-adjacent key behavior into ordered, value-keyed collections.

`Map` and `Set` remain ordered-vector based. v20.6 additionally introduces explicit hash-backed `HashMap` and `HashSet` types under a strict `Hashable` protocol. Hash-backed collections are opt-in and do not replace the default ordered collection literals.

The core change is:

```amber
m = {1: "int", "name": "str", :name: "sym"}

m[1]        # "int"
m["name"]  # "str"
m[:name]   # "sym"
```

and:

```amber
s = {[1, 2], (1, 2)}
s.count()  # 1
```

`Map` keys and `Set` elements are normalized and compared through a deterministic runtime key equality helper. Duplicate `Map` keys overwrite the stored value. Duplicate `Set` elements collapse.

Existing symbol-key maps remain compatible:

```amber
{name: value}
```

continues to mean:

```amber
{:name: value}
```

For an expression key named by a variable or arbitrary expression, use parentheses:

```amber
{(name): value}
{(user.id): value}
{(compute_key()): value}
```

Pattern matching and `deconstruct_keys` remain symbol-key based. Non-symbol keys are preserved in maps but are not matched by named-key map patterns.

Explicit collection literal constructors are also available:

```amber
Map{a: 1}              # explicit ordered Map
Set{1, 2, 3}           # explicit ordered Set
HashMap{a: 1}          # hash-backed map
HashSet{1, 2, 3}       # hash-backed set
```

Plain `{...}` remains ordered; one-letter prefixes such as `u{...}` and `o{...}` are intentionally not introduced.

Spread expansion (`fn(*args, **kwargs)`, `[1, *items]`, `{a: 1, **other}`) is intentionally deferred to Amber v20.7 so that v20.6 remains focused on key semantics, hashability and typed collection literals.

---

# 1. Design principles

## 1.1. `Map` and `Set` are ordered value collections

`Map` and `Set` preserve insertion order. This patch changes only key/element comparison semantics.

Implementation remains conceptually:

```text
MapEntry[] entries
```

not:

```text
HashTable buckets
```

Key lookup is linear in the number of entries in the reference implementation profile.

Future implementations may add hash acceleration only if observable key equality, insertion order, overwrite behavior and diagnostics remain identical.

---

## 1.2. Value-key equality is explicit runtime behavior

`Map` and `Set` use a dedicated runtime key equality helper, not generic object identity and not host-language pointer equality.

The helper is frame-aware because user-defined object equality may execute Amber code:

```text
map_key_equal(vm, frame, stored_key, lookup_key) -> Bool | Error
```

For built-in scalar and structural keys, the helper uses Amber value semantics.

For instance objects, it uses the stored key's `==` method, subject to strict validation rules.

---

## 1.3. User objects need meaningful value equality

Instance objects are accepted as `Map` keys and `Set` elements only when equality lookup resolves to a user-defined, non-default value equality implementation.

A default identity fallback such as `Object#==`, if present, is not sufficient.

Inherited user-defined equality is sufficient:

```amber
class A:
  def ==(other):
    other.is_a?(A)

class B < A:
  noop

{B(): 1}  # valid, because meaningful equality is inherited
```

Objects without meaningful value equality are rejected when used as keys or set elements.

Required error:

```text
TypeError
object used as Map key must define value equality with `==`
```

For `Set`:

```text
TypeError
object used as Set element must define value equality with `==`
```

---

## 1.4. Equality is stored-key dispatched

For object keys, lookup compares:

```amber
stored_key == lookup_key
```

not:

```amber
lookup_key == stored_key
```

and not a symmetric combination of both.

This rule is intentional.

It makes ordered-vector lookup deterministic and makes duplicate insertion depend on existing stored keys in insertion order.

Example:

```amber
class A:
  def ==(other):
    true

class B:
  def ==(other):
    false

a = A()
b = B()

m = {a: :stored_a}
m[b]       # :stored_a, because a == b

m2 = {b: :stored_b}
m2[a]      # KeyError, because b == a is false
```

---

## 1.5. Duplicate overwrite preserves the first stored key

When a duplicate key is inserted into a `Map`, only the value is updated. The stored normalized key is not replaced.

```amber
m = {1: "int", 1.0: "float"}

m[1]       # "float"
m[1.0]     # "float"
m.keys()   # [1]
```

Rationale:

* insertion order remains stable;
* the first spelling/representation remains visible through `keys`, `entries`, `each` and `to_a`;
* duplicate insertion has minimal mutation surface.

For `Set`, duplicate insertion keeps the first stored normalized element.

---

## 1.6. Composite keys are normalized snapshots

Array/list keys are normalized to immutable tuple snapshots before storage and lookup.

Nested arrays/lists inside tuple/list keys are normalized recursively.

```amber
xs = [1, 2]
m = {xs: :ok}

xs[0] = 9

m[(1, 2)]  # :ok
m[(9, 2)]  # KeyError
```

This normalization is equivalent to the built-in Array-to-Tuple conversion for built-in arrays, but it is a runtime-internal key normalization operation. It must not dynamically dispatch to user-defined conversion hooks.

---

## 1.7. Cyclic composite keys are rejected

Cyclic Array/Tuple structures are not supported as keys in this patch.

Invalid:

```amber
a = []
a.push(a)

{a: 1}
```

Required error:

```text
TypeError
cyclic composite Map keys are not supported
```

For `Set`:

```text
TypeError
cyclic composite Set elements are not supported
```

Rationale:

* cycle-aware structural equality is significantly more complex than ordinary tuple equality;
* rejecting cycles is deterministic and easy to diagnose;
* this patch does not introduce graph isomorphism semantics for collection keys.

---

# 2. Runtime representation

## 2.1. `MapEntry`

Runtime `MapEntry` changes from:

```text
MapEntry(symbol_id, value)
```

to:

```text
MapEntry(key: Value, value: Value)
```

The stored `key` is always a normalized key value.

Compatibility helper remains:

```text
make_symbol_map_value(...)
```

It builds a `Map` whose keys are `Symbol` values.

New canonical helper:

```text
make_map_value(entries: [(Value key, Value value)])
```

This helper normalizes and validates keys and applies duplicate-overwrite semantics.

---

## 2.2. Required key helper boundary

The runtime should expose or internally centralize the following helper boundaries:

```text
map_key_normalize(vm, frame, value) -> Value | Error
map_key_validate(vm, frame, normalized_key) -> void | Error
map_key_equal(vm, frame, stored_key, lookup_key) -> Bool | Error
map_find_index(vm, frame, map, lookup_key) -> Index | NotFound | Error
map_upsert(vm, frame, map, key, value) -> void | Error
```

`Set` must use the same normalization and equality machinery:

```text
set_find_index(vm, frame, set, element) -> Index | NotFound | Error
set_add(vm, frame, set, element) -> void | Error
```

Lookup key normalization occurs once per lookup operation, not once per existing entry.

---

# 3. Supported key types

## 3.1. Built-in scalar keys

The following built-in values are valid keys:

```text
Null
Bool
Symbol
Str
Int
Float
```

Examples:

```amber
{
  null: "null",
  true: "bool",
  :name: "symbol",
  "name": "string",
  1: "int",
  1.5: "float",
}
```

Numeric key equality follows Amber numeric equality. If Amber equality says `1 == 1.0`, then `Map` and `Set` treat them as equal keys/elements.

---

## 3.2. Float edge cases

`0.0` and `-0.0` compare according to Amber numeric equality. If Amber numeric equality treats them as equal, `Map` and `Set` must also treat them as equal.

```amber
m = {0.0: :zero}
m[-0.0]  # :zero, if 0.0 == -0.0 in Amber
```

`NaN` float values are rejected as `Map` keys and `Set` elements unless Amber explicitly defines `NaN == NaN` as true.

Recommended v1 rule:

```text
TypeError
NaN cannot be used as a Map key
```

For `Set`:

```text
TypeError
NaN cannot be used as a Set element
```

Rationale: a key that cannot equal itself cannot be reliably found after insertion.

---

## 3.3. Range keys

Ranges are valid keys.

Range keys compare structurally by effective range model:

```text
start
finish / open-ended marker
inclusive vs exclusive end
step
range kind / numeric endpoint behavior
```

Range key equality must not materialize the range.

Open-ended ranges are valid keys if their structure is equal.

```amber
m = {(1..): :open}

m[(1..)]     # :open
m[(1..:2)]   # KeyError
```

Float range validation remains governed by range construction rules. A float range without an explicit step is rejected before or during range construction, not by `Map` key validation.

Invalid:

```amber
{(1.0..2.0): :bad}
```

Required diagnostic remains:

```text
E_RANGE_FLOAT_STEP_REQUIRED
float ranges require an explicit step
```

---

## 3.4. Tuple keys

Tuples are valid keys.

Tuple keys compare element-by-element using the same key equality rules recursively.

```amber
m = {(1, "x"): :ok}

m[(1, "x")]    # :ok
m[(1.0, "x")]  # :ok, if 1 == 1.0
```

Nested arrays/lists inside tuple keys are normalized recursively to tuple snapshots.

---

## 3.5. Array/list keys

Arrays/lists are accepted as keys only through normalization to tuple snapshots.

```amber
m = {[1, 2]: :ok}

m[(1, 2)]  # :ok
m[[1, 2]]  # :ok
```

The original array identity is not part of the key.

---

## 3.6. Instance object keys

Instance object keys are valid only if they provide meaningful value equality through `==`.

The equality call must return `Bool`.

If `==` is missing, resolves only to default identity equality, or returns a non-`Bool`, key comparison raises `TypeError`.

Invalid equality result:

```amber
class BadEq:
  def ==(other):
    "yes"

m = {BadEq(): 1}
m[BadEq()]
```

Required error:

```text
TypeError
key equality method `==` must return Bool
```

---

# 4. Map literal syntax

## 4.1. Bare identifier shorthand is preserved

A bare identifier before `:` remains a symbol key shorthand.

```amber
{name: value}
```

means:

```amber
{:name: value}
```

This rule preserves existing symbol-key map behavior and conditional map entry syntax.

---

## 4.2. Explicit symbol keys

Explicit symbol keys remain valid:

```amber
{:name: value}
```

This is equivalent to:

```amber
{name: value}
```

except that the explicit symbol form is unambiguous at the source level.

---

## 4.3. Literal expression keys

The following literal keys are expression keys:

```amber
{1: value}
{1.5: value}
{"name": value}
{null: value}
{true: value}
{false: value}
{(1, 2): value}
{[1, 2]: value}
```

Important change:

```amber
{"name": 1}
```

has a `Str` key, not a symbol-compatible key.

Therefore:

```amber
m = {"name": 1}

m["name"]  # 1
m[:name]   # KeyError
m[?:name]  # null
```

---

## 4.4. Parenthesized expression keys

Any non-literal expression key must be parenthesized:

```amber
{(name): value}
{(user.id): value}
{(compute_key()): value}
{(if ok then a else b): value}
```

A bare identifier is never a variable key in map literal key position.

```amber
{name: value}    # Symbol key :name
{(name): value}  # value of variable/expression `name`
```

Rationale:

* preserves existing shorthand;
* makes expression keys explicit;
* avoids parser ambiguity with labels, conditional entries and future map-entry extensions.

---

## 4.5. Range keys require parentheses

Range expression keys must be parenthesized.

Valid:

```amber
{(1..5): value}
{(1...5): value}
{(1..5:2): value}
{(1..): value}
```

Invalid:

```amber
{1..5: value}
{1..5:2: value}
```

Required diagnostic:

```text
E_MAP_KEY_RANGE_PARENS_REQUIRED
range expression keys in map literals must be parenthesized
```

Rationale:

Range step syntax already uses `:`. Parenthesizing range keys keeps map entries visually and grammatically unambiguous.

---

## 4.6. Conditional map entries

Conditional map entries continue to apply to the whole entry, not only the value expression.

```amber
{
  name: user.name,
  "debug": true if debug?,
  (dynamic_key): value unless skip?,
}
```

If the condition is falsy, neither the key expression nor the value expression is evaluated for that entry.

This rule is especially important for dynamic expression keys with side effects.

---

# 5. Set literal behavior

`Set` construction uses the same key normalization and key equality as `Map`.

```amber
s = {[1, 2], (1, 2), 1, 1.0}

s.count()  # 2, if 1 == 1.0
```

Array/list elements are normalized to tuple snapshots.

Duplicate elements collapse while preserving the first stored normalized element.

Conditional set elements continue to control element presence:

```amber
{
  :read,
  :write if can_write?,
  [1, 2] if include_pair?,
}
```

If the condition is falsy, the element expression is not evaluated.

---

# 6. Map operations

All `Map` operations that observe, lookup, create or transform keys must use normalized arbitrary keys.

Required affected operations:

```amber
map[key]
map[?key]
map[key] = value
map.contains?(key)
map.include?(key)
map.keys()
map.values()
map.entries()
map.to_a()
map.each |k, v|:
  ...
map.map |k, v|:
  ...
map.select |k, v|:
  ...
map.reject |k, v|:
  ...
map.transform |k, v|:
  (new_key, new_value)
map.transform_values |v, k|:
  ...
map.merge(other)
map + other
map | other
pairs.to_map()
```

Strict lookup:

```amber
map[key]
```

raises `KeyError` when the normalized key is valid but absent.

Optional lookup:

```amber
map[?key]
```

returns `null` when the normalized key is valid but absent.

Optional lookup must not swallow key validation errors or equality protocol errors.

Example:

```amber
map[?bad_key]
```

still raises `TypeError` if `bad_key` is not a valid key.

---

# 7. Set operations

All `Set` operations that observe, lookup, create or transform elements must use the same normalization and equality rules as `Map` keys.

Required affected operations:

```amber
set.add(value)
set.delete(value)
set.include?(value)
set.contains?(value)
set.each |x|:
  ...
set.map |x|:
  ...
set.select |x|:
  ...
set.reject |x|:
  ...
set | other
set & other
set - other
set ^ other
items.to_set()
```

Array and Tuple membership outside `Set` remains governed by existing `value_equals` / collection equality behavior unless the operation explicitly constructs or operates through a `Set`.

---

# 8. Pattern matching and `deconstruct_keys`

Pattern matching remains symbol-key based.

Named-key map patterns observe only entries whose normalized stored key is `Symbol`.

```amber
m = {name: "Ada", "name": "string", 1: "one"}

case m
in {name: n}
  n  # "Ada"
end
```

Non-symbol keys do not satisfy named-key patterns.

`deconstruct_keys` remains symbol-key based. Non-symbol map keys are preserved in the source map and are treated as extra entries for full-map/rest behavior.

Rest behavior:

```amber
case {name: "Ada", "name": "str", 1: "one"}
in {name: n, **rest}
  rest
end
```

`rest` contains the unmatched non-symbol entries unless a more specific future pattern RFC changes rest semantics.

This patch does not introduce expression-key map patterns.

Invalid / not introduced:

```amber
case m
in {"name": x}
  ...
end
```

unless already valid under a separate pattern-matching rule outside this patch.

---

# 9. Bytecode

## 9.1. Existing `MakeMap`

Existing `MakeMap` remains valid for symbol-immediate maps and old bytecode/tests.

It constructs maps whose keys are `Symbol` values.

The existing bytecode format remains loadable.

---

## 9.2. New `MakeMapDyn`

Add bytecode opcode:

```text
MakeMapDyn = 0x0D
```

Operands:

```text
dst, count, (key_reg, value_reg)*
```

Example conceptual encoding:

```text
MakeMapDyn r0, 2, r1, r2, r3, r4
```

means:

```text
r0 = make_map_value([(r1, r2), (r3, r4)])
```

The VM normalizes keys, validates keys and applies duplicate overwrite semantics while constructing the map.

---

## 9.3. Emitter strategy

Recommended emitter strategy:

1. If all keys are static symbol keys, emit existing `MakeMap`.
2. If any key is dynamic or non-symbol, emit `MakeMapDyn`.

Examples:

```amber
{name: v, age: a}
```

may emit old `MakeMap`.

```amber
{"name": v}
```

must emit `MakeMapDyn`.

```amber
{(name): v}
```

must emit `MakeMapDyn`.

```amber
{:name: v}
```

may emit old `MakeMap` as an optimization.

---

## 9.4. Verifier requirements

The bytecode verifier must validate:

```text
count >= 0
operand_count == 2 * count + 2
all key registers are initialized before the opcode
all value registers are initialized before the opcode
source register references are in range
dst register is in range
```

If `dst` aliases a key or value source register, the VM must read all source operands before writing `dst`, or the verifier must reject such aliasing.

Malformed `MakeMapDyn` bytecode must fail verification deterministically.

---

## 9.5. Serializer, disassembler and quick analyses

The bytecode serializer, deserializer, disassembler, verifier, quick analyses and opcode registry must be updated for `MakeMapDyn`.

Disassembly should preserve the key/value register pair structure:

```text
MakeMapDyn r0, count=2, (r1 => r2), (r3 => r4)
```

or equivalent deterministic formatting.

---

# 10. Parser and HIR lowering

## 10.1. AST/HIR distinction

The parser should preserve source-faithful map key forms:

```text
BareIdentifierKey(name)
SymbolLiteralKey(:name)
StringLiteralKey("name")
LiteralExprKey(...)
ParenExprKey(...)
```

HIR lowering determines whether the map can use static symbol-key construction or dynamic key construction.

---

## 10.2. Recommended lowering

```amber
{name: value}
```

lowers to a symbol key:

```text
Symbol(:name), value
```

```amber
{:name: value}
```

lowers to the same symbol key.

```amber
{"name": value}
```

lowers to a string key expression.

```amber
{(name): value}
```

lowers to the expression value of `name`.

```amber
{[1, 2]: value}
```

lowers to an array expression key; runtime key normalization converts it to a tuple snapshot.

---

# 11. Diagnostics

## 11.1. Invalid object key

```text
TypeError
object used as Map key must define value equality with `==`
```

```text
TypeError
object used as Set element must define value equality with `==`
```

---

## 11.2. Non-Bool equality result

```text
TypeError
key equality method `==` must return Bool
```

For `Set`:

```text
TypeError
set element equality method `==` must return Bool
```

---

## 11.3. Cyclic composite key

```text
TypeError
cyclic composite Map keys are not supported
```

```text
TypeError
cyclic composite Set elements are not supported
```

---

## 11.4. NaN key

```text
TypeError
NaN cannot be used as a Map key
```

```text
TypeError
NaN cannot be used as a Set element
```

---

## 11.5. Range key parentheses

```text
E_MAP_KEY_RANGE_PARENS_REQUIRED
range expression keys in map literals must be parenthesized
```

---

## 11.6. Unsupported expression key spelling

For arbitrary unparenthesized non-literal expression keys:

```text
E_MAP_KEY_EXPR_PARENS_REQUIRED
expression keys in map literals must be parenthesized
```

Example invalid form if the parser would otherwise accept it:

```amber
{user.id: value}
```

Required form:

```amber
{(user.id): value}
```

---

# 12. Implementation rollout

## 12.1. Phase 1: runtime key model

* Change `MapEntry` to `key: Value, value: Value`.
* Add key normalization, validation, equality, find and upsert helpers.
* Update `Map` lookup, assignment and construction paths.
* Update `Set` construction and membership to use the same helper.
* Preserve old symbol-key behavior through `make_symbol_map_value(...)`.

---

## 12.2. Phase 2: compatibility and existing tests

* Keep existing `MakeMap` bytecode valid.
* Keep existing symbol-key map tests green.
* Keep pattern matching and `deconstruct_keys` symbol-key based.
* Ensure old bytecode deserializes and executes unchanged.

---

## 12.3. Phase 3: `MakeMapDyn`

* Add opcode registry entry.
* Add serializer/deserializer support.
* Add verifier support.
* Add disassembler output.
* Add quick-analysis support.
* Add VM execution support.

---

## 12.4. Phase 4: parser/HIR syntax

* Preserve `{name: value}` as symbol shorthand.
* Add literal expression map keys.
* Add parenthesized expression keys.
* Require parentheses for range keys.
* Ensure conditional map entries still control whole-entry presence.

---

## 12.5. Phase 5: collection operation audit

Update and test:

```text
Map: [], [?], assignment, contains?, include?, keys, entries, each, map, select,
     reject, transform, transform_values, merge, +, |, casts from pairs

Set: construction, add, delete, include?, contains?, algebra, to_set
```

---

# 13. Conformance tests

## 13.1. Parser/HIR tests

```amber
{name: v}        # Symbol key
{:name: v}       # Symbol key
{"name": v}     # Str key
{1: v}           # Int key
{1.5: v}         # Float key
{null: v}        # Null key
{true: v}        # Bool key
{(name): v}      # expression key
{[1, 2]: v}      # Array key, normalized at runtime
{(1, 2): v}      # Tuple key
{(1..5): v}      # Range key
{(1..5:2): v}    # stepped Range key
```

Invalid:

```amber
{1..5: v}
{1..5:2: v}
{user.id: v}
```

---

## 13.2. Bytecode tests

* `MakeMap` still serializes, deserializes, verifies and disassembles.
* `MakeMapDyn` serializes, deserializes, verifies and disassembles.
* Malformed `MakeMapDyn` operand count fails verification.
* `MakeMapDyn` with uninitialized key register fails verification.
* `MakeMapDyn` with uninitialized value register fails verification.
* Old bytecode using symbol maps remains valid.

---

## 13.3. Numeric key tests

```amber
m = {1: "a", 1.0: "b"}

assert(m[1] == "b")
assert(m[1.0] == "b")
assert(m.keys() == [1])
```

```amber
m = {0.0: :zero}
assert(m[-0.0] == :zero)
```

If `Float.nan` is available:

```amber
assert_raises(TypeError):
  {Float.nan: 1}
```

---

## 13.4. String vs Symbol key tests

```amber
m = {"name": 1}

assert(m["name"] == 1)
assert(m[?:name] == null)
assert_raises(KeyError):
  m[:name]
```

```amber
m = {name: 1}

assert(m[:name] == 1)
assert(m[?"name"] == null)
```

---

## 13.5. Range key tests

```amber
m = {(1..3): :a}

assert(m[(1..3)] == :a)
assert_raises(KeyError):
  m[(1...3)]
```

```amber
m = {(1..5:2): :odd}

assert(m[(1..5:2)] == :odd)
assert_raises(KeyError):
  m[(1..5:3)]
```

```amber
m = {(1..): :open}

assert(m[(1..)] == :open)
assert_raises(KeyError):
  m[(1..:2)]
```

---

## 13.6. Tuple and Array normalization tests

```amber
m = {(1, 2): :tuple}

assert(m[[1, 2]] == :tuple)
assert(m[(1.0, 2.0)] == :tuple)
```

```amber
m = {[1, 2]: :array}

assert(m[(1, 2)] == :array)
assert(m[[1, 2]] == :array)
```

Mutation snapshot:

```amber
xs = [1, 2]
m = {xs: :ok}

xs[0] = 9

assert(m[(1, 2)] == :ok)
assert_raises(KeyError):
  m[(9, 2)]
```

Nested normalization:

```amber
m = {([1, [2, 3]]): :nested}

assert(m[(1, (2, 3))] == :nested)
```

Cycle rejection:

```amber
a = []
a.push(a)

assert_raises(TypeError):
  {a: 1}
```

---

## 13.7. Set tests

```amber
s = {[1, 2], (1, 2)}
assert(s.count() == 1)
```

```amber
s = {1, 1.0}
assert(s.count() == 1)
```

```amber
a = []
a.push(a)

assert_raises(TypeError):
  {a}
```

---

## 13.8. Custom object equality tests

Working custom object key:

```amber
class Point:
  def init(@x, @y):
    noop

  attr x
  attr y

  def ==(other):
    other.is_a?(Point) and @x == other.x and @y == other.y

m = {Point(1, 2): :p}

assert(m[Point(1, 2)] == :p)
```

Inherited equality:

```amber
class A:
  def ==(other):
    other.is_a?(A)

class B < A:
  noop

m = {B(): :b}
assert(m[B()] == :b)
```

Missing equality:

```amber
class NoEq:
  noop

assert_raises(TypeError):
  {NoEq(): 1}
```

Non-Bool equality result:

```amber
class BadEq:
  def ==(other):
    "yes"

m = {BadEq(): 1}

assert_raises(TypeError):
  m[BadEq()]
```

Asymmetric stored-key dispatch:

```amber
class A:
  def ==(other):
    true

class B:
  def ==(other):
    false

a = A()
b = B()

m = {a: :a}
assert(m[b] == :a)

m2 = {b: :b}
assert_raises(KeyError):
  m2[a]
```

---

## 13.9. Optional bracket tests

```amber
m = {[1, 2]: :ok}

assert(m[?(1, 2)] == :ok)
assert(m[?[1, 3]] == null)
```

Optional lookup does not hide invalid keys:

```amber
class NoEq:
  noop

m = {}

assert_raises(TypeError):
  m[?NoEq()]
```

---

## 13.10. Pattern matching regression tests

```amber
m = {name: "Ada", "name": "string", 1: "one"}

case m
in {name: n}
  assert(n == "Ada")
end
```

Rest includes non-symbol unmatched keys:

```amber
case {name: "Ada", "name": "str", 1: "one"}
in {name: n, **rest}
  assert(rest["name"] == "str")
  assert(rest[1] == "one")
end
```

`deconstruct_keys` remains symbol-key based.

---

## 13.11. Regression tests

* Existing symbol-key maps.
* Existing `Map` methods.
* Existing `Set` methods.
* Conditional map entries.
* Conditional set elements.
* Optional bracket access.
* Range syntax and range-step diagnostics.
* Map patterns and `deconstruct_keys`.
* Current dirty-worktree tests must be preserved and built on, not reverted.

---

# 14. Compatibility notes

## 14.1. Source compatibility

Existing symbol-key map literals remain valid:

```amber
{name: value}
```

continues to mean symbol key `:name`.

String-key maps change only if an implementation previously treated string literal keys as symbol-compatible. Under this patch:

```amber
{"name": value}
```

is always a real `Str` key.

---

## 14.2. Bytecode compatibility

Old bytecode using `MakeMap` remains valid.

New bytecode using expression keys must use `MakeMapDyn`.

---

## 14.3. Pattern compatibility

Existing map patterns remain symbol-key based.

This patch does not make string keys match symbol-name patterns.

---

# 15. Summary of normative decisions

1. `Map` entries store `key: Value` and `value: Value`.
2. `Map` and `Set` remain ordered-vector based in the reference implementation.
3. Built-in scalar keys include `Null`, `Bool`, `Symbol`, `Str`, `Int`, `Float`.
4. `NaN` keys/elements are rejected unless Amber explicitly defines self-equality for NaN.
5. Numeric key equality follows Amber numeric equality.
6. Duplicate `Map` insertion overwrites only the value and preserves the first stored key.
7. Duplicate `Set` insertion preserves the first stored element.
8. Range keys are structural and never materialized for equality.
9. Tuple keys compare recursively through key equality.
10. Array/list keys normalize to immutable tuple snapshots.
11. Cyclic composite keys/elements are rejected.
12. Instance object keys require meaningful non-default `==` equality.
13. Object key equality is stored-key dispatched.
14. `==` used for key equality must return `Bool`.
15. `{name: value}` remains symbol shorthand.
16. `{(name): value}` is an expression key.
17. `{"name": value}` is a `Str` key.
18. Range keys in map literals require parentheses.
19. `MakeMap` remains for symbol-immediate maps.
20. `MakeMapDyn = 0x0D` constructs dynamic-key maps.
21. Optional lookup suppresses absence only, not invalid-key or equality-protocol errors.
22. Pattern matching and `deconstruct_keys` remain symbol-key based.

---

# 16. Assumptions

* No implementation edits were made in this planning document.
* Existing uncommitted parser/HIR/emitter/runtime/tests changes must be preserved and built on, not reverted.
* No `<=>` protocol is implemented for key equality in this change.
* No hash table is introduced in this change.
* No expression-key map patterns are introduced in this change.

---

# 17. `HashMap`, `HashSet` and strict `Hashable` protocol

## 17.0. Integration note

This section is a normative v20.6 addition to the value-keyed `Map` / `Set` patch.

`Map` and `Set` remain the default ordered, vector-backed, value-keyed collections described above. v20.6 also introduces explicit hash-backed collection types for workloads that require average constant-time lookup under a stricter key protocol:

```amber
Map{a: 1}              # explicit ordered Map
Set{1, 2, 3}           # explicit ordered Set

HashMap{a: 1}          # hash-backed map
HashSet{1, 2, 3}       # hash-backed set
```

Plain collection literals remain ordered:

```amber
{a: 1}                 # Map
{1, 2, 3}              # Set
```

No `u{...}` / `o{...}` one-letter literal prefixes are introduced.

---

## 17.1. Type model

Amber v20.6 has four core associative collection families:

| Type | Backing model | Key / element protocol | Iteration order |
|---|---|---|---|
| `Map` | ordered vector | value-key equality | insertion order |
| `Set` | ordered vector | value-key equality | insertion order |
| `HashMap` | hash table | strict `Hashable` | unspecified |
| `HashSet` | hash table | strict `Hashable` | unspecified |

`HashMap` and `HashSet` are performance-oriented collection types. They do not replace `Map` and `Set`.

Normative rule:

> `HashMap` and `HashSet` do not guarantee iteration order. Implementations may produce a stable order in a particular run or build, but programs must not rely on it.

If deterministic iteration is required, use `Map` / `Set`.

---

## 17.2. Performance contract

`Map` / `Set` lookup and insertion are linear in the number of stored entries in the reference profile:

```text
Map / Set lookup: O(n * equality_cost)
```

`HashMap` / `HashSet` provide average constant-time lookup when hash distribution is suitable:

```text
HashMap / HashSet lookup: average O(1), worst-case O(n)
```

Worst-case behavior remains linear because adversarial or poor hash distribution may place many keys in the same bucket.

The language does not guarantee hard real-time lookup bounds for hash-backed collections.

---

## 17.3. `Hashable` protocol

A value is `Hashable` if it satisfies one of the following:

1. it is a supported built-in hashable value;
2. it is a composite value whose normalized elements are all hashable;
3. it is an instance object with both:
   * a user-defined, non-default `==`; and
   * a user-defined, non-default `hash()` method returning `Int`.

Contract:

```text
if a == b, then a.hash() == b.hash()
```

This is a semantic requirement on user code. The runtime may diagnose obvious violations, but it is not required to prove the contract universally.

Required object shape:

```amber
class Point:
  attr x
  attr y

  def ==(other):
    other.is_a?(Point) and @x == other.x and @y == other.y

  def hash():
    Hash.combine(@x, @y)
```

Invalid:

```amber
class Point:
  def ==(other):
    true

HashMap{Point(): 1}
```

Required error:

```text
TypeError
HashMap key requires `hash`
```

For `HashSet`:

```text
TypeError
HashSet element requires `hash`
```

If `hash()` returns a non-`Int` value:

```text
TypeError
`hash` must return Int
```

---

## 17.4. Built-in hashable values

The following built-in values are hashable:

```text
Null
Bool
Symbol
Str
Int
Float except NaN
Range
Tuple of hashable keys
```

`Array` / list values used as `HashMap` keys or `HashSet` elements are normalized to immutable tuple snapshots before hashing, exactly as for ordered `Map` / `Set` key normalization.

Nested arrays/lists are normalized recursively.

Cyclic composite keys are rejected.

```amber
xs = [1, 2]
h = HashMap{xs: :ok}

xs[0] = 9

h[(1, 2)]  # :ok
h[(9, 2)]  # KeyError
```

`NaN` is rejected as a hash key or hash set element if Amber numeric equality treats `NaN != NaN`.

Required error:

```text
TypeError
NaN cannot be used as HashMap key
```

For `HashSet`:

```text
TypeError
NaN cannot be used as HashSet element
```

---

## 17.5. Hash normalization and equality

Hash-backed collections use the same normalized key model as `Map` / `Set` before hashing.

Required invariants:

1. `1` and `1.0` compare equal if Amber numeric equality says they compare equal.
2. Equal numeric keys must produce compatible hash values.
3. Array/list keys are normalized to tuple snapshots before hash computation.
4. Range hashes are structural and must not materialize ranges.
5. Tuple hashes are structural and recursively use normalized element hashes.
6. Instance object hashes are obtained by calling stored key `hash()` and validating that the result is `Int`.

Object equality remains stored-key dispatched for collision resolution:

```amber
stored_key == lookup_key
```

Hash lookup first chooses a bucket by normalized hash, then applies the same key equality semantics as ordered `Map` / `Set` inside the candidate bucket.

---

## 17.6. Duplicate insertion rule

`HashMap` duplicate insertion follows `Map` semantics:

* the later value overwrites the earlier value;
* the first stored normalized key is retained;
* iteration order remains unspecified.

Example:

```amber
hm = HashMap{1: "int", 1.0: "float"}

hm[1]       # "float"
hm[1.0]     # "float"
hm.keys()   # contains first stored key `1`; order unspecified
```

`HashSet` duplicate insertion keeps the first stored normalized element.

---

## 17.7. HashMap operations

`HashMap` supports the same user-facing operation family as `Map` where the operation is meaningful:

```amber
hm[key]
hm[?key]
hm[key] = value
hm.contains?(key)
hm.include?(key)
hm.keys()
hm.values()
hm.entries()
hm.each |k, v|:
  ...
hm.map |k, v|:
  ...
hm.select |k, v|:
  ...
hm.reject |k, v|:
  ...
hm.transform |k, v|:
  (new_key, new_value)
hm.transform_values |v, k|:
  ...
hm.merge(other)
hm + other
hm | other
```

Any operation that inserts or transforms keys into a `HashMap` must validate the resulting keys against the `Hashable` protocol.

`hm[?key]` suppresses absence only. It must not suppress `TypeError`, invalid hash protocol errors, cyclic key errors, non-`Int` hash errors or equality protocol errors.

---

## 17.8. HashSet operations

`HashSet` supports the same user-facing operation family as `Set` where the operation is meaningful:

```amber
hs.add(value)
hs.delete(value)
hs.contains?(value)
hs.include?(value)
hs.each |x|:
  ...
hs.map |x|:
  ...
hs.select |x|:
  ...
hs.reject |x|:
  ...
hs | other
hs & other
hs - other
```

Any operation that inserts elements into a `HashSet` must validate the inserted element against the `Hashable` protocol.

---

# 18. Explicit collection literal constructors

## 18.1. Canonical forms

v20.6 introduces explicit collection literal constructors:

```amber
Map{a: 1}
Set{1, 2, 3}
HashMap{a: 1}
HashSet{1, 2, 3}
```

Plain `{...}` remains ordered and continues to auto-disambiguate between `Map` and `Set` according to existing map-entry vs set-element rules:

```amber
{a: 1}       # Map
{1, 2, 3}    # Set
{}           # Map, unless the host specification already fixes another empty-literal rule
```

The explicit constructors are syntax, not ordinary method calls.

---

## 18.2. Type restrictions

`Map{...}` and `HashMap{...}` require map entries:

```amber
Map{a: 1}          # valid
HashMap{a: 1}      # valid

Map{1, 2, 3}       # invalid
HashMap{1, 2, 3}   # invalid
```

`Set{...}` and `HashSet{...}` require set elements:

```amber
Set{1, 2, 3}       # valid
HashSet{1, 2, 3}   # valid

Set{a: 1}          # invalid
HashSet{a: 1}      # invalid
```

Suggested diagnostics:

```text
E_COLLECTION_LITERAL_KIND
Map literal constructor requires key-value entries
```

```text
E_COLLECTION_LITERAL_KIND
Set literal constructor requires elements, not key-value entries
```

---

## 18.3. Grammar

Reference grammar:

```ebnf
CollectionLiteral
  ::= PlainCollectionLiteral
   |  TypedCollectionLiteral

PlainCollectionLiteral
  ::= "{" CollectionItems? "}"

TypedCollectionLiteral
  ::= CollectionType "{" CollectionItems? "}"

CollectionType
  ::= "Map"
   |  "Set"
   |  "HashMap"
   |  "HashSet"
```

`CollectionType` names are recognized only when immediately followed by `{` with no intervening newline. The formatter should emit no whitespace between the collection type and `{`:

```amber
HashMap{a: 1}
```

not:

```amber
HashMap {a: 1}
```

An implementation may accept whitespace and normalize it, but the canonical format has no whitespace.

---

## 18.4. Interaction with expression map keys

Typed map literals use the same key syntax rules as plain map literals:

```amber
HashMap{name: value}          # Symbol key :name
HashMap{:name: value}         # explicit Symbol key
HashMap{"name": value}       # Str key
HashMap{1: value}             # Int key
HashMap{(name): value}        # expression key
HashMap{(1..5): value}        # Range key
HashMap{(1..5:2): value}      # stepped Range key
```

The additional `HashMap` constraint is that the normalized key must be `Hashable`.

---

## 18.5. Interaction with conditional collection elements

Typed literals support conditional entries/elements using the existing trailing-condition model:

```amber
HashMap{
  a: 1,
  b: 2 if enabled?,
}

HashSet{
  :read,
  :write if user.editor?,
}
```

If a conditional entry/element is skipped, its key/value/element expression is not evaluated.

If it is included, `HashMap` / `HashSet` perform ordinary `Hashable` validation.

---

## 18.6. Future spread compatibility

If a later patch introduces collection spread syntax, typed hash literals follow this rule:

```amber
HashMap{a: 1, **other}
HashSet{1, *items}
```

`HashMap{**other}` may accept ordered `Map` or `HashMap` inputs, but every inserted key must be hashable.

`HashSet{*items}` may accept ordered `Set`, `HashSet`, array, tuple or finite spreadable inputs, but every inserted element must be hashable.

This patch does not introduce spread syntax; it only reserves the compatibility rule.

---

# 19. Rejected literal-prefix alternatives

## 19.1. Rejected: one-letter collection prefixes

The following forms are intentionally not introduced:

```amber
u{a: 1}    # rejected unordered/hash-backed map prefix
o{a: 1}    # rejected ordered map prefix
u{1, 2}    # rejected unordered/hash-backed set prefix
o{1, 2}    # rejected ordered set prefix
```

Rationale:

1. `u` / `o` are opaque and do not communicate `HashMap` / `Map` clearly.
2. `unordered` is an iteration-order property, not the implementation type name.
3. Single-letter prefixes do not scale to future collection families such as `SortedMap`, `IdentityMap`, `WeakMap` or `PersistentMap`.
4. `u{...}` is visually close to ordinary identifier/block syntax:

   ```amber
   u { a: 1 }
   u{a: 1}
   ```

5. The same prefix would need to mean `HashMap` or `HashSet` depending on literal contents, which makes diagnostics and readability weaker.
6. Formatter behavior would become whitespace-sensitive in a way that is not justified by the minor brevity gain.

Preferred explicit forms:

```amber
Map{a: 1}
Set{1, 2}
HashMap{a: 1}
HashSet{1, 2}
```

---

# 20. Additional diagnostics

## 20.1. Missing `hash`

```text
TypeError
HashMap key requires `hash`
```

```text
TypeError
HashSet element requires `hash`
```

## 20.2. Non-Int hash result

```text
TypeError
`hash` must return Int
```

## 20.3. Invalid hash key

```text
TypeError
value cannot be used as HashMap key because it is not Hashable
```

```text
TypeError
value cannot be used as HashSet element because it is not Hashable
```

## 20.4. Invalid typed literal contents

```text
E_COLLECTION_LITERAL_KIND
Map literal constructor requires key-value entries
```

```text
E_COLLECTION_LITERAL_KIND
Set literal constructor requires elements, not key-value entries
```

## 20.5. Rejected one-letter prefix

If the parser can recognize the likely intent, suggested diagnostic:

```text
E_COLLECTION_LITERAL_PREFIX
one-letter collection literal prefixes are not supported; use `HashMap{...}`, `HashSet{...}`, `Map{...}` or `Set{...}`
```

---

# 21. Additional conformance tests

## 21.1. Typed literal construction

```amber
assert(Map{a: 1}.is_a?(Map))
assert(Set{1, 2}.is_a?(Set))
assert(HashMap{a: 1}.is_a?(HashMap))
assert(HashSet{1, 2}.is_a?(HashSet))
```

## 21.2. Plain literals remain ordered

```amber
assert({a: 1}.is_a?(Map))
assert({1, 2}.is_a?(Set))
```

## 21.3. Hashable custom object

```amber
class Point:
  attr x
  attr y

  def init(@x, @y):
    noop

  def ==(other):
    other.is_a?(Point) and @x == other.x and @y == other.y

  def hash():
    Hash.combine(@x, @y)

m = HashMap{Point(1, 2): :ok}
assert(m[Point(1, 2)] == :ok)
```

## 21.4. Missing hash rejected

```amber
class EqOnly:
  def ==(other):
    true

assert_raises(TypeError):
  HashMap{EqOnly(): 1}

assert_raises(TypeError):
  HashSet{EqOnly()}
```

## 21.5. Non-Int hash rejected

```amber
class BadHash:
  def ==(other):
    true

  def hash():
    "not int"

assert_raises(TypeError):
  HashMap{BadHash(): 1}
```

## 21.6. Array key snapshot in HashMap

```amber
xs = [1, 2]
h = HashMap{xs: :ok}

xs[0] = 9

assert(h[(1, 2)] == :ok)
assert_raises(KeyError):
  h[(9, 2)]
```

## 21.7. Duplicate numeric key overwrite

```amber
hm = HashMap{1: "int", 1.0: "float"}

assert(hm[1] == "float")
assert(hm[1.0] == "float")
```

## 21.8. HashSet duplicate collapse

```amber
hs = HashSet{[1, 2], (1, 2)}
assert(hs.count() == 1)
```

## 21.9. Typed literal kind diagnostics

```amber
assert_syntax_error:
  HashMap{1, 2, 3}

assert_syntax_error:
  HashSet{a: 1}
```

## 21.10. Rejected one-letter prefixes

```amber
assert_syntax_error:
  u{a: 1}

assert_syntax_error:
  o{a: 1}
```

---

# 22. Updated normative decision summary

v20.6 now fixes the following additional decisions:

1. `Map` / `Set` remain default ordered value-keyed collections.
2. `HashMap` / `HashSet` are explicit hash-backed collection types.
3. Hash-backed collections require strict `Hashable` keys/elements.
4. Custom hashable objects must define non-default `==` and non-default `hash()` returning `Int`.
5. If `a == b`, user code must ensure `a.hash() == b.hash()`.
6. `HashMap` / `HashSet` do not guarantee iteration order.
7. `HashMap{...}` and `HashSet{...}` are canonical literal constructors.
8. `Map{...}` and `Set{...}` are canonical explicit ordered literal constructors.
9. Plain `{...}` remains ordered and backward-compatible.
10. One-letter prefixes such as `u{...}` and `o{...}` are rejected.
11. Spread expansion is deferred to v20.7.
