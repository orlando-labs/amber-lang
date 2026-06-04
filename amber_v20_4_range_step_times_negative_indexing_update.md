# Amber v20.4 Draft Patch: Range Step, Range Materialization, Negative Indexing and `Int#times`

**Status:** proposed normative extension for the main Amber language specification and stdlib layer  
**Target base:** Amber v20.3.1 language/spec layer + v20.3 stdlib planning layer  
**Patch scope:** range syntax, range runtime model, collection conversion, array indexing/slicing, `Int#times`, enumerable return contracts, diagnostics and conformance corpus  
**Non-goals:** Ruby compatibility aliases, typed ranges, lazy stream comprehensions, Python slice syntax, arbitrary float-indexed collections, infinite collection materialization, iterator protocol redesign

---

# 0. Integration note

This patch extends Amber's collection layer with a stricter and more Amber-native range model.

It introduces:

1. range step syntax:

```amber
start..end:step
start...end:step
start..:step
```

2. finite range materialization through:

```amber
range.to_array()
range.array
```

3. `InfiniteCollectionError` for invalid attempts to materialize infinite/open-ended collections;
4. negative indexing for arrays;
5. array slicing by `IntRange`;
6. `Int#times` as a Ruby-inspired but Amber-semantics iteration helper;
7. explicit `times.each` and `times.map` collection behavior.

This patch deliberately does **not** introduce `to_a()`.

Amber's canonical spelling is:

```amber
value.to_array()
value.array
```

not:

```amber
value.to_a()
```

`to_a` is considered too compatibility-oriented and not Amber-style.

---

# 1. Design principles

## 1.1. Amber-native conversion names

Collection materialization uses the existing conversion naming family:

```amber
to_array()
array
```

`to_array()` is the canonical method form.

`.array` is the property alias equivalent to `to_array()`.

No Ruby compatibility alias is introduced.

Invalid:

```amber
(1..5).to_a()
```

Required diagnostic:

```text
E_METHOD_NOT_FOUND
method `to_a` is not defined for Range; use `to_array()` or `.array`
```

If the implementation does not provide suggestion diagnostics at method lookup time, ordinary method-not-found behavior is acceptable.

---

## 1.2. Infinite collections must fail explicitly

Open-ended ranges and infinite lazy collections must not be silently exhausted.

Invalid:

```amber
(1..).to_array()
(1..).array
```

Both raise:

```text
InfiniteCollectionError
```

Rationale:

* materialization is eager;
* an open-ended range has no finite cardinality;
* silent exhaustion is impossible;
* silent truncation would be data loss;
* implicit bounds would be surprising.

---

## 1.3. Range step is explicit direction, not guessed direction

For integer ranges, omitted step defaults to `1`.

```amber
1..5
```

is equivalent to:

```amber
1..5:1
```

Descending ranges require an explicit negative step.

```amber
5..1       # empty
5..1:-1    # 5, 4, 3, 2, 1
```

The implementation must not infer `-1` from `start > end`.

Rationale:

* runtime endpoints should not make step direction implicit;
* `a..b` should have stable meaning regardless of values;
* explicit descending ranges are clearer;
* empty range is preferable to hidden direction switching.

---

## 1.4. Float ranges require explicit step

Integer ranges may omit step.

Float ranges must declare step.

Valid:

```amber
1..5
1..5:2
1.0..5.0:0.5
1..5.0:0.5
```

Invalid:

```amber
1.0..5.0
1..5.0
```

Required diagnostic:

```text
E_RANGE_FLOAT_STEP_REQUIRED
float ranges require an explicit step
```

Rationale:

* there is no universally obvious default float step;
* `1.0` is too arbitrary;
* explicit step improves numerical readability;
* conformance tests become deterministic.

---

# 2. Range syntax

## 2.1. Inclusive range

Existing inclusive range syntax remains:

```amber
start..end
```

With step:

```amber
start..end:step
```

Examples:

```amber
1..5        # 1, 2, 3, 4, 5
1..5:2      # 1, 3, 5
5..1:-1     # 5, 4, 3, 2, 1
```

---

## 2.2. Exclusive range

Existing exclusive range syntax remains:

```amber
start...end
```

With step:

```amber
start...end:step
```

Examples:

```amber
1...5       # 1, 2, 3, 4
1...5:2     # 1, 3
5...1:-1    # 5, 4, 3, 2
```

---

## 2.3. Open-ended range

Open-ended ranges are supported:

```amber
start..
start..:step
```

Examples:

```amber
1..         # 1, 2, 3, 4, ...
1..:2       # 1, 3, 5, 7, ...
10..:-1     # 10, 9, 8, 7, ...
0.0..:0.5   # 0.0, 0.5, 1.0, 1.5, ...
```

For an open-ended integer range, omitted step defaults to `1`.

For an open-ended float range, step is required.

Invalid:

```amber
0.0..
```

Required diagnostic:

```text
E_RANGE_FLOAT_STEP_REQUIRED
float ranges require an explicit step
```

---

## 2.4. Beginless ranges are not introduced

This patch does not introduce beginless ranges.

Invalid:

```amber
..5
...5
..5:2
```

Rationale:

* beginless ranges complicate array slicing semantics;
* negative indexing already covers common tail-relative indexing cases;
* beginless syntax can be added later with a separate lowering model.

---

# 3. Range step semantics

## 3.1. Step validity

`step` must be non-zero.

Invalid:

```amber
1..10:0
1.0..10.0:0.0
```

Required error:

```text
ArgumentError
range step must not be zero
```

A statically literal zero step should be diagnosed at compile time when possible.

A runtime zero step raises `ArgumentError`.

---

## 3.2. Integer range step

If both endpoints are `Int`, the range is an `IntRange`.

If no step is provided, step defaults to `1`.

If a step is provided, it must be an `Int`.

Valid:

```amber
1..10:2
10..1:-1
```

Invalid:

```amber
1..10:0.5
```

Required error:

```text
TypeError
integer range step must be Int
```

---

## 3.3. Float range step

If either endpoint is `Float`, the range is a `FloatRange`.

A `FloatRange` requires explicit step.

The step must be numeric and is interpreted as a `Float` step.

Valid:

```amber
1.0..3.0:0.5
1..3.0:0.5
1.0..3:0.5
```

Invalid:

```amber
1.0..3.0
1..3.0
1.0..3
```

Required diagnostic:

```text
E_RANGE_FLOAT_STEP_REQUIRED
float ranges require an explicit step
```

---

## 3.4. Direction and emptiness

The sign of `step` determines range direction.

For ascending steps:

```amber
1..5:1       # non-empty
5..1:1       # empty
```

For descending steps:

```amber
5..1:-1      # non-empty
1..5:-1      # empty
```

If the step direction cannot reach the endpoint, the range is finite and empty. This is not an error.

Examples:

```amber
(5..1).array       # []
(5..1:1).array     # []
(1..5:-1).array    # []
```

---

## 3.5. Inclusive boundary

For inclusive ranges:

```amber
start..end:step
```

The range contains each value in the step sequence that does not pass `end`.

Examples:

```amber
(1..5:2).array      # [1, 3, 5]
(1..6:2).array      # [1, 3, 5]
(5..1:-2).array     # [5, 3, 1]
(6..1:-2).array     # [6, 4, 2]
```

---

## 3.6. Exclusive boundary

For exclusive ranges:

```amber
start...end:step
```

The range contains each value in the step sequence that does not reach or pass the exclusive `end` boundary.

Examples:

```amber
(1...5:2).array     # [1, 3]
(1...6:2).array     # [1, 3, 5]
(5...1:-2).array    # [5, 3]
(6...1:-2).array    # [6, 4, 2]
```

---

## 3.7. Float sequence definition

Float range values are defined by ordinal index, not by repeated mutation of an accumulator.

