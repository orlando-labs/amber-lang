# Amber v20.5 Draft Patch: `Array.of`, `Array.build`, `Array.filled` and Optional Bracket Access

**Status:** proposed normative extension for the main Amber language specification and core collections stdlib  
**Target base:** Amber v20.4 range / indexing layer + Amber v20.3.1 property and conversion layers  
**Patch scope:** array construction APIs, bracket access syntax, nullable indexed/keyed access, parser grammar, HIR lowering, diagnostics and conformance corpus  
**Non-goals:** safe navigation redesign, optional assignment, implicit collection resizing, deep-copy array initialization, typed arrays, lazy array builders, Ruby-compatible aliases

---

# 0. Integration note

This patch introduces two related improvements to Amber's collection ergonomics:

1. explicit array generation APIs:

```amber
Array.of(length) |index|:
  expr

Array.build(length) |index|:
  expr

Array.filled(length, value)
```

2. optional bracket access:

```amber
array[?index]
map[?key]
```

The intent is to replace Python-like array repetition as the canonical spelling:

```amber
arr = [initial_value] * N
```

with Amber-native, explicit APIs:

```amber
arr = Array.filled(N, initial_value)
arr = Array.of(N) |i|:
  expr_with(i)
```

The patch also keeps ordinary bracket access strict:

```amber
array[index]   # may raise IndexError
map[key]       # may raise KeyError
```

and adds an explicit nullable addressing mode for flow-dependent absence:

```amber
array[?index]  # value or null
map[?key]      # value or null
```

---

# 1. Design principles

## 1.1. Array initialization must say whether values are repeated or generated

Amber distinguishes two common cases:

```amber
Array.filled(N, value)
```

means every slot contains the same value reference.

```amber
Array.of(N) |i|:
  expr
```

means the expression is evaluated once per index and may create a fresh value per slot.

This distinction is especially important for mutable values:

```amber
Array.filled(3, [])
# [same_array, same_array, same_array]

Array.of(3):
  []
# [fresh_array, fresh_array, fresh_array]
```

The language must not hide cloning, copying or per-slot allocation behind a repeated-value API.

---

## 1.2. Bracket access remains the canonical collection addressing form

Amber code commonly uses bracket access for arrays and maps:

```amber
xs[i]
map[key]
```

A nullable/permissive lookup should therefore be expressed as a modification of bracket addressing, not as a separate method family such as:

```amber
xs.get!(i)
map.get!(key)
```

The canonical optional form is:

```amber
xs[?i]
map[?key]
```

This makes the addressing mode explicit at the lookup site while preserving the visual model of collection indexing.

---

## 1.3. `?` means optional lookup, not C-style ternary

Amber does not introduce C-style ternary syntax:

```amber
cond ? a : b
```

The `?` marker in optional bracket access is not an infix operator. It is a contextual marker inside bracket addressing.

```amber
xs[?i]
```

means:

```text
perform lookup at address `i`; if the address is absent, return null instead of raising the ordinary absence error
```

---

## 1.4. Optional bracket access is read-only

The patch does not introduce optional assignment:

```amber
xs[?i] = value   # invalid
map[?key] = value # invalid
```

Reasoning:

* optional read has clear nullable semantics;
* optional array assignment is ambiguous: no-op, append, gap-fill or error;
* map insertion already has ordinary strict assignment syntax;
* suppressing assignment errors would hide bugs.

---

# 2. Array construction APIs

## 2.1. `Array.of(length) |index|:`

Canonical generated-array constructor:

```amber
Array.of(length) |index|:
  expr
```

The block is evaluated exactly once for each integer index from `0` to `length - 1`.

Example:

```amber
Array.of(5) |i|:
  i * i
# [0, 1, 4, 9, 16]
```

`index` is an `Int`.

The result is a new `Array` of exactly `length` elements.

---

## 2.2. Shorthand block forms

Amber permits ordinary block shorthand according to the existing block/lambda profile.

Valid:

```amber
Array.of(N) |idx|:
  expr_with(idx)
```

Valid shorthand using implicit first block parameter:

```amber
Array.of(N):
  expr_with(_1)
```

Valid constant-expression generation:

```amber
Array.of(N):
  null
```

Important: the body is still evaluated once per index.

---

## 2.3. Fresh mutable values

