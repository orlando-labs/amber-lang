# Amber v20.3 Draft Patch: Computed Property Descriptors

**Status:** proposed normative extension for the main Amber language specification  
**Target base:** Amber v20.1 consolidated specification + Amber v20.2 draft patch  
**Patch scope:** core language syntax, object member access semantics, property descriptor declarations, getter/setter semantics, assignment lowering, callable references, AST/HIR lowering, diagnostics, reflection/MOP notes and conformance tests  
**Non-goals:** implicit invocation of ordinary functions, implicit invocation of methods with default arguments, Ruby-style public methods named `name=`, Python-style decorators, automatic memoization, static type inference, module-level writable properties

---

## 0. Integration note

This patch introduces **computed property descriptors**.

A property descriptor is a named language-level member that may contain:

1. a getter arm, exposed through ordinary access syntax;
2. a setter arm, exposed through assignment syntax.

The patch preserves the following existing design decisions:

1. Ordinary callable values are invoked with `fn(args...)`.
2. Ordinary functions and methods declared with `def` are not implicitly called by bare identifier/member access.
3. `&target` creates an immutable callable reference object, not a raw machine address.
4. `Class(args...)` remains ordinary `HCall` / `CALL` over a callable class object and follows the constructor path.
5. Parser output remains syntax-faithful. A property declaration must not be erased into an ordinary method declaration at AST level.
6. HIR is the semantic-core representation and must lower property get/set operations explicitly, or into ordinary send/call semantics with preserved property markers.
7. Deterministic diagnostics, stack traces, disassembly and golden outputs must not expose raw memory addresses.
8. This patch does not add hidden side effects to bare ordinary identifiers.

Recommended insertion point: after the existing sections on functions/methods/classes and callable references, with cross-references from postfix member access, assignment semantics, HIR lowering, MOP/reflection and diagnostics.

---

## 1. Motivation

Amber already distinguishes callable values, callable references and callable invocation:

```amber
fn = &Math.answer
fn()
```

This patch does not weaken that distinction.

The missing ergonomic feature is a way to expose values that are computed, validated, normalized or synchronized through field-like syntax:

```amber
class User:
  prop full_name:
    "#{@first} #{@last}"

user.full_name
```

and writable property syntax:

```amber
class User:
  prop full_name:
    get:
      "#{@first} #{@last}"

    set(value):
      parts = value.split(" ", limit: 2)
      @first = parts[0]
      @last = parts[1]

user.full_name = "Ada Lovelace"
```

A property lets library authors change an implementation from stored state to computed state without changing call sites:

```amber
user.email
user.full_name
settings.cache_dir
clock.monotonic_time
```

This is intentionally different from implicit nullary function calls:

```amber
def f():
  42

f      # not f()
f()    # ordinary call
&f     # callable reference
```

Bare ordinary names remain value access, not hidden call sites.

---

## 2. Terminology

| Term | Meaning |
|---|---|
| Stored field | A value stored in an object slot or binding cell. |
| Property | A named descriptor exposed through access and/or assignment syntax. |
| Property descriptor | The runtime/MOP entity containing optional getter and setter arms. |
| Getter arm | Executable body used by property read. |
| Setter arm | Executable body used by property assignment. |
| Readable property | Property descriptor with a getter arm. |
| Writable property | Property descriptor with a setter arm. |
| Read-write property | Property descriptor with both getter and setter arms. |
| Read-only property | Property descriptor with getter but no setter. |
| Write-only property | Property descriptor with setter but no getter. Allowed only in object-body contexts unless a future module-property RFC says otherwise. |
| Ordinary method | A method declared with `def`, invoked with call syntax or ordinary send syntax. |
| Callable reference | A first-class callable object created by `&target`. |

Normative distinction:

```amber
def f():
  42

prop g:
  42

f       # ordinary binding access; not an implicit call
f()     # ordinary callable call
&f      # callable reference, when target is valid

g       # property get; evaluates the getter
```

For object members:

```amber
obj.g       # property get if `g` is a readable property
obj.g = x   # property set if `g` is a writable property
```

---

## 3. Surface syntax

### 3.1. Compact getter shorthand

A property declaration may use the compact getter shorthand:

```amber
prop answer:
  42

answer    # 42
```