For each integer `n >= 0`:

```text
value_n = start + n * step
```

The range contains all `value_n` values that satisfy the range boundary rule.

This definition reduces implementation-dependent drift from repeated floating-point addition.

Implementations may still use optimized iteration internally, but observable behavior in conformance tests must match the ordinal-index definition within the standard Float comparison model.

---

# 4. Grammar

## 4.1. Reference grammar

```ebnf
RangeExpr ::= Expr RangeOperator Expr? RangeStep?

RangeOperator ::= ".."
                | "..."

RangeStep ::= ":" Expr
```

Interpretation constraints:

1. `RangeStep` is recognized only immediately after a range expression.
2. `RangeStep` is not a general postfix operator.
3. `:` does not gain new meaning outside range step position.
4. Open-ended range is allowed only for `..`, not for `...`, in this patch.

Valid:

```amber
1..10:2
1...10:2
1..:2
```

Invalid:

```amber
1...:2
```

Required diagnostic:

```text
E_RANGE_EXCLUSIVE_OPEN_ENDED
exclusive open-ended ranges are not supported
```

Rationale:

An exclusive range without an end boundary has no meaningful exclusive boundary.

---

## 4.2. Formatter rules

Formatter should emit no whitespace before the step colon:

```amber
1..10:2
```

not:

```amber
1..10 : 2
```

For complex step expressions, formatter should parenthesize the step expression:

```amber
1..10:(step + 1)
```

Formatter may preserve simple identifiers and literals without parentheses:

```amber
1..10:step
1..10:2
```

---

# 5. Range materialization

## 5.1. `Range#to_array()`

Finite ranges implement:

```amber
range.to_array()
```

The method returns a new `Array` containing the range values in iteration order.

Examples:

```amber
(1..5).to_array()       # [1, 2, 3, 4, 5]
(1...5).to_array()      # [1, 2, 3, 4]
(1..5:2).to_array()     # [1, 3, 5]
(5..1:-1).to_array()    # [5, 4, 3, 2, 1]
```

Each call returns a fresh array.

```amber
a = (1..3).to_array()
b = (1..3).to_array()
a.object_id == b.object_id    # false, subject to object_id profile availability
```

---

## 5.2. `Range#array`

Finite ranges expose:

```amber
range.array
```

This property is equivalent to:

```amber
range.to_array()
```

Examples:

```amber
(1..5).array       # [1, 2, 3, 4, 5]
(1...5).array      # [1, 2, 3, 4]
(1..5:2).array     # [1, 3, 5]
```

`array` is a property alias, not a method.

Invalid:

```amber
(1..5).array()
```

Unless the returned array is itself callable, this is an ordinary attempted call of the property result and must not be interpreted as a property getter call.

---

## 5.3. No `to_a()`

Amber v20.4 does not define:

```amber
to_a()
```

Invalid:

```amber
(1..5).to_a()
```

Recommended diagnostic:

```text
E_METHOD_NOT_FOUND
method `to_a` is not defined for Range; use `to_array()` or `.array`
```

---

## 5.4. Open-ended materialization

Open-ended ranges cannot be materialized without an explicit bound.

Invalid:

```amber
(1..).to_array()
(1..).array
(1..:2).to_array()
(0.0..:0.5).array
```

Required runtime error:

```text
InfiniteCollectionError
cannot materialize an open-ended range
```

---

# 6. `InfiniteCollectionError`

## 6.1. Error class

This patch introduces:

```text
InfiniteCollectionError
```

Recommended hierarchy:

```text
RuntimeError
  InfiniteCollectionError
```

Alternative acceptable hierarchy if the implementation already places collection errors under a common superclass:

```text
CollectionError
  InfiniteCollectionError
```

## 6.2. Required uses

`InfiniteCollectionError` is raised when an eager operation attempts to fully materialize an infinite or open-ended collection.

Required cases:

```amber
(1..).to_array()
(1..).array
(1..).count()
```

unless the operation receives an explicit finite bound.

