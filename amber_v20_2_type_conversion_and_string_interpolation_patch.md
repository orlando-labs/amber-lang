# Amber v20.2 Draft Patch: Type Conversion Sugar and String Interpolation

**Status:** proposed normative extension for the main Amber language specification  
**Target base:** Amber v20.1 consolidated specification  
**Patch scope:** core language semantics, string literal/interpolation parsing, stdlib conversion protocol, HIR lowering, runtime errors and conformance tests  
**Non-goals:** static type inference, implicit JavaScript/Python-style coercion, device-transfer APIs, host-specific serialization formats

---

## 0. Integration note

This patch is intended to be inserted into the main Amber specification as a small language-level extension after the existing sections on postfix expressions, callable class objects, `as TypeTerm`, and string/profile semantics.

The patch preserves the following existing design decisions:

1. `Class(args...)` is the preferred constructor-call form and is semantically equivalent to `Class.new(args...)`.
2. `expr as TypeTerm` is a runtime type assertion/check boundary, not a conversion operation.
3. Parser output remains syntax-faithful; surface forms such as string interpolation must not be erased in the parser.
4. HIR is the semantic-core representation and must lower interpolation and conversion sugar into explicit operations.
5. Deterministic diagnostics, stack traces, disassembly and golden outputs must not expose raw memory addresses.
6. Privacy/Taint profile labels propagate through string interpolation.

---

## 1. Terminology

Amber distinguishes four related but separate operations:

| Operation | Surface form | Meaning |
|---|---|---|
| Type assertion | `expr as TypeTerm` | Checks that the value already satisfies the type term. Does not convert. |
| Constructor/conversion call | `TargetType(value)` | Ordinary class-object call through the existing constructor/callable path. |
| Cast protocol | `value.cast(TargetType)` | Explicit runtime conversion protocol. |
| Sugar conversion method | `value.to_int()`, `value.to_str()`, etc. | Stdlib convenience wrapper around the cast/stringification protocol. |

Normative rule:

```amber
x as Int        # assertion/check only
Int(x)          # explicit construction/conversion
x.cast(Int)     # explicit protocol conversion
x.to_int()      # stdlib sugar for x.cast(Int)
```

`as` must never parse, coerce, stringify, truncate or otherwise transform the value.

---

## 2. Type assertion semantics

`expr as TypeTerm` performs a runtime type assertion. It lowers to a `TypeCheckProgram` and uses the normal type-check semantics for `TypeTerm`.

Examples:

```amber
x = "123"

x as Str       # ok, result is x
x as Int       # TypeError
x as Int?      # TypeError unless x is Int or null
```

The result of a successful assertion is the original value, not a copy or converted value.

```amber
user2 = user as User
user2.object_id == user.object_id    # true, subject to object_id availability/profile
```

---

## 3. Explicit conversion: `TargetType(value)`

The preferred explicit construction/conversion spelling is the existing callable class object form:

```amber
Int("123")
Float("3.14")
Str(42)
Bool("true")
Symbol("name")
Date("2026-06-02")
Point([10, 20])
```

This is ordinary `HCall` / `CALL` over a class object and follows the same observable rules as `TargetType.new(value)` unless the target class defines a more specific call/constructor behavior.

A type may implement conversion in one of these class-side forms:

```amber
class Int:
  class_method def cast(value):
    ...

  class_method def parse(str as Str):
    ...
```

Recommended convention:

| Method | Intended meaning |
|---|---|
| `TargetType.cast(value)` | General explicit conversion from an already materialized Amber value. |
| `TargetType.parse(str as Str)` | Text parsing with syntax/format validation. |
| `TargetType.new(...)` / `TargetType(...)` | Construction path, usually allowed to delegate to `cast` for single-argument cases. |

For builtin scalar types, `TargetType(value)` should delegate to `TargetType.cast(value)` when called with exactly one positional argument and no block, unless the type's constructor has a more specific normative behavior.

---

## 4. Cast protocol

### 4.1. Canonical form

The canonical protocol form is:

```amber
value.cast(TargetType)
```

Normative lowering:

```amber
value.cast(TargetType)
```

is an ordinary method send whose stdlib implementation dispatches to:

```amber
TargetType.cast(value)
```

The core language does not reserve `.to(...)` for type conversion.

Rationale: `.to(...)` is intentionally left available for domain APIs such as tensor device transfer, graph routing, unit endpoints, streams and transport-like operations.