One-liner form:

```amber
prop answer: 42
```

Normative rule:

> If the body of `prop name:` does not begin with a property arm label such as `get:` or `set(...)`, the whole property body is interpreted as the getter arm.

Therefore these two forms are equivalent:

```amber
prop answer:
  42
```

```amber
prop answer:
  get:
    42
```

The shorthand is not legacy syntax. It is the canonical compact form for read-only properties.

### 3.2. Grouped descriptor form

A property may be declared as a grouped descriptor:

```amber
prop name:
  get:
    getter_body

  set(value):
    setter_body
```

The grouped form is required when a setter arm is declared.

Valid grouped forms:

```amber
prop size:
  get:
    @items.length
```

```amber
prop size:
  set(value):
    resize_to(value)
```

```amber
prop size:
  get:
    @items.length

  set(value):
    resize_to(value)
```

### 3.3. Instance property

Inside a class body, `prop name:` declares an instance-side property descriptor:

```amber
class User:
  prop full_name:
    get:
      "#{@first} #{@last}"

    set(value):
      parts = value.split(" ", limit: 2)
      @first = parts[0]
      @last = parts[1]
```

The property bodies execute with the same receiver/self context as ordinary instance method bodies.

### 3.4. Class-side property

A class-side property is declared with `class_prop`:

```amber
class Build:
  class_prop version:
    get:
      "20.3"

Build.version    # "20.3"
```

Compact getter shorthand is also valid:

```amber
class Build:
  class_prop version:
    "20.3"
```

`class_prop` is intentionally separate from `class_method def`, mirroring the existing separation between instance methods and class methods.

### 3.5. Mixin property

A mixin may declare instance-side properties:

```amber
mixin Timestamped:
  prop age_seconds:
    Clock.now() - @created_at
```

When the mixin is included into a class, property lookup participates in the same ancestor/linearization model as ordinary instance methods, subject to the conflict rules in this patch.

Class-side properties inside mixins are not introduced by this patch. If a future Amber revision wants mixin-provided class-side properties, it should extend the existing `extend`/class-side composition rules explicitly.

### 3.6. Setter compact spelling not introduced

This patch does not introduce Ruby-style declarations such as:

```amber
prop f=(value):
  ...
```

Invalid:

```amber
prop f=(value):
  @f = value
```

The canonical setter spelling is the grouped descriptor arm:

```amber
prop f:
  set(value):
    @f = value
```

`f=` is not an ordinary identifier and this patch does not create public methods named `f=`.

---

## 4. Grammar additions

This patch adds `prop`, `class_prop`, `get` and `set` as contextual keywords in property declaration positions.

Reference grammar additions:

```ebnf
PropertyDef              ::= "prop" Identifier ":" PropertySuite
ClassPropertyDef         ::= "class_prop" Identifier ":" PropertySuite

PropertySuite            ::= CompactGetterSuite
                           | INDENT PropertyArm+ DEDENT

CompactGetterSuite       ::= Suite
                           | Expr

PropertyArm              ::= GetterArm
                           | SetterArm

GetterArm                ::= "get" ":" Suite
                           | "get" ":" Expr

SetterArm                ::= "set" "(" Identifier ")" ":" Suite
                           | "set" "(" Identifier ")" ":" Expr
```

Parser note:

- After `prop name:` / `class_prop name:`, the parser enters property-body mode.
- In property-body mode, an indented body whose first significant statement is `get:` or `set(identifier):` is parsed as grouped descriptor form.
- Otherwise the whole body is parsed as compact getter shorthand.
- `get` and `set` remain ordinary identifiers outside property arm-label position.

Invalid property headers:

```amber
prop scale(x):
  x * 2

prop scale(x = 10):
  x * 2

class User:
  prop full_name():
    "#{@first} #{@last}"
```

Invalid setter arms:

```amber
prop age:
  set():
    @age = null

prop age:
  set(value = 0):
    @age = value

prop age:
  set(old, new):
    @age = new
```

Required diagnostics are listed in section 13.

---

## 5. Getter semantics

### 5.1. Evaluation

A property get evaluates the getter arm each time the access is performed.

```amber
prop now:
  Clock.now()

a = now
b = now
# a and b may differ
```