Valid bounded operations:

```amber
(1..).first(5)      # [1, 2, 3, 4, 5]
(1..:2).first(3)    # [1, 3, 5]
```

## 6.3. Message guidance

Recommended messages:

```text
cannot materialize an open-ended range
cannot count an infinite collection without a bound
cannot convert an infinite collection to Array
```

Messages must be deterministic and must not include raw memory addresses.

---

# 7. Negative array indexing

## 7.1. Single index normalization

Arrays support negative integer indices.

For an array of length `n`, index `i` is normalized as:

```text
normalized = if i < 0 then n + i else i
```

The normalized index must satisfy:

```text
0 <= normalized < n
```

Otherwise `IndexError` is raised.

Examples:

```amber
xs = ["a", "b", "c", "d"]

xs[0]      # "a"
xs[1]      # "b"
xs[-1]     # "d"
xs[-2]     # "c"
```

Invalid:

```amber
xs[4]
xs[-5]
```

Both raise:

```text
IndexError
```

---

## 7.2. Negative index assignment

Negative indexing also applies to indexed assignment.

```amber
xs = ["a", "b", "c"]
xs[-1] = "z"
xs              # ["a", "b", "z"]
```

The right-hand side is evaluated exactly once and assignment returns the assigned value according to ordinary assignment semantics.

Invalid:

```amber
xs[-4] = "x"
```

raises:

```text
IndexError
```

---

# 8. Array slicing by `IntRange`

## 8.1. Basic rule

Arrays accept finite or open-ended `IntRange` values as index operands.

```amber
xs[range]
```

returns a new `Array` containing elements selected by the range's integer iteration order.

Examples:

```amber
xs = ["a", "b", "c", "d", "e"]

xs[1..3]       # ["b", "c", "d"]
xs[1...3]      # ["b", "c"]
xs[0..:2]      # ["a", "c", "e"]
xs[4..0:-2]    # ["e", "c", "a"]
```

The result is always a new `Array`.

---

## 8.2. Negative range endpoints

Range endpoints used for array slicing are normalized as array indices.

For an array of length `n`, each endpoint `i` is normalized as:

```text
normalized = if i < 0 then n + i else i
```

Examples:

```amber
xs = ["a", "b", "c", "d", "e"]

xs[-1]         # "e"
xs[-2]         # "d"
xs[-3..-1]     # ["c", "d", "e"]
xs[-1..0:-1]   # ["e", "d", "c", "b", "a"]
xs[-1...0:-1]  # ["e", "d", "c", "b"]
```

---

## 8.3. Open-ended range slices

Open-ended `IntRange` slices are bounded by the receiver array length.

Examples:

```amber
xs = ["a", "b", "c", "d", "e"]

xs[2..]        # ["c", "d", "e"]
xs[2..:2]      # ["c", "e"]
xs[-3..]       # ["c", "d", "e"]
xs[-1..:-1]    # ["e", "d", "c", "b", "a"]
```

Open-ended slice materialization is allowed because the receiver array supplies the finite bound.

This does not contradict `InfiniteCollectionError` for standalone range materialization.

```amber
(1..).array    # InfiniteCollectionError
xs[1..]        # valid, bounded by xs.length
```

---

## 8.4. Out-of-bounds slice endpoints

Single-element indexing remains strict and raises `IndexError` for out-of-bounds normalized index.

Range slicing is also strict for explicit endpoints.

Invalid:

```amber
xs[10..12]
xs[-10..-1]
```

Both raise:

```text
IndexError
```

Open-ended slices validate their explicit start endpoint.

Invalid:

```amber
xs[10..]
xs[-10..]
```

Both raise `IndexError`.

Rationale:

* Amber indexing should be explicit and diagnostic;
* silent clamping hides off-by-one errors;
* open-ended slicing is bounded by array length, but not forgiving of invalid starts.

---

## 8.5. Empty slices

A slice may be empty when the normalized range direction cannot reach the boundary.

