# Amber RFC: Bare-call для nullary-методов и chained callable-call `expr.()`

**Статус:** принято как проектное решение  
**Область:** surface syntax, postfix expressions, properties, callable values, dispatch/lowering, diagnostics  
**Ключевые формы:** `obj.member`, `obj.member()`, `obj.member.()`, `obj.member.call()`, `obj.?.member`, `obj.?.member.?.()`

---

## 0. Краткое резюме

Amber принимает ограниченный **bare-call** для nullary-методов: member access вида `receiver.name` может выполнять implicit zero-argument send, если `name` резолвится в метод с синтаксически пустой сигнатурой.

Одновременно вводится postfix callable-call segment:

```amber
expr.()
expr.(arg1, arg2)
expr.?.()
expr.?.(arg1, arg2)
```

Эта форма означает: **вызвать callable-значение, полученное выражением слева**, не теряя чейнинг и не требуя группировки выражения скобками в начале цепочки.

Итоговая триада:

```amber
obj.member       # read/query: property get OR implicit nullary method send
obj.member()     # explicit method send to member `member`
obj.member.()    # callable-call of the value produced by `obj.member`
```

`&target` сохраняет статус callable-reference контекста и никогда не вызывает target:

```amber
&Namespace.fn
&Class.method
&Class#method
```

---

## 1. Проблематика

В Amber уже существует красивая и строгая парадигма разделения:

```amber
prop size:
 @items.length

def size():
 @items.length
```

`prop` даёт field-like доступ:

```amber
collection.size
```

`def` даёт method-call доступ:

```amber
collection.size()
```

На уровне формальной модели это чисто: property — descriptor с getter/setter-поведением; method — ordinary callable member, вызываемый через call/send syntax.

Но на уровне пользовательского API появляется неудобство:

1. Потребитель API должен помнить, где член является `prop`, а где `def`.
2. Рефакторинг `prop -> def` или `def -> prop` меняет call sites.
3. Query-like методы вроде `size`, `empty?`, `present?`, `version`, `name`, `path`, `schema` выглядят тяжелее, чем должны.
4. В цепочках разница между `x.size` и `x.size()` создаёт шум, хотя семантически оба часто означают «получить значение».

Особенно проблемны API, где реализация естественно мигрирует между stored/computed value:

```amber
user.full_name
collection.size
settings.cache_dir
Build.version
```

Сегодня автор библиотеки должен выбрать surface shape заранее. Поздняя смена `prop` на `def` или обратно становится source-breaking даже если наблюдаемое значение не меняется.

---

## 2. Мотивация

### 2.1. Стабильность публичного API

Публичный API должен выражать семантическую роль члена, а не внутреннюю декларационную технику.

Если член концептуально является value/query, call site должен иметь устойчивую форму:

```amber
collection.size
user.full_name
Build.version
```

Внутренняя реализация может быть property descriptor:

```amber
class User:
 prop full_name:
  "#{@first} #{@last}"
```

или nullary method:

```amber
class User:
 def full_name():
  "#{@first} #{@last}"
```

Обе реализации должны быть refactor-compatible для read/query use case.

### 2.2. Ruby-like ergonomics без потери callable model

Amber уже ориентирован на Ruby-like object model, chaining, blocks и query-method suffixes `?` / `!`. Для такого языка естественна форма:

```amber
if users.empty?:
 render_empty_state()
```

Она читается лучше, чем:

```amber
if users.empty?():
 render_empty_state()
```

При этом Amber сохраняет explicit callable references через `&target`, поэтому можно отделить:

```amber
users.empty?     # invoke/read query
&Array#empty?    # reference to method, no invocation
```

### 2.3. Чистое различение invoke member vs invoke result

Главная опасность bare-call — callable-returning members.

Например:

```amber
factory.provider
```

может вернуть callable object или closure. Тогда нужен удобный способ вызвать это значение в chain без записи:

```amber
(factory.provider)()
```

Такая запись ломает локальность редактирования: чтобы добавить вызов, нужно возвращать каретку к началу выражения и ставить группирующие скобки.

Поэтому вводится Elixir-style postfix callable-call:

```amber
factory.provider.()
```

Он означает именно:

```amber
(factory.provider)()
```

но сохраняет chain-local editing.

---

## 3. Нормативное решение

### 3.1. Bare-call для nullary member methods

Member access:

```amber
receiver.name
```