```amber
tensor.to("cuda:0")       # domain-specific device transfer
tensor.cast(Float32)      # type/dtype conversion
edge.to(node)             # graph/domain API
```

### 4.2. Failure

If the target type cannot perform the conversion, the operation raises `TypeError` or a more specific subtype such as `ValueError` when the source type is acceptable but the value content is invalid.

Examples:

```amber
"abc".cast(Int)      # ValueError or TypeError, depending on stdlib error taxonomy
[].cast(Int)         # TypeError
```

Recommended distinction:

| Situation | Error |
|---|---|
| Source type/category unsupported | `TypeError` |
| Source category supported, content invalid | `ValueError` |
| Runtime bytes/string encoding invalid at boundary | `EncodingError` |

If the current runtime error registry does not include `ValueError`, this patch adds it.

### 4.3. Nullable/non-raising cast

The stdlib may provide:

```amber
value.cast?(TargetType)
```

Semantics:

```amber
x.cast?(T)
```

returns a converted value or `null`.

`cast?` only catches ordinary conversion failures raised by the cast protocol. It must not swallow arbitrary programmer errors, cancellation, isolation errors, fatal VM errors, verifier errors or policy violations.

Example:

```amber
port = env["PORT"].cast?(Int) or 5432
```

---

## 5. Sugar conversion methods for standard types

Amber stdlib defines convenience methods on `Object` or a narrow conversion mixin:

```amber
value.to_str()
value.to_int()
value.to_float()
value.to_bool()
value.to_symbol()
value.to_array()
value.to_tuple()
value.to_set()
value.to_map()
```

These methods are sugar, not new syntax.

### 5.1. Canonical mapping

| Sugar method | Canonical expansion |
|---|---|
| `value.to_str()` | `Amber.stringify(value, mode: :display)` |
| `value.to_int()` | `value.cast(Int)` |
| `value.to_float()` | `value.cast(Float)` |
| `value.to_bool()` | `value.cast(Bool)` |
| `value.to_symbol()` | `value.cast(Symbol)` |
| `value.to_array()` | `value.cast(Array)` |
| `value.to_tuple()` | `value.cast(Tuple)` |
| `value.to_set()` | `value.cast(Set)` |
| `value.to_map()` | `value.cast(Map)` |

The sugar methods are ordinary methods and participate in normal dispatch, method lookup, open-world invalidation and frozen-world rules.

### 5.2. Method naming

The `to_` prefix is reserved by stdlib convention for explicit value conversion methods.

The bare method name `.to(...)` is not reserved and not defined by core Amber.

Valid examples:

```amber
"123".to_int()
42.to_str()
"3.14".to_float()
pairs.to_map()
items.to_array()
```

Domain APIs remain valid:

```amber
tensor.to("cuda:0")
duration.to(:seconds)
route.to(destination)
```

### 5.3. Standard scalar conversions

#### `to_str`

`value.to_str()` uses display stringification.

It must return `Str`.

For `Str`, it returns the receiver unchanged.

```amber
"abc".to_str()      # "abc"
42.to_str()         # "42"
null.to_str()       # "null"
```

#### `to_int`

Recommended builtin behavior:

| Receiver | Result |
|---|---|
| `Int` | receiver unchanged |
| `Float` | integer conversion according to `Int.cast`; truncation or exact-only behavior must be specified by stdlib |
| `Str` | parse canonical integer syntax |
| `Bool` | either rejected or mapped by explicit stdlib decision; recommended: reject |
| `null` | `TypeError` |

To avoid silent data loss, the recommended v1 behavior is:

```amber
3.0.to_int()        # 3
3.5.to_int()        # ValueError unless stdlib explicitly chooses truncation
"42".to_int()       # 42
"04".to_int()       # 4, if canonical parser accepts leading zeroes
"4.2".to_int()      # ValueError
```

If truncation is desired, stdlib should expose a separate spelling:

```amber
x.trunc()
x.floor()
x.ceil()
x.round()
```

#### `to_float`

Recommended builtin behavior:

| Receiver | Result |
|---|---|
| `Float` | receiver unchanged |
| `Int` | exact or nearest representable Float |
| `Str` | parse canonical floating syntax |
| `Bool` | rejected |
| `null` | `TypeError` |

#### `to_bool`

Recommended builtin behavior is deliberately strict:

| Receiver | Result |
|---|---|
| `Bool` | receiver unchanged |
| `Str` | parse only explicit canonical booleans, e.g. `"true"` / `"false"` |
| `Int` | rejected by default |
| `null` | `TypeError` |