Examples:

```amber
xs = ["a", "b", "c"]

xs[2..0]       # [] because default step is +1
xs[0..2:-1]    # [] because step is -1 and end is ahead
```

This is not an error because the endpoints themselves are valid.

---

## 8.6. Float ranges are invalid as array indices

Only `IntRange` may be used as an array index operand.

Invalid:

```amber
xs[1.0..3.0:1.0]
```

Required error:

```text
TypeError
array slice index must be IntRange
```

---

# 9. `Int#times`

## 9.1. Purpose

`Int#times` provides compact finite repetition over integer indices.

It is Ruby-inspired but uses Amber return conventions for effect-oriented iteration.

Supported forms:

```amber
5.times |i|:
  body

5.times.each |i|:
  body

5.times.map |i|:
  expr
```

---

## 9.2. `Int#times` without block

Without a block, `Int#times` returns a finite `Times` enumerable equivalent to the integer range:

```amber
0...n
```

Examples:

```amber
5.times.array       # [0, 1, 2, 3, 4]
5.times.to_array()  # [0, 1, 2, 3, 4]
0.times.array       # []
(-3).times.array    # []
```

For `n <= 0`, `n.times` is empty.

`Times` may be implemented as a specialized lightweight enumerable or as a lowered `IntRange`.

Observable behavior must match:

```amber
if n <= 0 then 0...0 else 0...n
```

---

## 9.3. `Int#times` block form

With a block:

```amber
n.times |i|:
  body
```

lowers to effect iteration over `n.times.each`.

The block receives indices from `0` through `n - 1`.

Examples:

```amber
5.times |i|:
  print i
```

prints:

```text
0
1
2
3
4
```

The block form returns `null`.

```amber
result = 5.times |i|:
  i * 2

result == null    # true
```

Rationale:

* direct block `times` is effect-oriented;
* returning receiver is Ruby-specific and not Amber-style;
* result collection belongs to `.map`, not `.times`.

---

## 9.4. `times.each`

`times.each` iterates for effects and returns the materialized index array.

```amber
result = 5.times.each |i|:
  print i

result == [0, 1, 2, 3, 4]
```

Normative equivalence:

```amber
n.times.each |i|:
  body
```

is observationally equivalent to:

```amber
indices = n.times.array
indices.each |i|:
  body
indices
```

Therefore:

```amber
5.times.each |i|:
  i * 10
```

returns:

```amber
[0, 1, 2, 3, 4]
```

not:

```amber
[0, 10, 20, 30, 40]
```

Use `map` to collect block results.

---

## 9.5. `times.map`

`times.map` returns a new array containing the last expression result of each block execution.

```amber
5.times.map |i|:
  i * 2
```

returns:

```amber
[0, 2, 4, 6, 8]
```

Multi-statement block example:

```amber
5.times.map |i|:
  x = i * 2
  x + 1
```

returns:

```amber
[1, 3, 5, 7, 9]
```

For `n <= 0`, result is an empty array:

```amber
0.times.map |i|:
  i
# []

(-3).times.map |i|:
  i
# []
```

`times.map` propagates exceptions raised by the block.

---

## 9.6. Block arity

`times`, `times.each` and `times.map` pass exactly one positional argument to the block: the current index.

Valid:

```amber
5.times |i|:
  print i
```

Invalid according to ordinary block arity rules:

```amber
5.times |a, b|:
  noop
```

Required error:

```text
ArgumentError
block for Int#times expects 1 parameter
```

If Amber's block arity model permits ignored parameters or splats elsewhere, this rule should be aligned with the general callable/block arity rules.

---

# 10. Enumerable behavior

## 10.1. Range enumerable methods

Finite ranges participate in the standard Enumerable-like contract:

```amber
range.each |x|:
  ...

range.map |x|:
  ...

range.select |x|:
  ...

range.to_array()
range.array
range.lazy()
```

`to_a()` is not part of the Amber contract.

---