разрешает implicit invocation, если `name` резолвится в nullary method.

```amber
class Collection:
 def size():
  @items.length

collection.size    # implicit nullary send: collection.size()
collection.size()  # explicit nullary send
```

Nullary method — это метод с **синтаксически пустой сигнатурой**:

```amber
def name():
 ...
```

Методы с optional/default/rest/keyword/block параметрами не считаются bare-callable, даже если их можно вызвать без аргументов:

```amber
def format(mode = :short):
 ...

value.format       # invalid or ordinary member resolution failure; not implicit call
value.format()     # valid explicit call
```

Причина: bare-call должен означать field-like query, а не скрытый вызов операции с параметризуемой сигнатурой.

### 3.2. Explicit method call остаётся explicit method call

Форма:

```amber
receiver.name()
```

всегда означает explicit method send to selector `:name`.

Она не должна означать call of returned property value.

Если `name` является property, то:

```amber
receiver.name()
```

должно давать diagnostic:

```text
E_PROPERTY_CALLED_AS_METHOD
property `name` is not a method; use `receiver.name` or `receiver.name.()` if the property value is callable
```

Для вызова значения, возвращённого property или implicit-nullary member, используется:

```amber
receiver.name.()
```

### 3.3. Chained callable-call `expr.()`

Вводится postfix segment:

```amber
expr.()
expr.(arg1, arg2)
expr.(keyword: value)
expr.(*args, **kwargs)
```

Семантика:

```amber
expr.(args...)
```

эквивалентна:

```amber
(expr)(args...)
```

но является chain-preserving postfix form.

Пример:

```amber
factory.provider.().configure().start()
```

означает:

```amber
(factory.provider)().configure().start()
```

### 3.4. Safe callable-call `expr.?.()`

Вводится safe variant:

```amber
expr.?.()
expr.?.(arg1, arg2)
```

Семантика:

```amber
expr.?.(args...)
```

если `expr == null`, результат `null`; иначе вызывается callable value:

```amber
tmp = expr
if tmp == null:
 null
else:
 tmp(args...)
```

Пример:

```amber
factory.?.provider.?.().configure()
```

Здесь:

1. `factory.?.provider` безопасно читает/вызывает query-member `provider`;
2. `.?.()` безопасно вызывает полученный callable, если он не `null`;
3. дальнейшая цепочка продолжается от результата.

### 3.5. `.call()` остаётся ordinary method send

Форма:

```amber
expr.call()
```

не является специальным синтаксисом. Это обычный method send selector `:call`.

Это важно, потому что:

```amber
factory.provider.call()
```

значит:

1. вычислить `factory.provider`;
2. отправить результату метод `call()`.

А:

```amber
factory.provider.()
```

значит:

1. вычислить `factory.provider`;
2. вызвать результат через общий callable protocol / `HCall`.

Для ordinary callable objects эти формы могут быть наблюдаемо эквивалентны, но lowering различается:

```text
expr.call()  -> HSend(expr, :call, [])
expr.()      -> HCall(expr, [])
```

### 3.6. Callable references не меняются

`&target` — отдельный syntactic reference context. Он не производит invocation, даже если target является nullary method.

```amber
Build.version       # property get OR implicit nullary class-side send
&Build.version      # callable reference to class-side method, no invocation
&Build#version      # unbound instance method reference, no invocation
```

Если target является property getter/setter, callable reference для property остаётся отдельной будущей темой и не появляется автоматически в рамках этого решения.

### 3.7. `prop` сохраняет самостоятельную роль

После принятия bare-nullary `prop` перестаёт быть единственным способом получить field-like read syntax, но не становится ненужным.

`prop` нужен для:

1. assignment syntax;
2. read-write descriptors;
3. write-only descriptors;
4. validation/normalization on assignment;
5. future property metadata;
6. cached/lazy/observable properties;
7. property-specific reflection;
8. stable descriptor-level MOP.

Пример:

```amber
class Account:
 prop balance:
  get:
   @balance

  set(value):
   amount = Decimal(value)
   if amount < 0:
    raise ValueError("negative balance")
   @balance = amount

account.balance = 100
account.balance
```

Nullary `def` не становится assignable:

```amber
class Account:
 def balance():
  @balance

account.balance = 100  # invalid; no property setter
```

---

## 4. Surface syntax table