A generated array evaluates its block independently for each slot.

```amber
Array.of(3):
  []
```

returns an array containing three distinct arrays:

```amber
[[], [], []]
```

Mutating one nested array must not mutate the others unless the block itself deliberately returns the same shared object.

---

## 2.4. `Array.build` alias

`Array.build` is an alias for `Array.of`.

```amber
Array.build(length) |index|:
  expr
```

is equivalent to:

```amber
Array.of(length) |index|:
  expr
```

`Array.of` is the preferred compact spelling.

`Array.build` is accepted for users who prefer a more explicit builder-like name.

---

## 2.5. `Array.filled(length, value)`

`Array.filled` constructs a new array of `length` elements where every slot contains `value`.

```amber
Array.filled(5, 0)
# [0, 0, 0, 0, 0]

Array.filled(3, null)
# [null, null, null]
```

For object values, the same object reference is stored in every slot.

```amber
inner = []
xs = Array.filled(3, inner)

xs[0].object_id == xs[1].object_id
# true, subject to object_id profile availability
```

No implicit clone, copy, deep copy or per-slot allocation is performed.

---

## 2.6. Relationship to `[value] * N`

The form:

```amber
[value] * N
```

may remain as ordinary operator behavior if already supported by the implementation, but it is not the canonical array initialization spelling.

Recommended canonical forms:

```amber
Array.filled(N, value)
Array.of(N):
  expr
```

Recommended diagnostic for style-oriented linters:

```text
AMB-LINT-ARRAY-REPEAT-INIT
prefer `Array.filled(length, value)` or `Array.of(length): expr` over `[value] * length`
```

This is a linter-level recommendation, not a parser error.

---

# 3. Array construction errors

## 3.1. Negative length

Invalid:

```amber
Array.of(-1):
  0

Array.build(-1):
  0

Array.filled(-1, 0)
```

Required runtime error:

```text
ArgumentError
array length must be non-negative
```

If the length is a statically known negative integer literal, an implementation may diagnose at compile time.

---

## 3.2. Non-integer length

Invalid:

```amber
Array.of(3.5):
  0

Array.filled("3", 0)
```

Required error:

```text
TypeError
array length must be Int
```

Amber does not implicitly convert array lengths through `to_int()`.

---

## 3.3. Missing block for `Array.of` / `Array.build`

Invalid:

```amber
Array.of(10)
Array.build(10)
```

Required error:

```text
ArgumentError
Array.of requires a block
```

For `Array.build`, the diagnostic may say:

```text
ArgumentError
Array.build requires a block
```

Rationale: repeated-value initialization belongs to `Array.filled`, not blockless `Array.of`.

---

# 4. Optional bracket access

## 4.1. Canonical form

Strict bracket access:

```amber
receiver[address]
```

Optional bracket access:

```amber
receiver[?address]
```

The `?` marker belongs to the bracket access syntax. It is not part of the address expression.

---

## 4.2. Array optional access

For arrays:

```amber
xs[index]
```

uses strict indexed access.

If `index` is out of bounds after negative-index normalization, strict access raises:

```text
IndexError
```

Optional access:

```amber
xs[?index]
```

returns:

* the element at `index`, if the normalized index is valid;
* `null`, if the normalized index is invalid.

Examples:

```amber
xs = ["a", "b", "c"]

xs[0]      # "a"
xs[-1]     # "c"
xs[3]      # IndexError
xs[-4]     # IndexError

xs[?0]     # "a"
xs[?-1]    # "c"
xs[?3]     # null
xs[?-4]    # null
```

---

## 4.3. Tuple optional access

If tuples support bracket indexing, optional bracket access applies analogously:

```amber
tuple[?index]
```

returns the tuple element or `null` when the index is invalid.

---

## 4.4. Map optional access

For maps:

```amber
map[key]
```

uses strict keyed access.

If the key is absent, strict access raises:

```text
KeyError
```

Optional access:

```amber
map[?key]
```

returns:

* the value for `key`, if the key is present;
* `null`, if the key is absent.

Examples:

```amber
user = {
  name: "Ada",
  email: null,
}

user[:name]      # "Ada"
user[:age]       # KeyError

user[?:name]     # "Ada"
user[?:age]      # null
user[?:email]    # null
```

---

## 4.5. Stored `null` and absent value