## 10.2. Open-ended range enumerable methods

Open-ended ranges support lazy and bounded operations.

Valid:

```amber
(1..).lazy()
(1..).first(5)
(1..:2).first(3)
```

Invalid eager unbounded operations:

```amber
(1..).to_array()
(1..).array
(1..).count()
```

raise:

```text
InfiniteCollectionError
```

Eager block iteration over an open-ended range is permitted only if user control flow terminates it through ordinary language constructs such as `break`, if such constructs are supported.

Implementations must not attempt to pre-materialize an open-ended range for `each`.

---

# 11. AST and HIR notes

## 11.1. AST

Recommended AST nodes:

```text
AstRangeExpr(
  start_expr,
  end_expr?,
  boundary_kind,     # inclusive | exclusive
  step_expr?,
  span
)
```

where:

```text
boundary_kind = INCLUSIVE
              | EXCLUSIVE
```

For open-ended ranges:

```text
end_expr = null
boundary_kind = INCLUSIVE
```

Exclusive open-ended ranges are invalid in this patch.

---

## 11.2. HIR

Recommended HIR operation:

```text
HRange(start, end?, boundary_kind, step?, range_kind?)
```

Semantic analysis resolves:

```text
range_kind = IntRange | FloatRange
```

when statically possible.

If endpoint types are not statically known, runtime construction must enforce:

1. float ranges require explicit step;
2. integer ranges default omitted step to `1`;
3. step is non-zero;
4. integer range step is `Int`;
5. float range step is numeric and interpreted as `Float`.

---

## 11.3. `times` lowering

Direct block form:

```amber
n.times |i|:
  body
```

lowers to a specialized effect iteration equivalent to:

```amber
n.times.each |i|:
  body
null
```

Important: direct block `times` returns `null` even though `times.each` returns the index array.

---

# 12. Diagnostics

## 12.1. New diagnostics

```text
E_RANGE_FLOAT_STEP_REQUIRED
float ranges require an explicit step
```

```text
E_RANGE_ZERO_STEP
range step must not be zero
```

```text
E_RANGE_INT_STEP_TYPE
integer range step must be Int
```

```text
E_RANGE_EXCLUSIVE_OPEN_ENDED
exclusive open-ended ranges are not supported
```

```text
E_ARRAY_SLICE_RANGE_TYPE
array slice index must be IntRange
```

```text
E_INFINITE_COLLECTION_MATERIALIZATION
cannot materialize an infinite collection
```

---

## 12.2. Runtime errors

```text
InfiniteCollectionError
```

Raised by eager materialization of open-ended/infinite collections.

```text
IndexError
```

Raised by invalid single array index or explicit invalid slice endpoint.

```text
TypeError
```

Raised when a non-`IntRange` range is used as an array slice operand.

```text
ArgumentError
```

Raised by runtime zero step or invalid block arity, unless more specific diagnostics apply statically.

---

# 13. Conformance tests

## 13.1. Integer ranges

```amber
assert((1..5).array == [1, 2, 3, 4, 5])
assert((1...5).array == [1, 2, 3, 4])
assert((1..5:2).array == [1, 3, 5])
assert((1...5:2).array == [1, 3])
assert((5..1).array == [])
assert((5..1:-1).array == [5, 4, 3, 2, 1])
assert((1..5:-1).array == [])
```

---

## 13.2. Float ranges

```amber
assert((1.0..2.0:0.5).array == [1.0, 1.5, 2.0])
assert((1.0...2.0:0.5).array == [1.0, 1.5])
assert((2.0..1.0:-0.5).array == [2.0, 1.5, 1.0])
```

Invalid:

```amber
1.0..2.0
1..2.0
```

must diagnose:

```text
E_RANGE_FLOAT_STEP_REQUIRED
```

---

## 13.3. Infinite collection materialization

```amber
assert_raises(InfiniteCollectionError):
  (1..).to_array()

assert_raises(InfiniteCollectionError):
  (1..).array

assert((1..).first(3) == [1, 2, 3])
assert((1..:2).first(3) == [1, 3, 5])
```