Properties are not memoized by the language. A library may implement memoization explicitly inside the getter body.

### 5.2. Result

A getter body returns according to ordinary Amber body result rules. If it finishes without explicit `return`, it returns the current `$_`.

```amber
prop normalized_name:
  @name.strip()
  $_.downcase()
```

### 5.3. Arity

A getter arm declares no user-visible parameters.

Invalid:

```amber
prop scale:
  get(x):
    x * 2
```

Getter access is not a call expression and must not accept arguments:

```amber
obj.size       # property get
obj.size()     # call of property result only if `obj.size` first resolves to a callable value under ordinary expression rules; not a getter call syntax
```

Implementations must not reinterpret `obj.size()` as `obj.size` getter invocation with call punctuation. The property access happens first; any following `(...)` calls the resulting value.

---

## 6. Setter semantics

### 6.1. Assignment syntax

A property setter is invoked only through assignment syntax:

```amber
obj.f = value
```

For class-side properties:

```amber
Build.version = "20.4-dev"
```

The patch does not introduce ordinary method-call syntax for setters:

```amber
obj.f=(value)   # invalid unless a future revision adds methods named `f=`
```

### 6.2. Setter arity

A setter arm must declare exactly one required positional parameter.

Valid:

```amber
prop age:
  set(value):
    @age = value
```

Invalid:

```amber
prop age:
  set():
    @age = null

prop age:
  set(value = 0):
    @age = value

prop age:
  set(a, b):
    @age = b
```

Defaults, rest parameters, keyword parameters and block parameters are not allowed in setter arms in this patch.

### 6.3. Assignment result

A property assignment expression returns the original right-hand side value, not the setter body result.

```amber
class Box:
  prop value:
    get:
      @value

    set(v):
      @value = v
      :ok

box = Box()
x = (box.value = 10)
# x == 10, not :ok
```

This preserves the existing Amber assignment convention that assignment evaluates to the assigned value and updates `$_` with the assigned value.

### 6.4. Right-hand side evaluation

The right-hand side of a property assignment is evaluated exactly once.

```amber
obj.f = expensive()
```

Lowering must be observationally equivalent to:

```amber
$tmp = expensive()
PROPERTY_SET(obj, :f, $tmp)
$tmp
```

except that `$tmp` is not user-visible.

### 6.5. Receiver evaluation order

Property assignment evaluation order is left-to-right:

1. evaluate the receiver expression and any receiver chain needed to identify the assignment target;
2. evaluate the right-hand side exactly once;
3. invoke the setter arm;
4. return the right-hand side value.

Example:

```amber
get_user().profile.name = expensive()
```

Evaluation order:

1. `get_user()`;
2. `.profile` access;
3. `expensive()`;
4. `name` setter on the profile object;
5. result is the value returned by `expensive()`.

### 6.6. Setter failure

A setter signals invalid assignment by raising an exception.

```amber
prop age:
  set(value):
    if value < 0:
      raise ValueError("age must be non-negative")
    @age = value
```

Setter body return values are ignored for assignment-result purposes, but exceptions propagate normally.

---

## 7. Readability and writability

### 7.1. Read-only property

A property with a getter and no setter is readable but not assignable.

```amber
prop id:
  @id

obj.id       # ok
obj.id = 10  # error
```

Assignment to a read-only property raises or diagnoses `ReadOnlyPropertyError` / `E_PROP_MISSING_SETTER`, depending on whether the error is detected statically or dynamically.

### 7.2. Write-only property

A property with a setter and no getter is writable but not readable.

```amber
prop password:
  set(value):
    @password_hash = Password.hash(value)

user.password = "secret"  # ok
user.password             # error
```

Reading a write-only property raises or diagnoses `WriteOnlyPropertyError` / `E_PROP_MISSING_GETTER`.

Write-only properties are allowed in class and mixin object-body contexts. Top-level write-only properties are not introduced by this patch.

### 7.3. Read-write property

A property with both getter and setter is both readable and writable.

```amber
prop temperature_c:
  get:
    @temperature_c

  set(value):
    @temperature_c = Float(value)
```

---

## 8. Top-level and local properties

### 8.1. Top-level readable properties

Top-level readable properties are allowed:

```amber
prop version:
  "20.3"

version
```

