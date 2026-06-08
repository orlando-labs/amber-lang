# Amber v20.8 Draft Patch: Name-Indifferent Maps, Strict Exact-Key Maps, and Keyword Spread `kwargs` View

**Status:** proposed normative extension for the main Amber language specification  
**Target base:** Amber v20.7 spread expansion + Amber v20.6 value-keyed `Map` / `Set` + Amber v20.5 optional bracket access + Amber v20.3 computed properties  
**Patch scope:** associative container key semantics, `Map` / `HashMap` default behavior, exact-key map variants, JSON-facing ergonomics, map pattern matching, keyword spread validation, `kwargs` view protocol, diagnostics, HIR/runtime lowering, conformance corpus  
**Non-goals:** Ruby compatibility aliases, automatic conversion of arbitrary enumerables to keyword arguments, global string-to-symbol interning of external data, changing `Set` element semantics except where explicitly mentioned, changing ordinary identifier grammar, changing callable dispatch semantics

---

## 0. Integration note

This patch revises the Amber v20.6 `Map` / `HashMap` key model and the Amber v20.7 keyword spread model.

The core decision is:

```amber
Map / HashMap          # name-indifferent by default for Symbol/Str name keys
StrictMap / StrictHashMap  # exact-key containers preserving Symbol and Str as distinct keys
```

Ordinary associative containers now optimize for the common application-data case:

```amber
payload[:user_id]
payload["user_id"]

case payload
in {user_id: id}
  id
end
```

This works whether the payload was written in Amber source using symbol-key shorthand or came from JSON/deserialization with string keys.

Exact `Symbol` versus `Str` key separation remains available through explicit strict containers:

```amber
m = StrictMap{
  user_id: "symbol key",
  "user_id": "string key",
}

m[:user_id]    # "symbol key"
m["user_id"]  # "string key"
```

Keyword spread also becomes application-friendly:

```amber
fn(**opts)
```

is valid when `opts` can produce a keyword-argument view and every produced key can be converted to a valid Amber keyword name. Conceptually, `**opts` behaves like spread over `opts.kwargs`, but implementations may lower this as an intrinsic/protocol operation rather than an ordinary property access.

---

## 1. Background and prerequisites

Amber already has several interacting design choices that make this patch important.

### 1.1. Symbol-key shorthand

Amber source maps use Ruby-like shorthand:

```amber
{name: value}
```

which is syntax-faithfully represented as a symbol-key entry:

```amber
{:name: value}
```

This shorthand is idiomatic for application payloads, options, configuration objects, and pattern matching.

### 1.2. External data usually has string keys

JSON and many other serialization formats materialize object keys as strings:

```json
{
  "user_id": 123,
  "name": "Ada"
}
```

Without name-indifferent associative containers, Amber code that naturally expects symbol-key access can silently fail after deserialization:

```amber
payload = Json.parse(body)

payload[:user_id]      # KeyError or null in strict string-key maps
payload["user_id"]    # works
```

The same problem affects map pattern matching:

```amber
case payload
in {user_id: id}
  id
else
  null
end
```

If the pattern requests a symbol key but JSON produced string keys, the code looks correct but does not match.

### 1.3. Exact Symbol/Str separation is rarer than accidental mismatch

Some advanced code intentionally needs to distinguish a symbolic internal key from an external string key:

```amber
m = StrictMap{
  name: "internal symbolic key",
  "name": "external string key",
}
```

This is a real use case, but it is comparatively specialized. In ordinary API, JSON, config, controller, CLI, job, notebook, and test code, having both `:name` and `"name"` as separate entries is more often a source of bugs than a useful distinction.

Therefore the default should optimize for the common case, and exact-key behavior should be explicit.

---

## 2. Rationale

### 2.1. Default ergonomics should match application-data reality

Amber aims to be Ruby-inspired and expression-oriented while retaining deterministic, explicit runtime semantics. In Ruby ecosystems, a long-standing pain point is the distinction between symbol keys written in code and string keys returned by JSON. Rails addresses this with `HashWithIndifferentAccess`; however, requiring an opt-in wrapper at every boundary is easy to forget.

Amber can make the safer choice at the language-container level:

```amber
payload[:id]
payload["id"]
```