---

## 13.4. No `to_a`

```amber
assert_raises(NoMethodError):
  (1..3).to_a()
```

If the implementation uses `MethodMissingError` or `E_METHOD_NOT_FOUND`, the test should match the implementation's standard missing-method class.

---

## 13.5. Negative indexing

```amber
xs = ["a", "b", "c", "d"]

assert(xs[0] == "a")
assert(xs[-1] == "d")
assert(xs[-2] == "c")

assert_raises(IndexError):
  xs[4]

assert_raises(IndexError):
  xs[-5]
```

Assignment:

```amber
xs = ["a", "b", "c"]
xs[-1] = "z"
assert(xs == ["a", "b", "z"])
```

---

## 13.6. Array slicing

```amber
xs = ["a", "b", "c", "d", "e"]

assert(xs[1..3] == ["b", "c", "d"])
assert(xs[1...3] == ["b", "c"])
assert(xs[0..:2] == ["a", "c", "e"])
assert(xs[4..0:-2] == ["e", "c", "a"])
assert(xs[-3..-1] == ["c", "d", "e"])
assert(xs[-1..0:-1] == ["e", "d", "c", "b", "a"])
assert(xs[2..] == ["c", "d", "e"])
assert(xs[-3..] == ["c", "d", "e"])
assert(xs[-1..:-1] == ["e", "d", "c", "b", "a"])
```

Invalid:

```amber
assert_raises(IndexError):
  xs[10..12]

assert_raises(IndexError):
  xs[-10..-1]

assert_raises(TypeError):
  xs[1.0..3.0:1.0]
```

---

## 13.7. `Int#times`

```amber
assert(5.times.array == [0, 1, 2, 3, 4])
assert(0.times.array == [])
assert((-3).times.array == [])
```

Direct block form returns `null`:

```amber
result = 5.times |i|:
  i * 2

assert(result == null)
```

`times.each` returns the index array:

```amber
result = 5.times.each |i|:
  i * 2

assert(result == [0, 1, 2, 3, 4])
```

`times.map` returns block results:

```amber
result = 5.times.map |i|:
  i * 2

assert(result == [0, 2, 4, 6, 8])
```

Multi-statement block:

```amber
result = 5.times.map |i|:
  x = i * 2
  x + 1

assert(result == [1, 3, 5, 7, 9])
```

---

# 14. Compatibility and migration notes

## 14.1. No Ruby `to_a`

Code written with Ruby expectations:

```amber
range.to_a()
```

must migrate to:

```amber
range.to_array()
```

or:

```amber
range.array
```

This is intentional.

Amber chooses explicit, readable conversion names over abbreviated compatibility aliases.

---

## 14.2. Descending ranges

Code expecting automatic descending behavior must specify negative step.

```amber
5..1       # []
5..1:-1    # [5, 4, 3, 2, 1]
```

---

## 14.3. Float ranges

Code using float endpoints must specify step.

```amber
0.0..1.0       # invalid
0.0..1.0:0.1   # valid
```

---

# 15. Summary

Amber v20.4 adds range and repetition ergonomics while preserving Amber-style explicitness:

```amber
(1..5).array          # [1, 2, 3, 4, 5]
(1..5:2).array        # [1, 3, 5]
(5..1:-1).array       # [5, 4, 3, 2, 1]
(1..).first(3)        # [1, 2, 3]

xs[-1]                # last element
xs[-3..-1]            # tail slice
xs[2..]               # from index 2 to end

5.times |i|:
  print i             # returns null

5.times.each |i|:
  print i             # returns [0, 1, 2, 3, 4]

5.times.map |i|:
  i * 2               # returns [0, 2, 4, 6, 8]
```

The patch deliberately avoids compatibility-only aliases and introduces `InfiniteCollectionError` as the required explicit failure mode for eager materialization of infinite/open-ended collections.