They behave as computed bindings in their declaring module/scope.

### 8.2. Top-level writable properties

Top-level writable properties are not introduced by this patch.

Invalid:

```amber
prop version:
  set(value):
    @version = value
```

Rationale: bare assignment already has lexical binding semantics:

```amber
version = "20.4"
```

Allowing this form to mean either local binding assignment or module property setter invocation would complicate binder rules and create non-local assignment effects.

A future module-property RFC may introduce explicit syntax for writable module properties if needed.

### 8.3. Local properties

Local properties inside function/block bodies are not introduced by this patch.

Invalid:

```amber
def f():
  prop x:
    42
```

Rationale: local property declarations would require additional closure/capture and assignment-resolution rules. This patch limits properties to module/object declaration contexts.

---

## 9. Member lookup and conflicts

### 9.1. Property namespace

A property descriptor occupies the external member name `name`.

For v20.3, a class or effective ancestor composition must not expose an ordinary method, stored public field accessor and property descriptor with the same external name unless an existing Amber override rule explicitly allows one declaration to replace another in a deterministic order.

Recommended v20.3 rule:

> A property descriptor conflicts with ordinary methods and generated public field accessors of the same external name in the same effective owner. Implementations should emit a deterministic conflict diagnostic rather than choosing an implicit precedence order.

Invalid:

```amber
class User:
  prop name:
    @name

  def name():
    "other"
```

### 9.2. Getter/setter descriptor merging

Within the same owner, grouped arms of a single `prop name:` declaration form one descriptor.

Multiple separate property declarations with the same name in the same owner are invalid in v20.3:

```amber
class User:
  prop name:
    get:
      @name

  prop name:
    set(value):
      @name = value
# invalid in v20.3
```

Use the grouped form instead:

```amber
class User:
  prop name:
    get:
      @name

    set(value):
      @name = value
```

Rationale: single grouped declarations make descriptor identity, source spans, documentation and diagnostics deterministic.

### 9.3. Mixin conflicts

If two included mixins contribute a property descriptor or method/field/property conflict for the same external name, the existing mixin linearization/conflict policy applies if it is explicit enough to resolve the conflict.

If the current composition rules cannot deterministically resolve the conflict, the owner must receive a `PropertyNameConflict` diagnostic.

---

## 10. Callable references

### 10.1. Ordinary functions and methods

No change:

```amber
def f():
  42

f()    # call
&f     # callable reference, when target is valid
```

### 10.2. Property getter references

This patch does not introduce a new top-level getter-reference syntax.

Invalid:

```amber
prop answer:
  42

&answer   # invalid in v20.3
```

For instance-side properties, `&Class#name` may refer to a readable property getter only if the implementation represents readable properties as property-compatible unbound member references and the spec profile explicitly enables this behavior.

The conservative v20.3 baseline is:

```amber
&User#full_name   # valid only for ordinary methods in v20.1; property getter references are reserved for a future RFC
```

Recommended diagnostic if attempted on a property:

```text
E_PROP_GETTER_REFERENCE_UNSUPPORTED
cannot take callable reference to property getter `full_name`; use an explicit closure/adapter
```

### 10.3. Property setter references

This patch does not introduce setter callable references.

Invalid:

```amber
&User#full_name=
```

Rationale: supporting setter references would require extending callable reference target grammar to selector names containing `=`, which this patch intentionally avoids.

---

## 11. AST and HIR requirements

### 11.1. AST

Parser output must preserve property surface forms.

Recommended AST nodes:

```text
AstPropertyDef(
  name,
  side = instance | class | module,
  form = compact_getter | grouped_descriptor,
  getter_arm?,
  setter_arm?,
  span,
  name_span,
  arm_spans
)

AstPropertyGet(
  receiver?,
  name,
  span
)

AstPropertySet(
  receiver,
  name,
  value_expr,
  span
)
```

A parser may initially represent `obj.name` as ordinary member access and let binder resolve it as property get, but AST dumps must still preserve property declarations distinctly.

### 11.2. Binder

Binder responsibilities:

1. recognize property declarations in valid declaration contexts;
2. reject property declarations in local/function/block contexts;
3. reject top-level properties with setter arms;
4. validate setter arity;
5. validate absence of setter defaults/rest/keyword/block params;
6. build property descriptor entries in the owner member table;
7. detect property/method/field conflicts;
8. resolve property read/write targets where statically knowable;
9. preserve ordinary `def` call semantics.