| Form | Meaning | Lowering intuition |
|---|---|---|
| `obj.member` | property get OR implicit nullary method send OR ordinary readable member | `HPropGet` / `HSend0Implicit` / member read |
| `obj.member()` | explicit method send | `HSend(obj, :member, [])` |
| `obj.member(arg)` | explicit method send with arguments | `HSend(obj, :member, [arg])` |
| `obj.member.()` | call value produced by `obj.member` | `HCall(HMemberReadOrImplicitSend(obj, :member), [])` |
| `obj.member.(arg)` | call value produced by `obj.member` with args | `HCall(..., [arg])` |
| `obj.member.call()` | ordinary send `:call` to value produced by `obj.member` | `HSend(..., :call, [])` |
| `&Class#member` | unbound instance method reference | `HUnboundMethodRef(Class, :member)` |
| `&Class.member` | class-side callable reference | `HCallableRef(...)` |
| `obj.?.member` | safe property get OR safe implicit nullary send | `HSafePropGet` / `HSafeSend0Implicit` |
| `obj.?.member()` | safe explicit method send | `HSafeSend(obj, :member, [])` |
| `obj.member.?.()` | safe call of returned callable value | `HSafeCall(...)` |

---

## 5. Parsing and grammar notes

### 5.1. New postfix segment

The postfix grammar receives one additional segment family:

```ebnf
PostfixSegment ::=
    "." Identifier CallArgs?
  | "." "(" ArgList? ")"
  | ".?." Identifier CallArgs?
  | ".?." "(" ArgList? ")"
  | "[" Expr "]"
  | ...
```

Examples:

```amber
expr.()
expr.(x, y)
expr.?.()
expr.?.(x, y)
```

The form is unambiguous because after `.` the parser sees `(` rather than an identifier.

### 5.2. `obj.member()` vs `obj.member.()`

The parser must preserve the distinction syntax-faithfully.

```amber
obj.member()
```

is a method-call segment.

```amber
obj.member.()
```

is a member-read/implicit-send segment followed by callable-call segment.

The AST must not erase this distinction.

---

## 6. Resolution model

### 6.1. Member read / implicit nullary send

For:

```amber
receiver.name
```

resolution proceeds conceptually as:

1. If `name` resolves to a readable property, perform property get.
2. Else if `name` resolves to a syntactically nullary method, perform implicit zero-argument send.
3. Else if `name` resolves to another readable member kind supported by the object model, perform ordinary read.
4. Else use static diagnostic or dynamic missing-member/method path depending on receiver knowledge.

Property/method conflicts for the same selector remain forbidden by conservative conflict policy.

### 6.2. Explicit method call

For:

```amber
receiver.name(args...)
```

resolution must target a method/sendable selector `:name`.

If the selected member is a property, diagnostic is preferred over call-of-property-result.

### 6.3. Callable-call segment

For:

```amber
expr.(args...)
```

resolution first evaluates `expr`, then checks the resulting value against the callable protocol.

If the value is not callable, runtime raises:

```text
TypeError / E_NOT_CALLABLE
```

Static implementations may reject known-non-callable expressions earlier.

### 6.4. Dynamic dispatch and `method_missing`

Implicit nullary send participates in normal dispatch semantics.

For dynamic receivers:

```amber
obj.foo
```

if `foo` is not found as a property/readable member but may be a method, the dynamic path may perform zero-argument send and therefore may trigger:

```amber
method_missing(:foo)
```

This is intentional for DSL and open-world compatibility, but static receivers should prefer early diagnostics when a member is known not to exist.

---

## 7. HIR / lowering

Recommended HIR additions or conventions:

```text
HMemberRead(receiver, selector)
HPropGet(receiver, selector)
HSend0Implicit(receiver, selector)
HSafeSend0Implicit(receiver, selector)
HCall(callable, pos_args[], kw_args[], block?)
HSafeCall(callable, pos_args[], kw_args[], block?)
```

Alternatively, `HMemberRead` may be binder-resolved into existing `HSend`/`HCall`/property nodes while preserving enough metadata for diagnostics and disassembly.

Canonical lowerings:

```amber
obj.size
```

```text
HSend0Implicit(obj, :size)
```

if `size` is a nullary method.

```amber
obj.size()
```

```text
HSend(obj, :size, [], {}, null)
```

```amber
obj.size.()
```

```text
tmp = HMemberReadOrImplicitSend(obj, :size)
HCall(tmp, [], {}, null)
```