Optional bracket access intentionally does not distinguish between:

1. present key with `null` value;
2. absent key;
3. invalid index.

Example:

```amber
m = {
  email: null,
}

m[?:email]   # null
m[?:age]     # null
```

Use explicit presence predicates when the distinction matters:

```amber
m.contains?(:email)  # true
m.contains?(:age)    # false
```

For arrays:

```amber
xs.has_index?(i)
```

returns whether `i` is a valid index after negative-index normalization.

---

## 4.6. Evaluation order

Optional bracket access evaluates left-to-right:

```amber
receiver[?address]
```

Evaluation order:

1. evaluate `receiver`;
2. evaluate `address` exactly once;
3. perform optional lookup;
4. return value or `null`.

The receiver expression is not suppressed. Errors while evaluating the receiver or address expression propagate normally.

Example:

```amber
get_users()[?compute_id()]
```

Evaluation order:

1. `get_users()`;
2. `compute_id()`;
3. optional lookup.

Only the ordinary absence error of the lookup operation is converted to `null`.

---

## 4.7. What optional access catches

For `Array` / `Tuple`, optional access catches only invalid-index absence:

```text
IndexError caused by out-of-bounds indexing
```

For `Map`, optional access catches only missing-key absence:

```text
KeyError caused by absent key lookup
```

It must not swallow:

* errors while evaluating receiver;
* errors while evaluating address;
* type errors for invalid address category, unless the collection's own strict access defines those as ordinary absence;
* cancellation;
* isolation errors;
* programmer errors inside user-defined lookup hooks;
* fatal VM errors.

---

# 5. Optional bracket assignment

## 5.1. Assignment is invalid

Invalid:

```amber
xs[?i] = value
map[?key] = value
```

Required diagnostic:

```text
E_OPTIONAL_BRACKET_ASSIGNMENT
optional bracket access is read-only; use strict `receiver[key] = value`
```

Rationale:

* array optional assignment has no obvious meaning;
* map insertion already uses `map[key] = value`;
* nullable read flow must not imply nullable write flow;
* hidden no-op assignment would be bug-prone.

---

# 6. Grammar

## 6.1. Reference grammar

```ebnf
PostfixExpr ::= PrimaryExpr PostfixPart*

PostfixPart ::= CallSuffix
              | MemberSuffix
              | BracketSuffix
              | SafeNavigationSuffix
              | ...

BracketSuffix ::= "[" BracketMode? Expr "]"

BracketMode ::= "?"
```

Interpretation constraints:

1. `?` is recognized as `BracketMode` only immediately after `[` in bracket access.
2. `?` is not a general prefix operator.
3. `?` does not become part of the address expression.
4. Existing method names ending in `?` remain unaffected.
5. C-style ternary remains invalid.

---

## 6.2. Parsing examples

```amber
xs[?i]
```

parses as optional access with address expression:

```amber
i
```

```amber
xs[?i + 1]
```

parses as optional access with address expression:

```amber
i + 1
```

Formatter may render this as:

```amber
xs[?(i + 1)]
```

```amber
map[?:name]
```

parses as optional access with address expression:

```amber
:name
```

```amber
xs[?-1]
```

parses as optional access with address expression:

```amber
-1
```

---

# 7. Formatter rules

## 7.1. Simple addresses

Formatter should preserve compact optional access for simple address expressions:

```amber
xs[?i]
xs[?-1]
map[?key]
map[?:name]
```

## 7.2. Complex addresses

Formatter should parenthesize complex address expressions after `?`:

```amber
xs[?(i + 1)]
map[?(prefix + key)]
matrix[?(row * width + col)]
```

This avoids visual ambiguity around whether `?` applies to the whole address expression.

---

# 8. AST

Recommended AST node extension:

```text
AstIndexExpr(
  receiver,
  address,
  mode,
  span
)
```

where:

```text
mode = STRICT
     | OPTIONAL
```

Parser output must preserve whether the source used strict or optional access.

The parser must not lower optional access into method calls immediately.

---

# 9. HIR lowering

## 9.1. Strict access

```amber
receiver[address]
```

lowers to ordinary strict index/key lookup:

```text
INDEX_GET(receiver, address, mode: STRICT)
```

## 9.2. Optional access

```amber
receiver[?address]
```