should address the same entry for ordinary maps.

### 2.2. Pattern matching must not be fragile at serialization boundaries

Pattern matching is part of Amber's core model. The following should be robust:

```amber
payload = Json.parse(body)

case payload
in {id: id, email: email}
  User(id, email)
end
```

If ordinary maps distinguish `:id` and `"id"`, then map patterns become brittle around JSON and host interop. Name-indifferent maps make pattern matching usable for external payloads without boilerplate normalization.

### 2.3. Strict maps remain available for precise modeling

The language should not remove expressiveness. Code that intentionally needs exact-key separation can say so:

```amber
StrictMap{name: 1, "name": 2}
```

This turns a rare, subtle distinction into an explicit type-level choice.

### 2.4. Avoid global string-to-symbol interning for external data

This patch does not require JSON or external strings to become globally interned symbols. Ordinary `Map` may store canonical name keys as strings or as an internal `NameKey` representation. This avoids unbounded symbol interning from untrusted input while preserving ergonomic access.

### 2.5. Keyword spread should validate, not reject common maps upfront

Given:

```amber
opts = {"mode": :fast, "limit": 10}
fn(**opts)
```

rejecting this solely because keys originated as strings is unnecessarily strict. The relevant question is whether every key can become a valid Amber keyword name. Therefore `**opts` should validate keys and raise a precise error only when invalid keys occur.

---

## 3. Terminology

| Term | Meaning |
|---|---|
| Name key | A key whose addressable name is a textual identifier-like name shared by `Symbol(:name)` and `Str("name")`. |
| Name-indifferent map | A map where `Symbol(:name)` and `Str("name")` address the same entry. |
| Exact-key map | A map where key type participates in identity/equality and `Symbol(:name)` is distinct from `Str("name")`. |
| Canonical name-key export | The value exposed by APIs such as `keys()` for a name key. In this patch it is `Str`. |
| Keyword-convertible key | A key that can be converted to a valid Amber keyword argument name during `**` spread. |
| Kwargs view | A finite map-like view used by keyword spread after validation. |

---

## 4. Design principles

### 4.1. Ordinary maps are application-data maps

Plain map literals and `Map` constructors create name-indifferent maps:

```amber
{name: 1}
{"name": 1}
Map{name: 1}
HashMap{"name": 1}
```

All of these support both symbol and string lookup for the same textual name.

### 4.2. Strictness is explicit

Exact-key behavior requires an explicit strict type:

```amber
StrictMap{name: 1, "name": 2}
StrictHashMap{name: 1, "name": 2}
```

### 4.3. Pattern matching follows the matched map's semantics

For ordinary `Map` / `HashMap`, named-key patterns are name-indifferent.

For `StrictMap` / `StrictHashMap`, named-key patterns remain exact-symbol-key based unless the strict object explicitly exposes a different `deconstruct_keys` behavior.

### 4.4. Keyword spread is validation-based

`**expr` obtains a kwargs view and validates keys. It is not limited to maps that physically store symbol keys.

### 4.5. User objects may participate through `kwargs`, but the protocol is narrow

User objects may participate in keyword spread when they expose a readable `kwargs` property. This uses Amber's property descriptor model rather than implicit nullary method invocation.

The returned value must itself be a valid keyword-spread value or keyword-entry view.

---

## 5. Surface syntax and container types

### 5.1. Ordinary map literals

Plain map literals create ordinary `Map`:

```amber
m = {name: "Ada"}
```

Lookup is name-indifferent:

```amber
m[:name]    # "Ada"
m["name"]  # "Ada"
```

String-key literal entries behave the same way:

```amber
m = {"name": "Ada"}

m[:name]    # "Ada"
m["name"]  # "Ada"
```

### 5.2. Explicit ordinary map constructors

```amber
Map{name: 1}
HashMap{"name": 1}
```

Both use name-indifferent key normalization for `Symbol`/`Str` name keys.

### 5.3. Strict exact-key containers

Amber adds:

```amber
StrictMap
StrictHashMap
```

Examples:

```amber
m = StrictMap{name: 1, "name": 2}

m[:name]    # 1
m["name"]  # 2
m.keys()    # [:name, "name"]
```

Strict containers preserve the current exact-key behavior for `Symbol` and `Str`.