```amber
obj.size.call()
```

```text
tmp = HMemberReadOrImplicitSend(obj, :size)
HSend(tmp, :call, [], {}, null)
```

Safe lowering:

```amber
obj.?.provider.?.()
```

```text
tmp1 = null_guard(obj) ? null : HMemberReadOrImplicitSend(obj, :provider)
tmp2 = tmp1 == null ? null : HCall(tmp1, [], {}, null)
```

---

## 8. Native/frozen performance model

This feature does not require a slow path when type information is available.

### 8.1. Static/native path

If receiver type is known and the member resolves to a nullary method, compiler can lower:

```amber
collection.size
```

to the same direct call as:

```amber
collection.size()
```

Possible optimizations:

1. direct method entry call;
2. monomorphic inline cache;
3. devirtualized call in frozen-world profile;
4. inlining if method body is known and safe;
5. intrinsic lowering for stdlib primitives such as collection size.

### 8.2. Dynamic/open-world path

If receiver type is unknown, implicit nullary send is an ordinary dynamic send:

```text
SEND0_IMPLICIT receiver, :size
```

This is not meaningfully slower than:

```text
SEND receiver, :size, argc=0
```

and can share inline-cache infrastructure.

### 8.3. World mutation and invalidation

Because implicit nullary sends depend on method/property tables, world mutations that add/remove/replace methods or properties must invalidate affected caches.

This is not a new category of invalidation; it is the same dispatch-relevant mutation already needed for ordinary method sends, `method_missing`, properties, open classes and mixins.

---

## 9. Diagnostics

### 9.1. Required diagnostics

```text
E_BARE_NON_NULLARY_METHOD
method `name` requires arguments; call it with explicit arguments or take a callable reference explicitly
```

```text
E_PROPERTY_CALLED_AS_METHOD
property `name` is not a method; use `obj.name` or `obj.name.()` if the property value is callable
```

```text
E_AMBIGUOUS_MEMBER_KIND
member `name` cannot be both property and method in the same lookup surface
```

```text
E_NOT_CALLABLE
left side of `.()` is not callable
```

```text
E_DOT_CALL_TARGET_REQUIRED
`.()` must follow an expression; it cannot start an expression
```

### 9.2. Recommended warnings

```text
W_BARE_BANG_CALL
bare call of mutating-looking method `clear!`; prefer `clear!()`
```

```text
W_EXPENSIVE_BARE_CALL
method marked expensive/io/async should be called with explicit parentheses
```

`W_EXPENSIVE_BARE_CALL` depends on future effect/cost annotations and is not required for v1.

### 9.3. Good fix-it suggestions

For:

```amber
factory.provider()
```

when `provider` is a property returning callable:

```text
Use `factory.provider.()` to call the property value.
```

For:

```amber
obj.format
```

when `format` has default parameters:

```text
Method `format` is not bare-callable because its signature is not syntactically nullary. Use `obj.format()`.
```

For:

```amber
cache.clear!
```

if warnings are enabled:

```text
Prefer `cache.clear!()` for mutating-looking methods.
```

---

## 10. Examples

### 10.1. Refactoring property to method

Before:

```amber
class Collection:
 prop size:
  @items.length

collection.size
```

After:

```amber
class Collection:
 def size():
  @items.length

collection.size
```

Call sites do not change.

### 10.2. Explicit method call still works

```amber
collection.size
collection.size()
```

Both are valid when `size` is nullary method.

The first is query/read syntax; the second is explicit invocation syntax.

### 10.3. Callable-returning property

```amber
class Factory:
 prop provider:
  | |: Service()

factory.provider       # returns callable
factory.provider.()    # calls returned callable
factory.provider()     # diagnostic: property is not a method
```

### 10.4. Callable-returning nullary method

```amber
class Factory:
 def provider():
  | |: Service()

factory.provider       # implicit call provider(), returns callable
factory.provider()     # explicit call provider(), returns callable
factory.provider.()    # implicit call provider(), then call returned callable
```

### 10.5. Ordinary `.call()`

```amber
factory.provider.call()
```

This sends `:call` to the value returned by `factory.provider`.

```amber
factory.provider.()
```

This invokes the value returned by `factory.provider` through the generic callable protocol.

### 10.6. Safe chain

```amber
service.?.factory.?.provider.?.().start()
```

Reading left to right:

1. safely read/call `factory` from `service`;
2. safely read/call `provider` from the factory;
3. safely call the returned callable provider;
4. call `start()` on the produced service.

---

## 11. Pros

### 11.1. Better API refactoring

The biggest benefit is that public query-like API no longer exposes whether implementation is `prop` or nullary `def`.

```amber
user.name
user.full_name
collection.size
```

can survive internal rewrites between descriptor and method forms.

### 11.2. Cleaner query syntax

Methods ending in `?` become visually natural:

```amber
if users.empty?:
 ...
```

This improves readability for predicates and cheap query methods.

### 11.3. Better chaining ergonomics

`expr.()` avoids disruptive grouping:

```amber
factory.provider.().configure().start()
```

instead of:

```amber
(factory.provider)().configure().start()
```

### 11.4. Callable references stay explicit

Because `&target` remains a separate context, bare-call does not steal the ability to refer to methods:

```amber
obj.size       # invoke/read
&Class#size    # reference
```

### 11.5. Performance model is straightforward

For statically known receivers, implicit nullary sends can lower to the same code as explicit zero-argument sends.

For dynamic receivers, the operation is an ordinary zero-argument send with inline-cache support.

### 11.6. Strong distinction between member-call and result-call

The accepted syntax gives a simple rule:

```amber
x.y()   # call member y
x.y.()  # call result of x.y
```

This is more precise than allowing `x.y()` to sometimes mean “call property result”.

---

## 12. Cons

### 12.1. Bare access can run user code

After this change:

```amber
obj.name
```

may execute code.

That code may allocate, compute, dispatch dynamically or trigger `method_missing`.

This is already true for properties, but the feature expands the surface where it can happen.

### 12.2. More semantic weight on naming conventions

The language cannot know whether a nullary method is cheap and pure.

Bad APIs may expose expensive or effectful methods as bare-callable:

```amber
socket.read
random.next
cache.clear!
```

The language should rely on style, lint and possibly future effect annotations.

### 12.3. `prop` vs `def` distinction becomes less visible at use sites

This is the point of the feature, but it also hides assignability and descriptor semantics.

A reader cannot tell from:

```amber
account.balance
```

whether `balance` is a property or nullary method.

If assignment exists:

```amber
account.balance = 10
```

then it must be property-backed. A nullary method alone is not assignable.

### 12.4. Parser and HIR gain one new postfix form

`expr.()` is simple but still a new form. It must be preserved through AST and diagnostics.

### 12.5. Dynamic `method_missing` gets broader reach

In dynamic contexts, `obj.foo` may now attempt zero-arg method dispatch and reach `method_missing(:foo)`.

This benefits DSLs but can make typos more dynamic unless static diagnostics or linting catch them.

---

## 13. Tradeoffs

### 13.1. Why not keep `obj.prop()` as call-result?

Because after bare-nullary it becomes ambiguous and misleading.

If `obj.name()` sometimes calls method `name`, sometimes calls result of property `name`, and sometimes competes with implicit nullary send, users cannot reason locally.

The accepted rule is sharper:

```amber
obj.name()   # method call
obj.name.()  # result call
```

### 13.2. Why not use only `.call()`?

`.call()` is useful and remains available, but it is ordinary method dispatch.

`expr.()` is generic callable invocation:

```text
expr.()     -> HCall(expr, [])
expr.call() -> HSend(expr, :call, [])
```

This matters for closures, native callable references, class objects and any value callable through the runtime callable protocol without exposing ordinary public method `call`.

### 13.3. Why only syntactically nullary methods?

Because this keeps bare-call from becoming hidden argument binding.

Allowed:

```amber
def size():
 ...

obj.size
```

Not allowed:

```amber
def size(scale = 1):
 ...

obj.size  # not bare-callable
```

A method with defaults is still an operation with a parameterized contract. It should require explicit call syntax.

### 13.4. Why allow explicit `obj.size()` too?

Because explicitness is sometimes useful:

1. to communicate that work is being done;
2. to avoid style debates in effectful code;
3. to preserve compatibility with users who prefer method-call shape;
4. to make `!` methods visually active.

Bare-call is accepted as ergonomic read/query syntax, not as a ban on parentheses.

### 13.5. Why not make top-level `f` call `f()`?

This RFC is about member/query access. Top-level/local bare identifiers should remain ordinary binding reads unless separately changed by another RFC.