lowers to optional lookup:

```text
INDEX_GET(receiver, address, mode: OPTIONAL)
```

or an equivalent explicit HIR operation:

```text
OPTIONAL_INDEX_GET(receiver, address)
```

The HIR operation is responsible for converting only ordinary absence from the receiver's lookup semantics into `null`.

---

# 10. Presence predicates

Optional access should be paired with explicit presence APIs.

## 10.1. Array

```amber
xs.has_index?(index)
```

Returns `true` if `index` is valid after negative-index normalization, otherwise `false`.

Examples:

```amber
xs = ["a", "b", "c"]

xs.has_index?(0)    # true
xs.has_index?(-1)   # true
xs.has_index?(3)    # false
xs.has_index?(-4)   # false
```

## 10.2. Map

```amber
map.contains?(key)
```

Returns `true` if `key` is present, even if the associated value is `null`.

Examples:

```amber
m = {
  email: null,
}

m.contains?(:email)  # true
m.contains?(:age)    # false
```

---

# 11. Diagnostics

## 11.1. Optional assignment

```amber
xs[?i] = value
```

Required diagnostic:

```text
E_OPTIONAL_BRACKET_ASSIGNMENT
optional bracket access is read-only; use strict `receiver[key] = value`
```

---

## 11.2. Missing block for `Array.of`

```amber
Array.of(10)
```

Required diagnostic:

```text
ArgumentError
Array.of requires a block
```

---

## 11.3. Negative array length

```amber
Array.of(-1):
  null
```

Required diagnostic:

```text
ArgumentError
array length must be non-negative
```

---

## 11.4. Non-Int array length

```amber
Array.filled("10", null)
```

Required diagnostic:

```text
TypeError
array length must be Int
```

---

# 12. Conformance examples

## 12.1. `Array.of`

```amber
Array.of(4) |i|:
  i + 1
# [1, 2, 3, 4]
```

```amber
Array.of(3):
  _1 * 2
# [0, 2, 4]
```

```amber
Array.of(3):
  null
# [null, null, null]
```

```amber
xs = Array.of(3):
  []

xs[0].push(1)
xs
# [[1], [], []]
```

---

## 12.2. `Array.build`

```amber
Array.build(3) |i|:
  "item-#{i}"
# ["item-0", "item-1", "item-2"]
```

`Array.build` must produce the same result as `Array.of` for the same block.

---

## 12.3. `Array.filled`

```amber
Array.filled(4, false)
# [false, false, false, false]
```

```amber
inner = []
xs = Array.filled(2, inner)
xs[0].push(:x)
xs
# [[:x], [:x]]
```

---

## 12.4. Optional array access

```amber
xs = ["a", "b", "c"]

xs[?0]    # "a"
xs[?2]    # "c"
xs[?3]    # null
xs[?-1]   # "c"
xs[?-4]   # null
```

Strict access remains strict:

```amber
xs[3]
# IndexError
```

---

## 12.5. Optional map access

```amber
m = {
  a: 1,
  b: null,
}

m[?:a]    # 1
m[?:b]    # null
m[?:c]    # null
```

Presence remains distinguishable through `contains?`:

```amber
m.contains?(:b)  # true
m.contains?(:c)  # false
```

Strict access remains strict:

```amber
m[:c]
# KeyError
```

---

# 13. Recommended final surface

```amber
# Generated arrays
Array.of(N) |i|:
  expr

Array.of(N):
  expr_using(_1)

Array.build(N):
  expr

# Repeated-value arrays
Array.filled(N, value)

# Strict access
xs[i]
map[key]

# Optional access
xs[?i]
map[?key]

# Presence checks
xs.has_index?(i)
map.contains?(key)
```

---

# 14. Rationale summary

`Array.of` and `Array.build` make generated array construction explicit and per-slot.

`Array.filled` makes repeated-value construction explicit and does not hide copying semantics.

Optional bracket access keeps the dominant collection-addressing syntax while making nullable lookup flow visible at the exact site where absence is accepted.

The final design is intentionally explicit:

```amber
xs[i]     # require presence
xs[?i]    # accept absence
```

and:

```amber
map[key]  # require key
map[?key] # accept missing key
```

This preserves strict defaults while supporting common flow-dependent indexing and keyed lookup without method-style detours.