---

## 6. Ordinary `Map` / `HashMap` key normalization

### 6.1. Name-key normalization

For ordinary `Map` and `HashMap`:

```text
Symbol(name) -> NameKey(name)
Str(name)    -> NameKey(name)
```

`NameKey(name)` is an abstract runtime key. Implementations may store it as:

1. an internal tagged key object;
2. a canonical string key;
3. a compact symbol/string pair with canonical comparison;
4. another representation with identical observable behavior.

### 6.2. Canonical export

Name keys export as `Str` through ordinary public key enumeration APIs:

```amber
m = {name: 1}

m.keys()     # ["name"]
m.entries()  # [["name", 1]] or equivalent entry representation
```

Rationale: external data formats, serialization, JSON interop, and diagnostics are better served by string export than by creating or preserving symbol provenance.

Implementations may expose provenance through debug/reflection APIs, but ordinary semantics must not depend on provenance.

### 6.3. Duplicate overwrite

Duplicate name keys overwrite the value and preserve the first normalized position:

```amber
m = {name: 1, "name": 2}

m[:name]    # 2
m["name"]  # 2
m.keys()    # ["name"]
```

Insertion order is based on the first occurrence of the normalized key.

### 6.4. Other key types

Non-`Symbol`/`Str` keys continue to follow ordinary value-keyed map semantics:

```amber
m = {1: "int", 1.0: "float"}
```

The exact equality relationship between numeric or structural values remains governed by the existing runtime key equality rules for the corresponding map type.

This patch only changes the default treatment of `Symbol` and `Str` textual name keys in ordinary maps.

---

## 7. Strict exact-key map semantics

`StrictMap` and `StrictHashMap` preserve exact `Symbol` versus `Str` key identity/equality.

```amber
m = StrictMap{name: "symbol", "name": "string"}

m[:name]    # "symbol"
m["name"]  # "string"
```

### 7.1. Duplicate overwrite in strict maps

Duplicates are determined by exact-key semantics:

```amber
m = StrictMap{name: 1, :name: 2, "name": 3}

m[:name]    # 2
m["name"]  # 3
m.keys()    # [:name, "name"]
```

The first exact key position is preserved for duplicates of that exact key.

### 7.2. Use cases for strict maps

Strict maps are intended for:

1. language tooling and AST metadata where symbolic keys and external field names must be distinguished;
2. protocol bridges that must preserve exact source key types;
3. debugging, migration, compatibility layers, and conformance tests;
4. advanced metaprogramming where key type has semantic meaning;
5. security-sensitive adapters that must reject ambiguous name-key collapse.

---

## 8. JSON and external data integration

### 8.1. Default JSON behavior

`Json.parse` should return ordinary `Map` by default:

```amber
payload = Json.parse(body)

payload[:user_id]
payload["user_id"]
```

This means deserialized object keys are accessible through both symbol and string addressing.

### 8.2. Pattern matching over JSON payloads

```amber
payload = Json.parse(body)

case payload
in {user_id: id, name: name}
  User(id, name)
end
```

The pattern must match JSON objects with string keys.

### 8.3. Exact preservation option

Exact preservation may be requested explicitly:

```amber
payload = Json.parse(body, map: StrictMap)
```

or, if the JSON package prefers key-mode terminology:

```amber
payload = Json.parse(body, keys: :strict)
```

This patch recommends the constructor-oriented spelling:

```amber
Json.parse(body, map: StrictMap)
```

because it composes with custom map implementations.

### 8.4. No default symbolization of external strings

JSON keys should not be converted into global symbols by default. Ordinary maps may expose name-indifferent access without symbol interning.

---

## 9. Pattern matching semantics

### 9.1. Ordinary maps

Named-key map patterns use ordinary lookup semantics of the matched map.

For ordinary `Map` / `HashMap`, lookup is name-indifferent:

```amber
case {"user_id": 123}
in {user_id: id}
  id   # 123
end
```

The following forms are equivalent for matching purposes in ordinary maps:

```amber
{user_id: 123}
{:user_id: 123}
{"user_id": 123}
```

### 9.2. Strict maps

For strict maps, named-key patterns request exact symbol keys unless the object exposes a custom `deconstruct_keys` implementation.