`to_bool()` is not truthiness. Truthiness remains the language rule where only `false` and `null` are falsy.

Examples:

```amber
0.to_bool()         # TypeError
"".to_bool()        # ValueError
"false".to_bool()   # false
```

#### `to_symbol`

Recommended builtin behavior:

| Receiver | Result |
|---|---|
| `Symbol` | receiver unchanged |
| `Str` | symbol with the same name, if valid |
| Other | `TypeError` |

### 5.4. Collection conversions

Collection conversion methods are shallow unless a target element type is explicitly provided by a future typed conversion API.

```amber
tuple.to_array()
array.to_tuple()
array.to_set()
pairs.to_map()
```

Recommended behavior:

| Method | Required behavior |
|---|---|
| `to_array()` | Materializes finite ordered values into `Array`. |
| `to_tuple()` | Materializes finite ordered values into `Tuple`. |
| `to_set()` | Materializes finite iterable values into `Set`. |
| `to_map()` | Requires map-like entries or key/value pairs. |

Lazy or infinite sequences must not be silently exhausted without an explicit bound.

```amber
range.to_array()          # ok for finite range
lazy_seq.to_array()       # ok only if finite/realizable
infinite.to_array()       # RuntimeError or specific InfiniteCollectionError if available
```

### 5.5. v20.3 computed-property aliases

As a back-update from the v20.3 computed-property patch, v20.2 also exposes
read-only conversion properties as semantic aliases for the corresponding
`to_*` methods:

| Property alias | Equivalent method |
|---|---|
| `value.str` | `value.to_str()` |
| `value.int` | `value.to_int()` |
| `value.float` | `value.to_float()` |
| `value.bool` | `value.to_bool()` |
| `value.symbol` | `value.to_symbol()` |
| `value.array` | `value.to_array()` |
| `value.tuple` | `value.to_tuple()` |
| `value.set` | `value.to_set()` |
| `value.map` | `value.to_map()` |

These aliases use property-access semantics. They do not define ordinary
methods named `str`, `int`, `float`, and so on. Explicit call syntax continues
through ordinary method dispatch, so `value.str()` does not invoke the alias.

---

## 6. Stringification protocol

String conversion for display is not identical to `Str(value)` construction in all cases. Amber therefore defines a stringification hook.

### 6.1. `Amber.stringify`

Canonical operation:

```amber
Amber.stringify(value, mode: :display)
```

Rules:

1. If `value` is `Str`, return it unchanged.
2. Else if `value` responds to `to_str`, call `value.to_str()` and require a `Str` result.
3. Else if `value` responds to `inspect`, call `value.inspect()` and require a `Str` result.
4. Else produce a deterministic object representation that does not include raw memory addresses.

The fallback representation should be stable for golden tests and stack traces. It may include class name and a deterministic object id only if that id is explicitly defined by the runtime and normalized in deterministic outputs.

### 6.2. `to_str` versus `inspect`

| Method | Purpose |
|---|---|
| `to_str()` | Human-facing display string. Used by interpolation and `value.to_str()`. |
| `inspect()` | Debug representation. Used as fallback and by explicit debug formatting. |

Example:

```amber
class User:
  def to_str():
    @name

  def inspect():
    "#<User name=#{@name.inspect()}>"
```

`to_str()` and `inspect()` must return `Str`. Returning any other value is `TypeError`.

---

## 7. Interpolated string literals

### 7.1. Syntax

Amber supports interpolation inside double-quoted string literals:

```amber
"Hello, #{name}"
"sum = #{a + b}"
"user = #{user.inspect()}"
```

Interpolation grammar:

```ebnf
InterpolatedString ::= '"' StringPart* '"'
StringPart         ::= TextChunk
                     | EscapeSequence
                     | Interpolation
Interpolation      ::= "#{" Expr "}"
```

`Expr` is the ordinary Amber expression grammar. Newlines do not terminate the expression while inside interpolation delimiters.

Empty interpolation is invalid:

```amber
"#{}"       # invalid
"#{ }"      # invalid
```

### 7.2. Escapes

Minimum escape set:

```amber
"\n"
"\r"
"\t"
"\""
"\\"
"\#"
"\u{1F600}"
```

Literal interpolation marker:

```amber
"\#{not_interpolated}"
```

produces:

```text
#{not_interpolated}
```

Invalid escape sequences are compile-time diagnostics when visible in source.