### 11.3. HIR

Recommended HIR forms:

```text
HPropertyDef(
  owner,
  name,
  getter_body?,
  setter_param?,
  setter_body?,
  visibility,
  source_spans
)

HPropGet(receiver, name)
HPropSet(receiver, name, value)
```

Alternative lowering is permitted if observational behavior is identical:

```text
HSend(receiver, :name, [], surface_kind = property_get)
HSend(receiver, :name_set, [value], surface_kind = property_set)
```

However, deterministic HIR dumps must distinguish property get/set from ordinary method calls.

### 11.4. Property assignment lowering

Surface:

```amber
receiver.name = rhs
```

HIR-equivalent:

```text
r = eval(receiver)
v = eval(rhs)
HPropSet(r, :name, v)
result = v
```

The setter body result must not become the assignment expression result.

### 11.5. Bytecode and VM

A VM may implement properties through:

1. dedicated opcodes such as `GET_PROP` / `SET_PROP`;
2. ordinary `SEND`/`CALL` with property metadata;
3. descriptor objects in method tables.

Regardless of implementation strategy:

- property get/set participates in open-world invalidation;
- property descriptor lookup must respect world/freeze rules;
- inline caches must be invalidated by owner method/property version and shape/world epoch as applicable;
- stack traces should identify getter/setter frames as property access frames when possible;
- disassembly/golden output must be deterministic.

---

## 12. Reflection and MOP

This patch recommends, but does not require for P0, a reflective descriptor API:

```amber
User.property(:full_name)
```

Recommended descriptor fields:

| Field | Meaning |
|---|---|
| `name` | Property external name as `Symbol` or equivalent. |
| `owner` | Declaring class/module/mixin. |
| `readable?` | Whether a getter arm exists. |
| `writable?` | Whether a setter arm exists. |
| `source_span` | Optional deterministic source location. |
| `visibility` | Property visibility if/when Amber adds visibility controls. |

The descriptor API must not expose raw memory addresses or host-specific implementation pointers.

`define_method` is not extended to define properties in this patch. A future MOP extension may add:

```amber
define_property(:name, get: ..., set: ...)
```

but v20.3 core syntax does not require it.

---

## 13. Diagnostics

Required diagnostic categories:

| Code | Situation |
|---|---|
| `E_PROP_INVALID_CONTEXT` | `prop` used in a function/block/local context where properties are not allowed. |
| `E_PROP_INVALID_HEADER` | Property declaration has a parameter list or invalid header shape. |
| `E_PROP_EMPTY_DESCRIPTOR` | Grouped descriptor has neither getter nor setter arm. |
| `E_PROP_DUPLICATE_GETTER` | More than one getter arm in one descriptor. |
| `E_PROP_DUPLICATE_SETTER` | More than one setter arm in one descriptor. |
| `E_PROP_SETTER_ARITY` | Setter arm does not declare exactly one parameter. |
| `E_PROP_SETTER_DEFAULT` | Setter parameter has a default value. |
| `E_PROP_SETTER_PARAM_KIND` | Setter parameter is rest/keyword/block/destructuring instead of one plain identifier. |
| `E_PROP_TOP_LEVEL_SETTER` | Top-level/module property declares a setter arm. |
| `E_PROP_MISSING_GETTER` | Read from write-only property. |
| `E_PROP_MISSING_SETTER` | Assignment to read-only property. |
| `E_PROP_NAME_CONFLICT` | Property conflicts with method, field accessor or another property. |
| `E_PROP_ASSIGN_TARGET` | Left-hand side is not an assignable property target. |
| `E_PROP_SETTER_CALL_SYNTAX` | Ruby-style `obj.f=(x)` or `prop f=(x):` attempted. |
| `E_PROP_GETTER_REFERENCE_UNSUPPORTED` | Callable reference attempted for property getter where unsupported. |
| `E_PROP_SETTER_REFERENCE_UNSUPPORTED` | Callable reference attempted for property setter. |

Diagnostics must include stable source spans and deterministic messages suitable for golden tests.

Example diagnostics:

```text
E_PROP_SETTER_ARITY: property setter `age` must declare exactly one parameter
```

```text
E_PROP_MISSING_SETTER: cannot assign to read-only property `id`
```

```text
E_PROP_TOP_LEVEL_SETTER: top-level writable properties are not part of Amber v20.3
```

---

## 14. Compatibility

This patch is source-compatible with existing Amber code unless that code already uses `prop`, `class_prop`, `get` or `set` in the newly introduced declaration/arm-label positions.

`get` and `set` remain ordinary identifiers outside property arm-label position.

No existing ordinary function or method becomes implicitly callable through bare access.

Existing callable reference syntax remains unchanged:

```amber
&NameSpace.some_fn
&Class.method
&Class#method
```

Existing constructor-call syntax remains unchanged:

```amber
Point(10, 20)
```

---

## 15. Examples

### 15.1. Read-only computed property

```amber
class User:
  prop full_name:
    "#{@first} #{@last}"

user.full_name
```

Equivalent explicit getter form:

```amber
class User:
  prop full_name:
    get:
      "#{@first} #{@last}"
```

### 15.2. Read-write validating property

```amber
class Account:
  prop balance:
    get:
      @balance

    set(value):
      amount = Decimal(value)
      if amount < 0:
        raise ValueError("balance cannot be negative")
      @balance = amount
```

### 15.3. Write-only property

```amber
class User:
  prop password:
    set(value):
      @password_hash = Password.hash(value)

user.password = "secret"  # ok
user.password             # error: write-only property
```

### 15.4. Class-side property

```amber
class Build:
  class_prop version:
    "20.3"

Build.version
```

### 15.5. Assignment result

```amber
x = (account.balance = 100)
# x == 100
```

---

## 16. Conformance tests

Minimum positive tests:

```amber
prop answer:
  42

assert answer == 42
```

```amber
prop answer:
  get:
    42

assert answer == 42
```

```amber
class Box:
  prop value:
    get:
      @value

    set(v):
      @value = v
      :ignored

box = Box()
r = (box.value = 10)
assert r == 10
assert box.value == 10
```

```amber
class User:
  prop password:
    set(value):
      @password_hash = value.to_str()

user = User()
user.password = "secret"
```

```amber
class Build:
  class_prop version:
    "20.3"

assert Build.version == "20.3"
```

Minimum negative tests:

```amber
prop scale(x):
  x * 2
# E_PROP_INVALID_HEADER
```

```amber
prop age:
  set():
    pass
# E_PROP_SETTER_ARITY
```

```amber
prop age:
  set(value = 0):
    @age = value
# E_PROP_SETTER_DEFAULT
```

```amber
prop f=(value):
  pass
# E_PROP_SETTER_CALL_SYNTAX or E_PROP_INVALID_HEADER
```

```amber
def f():
  prop x:
    42
# E_PROP_INVALID_CONTEXT
```

```amber
prop version:
  set(value):
    pass
# E_PROP_TOP_LEVEL_SETTER
```

```amber
class User:
  prop name:
    @name

  def name():
    "other"
# E_PROP_NAME_CONFLICT
```

```amber
class Box:
  prop id:
    @id

box = Box()
box.id = 10
# E_PROP_MISSING_SETTER or runtime ReadOnlyPropertyError
```

---

## 17. Open extension points

The following are intentionally left for future RFCs:

1. getter/setter callable references;
2. module-level writable properties;
3. local properties;
4. `define_property` MOP APIs;
5. property visibility modifiers;
6. property annotations such as cached/lazy/observable;
7. class-side properties contributed by mixins through `extend`;
8. typed property declarations;
9. property override rules beyond the conservative conflict policy;
10. Ruby-compatible `name=` selector syntax.

---

## 18. Summary

Amber v20.3 adds property descriptors through:

```amber
prop f:
  get:
    ...

  set(value):
    ...
```

and keeps the compact read-only getter shorthand:

```amber
prop f:
  ...
```

The compact form is defined normatively as:

```amber
prop f:
  get:
    ...
```

The extension deliberately does not make ordinary `def` callable through bare access. Property get and property set are explicit descriptor semantics, with syntax-faithful AST, deterministic binder validation, explicit HIR lowering and assignment behavior that returns the original RHS value.