```amber
case StrictMap{"user_id": 123}
in {user_id: id}
  id
else
  null
end
# => null
```

```amber
case StrictMap{user_id: 123}
in {user_id: id}
  id
end
# => 123
```

### 9.3. Rest capture

For ordinary maps, rest capture observes canonical exported keys:

```amber
case {"id": 1, "name": "Ada"}
in {id: id, **rest}
  rest.keys()  # ["name"]
end
```

For strict maps, rest capture preserves exact keys.

### 9.4. `deconstruct_keys`

`Map#deconstruct_keys(keys)` and `HashMap#deconstruct_keys(keys)` must use name-indifferent lookup.

`StrictMap#deconstruct_keys(keys)` and `StrictHashMap#deconstruct_keys(keys)` use exact-symbol lookup for named-key patterns unless overridden by a user-defined/custom implementation.

---

## 10. Keyword spread semantics

### 10.1. Surface form

Keyword spread remains:

```amber
fn(**opts)
```

### 10.2. Conceptual lowering

Conceptually:

```amber
fn(**opts)
```

behaves like:

```amber
fn(**opts.kwargs)
```

However, this is a semantic description, not a required source-to-source rewrite. Implementations may lower keyword spread to an intrinsic/protocol operation:

```text
KWARGS_VIEW(opts)
VALIDATE_KWARGS(view)
CALL_WITH_KWARGS(fn, view)
```

This avoids exposing intermediate objects and allows precise diagnostics at the spread site.

### 10.3. Evaluation order

For:

```amber
fn(a(), **b(), c: d(), **e())
```

Evaluation order is:

```text
fn
a()
b()
kwargs view/validation for b result
d()
e()
kwargs view/validation for e result
call
```

This preserves ordinary left-to-right spread evaluation.

### 10.4. Accepted operands

Keyword spread accepts:

1. `Map`;
2. `HashMap`;
3. `StrictMap`;
4. `StrictHashMap`;
5. objects exposing a readable `kwargs` property whose result is itself a valid keyword-spread value or keyword-entry view.

Future revisions may extend this protocol to dedicated keyword-entry view types. This patch deliberately does not accept arbitrary arrays of pairs, generic enumerables, or `each_pair`-style protocols.

### 10.5. Keyword-convertible keys

A key is keyword-convertible iff it is one of:

1. `Symbol(name)` where `name` is a valid Amber keyword argument name;
2. `Str(name)` where `name` is a valid Amber keyword argument name;
3. ordinary `Map` / `HashMap` `NameKey(name)` where `name` is a valid Amber keyword argument name.

Valid:

```amber
fn(**{mode: :fast})
fn(**{"mode": :fast})
fn(**{:mode: :fast})
```

Invalid:

```amber
fn(**{"user-id": 1})
fn(**{"first name": "Ada"})
fn(**{"1st": true})
fn(**{1: "one"})
fn(**{null: "x"})
```

### 10.6. Keyword name grammar

Keyword argument names use Amber parameter-name identifier rules.

If the base language permits parameter identifiers with `?` or `!` suffixes, then such names are valid for keyword spread. If parameter identifiers exclude those suffixes, keyword spread must reject them.

This patch does not independently expand identifier grammar.

### 10.7. Duplicate keyword detection

After key conversion, duplicate keyword names are an error unless the duplicate was already collapsed by the source map before spread.

Ordinary `Map` collapses name duplicates during construction:

```amber
opts = {name: 1, "name": 2}
fn(**opts)  # ok; keyword name receives 2
```

Strict maps may preserve both keys, so spread may detect a duplicate:

```amber
opts = StrictMap{name: 1, "name": 2}
fn(**opts)
```

Required runtime error:

```text
KeywordArgumentError
duplicate keyword argument `name`
```

### 10.8. Invalid key diagnostics

Invalid keyword-spread keys raise `KeywordArgumentError`.

Examples:

```amber
fn(**{"user-id": 1})
```

```text
KeywordArgumentError
map key `"user-id"` cannot be used as a keyword argument name
```

```amber
fn(**{1: "one"})
```

```text
KeywordArgumentError
map key `1` cannot be used as a keyword argument name
```

### 10.9. Non-spreadable operands