### 7.3. Evaluation order

Interpolation parts are evaluated left-to-right.

```amber
"#{a()} #{b()} #{c()}"
```

is observationally equivalent to evaluating `a()`, stringifying it, then `b()`, stringifying it, then `c()`, stringifying it, and finally concatenating/building the result.

Embedded expressions are ordinary expression evaluations in the current scope. They update `$_` according to the existing `$_` rules. The final string expression then updates `$_` to the completed string.

Example:

```amber
s = "#{1 + 2} #{$_}"
# second interpolation observes $_ == 3
# s == "3 3"
```

### 7.4. Stringification in interpolation

Each embedded value is converted by:

```amber
Amber.stringify(value, mode: :display)
```

Therefore:

```amber
"#{x}"
```

is equivalent to:

```amber
Amber.stringify(x, mode: :display)
```

inside the string builder.

---

## 8. AST and HIR

### 8.1. AST

The parser must emit a syntax-faithful string node:

```text
AstStringLiteral(
  quote_kind: double,
  interpolation: true | false,
  parts: [
    AstStringText(value, span),
    AstStringEscape(kind, source, value, span),
    AstStringExpr(expr, span)
  ],
  span
)
```

The parser must not lower interpolation to concatenation.

### 8.2. HIR

HIR lowers interpolated strings into explicit string-build semantics:

```text
HStringBuild(
  parts: [
    HConstStr("Hello, "),
    HStringify(HLocalGet(:name), mode=:display),
    HConstStr("!")
  ],
  source_span_id
)
```

`HStringify` is explicit so that diagnostics, profile hooks, privacy/taint propagation, and optimizer decisions do not depend on hidden parser behavior.

### 8.3. Bytecode

A bytecode implementation may use any of these equivalent lowerings:

1. `STR_BUILD` convenience opcode;
2. ordinary `StringBuilder` object calls;
3. optimized concatenation for fully constant strings;
4. partial constant folding plus builder append operations.

Observable behavior must remain equivalent.

A minimal lowering through ordinary calls is acceptable:

```text
builder = Amber::StringBuilder.new()
builder.append("Hello, ")
builder.append(Amber.stringify(name, mode: :display))
builder.append("!")
builder.finish()
```

---

## 9. Privacy, taint and lineage

If the Amber/Privacy, Taint & Lineage profile is enabled, string interpolation propagates labels from every interpolated value into the result string.

```amber
email as Str @pii
msg = "User email: #{email}"
# msg carries @pii
```

Literal text itself may contribute source/location metadata but normally carries no privacy label unless a host policy assigns one.

Logs/traces using interpolated strings must observe the same policy checks as any other labeled string. Policy violation raises `PolicyViolationError`.

---

## 10. Diagnostics and runtime errors

### 10.1. Compile-time diagnostics

This patch adds or standardizes:

```text
AMB_STRING_UNTERMINATED
AMB_STRING_BAD_ESCAPE
AMB_STRING_INTERP_UNTERMINATED
AMB_STRING_INTERP_EMPTY
AMB_STRING_INTERP_PARSE_ERROR
AMB_STRING_INTERP_NESTING_LIMIT
```

Recommended diagnostic behavior:

| Condition | Diagnostic |
|---|---|
| Missing closing quote | `AMB_STRING_UNTERMINATED` |
| Unknown escape | `AMB_STRING_BAD_ESCAPE` |
| Missing `}` in interpolation | `AMB_STRING_INTERP_UNTERMINATED` |
| Empty interpolation body | `AMB_STRING_INTERP_EMPTY` |
| Invalid Amber expression inside interpolation | `AMB_STRING_INTERP_PARSE_ERROR` |
| Implementation-defined nesting bound exceeded | `AMB_STRING_INTERP_NESTING_LIMIT` |

### 10.2. Runtime errors

This patch uses existing errors where possible:

| Error | Use |
|---|---|
| `TypeError` | Unsupported cast, wrong target type, `to_str`/`inspect` returning non-`Str`. |
| `EncodingError` | Invalid runtime string/buffer encoding at a boundary. |
| `PolicyViolationError` | Privacy/taint export/logging violation. |

This patch adds, if absent:

```text
ValueError
```

Use `ValueError` when the source value has an acceptable type/category but invalid content for the requested conversion.

Examples:

```amber
"abc".to_int()       # ValueError
[].to_int()          # TypeError
```

---

## 11. Standard library contract

### 11.1. Required methods

The following methods are required in the reference stdlib:

```amber
Object#cast(TargetType)
Object#cast?(TargetType)

Object#to_str()
Object#to_int()
Object#to_float()
Object#to_bool()
Object#to_symbol()
Object#to_array()
Object#to_tuple()
Object#to_set()
Object#to_map()

Amber.stringify(value, mode: :display)
```

A minimal implementation may place the `to_*` methods in a conversion mixin included by `Object`, provided ordinary method lookup observes them as object methods.

### 11.2. Required class-side cast methods

The following builtin types must provide class-side `cast`:

```amber
Str.cast(value)
Int.cast(value)
Float.cast(value)
Bool.cast(value)
Symbol.cast(value)
Array.cast(value)
Tuple.cast(value)
Set.cast(value)
Map.cast(value)
```

### 11.3. Optional aliases

The stdlib may provide:

```amber
value.to_type(TargetType)
```

as an alias for:

```amber
value.cast(TargetType)
```

However, `to_type` is not the canonical spelling in core documentation, diagnostics or lowering. The canonical protocol remains `cast`.

---

## 12. Examples

### 12.1. Environment parsing

```amber
port = env["PORT"].cast?(Int) or 5432
debug = env["DEBUG"].cast?(Bool) or false
```

### 12.2. User-defined conversion

```amber
class Money:
  def init(@cents as Int):
    pass

  class_method def cast(value):
    case value
    when Money:
      value
    when Int:
      Money(value)
    when Str:
      Money.parse(value)
    else:
      raise TypeError.new("cannot cast to Money")

  class_method def parse(str as Str):
    cents = parse_decimal_cents(str)
    Money(cents)

"1200".cast(Money)
```

### 12.3. Display versus debug

```amber
class User:
  def init(@name):
    pass

  def to_str():
    @name

  def inspect():
    "#<User name=#{@name.inspect()}>"

u = User("Ada")

"hello #{u}"          # "hello Ada"
u.inspect()           # "#<User name=\"Ada\">", subject to exact Str#inspect escaping
```

### 12.4. Avoiding `.to(...)` conflict

```amber
tensor.to("cuda:0")       # device transfer, library-defined
tensor.to_float()         # scalar conversion if tensor is scalar-like and library supports it
tensor.cast(Float32)      # dtype/type conversion, target type explicit
```

---

## 13. Conformance tests

### 13.1. Parser tests

Required fixtures:

```amber
"plain"
"hello #{name}"
"sum #{a + b}"
"\#{literal}"
"#{if ok: "yes" else: "no"}"
```

Negative fixtures:

```amber
"#{"
"#{}"
"#{ }"
"\q"
"unterminated
```

### 13.2. HIR golden tests

Required HIR cases:

1. Plain string remains constant.
2. Interpolated string lowers to `HStringBuild`.
3. Each expression part is wrapped in `HStringify`.
4. Source spans inside interpolation point to the embedded expression.
5. Constant-only interpolated strings may be folded only after preserving equivalent diagnostics.

### 13.3. Runtime tests

Required behavior:

```amber
"#{1 + 2}" == "3"
"#{null}" == "null"
"#{true}" == "true"
"#{false}" == "false"
"#{1 + 2} #{$_}" == "3 3"

"123".to_int() == 123
123.to_str() == "123"
"true".to_bool() == true
"false".to_bool() == false

expect_error ValueError:
  "abc".to_int()

expect_error TypeError:
  [].to_int()
```

### 13.4. Privacy profile tests

Required behavior:

```amber
email as Str @pii
msg = "email=#{email}"
# msg carries @pii
```

Attempting to export/log `msg` under a policy that denies `@pii` must raise `PolicyViolationError`.

---

## 14. Patch summary

This extension adds:

1. Explicit cast protocol: `value.cast(TargetType)`.
2. Nullable cast attempt: `value.cast?(TargetType)`.
3. Standard sugar methods: `to_str`, `to_int`, `to_float`, `to_bool`, `to_symbol`, `to_array`, `to_tuple`, `to_set`, `to_map`.
4. Stringification protocol: `Amber.stringify(value, mode: :display)`.
5. Interpolated string literals using `#{ Expr }`.
6. AST/HIR contracts for interpolation.
7. Deterministic fallback string representation.
8. Privacy/taint propagation through interpolation.
9. Diagnostics for malformed strings/interpolation.
10. Optional `ValueError` runtime error for invalid conversion content.

The extension deliberately does not define `.to(...)` as a type conversion method.