Reason: local variables and functions share lexical space more directly than object members. Making `f` call `f()` would create larger ambiguity around first-class functions, closures and local binding shadowing.

---

## 14. Style guidance

Recommended bare-call style:

```amber
collection.size
collection.empty?
user.full_name
Build.version
node.parent
path.dirname
```

Recommended explicit-call style:

```amber
cache.clear!()
user.save!()
db.connect()
socket.read()
random.next()
clock.now()
```

Borderline cases should prefer explicit `()` when the operation is:

1. mutating;
2. effectful;
3. expensive;
4. time-varying;
5. surprising as a field-like read;
6. semantically an action rather than a query.

`?` query methods are good candidates for bare-call if they are cheap and side-effect-free:

```amber
users.empty?
config.valid?
connection.open?
```

`!` methods should generally use explicit `()`:

```amber
cache.clear!()
record.save!()
```

---

## 15. Compatibility impact

This is a compatibility-affecting change relative to the previous property-only model.

### 15.1. Source behavior changes

Previously:

```amber
obj.name
```

would not call ordinary method `name`.

After this RFC, it may call `name()` if the method is syntactically nullary.

### 15.2. Property call-result change

Previously, a design could allow:

```amber
obj.prop()
```

to mean call of result returned by `obj.prop`.

This RFC rejects that interpretation and reserves:

```amber
obj.prop()
```

for method call syntax only.

Call-result must be written:

```amber
obj.prop.()
```

or, if grouping is preferred:

```amber
(obj.prop)()
```

### 15.3. Migration assistance

Compiler should provide targeted fix-its:

```amber
obj.prop()
```

if `prop` is known property returning callable:

```amber
obj.prop.()
```

if user wanted method call but `prop` is property:

```amber
# define `def prop():` or call an actual method
```

---

## 16. Conformance tests

### 16.1. Positive tests

```amber
class Box:
 def size():
  10

box = Box()
assert box.size == 10
assert box.size() == 10
```

```amber
class Box:
 prop provider:
  | |: 42

box = Box()
assert box.provider.() == 42
```

```amber
class Box:
 def provider():
  | |: 42

box = Box()
assert box.provider.() == 42
assert box.provider().() == 42
```

```amber
class Box:
 prop value:
  10

box = Box()
assert box.value == 10
```

```amber
class Build:
 class_method def version():
  "1.0"

assert Build.version == "1.0"
assert Build.version() == "1.0"
```

```amber
maybe_provider = null
assert maybe_provider.?.() == null
```

### 16.2. Negative tests

```amber
class Box:
 def format(mode = :short):
  "x"

box = Box()
box.format
### E_BARE_NON_NULLARY_METHOD or equivalent diagnostic
```

```amber
class Box:
 prop provider:
  | |: 42

box = Box()
box.provider()
### E_PROPERTY_CALLED_AS_METHOD
```

```amber
x = 10
x.()
### E_NOT_CALLABLE
```

```amber
.()
### E_DOT_CALL_TARGET_REQUIRED
```

```amber
class Box:
 prop name:
  "a"

 def name():
  "b"
### E_AMBIGUOUS_MEMBER_KIND / E_PROP_NAME_CONFLICT
```

---

## 17. Open questions / future RFCs

1. Whether instance-bound method reference should receive a dedicated spelling such as `obj.&method`.
2. Whether property getter/setter callable references should become first-class and what spelling they should use.
3. Whether effect annotations should influence bare-call diagnostics.
4. Whether formatter should normalize query-like explicit calls `obj.empty?()` to `obj.empty?`.
5. Whether top-level nullary functions should ever receive bare-call syntax. This RFC says no.
6. Whether `!` bare-call should be warning-only or forbidden in strict profiles.
7. Whether `expr.()` should support block suffix directly:

```amber
provider.() |x|:
 ...
```

or only ordinary call arguments in v1.

---

## 18. Final accepted rule

Amber adopts this rule set:

```amber
obj.member       # property get OR implicit nullary method send
obj.member()     # explicit method send only
obj.member.()    # call returned callable value
obj.member.call()# ordinary `call` method send to returned value
```

Safe variants:

```amber
obj.?.member
obj.?.member()
obj.?.member.?.()
```

Callable references:

```amber
&Namespace.fn
&Class.method
&Class#method
```

never perform actual invocation.

`prop` remains the descriptor mechanism for assignability, validation, getter/setter behavior, property reflection and future descriptor-level features.