```amber
fn(**42)
```

Required runtime error:

```text
KeywordArgumentError
object of type Int cannot be used as keyword spread operand
```

Implementations may use a more specific protocol error code if the broader runtime error registry requires it, but the error class must be deterministic and attributable to the spread site.

---

## 11. User-defined `kwargs` property protocol

### 11.1. Motivation

User-defined objects often represent options/configuration bundles:

```amber
class RequestOptions:
  def init(@timeout, @retries):
    noop
```

It is ergonomic to pass such an object as keyword arguments:

```amber
fn(**RequestOptions(30, 3))
```

without requiring the caller to manually extract a map.

### 11.2. Property-based protocol

A user object may participate in keyword spread by exposing a readable `kwargs` property:

```amber
class RequestOptions:
  def init(@timeout, @retries):
    noop

  prop kwargs:
    {
      timeout: @timeout,
      retries: @retries,
    }

fn(**RequestOptions(30, 3))
```

The `kwargs` property is evaluated exactly once.

### 11.3. Result validation

The result of `kwargs` must be a valid keyword-spread operand or a dedicated keyword-entry view recognized by the runtime.

Valid:

```amber
prop kwargs:
  {timeout: @timeout, retries: @retries}
```

Invalid:

```amber
prop kwargs:
  [["timeout", @timeout]]
```

unless a future revision explicitly accepts arrays of pairs.

### 11.4. Error attribution

Errors raised while evaluating the `kwargs` property propagate normally, but validation errors after property evaluation are attributed to the `**` spread site.

### 11.5. No implicit nullary method call

The protocol uses a property, not an implicit call to a method named `kwargs`.

If a class declares:

```amber
def kwargs():
  {mode: :fast}
```

then `fn(**obj)` does not implicitly call `obj.kwargs()` unless the language's property/method model separately defines such behavior. To participate, the class should declare:

```amber
prop kwargs:
  {mode: :fast}
```

or expose an equivalent property descriptor.

---

## 12. HIR and runtime lowering

### 12.1. Map literal lowering

Ordinary map literals lower to construction of ordinary `Map` with name-key normalization:

```amber
{name: 1, "name": 2}
```

HIR must preserve syntax-faithful source information for diagnostics and formatting but the runtime construction operation normalizes both keys to the same `NameKey("name")`.

### 12.2. Strict map literal lowering

```amber
StrictMap{name: 1, "name": 2}
```

lowers to strict map construction. No name-key collapse occurs between `Symbol(:name)` and `Str("name")`.

### 12.3. Lookup lowering

For ordinary maps:

```amber
m[:name]
m["name"]
```

both lower to keyed lookup whose runtime key normalization resolves to `NameKey("name")`.

For strict maps, lookup uses exact-key semantics.

### 12.4. Pattern lowering

Named-key map pattern lowering must call `deconstruct_keys` or the equivalent pattern lookup helper using the matched object's semantics.

For ordinary maps, the helper uses name-indifferent lookup.

For strict maps, the helper uses exact-symbol lookup unless a custom implementation is provided.

### 12.5. Keyword spread lowering

Keyword spread lowers to a staged operation:

```text
1. Evaluate operand.
2. Obtain kwargs view.
3. Validate finite keyword entries.
4. Convert keys to keyword symbols/names.
5. Detect duplicates across explicit and spread keywords.
6. Perform call.
```

Example:

```amber
fn(a: 1, **opts, b: 2)
```

must detect duplicates among `a`, keys from `opts`, and `b` after all keyword names are normalized.

### 12.6. Bytecode considerations

Implementations may add helper opcodes or runtime calls such as:

```text
KWARGS_VIEW
KWARGS_VALIDATE
KWARGS_MERGE
CALL_KW
```

or fold these into existing `CALL` metadata. Observable behavior must remain deterministic.

---

## 13. Diagnostics

Suggested diagnostic/error names:

| Code / Error | Situation |
|---|---|
| `KeywordArgumentError` | Runtime class for invalid keyword spread conversion or duplicate keyword after spread. |
| `E_KWARG_SPREAD_KEY` | Key cannot be converted to a valid keyword name. |
| `E_KWARG_SPREAD_DUPLICATE` | Duplicate keyword produced after spread key conversion. |
| `E_KWARG_SPREAD_OPERAND` | Operand cannot produce a kwargs view. |
| `E_KWARG_SPREAD_RESULT` | User object's `kwargs` property produced an invalid spread value. |
| `E_STRICT_MAP_DUPLICATE_DEBUG` | Optional debug/lint diagnostic for exact keys that become duplicate keyword names under spread. |

### 13.1. Invalid key

```amber
fn(**{"user-id": 1})
```

```text
KeywordArgumentError
map key `"user-id"` cannot be used as a keyword argument name
```

### 13.2. Duplicate after strict spread

```amber
fn(**StrictMap{name: 1, "name": 2})
```

```text
KeywordArgumentError
duplicate keyword argument `name`
```

### 13.3. Non-spreadable operand

```amber
fn(**42)
```

```text
KeywordArgumentError
object of type Int cannot be used as keyword spread operand
```

### 13.4. Invalid `kwargs` result

```amber
class BadOptions:
  prop kwargs:
    42

fn(**BadOptions())
```

```text
KeywordArgumentError
`kwargs` property must return a keyword-spreadable value
```

---

## 14. Compatibility impact

### 14.1. Changes from v20.6

The v20.6 behavior where ordinary `Map` treats `:name` and `"name"` as distinct keys is replaced.

Old ordinary map behavior moves to:

```amber
StrictMap
StrictHashMap
```

### 14.2. Changes to map pattern matching

Old behavior:

```amber
case {"id": 1}
in {id: id}
  id
else
  null
end
# old result: null
```

New behavior for ordinary maps:

```amber
# new result: 1
```

Strict maps preserve old behavior:

```amber
case StrictMap{"id": 1}
in {id: id}
  id
else
  null
end
# null
```

### 14.3. Changes from v20.7 keyword spread

Keyword spread is no longer restricted to maps with physical symbol keys. It validates name-convertible keys instead.

```amber
fn(**{"mode": :fast}) # now valid
```

Invalid keys still raise:

```amber
fn(**{"user-id": :bad}) # KeywordArgumentError
```

### 14.4. Migration guide

Code that intentionally relied on `:name` and `"name"` being separate in ordinary maps should migrate to `StrictMap`:

```amber
# old
m = {name: 1, "name": 2}

# new
m = StrictMap{name: 1, "name": 2}
```

Code that performed manual JSON key normalization can often remove it:

```amber
# old
payload = Json.parse(body).symbolize_keys()

# new
payload = Json.parse(body)
payload[:id]
```

---

## 15. Security and robustness notes

### 15.1. Avoid symbol-interning denial of service

Name-indifferent lookup must not require turning every external string key into a globally interned symbol. Implementations should prefer canonical string/name-key storage for ordinary maps.

### 15.2. Keyword spread validates keys at call boundaries

External maps may be used as keyword spread operands only if every key is a valid keyword name. This prevents invalid external field names such as `"user-id"` from silently becoming call keywords.

### 15.3. Explicit strict containers for ambiguous data

When key provenance matters, use `StrictMap` or `StrictHashMap`.

### 15.4. User-defined `kwargs` should be narrow

The `kwargs` property protocol is intentionally narrower than generic enumeration. This avoids surprising calls from arbitrary pair-like objects and keeps call diagnostics deterministic.

---

## 16. Conformance tests

### 16.1. Ordinary map name-indifferent lookup

```amber
m = {name: "Ada"}
assert_equal("Ada", m[:name])
assert_equal("Ada", m["name"])
```

### 16.2. String literal key lookup by symbol

```amber
m = {"name": "Ada"}
assert_equal("Ada", m[:name])
assert_equal("Ada", m["name"])
```

### 16.3. Duplicate collapse

```amber
m = {name: 1, "name": 2}
assert_equal(2, m[:name])
assert_equal(2, m["name"])
assert_equal(["name"], m.keys())
```

### 16.4. Strict map preserves distinction

```amber
m = StrictMap{name: 1, "name": 2}
assert_equal(1, m[:name])
assert_equal(2, m["name"])
assert_equal([:name, "name"], m.keys())
```

### 16.5. JSON pattern matching

```amber
payload = Json.parse('{"user_id": 123}')

result = case payload
in {user_id: id}
  id
else
  null
end

assert_equal(123, result)
```

### 16.6. Strict pattern mismatch

```amber
payload = StrictMap{"user_id": 123}

result = case payload
in {user_id: id}
  id
else
  null
end

assert_equal(null, result)
```

### 16.7. Keyword spread from symbol-key map

```amber
def f(mode:):
  mode

assert_equal(:fast, f(**{mode: :fast}))
```

### 16.8. Keyword spread from string-key map

```amber
def f(mode:):
  mode

assert_equal(:fast, f(**{"mode": :fast}))
```

### 16.9. Keyword spread invalid string key

```amber
def f(**kwargs):
  kwargs

assert_raises(KeywordArgumentError):
  f(**{"user-id": 1})
```

### 16.10. Keyword spread invalid non-name key

```amber
def f(**kwargs):
  kwargs

assert_raises(KeywordArgumentError):
  f(**{1: "one"})
```

### 16.11. Strict duplicate keyword after spread

```amber
def f(name:):
  name

assert_raises(KeywordArgumentError):
  f(**StrictMap{name: 1, "name": 2})
```

### 16.12. User object `kwargs` property

```amber
class Options:
  def init(@mode):
    noop

  prop kwargs:
    {mode: @mode}

def f(mode:):
  mode

assert_equal(:fast, f(**Options(:fast)))
```

### 16.13. Invalid user object `kwargs` result

```amber
class BadOptions:
  prop kwargs:
    42

def f(**kwargs):
  kwargs

assert_raises(KeywordArgumentError):
  f(**BadOptions())
```

---

## 17. Reference implementation checklist

### Parser / AST

- Preserve map literal key surface form for formatter and diagnostics.
- Preserve `StrictMap{...}` / `StrictHashMap{...}` typed literal constructor nodes.
- Preserve `**expr` spread nodes.

### HIR

- Lower ordinary map literals to name-indifferent map construction.
- Lower strict map constructors to exact-key construction.
- Lower keyword spread to explicit kwargs-view and validation stages or equivalent intrinsic nodes.

### Runtime

- Add `NameKey` normalization for ordinary `Map` / `HashMap`.
- Add `StrictMap` / `StrictHashMap` types.
- Implement canonical string export for ordinary name keys.
- Implement `Map#deconstruct_keys` with name-indifferent lookup.
- Implement strict map exact-key `deconstruct_keys` behavior.
- Implement keyword-spread validation and duplicate detection.
- Implement `kwargs` property participation for user objects.

### Diagnostics

- Add deterministic `KeywordArgumentError` messages.
- Attribute validation errors to the spread site.
- Avoid memory addresses or host-specific formatting in diagnostics.

### Stdlib / JSON

- Make `Json.parse` return ordinary `Map` by default.
- Add explicit strict preservation option, preferably `map: StrictMap`.
- Document that JSON keys are not globally symbolized by default.

---

## 18. Open implementation choices

The following are intentionally left to implementation, provided observable behavior matches this patch:

1. whether ordinary map `NameKey` is represented as a tagged internal value or canonical string;
2. whether keyword names are represented internally as symbols, strings, or call-site keyword IDs;
3. whether `KWARGS_VIEW` is a bytecode opcode, HIR intrinsic, runtime helper, or folded into `CALL`;
4. whether reflection APIs expose key provenance for name-indifferent map entries;
5. whether a future dedicated `Kwargs` view type is introduced.

---

## 19. Summary

This patch makes ordinary Amber associative containers safer and more useful for the most common application-data workflows.

The default becomes:

```amber
m = {name: "Ada"}

m[:name]    # "Ada"
m["name"]  # "Ada"
```

Pattern matching becomes robust across source maps and JSON maps:

```amber
case Json.parse(body)
in {user_id: id}
  id
end
```

Keyword spread becomes validation-based and ergonomic:

```amber
fn(**{"mode": :fast}) # ok
fn(**{"user-id": 1}) # KeywordArgumentError
```

Exact distinction remains available when it is truly needed:

```amber
StrictMap{name: "symbol", "name": "string"}
```

The resulting model favors the common, less error-prone path while preserving precise low-level semantics through explicit strict containers.
