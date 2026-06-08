# Amber — единая финальная спецификация языка и runtime-проектирования

**Назначение документа:** единый Markdown-файл, в котором собраны и структурированы язык Amber, стандартная библиотека, runtime-facing API и компилируемый проектный слой.

**Редакторская политика этой редакции:** текст представлен как единая финальная редакция. Исторические ссылки на отдельные патчи, их номера и последовательность появления удалены. Сохранены мотивации, дизайн-принципы, нормативные решения, diagnostics, lowering notes, conformance expectations и implementation contracts, когда они описывают смысл или наблюдаемое поведение языка.

**Граница документа:** первые части описывают surface language и семантику; следующие части фиксируют stdlib/runtime-facing слой и инженерные контракты reference implementation. Все разделы ниже следует читать как согласованное состояние Amber, а не как журнал изменений.

---

## Оглавление

- [Часть I. Языковая спецификация Amber](#часть-i-языковая-спецификация-amber)
- [Часть II. Интегрированные языковые решения](#часть-ii-интегрированные-языковые-решения)
- [Часть III. Standard library и runtime-facing API](#часть-iii-standard-library-и-runtime-facing-api)
- [Часть IV. Компилируемый Amber: implementation blueprint](#часть-iv-компилируемый-amber-implementation-blueprint)

---
# Часть I. Языковая спецификация Amber

## 1. Дизайн-якоря языка

Amber — это язык с такими базовыми обязательствами:

- от Python берутся отступы как способ задавать блоки и отказ от `end`;
- от Ruby берутся объектная модель, сигилы полей `@` и `@@`, чейнинг методов, блоки как основной способ передавать замыкания, именование методов с суффиксами `?` и `!`, а также ориентация на метапрограммирование;
- управляющие конструкции являются выражениями;
- pattern matching — часть ядра, а не библиотечный сахар;
- методы не требуют явного первого параметра `self` или `cls`;
- callable-значения являются first-class: `&target` создаёт callable reference, `fn(args...)` является каноническим вызовом callable, а class object может вызываться как constructor `Class(args...)`;
- spread expansion является синтаксическим механизмом сборки аргументов и коллекций, а не общим prefix-оператором;
- базовая коллекционная модель — `map/select/reduce/...` как методы, а не питоновские внешние `map/filter`.

## 2. Лексика, идентификаторы и блоки

### 2.1. Отступы и блоки

Блок открывается двоеточием `:` и продолжается отступом, как в Python.

Это относится к:

- `if / else if / elif / elsif / else`;
- `case` / `case!`;
- `while / until / do... while / loop`;
- `def`;
- `class`;
- `mixin`;
- `class_method def`;
- блокам, привязанным к вызовам (block suffix).

Пустой блок без тела запрещён. Для намеренно пустого тела используются `pass` или `noop`.

```amber
def todo():
 pass

if feature_enabled?:
 noop
else:
 log "disabled"
```

### 2.2. Идентификаторы

Базовые правила:

- имена методов, функций, локальных переменных, полей и констант являются UTF-8 identifier tokens;
- базовый профиль идентификаторов v1 разрешает ASCII-буквы, `_`, ASCII-цифры не в первой позиции, а также греческие и кириллические буквы в первой и последующих позициях;
- суффиксы `?` и `!` разрешены только в конце идентификатора;
- никакой магии двойных подчёркиваний в обычных именах нет;
- exact-token `_` в pattern-контексте означает wildcard и не создаёт биндинг;
- `$_` — служебная read-only переменная «результат последнего выражения» в текущем scope;
- `_1`, `_2`,... — это нумерованные аргументы блока только в блоках без `|...|`;
- обычные имена, начинающиеся с `_`, включая `_tmp` и `__cache`, считаются обычными идентификаторами, если не попадают под спец-формы `_`, `$_`, `_N`.

Примеры валидных имён:

```amber
active?
clear!
empty?
present?
_tmp
__cache
масса
коэффициент
α
β2
скорость!
```

Примеры невалидного использования:

```amber
foo?bar # ? не внутри имени
bang!name # ! не внутри имени
```

## 3. Общая модель выражений и операторов

### 3.1. Выражения

Amber ориентирован на выражения. В частности, выражениями являются:

- литералы;
- вызовы, чейнинг, индексация;
- `if`;
- `case` / `case!`;
- циклы;
- `class`, `mixin` и `def` (с оговорённой ниже семантикой результата).

#### 3.1.1. Литералы коллекций

Нормативные литералы коллекций v1:

```amber
[expr1, expr2] # Array
(expr1, expr2) # Tuple, только если внутри скобок есть запятая
{expr1, expr2} # Set, если элементы не являются key/value парами
{key: value} # Map с value-key + name-indifferent семантикой
Map{key: value} # explicit ordinary ordered Map
Set{expr1, expr2} # explicit ordered Set
StrictMap{key: value} # exact-key ordered Map
```

`{}` в expression-контексте остаётся пустым `Map`. Непустая форма `{expr}`
является одноэлементным `Set`, если содержимое верхнего уровня не разбирается
как map-entry. Элементы `Array`/`Set` и значения `Map` вычисляются слева
направо. Повторные ключи `Map` заменяют предыдущие значения, а повторные
элементы `Set` схлопываются по runtime-семантике равенства.

Начиная с `Map` key может быть:

- legacy identifier key: `{name: value}` означает `{:name: value}`;
- explicit symbol key: `{:name: value}`;
- string/scalar literal key: `{"name": value, 1: value, true: value}`;
- expression key в скобках: `{(name): value}`, `{(user.id): value}`;
- structural key из tuple/list/array, где mutable list/array key нормализуется в immutable tuple snapshot.

В ordinary `Map` / `HashMap` нормализуют `Symbol(:name)` и `Str("name")` в один name key. Поэтому `{name: 1}["name"]`, `{"name": 1}[:name]`, `{name: 1}[:name]` и `{"name": 1}["name"]` обращаются к одной entry. Дубликаты name keys схлопываются по нормализованному name key и сохраняют первую позицию:

```amber
m = {name: 1, "name": 2}
m[:name] # 2
m["name"] # 2
m.keys() # ["name"]
```

Exact `Symbol` / `Str` separation требует explicit strict container:

```amber
m = StrictMap{name: 1, "name": 2}
m[:name] # 1
m["name"] # 2
m.keys() # [:name, "name"]
```

Non-name keys продолжают использовать value-key equality. При `{1: "int", 1.0: "float"}` stored key остаётся первым (`1`), а значение становится `"float"`. `NaN` запрещён как `Map` key и `Set` element. `Map` / `Set` остаются ordered-vector коллекциями в reference profile; `HashMap{...}` и `HashSet{...}` допустимы как parser-level explicit constructor spelling для совместимости с drafts. `StrictHashMap{...}` является strict exact-key spelling; hash-backed storage не требуется в P0/P1 reference implementation.

#### 3.1.2. Spread expansion в коллекциях

`*` и `**` являются contextual spread markers. Они не являются обычными prefix operators и невалидны вне call arguments и collection literals.

```amber
[1, *items, 9]
{1, *items, 9} # Set spread
{a: 1, **other, b: 2} # Map spread
```

Spread вычисляется слева направо. Conditional collection syntax применяется и к spread entries:

```amber
[*extra if enabled?, fallback]
{**opts unless locked?, mode::fast}
```

Если condition falsy для `if` или truthy для `unless`, spread operand не вычисляется. Spread eager; open-ended/infinite collections не могут быть spread operands и дают `InfiniteCollectionError`.

Positional collection spread принимает `Array`, `Tuple` и finite `Range`. Core spread не вызывает пользовательские `to_array()` / `to_tuple()` методы. Map spread `**` принимает keyword-spreadable maps/views по rules: keys должны быть keyword-convertible.

### 3.2. Truthiness

Ложными считаются только:

- `false`;
- `null`.

Всё остальное truthy, включая:

- `0`;
- `""`;
- `[]`;
- `{}`.

### 3.3. Логические операторы

Поддерживаются:

- `not`
- `and`
- `or`

Приоритет:

1. `not`
2. `and`
3. `or`

`and` и `or` работают по short-circuit-семантике и возвращают операнды, а не обязательно `Bool`.

```amber
a and b
a or b
not a
```

Семантика:

- `A and B` возвращает `A`, если `A` falsy, иначе возвращает `B`;
- `A or B` возвращает `A`, если `A` truthy, иначе возвращает `B`;
- `not A` возвращает булев результат.

### 3.4. Оператор принадлежности `in`

`in` — это infix-оператор в выражениях.

```amber
elem in container
```

Приоритет `in` — на уровне сравнений: ниже арифметики, выше `and/or`.

```amber
x + 1 in xs and ok
# читается как:
# ((x + 1) in xs) and ok
```

Минимальный контракт:

- если у правого операнда есть `contains?(lhs)` — вызывается он;
- иначе — `TypeError`.

Рекомендуемая семантика стандартной библиотеки:

- `Array / Tuple / Set` — проверка наличия элемента;
- `Map` — проверка ключа;
- `Str` — проверка подстроки;
- `Range` — попадание в диапазон.

### 3.5. Presence-операции поверх Ruby-like truthiness

Так как `0`, `""` и пустые коллекции truthy, в языке отдельно фиксируются value-presence операции:

- `x ?? default` — null-coalescing, только для `null`;
- `value.presence()` — вернёт `null`, если значение «blank», иначе само значение;
- `value.nonempty()` — вернёт `null`, если значение пустое;
- `value.nonzero()` — вернёт `null`, если значение равно нулю.

```amber
name = input.strip().presence() or "anon"
limit = cfg.limit.nonzero() or 100
items = arr.nonempty() or default_items
```

## 4. Postfix-выражения, чейнинг и block suffix

### 4.1. Базовые postfix-операции

В call/postfix-зоне выражений поддерживаются:

- доступ к члену;
- вызов метода;
- вызов callable-объекта через `callee(args...)`;
- создание callable reference через prefix-form `&target`;
- индексация;
- safe-variants через `.?.`;
- block suffix после каждого применимого вызовного сегмента.

Базовые формы:

```amber
obj.field
obj.method(arg1, arg2)
obj.method arg1, arg2
obj[index]
fn = &NameSpace.some_fn
fn(args)
klass = Point
klass(args)
```

### 4.2. Block suffix

У блока есть две формы:

1. с явными параметрами;
2. без списка параметров, но с `_1`, `_2`,... внутри тела.

```amber
numbers.map |x|: x * 2
numbers.map: _1 * 2
```

Блок **всегда относится к ближайшему вызову слева**, а не «перепрыгивает» на следующий сегмент цепочки.

```amber
numbers.map: _1 * 2.select: _1 > 0
# читается как:
# (numbers.map {... }).select {... }
```

### 4.3. One-liner block boundary rule

Чтобы one-liner-блоки были однозначны в чейнинге, фиксируется лексическое правило.

- обычный доступ/вызов: **без пробела перед точкой**;
- продолжение цепочки после one-liner блока: **точка с пробелом слева**.

```amber
numbers.map: _1 * 2.select: _1 > 0.reduce 0: _1 + _2
```

Внутри one-liner блока чейнинг разрешён, но только без пробела перед точкой:

```amber
users.map: _1.email.downcase().strip().uniq()
```

То есть:

- `_1.email.downcase()` — часть тела блока;
- `.uniq()` — продолжение внешней цепочки.

### 4.4. Скобки в чейнинге

В этой редакции статус скобочной формы **закрыт**:

```amber
numbers.map(_1 * 2).select(_1 > 0)
```

трактуется **только как обычный вызов с аргументами в скобках**, а не как альтернативная компактная форма блока.

Следствия:

- `_1`, `_2`,... существуют только внутри block suffix без `|...|`;
- запись `map(_1 * 2)` в v1 **невалидна**, потому что `_1` вне блока не существует;
- для компактного трансформационного стиля нужно писать либо `map: _1 * 2`, либо `map |x|: x * 2`.

То есть допустимы:

```amber
numbers.map: _1 * 2
numbers.map |x|: x * 2
```

А это не входит в v1:

```amber
numbers.map(_1 * 2) # invalid in v1
```

Такое решение сознательно выбрано ради:

- детерминированной грамматики;
- более простого Pratt/recursive-descent парсера;
- более предсказуемой компиляции в AST/HIR;
- отсутствия скрытой «второй формы блока».

### 4.5. Safe navigation `.?.`


Amber использует `.?.` как оператор безопасной навигации, чтобы не конфликтовать с рубишными именами методов, оканчивающимися на `?`.

Поддерживаются:

```amber
expr.?.method(...)
expr.?.field
expr.?.[key]
fn.?.(args)
users.?.map: _1.email
```

Семантика: если receiver равен `null`, результат safe-сегмента — `null`, и дальнейшее вычисление соответствующего safe-шагa не продолжается.


### 4.6. Callable references `&...` и канонический callable-call

Amber фиксирует first-class callable references как часть core syntax.

Каноническая форма вызова callable-значения:

```amber
fn(args...)
```

Форма `fn.()` в язык не вводится и не является альтернативным spelling'ом. Точка остаётся только operator'ом member access / method send, а `fn(args...)` понижается в `HCall`.

Prefix `&` создаёт **callable reference object**, а не raw machine address. Пользователь не получает числовой адрес функции, FFI pointer или стабильный code pointer. Runtime вправе представлять callable reference как closure, descriptor object, send-reference, loader-backed entry или иной объект, если выполняется наблюдаемый callable contract.

Поддерживаемые v1-формы:

```amber
fn = &NameSpace.some_fn # module/top-level callable binding или export
cm = &User.find # class-side method reference: receiver = User, selector =:find
m = &User#full_name # unbound instance method reference
```

Семантика:

- `&NameSpace.some_fn` создаёт callable reference на callable binding / module export. Если target статически не резолвится или очевидно не является callable, frontend обязан диагностировать это до runtime; в dynamic path нарушение завершается `TypeError` или `NoMethodError` по обычным правилам резолюции.
- `&Class.method` создаёт bound class-side send-reference: вызов `cm(args...)` наблюдаемо эквивалентен `Class.method(args...)` и участвует в обычных lookup, `method_missing`, open-world invalidation и frozen-world guard rules.
- `&Class#method` создаёт unbound instance method reference. Вызов требует явный receiver первым positional-аргументом:

```amber
m = &User#full_name
m(user)
m(user, arg1, arg2)
```

Для `&Class#method` runtime сначала проверяет, что первый аргумент удовлетворяет `Class === receiver`; затем вызывает instance-side selector на этом receiver с оставшимися аргументами и тем же optional block. Если receiver отсутствует или не удовлетворяет owner-class constraint, это `TypeError`. Если method lookup не находит selector и `method_missing` не срабатывает, это `NoMethodError`.

В v1 `&` принимает только reference-target, а не произвольное выражение. Невалидны:

```amber
&obj.method # instance-bound method reference не входит в v1 spelling
&foo() # нельзя взять reference результата вызова через &
&(foo + bar) # нельзя брать reference произвольного выражения
```

Если нужен bound instance callable, v1 использует обычный closure/block-level adapter, а не новый surface spelling. Более явные формы вроде `obj.&method` могут быть добавлены отдельным будущим RFC, но не являются частью.

### 4.7. Spread arguments `*` и `**`

В call argument lists принимают positional и keyword spread:

```amber
fn(1, *args, mode::fast, **opts)
obj.run(*items, **kwargs)
```

`*expr` раскрывается в positional arguments. В core language допустимы только finite `Array`, `Tuple` и `Range`; open-ended ranges дают `InfiniteCollectionError`, остальные значения дают `TypeError`.

`**expr` раскрывается в keyword arguments через validation-based kwargs view. Допустимые operands:

- `Map` / `HashMap`;
- `StrictMap` / `StrictHashMap`;
- объект с readable property `kwargs`, результат которого сам является keyword-spreadable value.

Keyword-convertible key — это `Symbol(name)`, `Str(name)` или ordinary-map `NameKey(name)`, где `name` удовлетворяет правилам Amber keyword/parameter identifier. После конверсии duplicate keyword names являются `KeywordArgumentError`, кроме случая, когда ordinary `Map` уже схлопнул duplicate name keys при построении.

```amber
fn(**{mode::fast}) # ok
fn(**{"mode"::fast}) # ok
fn(**{"user-id": 1}) # KeywordArgumentError
fn(**StrictMap{a: 1, "a": 2}) # KeywordArgumentError: duplicate `a`
```

Порядок вычисления строго слева направо: callee, positional values/spreads, explicit keyword values, keyword spreads, затем сам dispatch. Spread markers не разрешены как standalone expressions:

```amber
*xs # invalid
**xs # invalid
```

## 5. Блоки, лямбда-аргументы и placeholders

### 5.1. Каноническая форма блока

Основная форма блока — ruby-like:

```amber
collection.each |x|:
 puts x
```

One-liner:

```amber
collection.map |x|: x * 2
```

### 5.2. Блок без `|...|`

Если список параметров не указан, внутри блока доступны placeholders `_1`, `_2`,...

```amber
numbers.map: _1 * 2
numbers.reduce 0: _1 + _2
```

Правила:

- placeholders доступны только в блоке без `|...|`;
- `_k` read-only;
- номера должны быть плотными: нельзя использовать `_1` и `_3`, пропустив `_2`;
- арность блока определяется максимальным использованным `_k`;
- для стандартных методов арность блока должна совпадать с их контрактом.

### 5.3. Явные параметры блока как паттерны

В блоке с `|...|` список аргументов — это список паттернов.

```amber
ary.map |a, (b, c), {d:, e:}|:
 puts a, b, c, d, e
```

Следствия:

- разрешена декомпозиция tuple/list;
- разрешена декомпозиция map/object;
- если аргументы не сматчились с паттернами, это `MatchError`.

## 6. Специальная переменная `$_`

`$_` — это встроенная read-only переменная «результат последнего выражения» в текущем scope.

`_` в этой редакции **не** является last-result-переменной: это wildcard в pattern-контекстах и не читается как обычное имя.

Scope-уровни:

- функция;
- метод;
- блок;
- фибра / таск.

Правила:

- после любого expression-statement `$_` обновляется значением этого выражения;
- присваивание тоже обновляет `$_`;
- `pass` и `noop` дают `null`;
- если функция или блок завершаются без `return`, возвращается текущее `$_`.

Примеры:

```amber
1 + 2
puts $_
# 3

def normalize(str):
 str.strip()
 log $_
 $_.downcase()
 $_
```

### 6.1. Конструкции, которые обновляют `$_`

Фиксированные случаи:

- `x = expr` -> `$_ = x`
- `@x = expr` -> `$_ = присвоенное значение`
- `@@x = expr` -> `$_ = присвоенное значение`
- `pass` / `noop` -> `$_ = null`
- `def name(...):...` как выражение даёт `:name`
- `class Name:...` как выражение даёт созданный объект класса
- `mixin Name:...` как выражение даёт созданный mixin object
- module directives (`package`, `import`, `from`, `export`) не меняют `$_`

## 7. Управляющие конструкции как выражения

### 7.1. `if`, `else if`, `elif`, `elsif`, `else`

Приоритетная каноническая форма — `else if`, но разрешены также `elif` и `elsif`.

```amber
if cond1:
 body1
else if cond2:
 body2
else:
 body3
```

`if` — выражение.

Значение:

- последнее выражение выполненной ветки;
- если ни одна ветка не выполнилась и `else` нет — `null`.

### 7.2. `unless`

`unless` — симметричная форма для отрицательного условия.

```amber
unless user:
 raise "no user"
```

Семантика значения — как у `if`.

### 7.3. Циклы

Поддерживаются:

- `while`
- `until`
- `do... while`
- `loop:`

```amber
while cond:
 work()

until done?:
 step()

do:
 x += 1
while x < 10

loop:
 tick()
```

#### Значение цикла

Цикл — выражение.

- если выход произошёл через `break value`, значением цикла является `value`;
- если выход произошёл по условию или по голому `break`, значением цикла является `null`.

```amber
result = loop:
 if ready?:
 break:ok
# result ==:ok
```

### 7.4. `break`

`break` может нести значение:

```amber
break:some_symbol
```

## 8. Функции, методы, классы и объектная модель

### 8.1. `def`

Поддерживаются многострочная и one-liner формы.

```amber
def add(a, b):
 a + b

def double(x): x * 2
```

### 8.2. `class` и наследование

```amber
class User:
 pass

class Admin < User:
 pass
```

### 8.3. `class_method def`

Классовые методы объявляются отдельным ключевым словом.

```amber
class User:
 class_method def find(id):...
```

### 8.4. Конструктор `init`, `new` и constructor-call sugar

Конструктор называется `init`.

```amber
class Point:
 def init(@x, @y):
 pass
```

`new(...)` остаётся явным классовым путём создания объекта: он выделяет объект и вызывает `init(...)`, если тот существует.

Amber дополнительно фиксирует preferred constructor-call form:

```amber
p1 = Point.new(10, 20) # explicit construction path
p2 = Point(10, 20) # preferred constructor-call sugar
```

Нормативно class object является callable. Поэтому форма:

```amber
ClassName(args...)
```

наблюдаемо эквивалентна:

```amber
ClassName.new(args...)
```

Та же семантика применяется к динамически полученному class object:

```amber
factory = Point
p = factory(10, 20)
```

То есть `HCall` / `CALL` по class object выполняет constructor path через selector `:new` с теми же positional/keyword-аргументами и optional block. `.new(...)` не удаляется: он остаётся частью явного MOP/reflection story и может использоваться через ordinary send, `send(Point,:new,...)` или callable reference `&Point.new`.

### 8.5. Поля `@` и `@@`

- `@name` — поле экземпляра;
- `@@name` — поле класса / class storage.

### 8.6. Auto-assign в параметрах

Параметр, записанный как `@x` или `@@x`, одновременно:

- объявляет обычный внешний параметр `x`;
- просит автоматически присвоить его в соответствующее поле после биндинга аргументов.

```amber
def init(@name, @email, @active = true):
 validate_email(@email)
```

### 8.7. Параметры и сигнатуры

Формы параметров:

#### Позиционные

```amber
x
x = expr
@x
@x = expr
x as T
x as T = expr
@x as T
@x as T = expr
```

#### Keyword

```amber
name:
name: expr
@name:
@name: expr
name as T:
name as T: expr
@name as T:
@name as T: expr
```

Внешнее имя keyword-параметра всегда без `@`/`@@`.

```amber
def init(@host:, @port: 5432):
 pass

Connection.new(host: "db", port: 1000)
```

### 8.8. Значения по умолчанию

Ключевая отличительная черта относительно Python:

- default-выражения вычисляются **на каждом вызове**;
- default-выражения могут ссылаться на `@...` и `@@...`.

```amber
def init(@env: @@default_env, timeout = @timeout):
 pass
```

### 8.9. Порядок вычисления аргументов, дефолтов и auto-assign

Нормативный порядок:

1. на стороне вызывающего вычисляются receiver и фактические аргументы слева направо;
2. выполняется preflight: арность, keywords, неизвестные ключи, дубли;
3. явные аргументы связываются с параметрами;
4. default-значения вычисляются слева направо по сигнатуре;
5. выполняется typecheck для параметров с `as Type`;
6. только после этого происходит commit auto-assign в `@...` / `@@...`;
7. затем исполняется тело.

Важное правило:

- внутри default-выражений `@x` и `@@x` означают **старое состояние объекта / класса**, а не уже «подготовленное» значение параметра;
- если default одного параметра зависит от другого параметра, нужно ссылаться на локальное имя параметра, а не на `@field`.

Пример:

```amber
def init(@x, @y = x):
 pass
```

Здесь `y` зависит от локального параметра `x`, а не от поля `@x`.

Если ни одна multi-clause ветка не подошла, auto-assign не коммитится.

### 8.10. Пакеты, imports и exports

Amber v1 фиксирует **static source-level module system**, отделённый от будущего MOP/`include`-слоя.

Один исходный файл соответствует одной compilation unit.

#### Заголовок пакета

Импортируемый модуль может начинаться с file-header декларации:

```amber
package net.http
```

Нормативно:

- `package` может появляться только как первая непустая non-comment top-level форма файла;
- в одном файле допускается не более одной `package`-декларации;
- `package` задаёт **logical module id** compilation unit;
- `package` не открывает блок и не требует дополнительного отступа;
- если `package` отсутствует, единица компиляции считается script/entry unit и не обязана быть импортируемой по имени в v1.

#### Imports

Поддерживаются только статические top-level формы:

```amber
import net.http
import net.http as http

from net.http import Client, RequestError
from net.http import Client as HttpClient
```

Нормативно:

- `import` и `from... import...` разрешены только на top-level;
- import-секция идёт после optional `package` и до первого non-import top-level item;
- bare `import a.b.c` создаёт локальный read-only binding с именем последнего сегмента пути, то есть `c`;
- `import a.b.c as x` создаёт локальный binding `x`;
- `from a.b.c import Name as Alias` создаёт локальный read-only binding `Alias`, связанный с export `Name` из целевого модуля;
- все imports входят в статический dependency graph и сериализуются в `.amberbc` через `DEPS`;
- относительные импорты (`.foo`, `..bar`) в v1 не вводятся;
- `from... import *` в v1 не вводится;
- присваивание импортированному имени или import-alias — compile-time error.

Imported bindings являются **live read-only aliases** к export-cells зависимого модуля. Namespace-object, получаемый через `import`, наблюдает те же live exports через `.`-доступ.

#### Export model

Экспорт оформляется отдельной top-level формой:

```amber
export Client, get, RequestError
export HttpClient as Client
```

Нормативно:

- top-level bindings приватны по умолчанию;
- только явно экспортированные имена видимы из других модулей;
- `export` разрешён только на top-level;
- `export X` публикует top-level binding `X` под тем же именем;
- `export Local as Public` публикует binding `Local` под внешним именем `Public`;
- `export` может ссылаться как на локально объявленное top-level имя, так и на импортированный alias, тем самым поддерживая explicit re-export;
- два экспорта с одинаковым public-именем в одном модуле — compile-time error;
- экспорт имени, которое к концу module-binding phase не резолвится в top-level binding, — compile-time error.

#### Namespace-object и циклы

`import net.http as http` связывает `http` с module namespace object.

Нормативно:

- module namespace object открывает только export-таблицу модуля;
- `from`-импорт обязан проверяться против `EXPT` целевого модуля при link/load phase;
- отсутствие запрошенного export даёт `ImportError`;
- при циклической загрузке ранний доступ к ещё неинициализированному export обязан следовать уже зафиксированной loader-semantics: наблюдается `initializing`, а раннее чтение может завершиться `ModuleInitError`.

#### Что не входит в это решение

- `require` не имеет специальной loader-семантики в v1; если такое имя встречается в коде, оно трактуется как обычный вызов/идентификатор;
- `include` не является формой загрузки модулей; поведенческая композиция описывается отдельно в mixin/`include` profile ниже;
- runtime/dynamic import как отражательная операция не входит в source-level v1-core.

### 8.11. `mixin`

Amber фиксирует **named mixin object profile**, отделённый и от package/import system, и от ordinary class inheritance.

Примеры:

```amber
mixin Timestamped:
 def touch!():
 @updated_at = clock.now()

mixin AuditFields:
 include Timestamped

 def audit_label():
 "#{@created_by}:#{@updated_at}"
```

Нормативно:

- named `mixin`-форма создаёт или reopen-ит **mixin object**;
- если имя ещё не связано в текущем lexical owner — создаётся новый mixin object;
- если имя уже связано с mixin object — форма означает **reopen** этого mixin'а;
- если имя связано не с mixin object — это `TypeError` либо более точная диагностируемая ошибка;
- `mixin` не поддерживает superclass clause в v1;
- один syntactic mixin-body коммитится **атомарно** по тем же правилам publish-point transaction, что и class-body;
- mixin object является именованным binding'ом и может импортироваться/экспортироваться обычным `import` / `from... import...` / `export`;
- mixin body допускает `def`, nested `class`, nested `mixin`, `include` и `pass`;
- `class_method def` внутри mixin body в v1 запрещён как compile-time error;
- методы mixin'а живут на **instance-side** и участвуют в lookup только после включения через `include`.

`mixin Name:...` как выражение даёт соответствующий mixin object.

### 8.12. `include` и ancestor composition

`include` в — это **declarative body form**, а не loader-форма и не namespace-import.

Surface syntax:

```amber
class User:
 include Timestamped, AuditFields

mixin AuditFields:
 include Comparable
```

Нормативно:

- `include` разрешён только непосредственно внутри body `class` или `mixin`;
- `include` запрещён на module top-level, внутри `def`, внутри block body и в expression-position;
- каждый операнд `include` обязан резолвиться в mixin object; попытка включить класс, module namespace object или произвольное значение даёт `TypeError` (или compile-time error, если это статически очевидно);
- `include` влияет только на **instance-side** method lookup;
- `include` не создаёт новых lexical bindings и не импортирует top-level exports в scope;
- packages/imports отвечают за namespace, а `include` отвечает только за поведенческую композицию;
- повторное включение уже присутствующего mixin'а в результирующей ancestor linearization является идемпотентным no-op;
- прямые или косвенные циклы в include-графе запрещены и обязаны завершаться `IncludeCycleError` (компилятор/loader вправе диагностировать их раньше, если цикл статически прозрачен).

#### Lookup order

Для lookup используется linearized ancestor order.

Нормативная интуиция:

- локальные методы класса всегда доминируют над mixin-методами;
- более поздний `include` доминирует над более ранним;
- сам mixin доминирует над mixins, которые он включает;
- superclass chain идёт после локального класса и его linearized mixins.

Reference linearization:

```text
ancestors(Class C):
 [C]
 + expand(reverse(C.direct_includes))
 + ancestors(C.superclass)

ancestors(Mixin M):
 [M]
 + expand(reverse(M.direct_includes))

expand(mixins):
 depth-first, preserving the first occurrence
```

Где `expand(...)` обязан:

- обходить direct includes справа налево относительно source order;
- добавлять mixin в результат до его собственных included mixins;
- игнорировать поздние дубликаты уже встреченного mixin'а;
- бросать `IncludeCycleError`, если текущий DFS-path повторно входит в уже активный mixin.

Следствие для разрешения конфликтов по selector'у:

1. local class method;
2. last included direct mixin и его include-цепочка;
3. более ранние direct mixins и их include-цепочки;
4. superclass и его linearized includes.

Те же правила применяются и к `method_missing`.

#### Relation to reopen / frozen-world

`include` является world mutation, если он меняет direct include-set класса или mixin'а.

Нормативно:

- include-директивы внутри одного syntactic class/mixin body коммитятся атомарно вместе с методами этого body;
- reopen `class`/`mixin` может добавлять новые include-директивы;
- после freeze barrier любая такая операция обязана давать `WorldFrozenError`;
- class-side mixins / `extend` не входят в минимальный -профиль этой подчасти; финальная -норма позже добавляет отдельную declarative form `extend` на class-side, см. Q15 и последующие реализационные части.

### 8.13. Open classes


Amber сохраняет **minimal open-class model** и сочленяет его с именованными mixin'ами.

Named `class`-форма:

- если имя ещё не связано в текущем lexical owner — создаёт новый class object;
- если имя уже связано с class object — **reopen**-ит его;
- если имя связано не с классом — это `TypeError` либо более точная диагностируемая ошибка времени выполнения.

Пример:

```amber
class User:
 def full_name(): "#{@first} #{@last}"

class User:
 def admin?(): false
```

Нормативно:

- reopenable `class` не является новым синтаксисом: используется та же surface-form `class Name:...`;
- superclass clause может присутствовать у первоначального объявления;
- при reopen superclass clause либо опускается, либо должен резолвиться в тот же superclass; несовместимость даёт `SuperclassMismatchError` (компилятор вправе диагностировать это раньше);
- один syntactic class-body коммитится **атомарно**: методы/классовые методы, определённые внутри, становятся видимы целиком после успешного завершения body;
- поздний reopen **заменяет** целый method entry по данному selector'у на соответствующей стороне dispatch, а не «добавляет ещё одну clause» к уже существующему multi-clause `def`;
- clause aggregation по правилам §10 работает только внутри одного syntactic def-group / class-body, а не через отдельные reopen-операции.

### 8.14. Reflective `define_method`

Минимальный reflective API задаётся builtin-функцией:

```amber
define_method(User,:greet) |name|:
 "Hello, #{name}"
```

Нормативно:

- специальная семантика действует только когда имя `define_method` резолвится в builtin prelude binding; локальное затенение превращает форму в обычный вызов;
- первый аргумент обязан быть class object или mixin object;
- второй аргумент обязан быть `Symbol` или `Str`;
- реализация метода задаётся либо block suffix, либо явным третьим callable-аргументом, но не обоими сразу;
- в v1 `define_method` определяет или заменяет **instance method** в target class/mixin;
- если используется block-form, сигнатура устанавливаемого метода берётся из параметров блока; доступны обычный `self` и обычный method-call context;
- reflective `define_method` не добавляет clause к существующему multi-clause методу: он заменяет целый method entry для selector'а;
- class-side reflective `define_method`, remove/alias/visibility hooks и richer class-side composition сверх later-added declarative `extend` в минимальный -профиль не входят.

Успешный `define_method` считается world mutation и обязан обновлять dispatch invalidation metadata по правилам части X.

### 8.15. Reflective `send`

Reflective dispatch фиксируется как builtin-функция:

```amber
send(user,:full_name)
send(user, selector, arg1, arg2)
```

Нормативно:

- специальная семантика действует только когда имя `send` резолвится в builtin prelude binding;
- первый аргумент — receiver;
- второй аргумент — selector, обязанный быть `Symbol` или `Str`;
- дальнейшие позиционные/keyword-аргументы и optional block suffix передаются как у обычного метода;
- после резолюции selector'а правила lookup / guard / block forwarding совпадают с обычным method call;
- если selector является compile-time literal symbol/string, lowering вправе понизить `send(...)` в обычный `HSend` / `SEND`;
- если selector неизвестен статически, lowering обязан использовать reflective slow-path (`HSendDyn` / `SEND_DYN`).

### 8.16. `method_missing`

`method_missing` получает в минимальную, но нормативную семантику.

Правило для обычного `obj.foo(...)` и reflective `send(obj, sel,...)`:

1. сначала выполняется обычный method lookup;
2. если target найден — вызов идёт обычным путём;
3. если target не найден — lookup пытается найти selector `method_missing`;
4. если `method_missing` найден, он вызывается с первым positional-аргументом = missing selector (`Symbol`), затем со всеми исходными аргументами и тем же block/value context;
5. если и `method_missing` не найден — бросается `NoMethodError`.

Дополнительно:

- `method_missing` — обычное имя метода, а не keyword;
- `method_missing` сам **не** получает рекурсивного fallback на ещё один `method_missing`, если lookup этого selector'а тоже не удался;
- изменение или переопределение `method_missing` считается dispatch-relevant world mutation.

### 8.17. World mutation и frozen-world boundary

Amber разводит **data mutation** и **world mutation**.

К world mutation относятся:

- создание нового named class object;
- создание нового named mixin object;
- reopen существующего класса или mixin'а;
- `define_method`;
- `include`, меняющий direct include-set;
- любая операция, меняющая method table / ancestor-composition / dispatch fallback policy;
- загрузка нового Amber-модуля в тот же frozen dispatch-world после freeze barrier.

Не считаются world mutation:

- запись в `@field` / `@@field`;
- изменение обычных объектов данных;
- allocation/deallocation как таковые;
- обычный вызов `send(...)`, если он не меняет мир.

Reference model использует два состояния dispatch-world:

```text
open -> frozen
```

Нормативно:

- обычный dynamic Amber может жить в состоянии `open` неограниченно долго;
- **Amber/Frozen** — это build/runtime profile, а не отдельный source-keyword;
- loader/linker/module-init выполняются при world-state = `open`;
- freeze transition инициируется host/toolchain после успешной загрузки и инициализации выбранного набора модулей;
- после перехода в `frozen` любая world mutation обязана бросать `WorldFrozenError` либо быть отклонена раньше verifier/loader/toolchain-слоем;
- `send(...)` и `method_missing` остаются **законными** и после freeze, но компилятор не обязан их де-виртуализовать: такие места могут оставаться на reflective slow-path.

### 8.18. Что не входит в minimal MOP 

Сознательно вне минимального v1-профиля остаются:

- class-side mixins / `extend`;
- reflective remove/alias/visibility API;
- общий introspection/reflection API поверх method tables, ancestors и source locations;
- hot reload и package-manager policy;
- «позднее добавление clause» к уже существующему методу через reopen/`define_method`.

Подробная нормативная модель MOP/frozen-boundary закреплена в части X.

## 9. Pattern matching v1

### 9.1. Где работает pattern matching

Паттерны используются в:

- `case` / `case!`;
- деструктурирующем присваивании `PATTERN = expr`;
- параметрах блоков `|...|`;
- `when`-клаузах multi-clause `def`.

Не каждая pattern-form доступна во всех этих контекстах: bare matcher expressions и dynamic pattern objects имеют отдельные ограничения, описанные ниже.

### 9.2. `case` и `case!`


```amber
case expr:
 when PATTERN if GUARD:
 body
 when PATTERN:
 body
 else:
 body
```

Строгая форма:

```amber
case! expr:
 when PATTERN if GUARD:
 body
 when PATTERN:
 body
 else:
 body
```

`case` и `case!` — match-expression'ы с одинаковой clause-grammar и одинаковым pattern-engine.

Алгоритм общий:

1. вычислить `expr` -> `value`;
2. идти по веткам сверху вниз;
3. для каждой ветки попытаться сматчить `PATTERN` на `value`;
4. если матч успешен — вычислить guard `if...`, если он есть;
5. первая ветка, у которой match + guard, побеждает;
6. если нет совпадения:
 - если есть `else` — выполняется `else`;
 - иначе для обычного `case` результатом является `null`, и `$_` становится `null`;
 - иначе для `case!` поднимается `MatchError`.

Нормативно:

- `case!` является strict-form того же `case`, а не отдельным паттерн-языком;
- lowering для `case!` обязан использовать тот же `HMatchDispatch`, меняя только `fail_mode`;
- `case!` лексируется как отдельная keyword-form, а не как `case` + postfix `!`.


### 9.3. Допустимые паттерны v1

#### Wildcard

```amber
_
```

`_` ничего не биндит и может встречаться сколько угодно раз.

#### Binding-name

```amber
name
_tmp
```

Lowercase / underscore-start identifier связывает новое имя.

Ограничение: дубликаты имён в одном паттерне запрещены.

```amber
(x, x) # compile-time error
```

#### Литералы

```amber
null
true
false
42
"str":ok
```

Матч по значению.

#### Tuple-паттерны

```amber
(a, b)
(x, _, y)
(head, *tail)
```

`*tail` допускается только один раз и только в конце.

#### List / Array-паттерны

```amber
[]
[a, b, c]
[head, *tail]
```

#### Map / Object-паттерны

```amber
{id:, name:}
{id: user_id, email: mail}
{email: _}
```

Named-key map patterns use the matched object's map semantics. Для ordinary
`Map` / `HashMap` это name-indifferent lookup:

```amber
case {"id": 1}:
 when {id: id}:
 id
# => 1
```

Для `StrictMap` / `StrictHashMap` named-key pattern по умолчанию запрашивает
exact symbol key, если объект не предоставляет собственный
`deconstruct_keys`.

#### Map-rest

```amber
{a:, b:, **rest}
{a:, b:, **_}
{a:, b:, **null}
```

Семантика:

- без `**...` лишние ключи допускаются;
- `**rest` — захватить остальные ключи в map;
- `**_` — явно проигнорировать остаток;
- `**null` — строгий матч: лишних ключей быть не должно.

Для ordinary maps rest-capture возвращает canonical exported keys
(`Str` для name keys). Для strict maps rest-capture сохраняет exact key values.

#### Pin-pattern

```amber
^x
```

Матч только если значение равно уже существующему `x`.

#### As-pattern и typed binding

```amber
whole as PATTERN
n as Int
Point(x: x as Int, y: y as Int)
```

Сначала whole-value биндится в `whole`, затем на то же значение применяется `PATTERN`.

В pattern-контексте `as` остаётся именно as-pattern, а не type-annotation sugar.

#### OR-pattern

```amber
p1 | p2
```

Пробуются альтернативы слева направо.

Ограничение: все альтернативы OR-паттерна должны биндить один и тот же набор имён.

Внутри `|...|` у block-параметров OR-паттерны должны быть дополнительно обёрнуты в скобки, чтобы не конфликтовать с разделителями блока:

```amber
ary.map |((0, x) | (x, 0))|: x
```

#### Constant / Type pattern

Идентификатор, начинающийся с Uppercase, трактуется как constant/type pattern:

```amber
Int
Str
User
Point
```

Семантика:

```amber
T === value
```

#### Typed destructuring

```amber
T(p1, p2,...)
T(x:, y:,...)
```

Семантика:

- сначала `T === value`;
- затем вызывается `deconstruct()` или `deconstruct_keys(keys)`.

#### Dynamic pattern object (explicit-binding profile)

```amber
pattern(expr)
pattern(expr) with {id:, **null}
```

Семантика:

- `pattern(expr)` вычисляет `expr` и трактует результат как dynamic matcher object;
- `pattern(expr)` без `with` не вводит новых имён;
- `pattern(expr) with MAP_PATTERN` сначала исполняет dynamic matcher, затем матчится against returned bindings-map;
- наружу попадают только имена, явно связанные `MAP_PATTERN`;
- скрытая инъекция локалов из matcher object запрещена.

Контекстные ограничения v1:

- dynamic pattern objects разрешены в `case`, `case!` и в clause-style `def`;
- dynamic pattern objects запрещены в block params и pattern assignment.

### 9.4. Протокол `===`, `deconstruct()`, `deconstruct_keys(keys)`, `match(value)`

#### `===`

Используется для type/constant patterns и bare matcher expressions в `case` / `case!`.

Для классов ожидается ruby-like семантика `Class#=== ~= is_a?`.

Если `===` возвращает не булево значение — это ошибка протокола (`TypeError`).

#### `deconstruct()`

Контракт:

- возвращает sequence/list/tuple-like значение;
- либо `null` для no-match.

Если возвращён не-список — `TypeError`.

#### `deconstruct_keys(keys)`

Контракт:

- возвращает map;
- либо `null` для no-match.

Если возвращён не-map — `TypeError`.

Какие `keys` передавать:

- если map-паттерн перечисляет только конкретные ключи — можно передать список этих ключей;
- если используется `**rest` или `**null`, нужно получить полное раскрытие, чтобы проверить остаток.

#### `match(value)`

Используется только для dynamic pattern objects вида `pattern(expr)`.

Контракт:

```text
matcher.match(value) -> DynamicMatchResult
DynamicMatchResult(success: Bool, bindings: Map)
```

Нормативные требования:

- при `success = false` `bindings` обязаны быть пустыми;
- при `success = true` и отсутствии `with...` `bindings` также обязаны быть пустыми;
- если `with MAP_PATTERN` присутствует, returned `bindings` матчится как обычный map-pattern;
- только имена, связанные `MAP_PATTERN`, попадают в локальный scope;
- любое нарушение контракта является `TypeError`.

### 9.5. Безголовые `{...}` и `[...]`

Для «голых» map/list паттернов порядок такой:

#### `{...}`

1. если значение — нативный `Map/Hash`, матчим напрямую;
2. иначе, если объект умеет `deconstruct_keys`, вызываем его и матчим результат;
3. иначе — no-match.

#### `[...]`

1. если значение — нативный `Array/List`, матчим напрямую;
2. иначе, если объект умеет `deconstruct`, вызываем его и матчим результат;
3. иначе — no-match.

### 9.6. Matcher expressions в `case` / `case!`

Только в `case` и `case!` разрешается fallback-форма, когда `when...` содержит не структурный паттерн, а выражение-матчер.

```amber
case x:
 when 1..10::small
 when String::str
 when {id:, **null}::obj
 else::other
```

Если после `when` запись не разбирается как `Pattern`, она трактуется как `MatcherExpr`, и проверяется:

```amber
MatcherExpr === value
```

Отдельно от bare matcher expressions в v1 разрешены dynamic pattern objects явной формы:

```amber
when pattern(route("/users/:id")) with {id:, **null}:...
```

Различие нормативно:

- bare matcher expression не экспортирует bindings;
- `pattern(expr)` может участвовать в matching как runtime-configured matcher object;
- bindings наружу возможны только через явный `with MAP_PATTERN`.

В `def`-клаузах, block params и в деструктурирующем присваивании bare matcher expressions запрещены. Dynamic pattern objects в v1 разрешены только в clause-style `def`, `case` и `case!`.

## 10. Multi-clause def

### 10.1. Каноническая форма

Для богатых сигнатур фиксируется каноническая clause-style форма:

```amber
def f(base_signature):
 when CLAUSE_PATTERN if GUARD:
 body
 when CLAUSE_PATTERN:
 body
 else:
 body
```

Именно эта форма обязательна для сигнатур, где есть:

- defaults;
- keyword-параметры;
- `@`/`@@` auto-assign;
- типовые аннотации.

### 10.2. Что матчится в `CLAUSE_PATTERN`

Правило:

- если `CLAUSE_PATTERN` — map pattern `{...}`, матч идёт по `ArgsMap` — карте всех аргументов по именам параметров;
- если `CLAUSE_PATTERN` — tuple pattern `(... )`, матч идёт по `ArgsTuple` — кортежу позиционных аргументов;
- иначе форма разрешена только если в сигнатуре ровно один позиционный параметр; тогда матч идёт по нему.

Примеры:

```amber
def fmt(x, mode::short):
 when {mode::short}:
 "S: #{x}"
 when {mode::long}:
 "LONG: #{x}"
 else:
 "??"

def add(x, y):
 when (0, y): y
 when (x, 0): x
 else: x + y

def area(shape):
 when Point(x, y): x * y
 when Rect(w:, h:): w * h
 else: 0
```

### 10.3. Семантика вызова multi-clause def

Порядок:

1. preflight;
2. bind явных аргументов;
3. defaults слева направо;
4. typecheck;
5. dispatch по `when`-клауза;
6. если клауза выбрана — commit auto-assign;
7. выполнить тело выбранной ветки.

Если ни одна клауза не подошла и `else` нет — `MatchError`, а поля не меняются.

### 10.4. Sugar «несколько def подряд»

Сохраняется ограниченный erlang/elixir-style sugar для простого случая:

```amber
def fact(0): 1
def fact(n) if n > 0: n * fact(n - 1)
```

Этот сахар разрешается только для v1-случая:

- только позиционные параметры;
- без defaults;
- без `@`/`@@` auto-assign;
- без keyword-параметров;
- без `as Type` в заголовке;
- guard — через `if`.

Внутренне он компилируется в каноническую clause-style форму.

## 11. Ошибки и диагностики

### 11.1. Compile-time errors

- дубликаты имён в одном паттерне;
- разный набор биндингов в альтернативах OR-паттерна;
- `*rest` / `**rest` вне конца паттерна;
- неоднозначный `CLAUSE_PATTERN` при многопозиционной сигнатуре;
- смешивание `_1/_2/...` с явным списком параметров блока;
- неплотная нумерация placeholders;
- `include` вне class/mixin body;
- `class_method def` внутри mixin body;
- недопустимый callable reference target: `&foo()`, `&(expr)`, `&obj.method` в v1;
- использование `#` вне формы unbound callable reference `&Class#method`.

### 11.2. Runtime errors

Начиная с этой редакции runtime error surface считается нормализованной на уровне **канонических имён**. Ранее встречавшиеся формулировки вида «либо эквивалентная ошибка» считаются закрытыми в пользу перечисленных ниже names. Если ситуация статически очевидна, компилятор/loader вправе выдать более раннюю compile-time diagnostic и не доводить программу до runtime; но если ошибка остаётся runtime-observable, conformance suite обязан видеть именно каноническое имя.

#### `MatchError`

Выбрасывается в случаях:

- pattern assignment не сматчился;
- параметры блока не сматчились при вызове;
- multi-clause `def` не нашёл ветку и `else` отсутствует.

`case` без совпадения и без `else` в текущей редакции не бросает исключение: он возвращает `null`.

#### `ArgumentError`

Выбрасывается, когда callable или builtin collection operation вызваны с
недопустимой формой аргументов: неверная arity, неизвестный или повторный
keyword, отрицательный/нулевой размер окна там, где требуется положительный
размер, либо eager-операция запрошена у open-ended collection без конечной
границы.

#### `EmptyCollectionError`

Выбрасывается, когда collection operation нормативно требует хотя бы один
элемент, но receiver пуст. Минимально обязательный случай: `reduce` без `init`
на пустой последовательности.

#### `IndexError`

Выбрасывается, когда обычный builtin indexed access (`[]`) обращается за
границы `Array`, `Tuple`, `Set`, `Range` или `LazySeq`. Safe-navigation
`expr.?.[key]` может вернуть `null` только из-за `null` receiver; она не
переименовывает out-of-bounds доступ в успешное значение.

#### `KeyError`

Выбрасывается, когда обычный `Map#[]` требует присутствующий key, но key в map
отсутствует. Для проверки наличия key используются `Map#contains?` /
`Map#include?`, которые возвращают `Bool` и не бросают `KeyError` при
отсутствии key.

#### `TypeError`

Выбрасывается при нарушении протоколов:

- `===` вернул не булево значение;
- `deconstruct()` вернул не sequence;
- `deconstruct_keys()` вернул не map;
- `in` применён к объекту, который не является контейнером и не поддерживает `contains?`;
- `include` или `extend` пытается использовать значение, не являющееся mixin object;
- `HCall` / `CALL` применён к значению, которое не является callable object и не является class object;
- unbound method reference `&Class#method` вызван без явного receiver или с receiver, для которого `Class === receiver` возвращает `false`.

#### `WatchTargetError`

Выбрасывается в Amber/Notebook Watch Profile, когда `Kernel.watch(...)` вызывается на форме, которая не является допустимым syntactic watch-target, либо когда runtime API получает target, для которого нельзя построить watch binding/object handle.

Если недопустимость цели статически очевидна, frontend обязан предпочесть compile-time diagnostic. Runtime `WatchTargetError` нужен для host/kernel API, dynamic notebook execution и случаев, где intrinsic-form была построена после parsing/binding.

#### `NoMethodError`

Выбрасывается, когда обычный method lookup не нашёл target и fallback через `method_missing` тоже не сработал.

#### `ImportError`

Выбрасывается, когда source-level `import` / `from... import...` не может быть удовлетворён на loader/linker path: отсутствует требуемый модуль, отсутствует запрошенный export или нарушен обязательный dependency contract.

#### `ModuleInitError`

Выбрасывается, когда код пытается наблюдать export модуля в состоянии `initializing` до завершения его init-phase.

#### `IsolationError`

Выбрасывается при нарушении shareable/strand-confined boundary: например, при cross-strand захвате non-shareable значения, отправке его через `Channel` или доступе к confined object из чужого strand.

#### `DestroyedAccessError`

Выбрасывается, когда обычный send / field access / indexing обращается к объекту в состоянии `destroyed`.

#### `UseAfterFreeError`

Выбрасывается, когда обычный send / field access / indexing обращается к уже `deallocated` объекту либо pin/lifecycle API наблюдает stale pointer state.

#### `LifetimeError`

Выбрасывается при незаконных user-visible lifecycle-операциях, которые не сводятся к ordinary use-after-free: например, при попытке пользовательского `destroy!`/`dealloc` для неподходящего runtime object.

#### `IncludeCycleError`

Выбрасывается, когда direct/indirect `include`-граф образует цикл. Компилятор или loader вправе диагностировать такой случай раньше, если цикл статически очевиден.

#### `WorldFrozenError`

Выбрасывается, когда код в frozen-profile пытается выполнить world mutation после freeze barrier: reopen класса или mixin'а, `define_method`, `include`/`extend`, меняющие ancestor graph, либо позднюю загрузку Amber-модуля в уже frozen dispatch-world.

#### `SuperclassMismatchError`

Выбрасывается, когда reopen формы `class Name < Base:` пытается переоткрыть существующий класс с несовместимым superclass. Компилятор вправе диагностировать такой случай раньше, если он статически очевиден.

#### `TimeoutError`

Выбрасывается, когда `wait(timeout:...)` или иной нормативный deadline-aware runtime path не успевает завершиться до дедлайна.

#### `CancelledError`

Выбрасывается, когда task наблюдает ранее поставленный cancellation flag в safe-point и не завершилась естественным образом раньше этого места.

#### `ChannelClosedError`

Выбрасывается, когда код пытается `send()` в уже закрытый channel либо делает `recv()` из закрытого и уже пустого channel.

#### `DeadlockError`

Выбрасывается, когда non-reentrant `Mutex` наблюдает повторный `lock()` тем же владельцем без промежуточного `unlock()`.

#### `OwnershipError`

Выбрасывается, когда владелец синхронизационного объекта не совпадает с вызывающим контекстом: например, при `Mutex.unlock()` на незахваченном mutex или при unlock не-владельцем.

#### `AtomicError`

Базовый класс ошибок atomic API.

#### `AtomicCompatibilityError`

Выбрасывается, когда `Atomic.new`, `Atomic.set`, `Atomic.compare_and_set` или `Atomic.update` пытается записать значение, не входящее в v1 atomic-compatible payload set.

#### `CapabilityError`

Выбрасывается в Amber/Capabilities & Sandbox Profile, когда код пытается выполнить host-resource operation без выданной capability: filesystem, network, environment, process, clock, random, FFI, GPU/device, database, secret store или другой host-gated ресурс.

Если отсутствие capability статически очевидно по manifest/effect contract, toolchain обязан предпочесть compile-time diagnostic или load-time rejection. Runtime `CapabilityError` нужен для dynamic hosts, plugin execution, notebooks и позднего capability negotiation.

#### `EffectViolationError`

Выбрасывается в Amber/Effects Profile, когда callable объявлен с более узким effect row, чем фактически наблюдаемое действие, либо когда caller/host запрещает effect, который runtime пытается выполнить.

Статически доказуемые нарушения должны диагностироваться до исполнения; runtime error остаётся обязательным observable fallback для dynamic, reflective и host-injected paths.

#### `DeterminismError`

Выбрасывается в Amber/Reproducible Execution Profile, когда deterministic scope пытается использовать недетерминированный источник без recorded/virtualized provider: реальное время, случайность, unordered external I/O, racing schedule edge или host callback вне replay log.

#### `ReplayDivergenceError`

Выбрасывается при replay, если фактическое исполнение расходится с recorded trace: другой task interleaving, другой external input digest, другое значение виртуального clock/random source, несовпадение dependency fingerprint или нарушение canonical event ordering.

#### `SchemaViolationError`

Выбрасывается в Amber/Schema & API Contracts Profile, когда значение не удовлетворяет declared schema при encode/decode, migration, wire-boundary validation или API contract validation.

#### `ContractViolationError`

Выбрасывается в Amber/Contracts Profile, когда `require`, `ensure`, invariant или property-test shrinker находит нарушение объявленного контракта. Для property-based testing ошибка должна включать минимизированный counterexample, если shrinker смог его построить.

#### `PolicyViolationError`

Выбрасывается в Amber/Privacy, Taint & Lineage Profile, когда данные с запрещённой меткой покидают разрешённый boundary, экспортируются без redaction, соединяются с несовместимым policy context или теряют обязательную lineage metadata.

#### `WorkflowError`

Выбрасывается в Amber/Durable Workflow Profile, когда durable step history, idempotency key, compensation, retry policy или replayable workflow log нарушают нормативный workflow contract.

#### `AcceleratorError`

Выбрасывается в Amber/Accelerator Profile, когда GPU/SIMD/device kernel нарушает разрешённый kernel subset, обращается к неподдерживаемому типу, использует host-only object, наблюдает device-lifetime fault или получает несовместимый device capability.


## 12. Типовая система: minimal type envelope для implementation gate

Типовая система по-прежнему **не является завершённой нормативной частью full-checker'а**, но закрывает минимальный контур, достаточный для parser/HIR/runtime hooks и старта reference implementation.

Принятые решения:

- язык остаётся gradual / optional typed;
- без аннотаций код ведёт себя как динамический;
- с аннотациями допускаются runtime type-hooks уже в первой реализации;
- `as` сохраняется как единая surface-form для binding annotation и checked cast;
- `name as {id:, name:}` и `name as Int` в pattern-контексте остаются as-pattern, а не типизацией, потому что справа pattern-term, а не type-term.

### 12.1. Return type syntax

Разрешается следующая форма:

```amber
def parse(src as Str) -> Ast:...

class Parser:
 class_method def load(path as Str) -> Parser:...
```

Нормативно:

- `-> TypeTerm` допускается после списка параметров `def` и `class_method def`;
- return type относится ко всему callable;
- return boundary использует тот же type-hook contract, что и параметрический `as TypeTerm`.

### 12.2. Минимальная grammar `TypeTerm`

```ebnf
TypeTerm::= TypeUnion
TypeUnion::= TypeSuffix { "|" TypeSuffix }
TypeSuffix::= TypePrimary [ "?" ]
TypePrimary::= ConstPath
 | ConstPath "[" TypeTerm { "," TypeTerm } [ "," ] "]"
 | "(" TypeTerm { "," TypeTerm } [ "," ] ")"
 | "{" TypeField { "," TypeField } [ "," ] [ "," "**" TypeTerm ] "}"
TypeField::= Name ":" TypeTerm
```

### 12.3. Минимальная семантика `TypeTerm`

Нормативно:

- `T?` — sugar для `T | Null`;
- `(A, B, C)` — tuple type фиксированной арности;
- `Vec[Int]`, `Map[Str, Int]` и подобные формы допустимы как generic-looking surface syntax без обещания полной variance-модели в v1;
- `{id: Int, name: Str}` — record/map type, который требует наличие указанных ключей;
- `**T` в record-type означает, что дополнительные ключи допустимы, а их значения обязаны удовлетворять `T`;
- record-types в implementation gate считаются **open by default**; политика exact-record остаётся вопросом второй волны.

### 12.4. Что обязана делать первая реализация

Первая реализация обязана:

- парсить `TypeTerm` syntax-faithfully;
- сохранять type-term в AST и HIR;
- выполнять runtime checks на границах:
 - parameter bind,
 - `expr as TypeTerm`,
 - return boundary.

Первая реализация **не обязана** иметь полный статический вывод типов, variance-анализ generics, typing для `and/or`, typing для `$_`, narrowing через `case` и полную интеграцию типов с MOP.

## 13. Модель concurrency / threading v1: без GIL

В этой редакции concurrency больше **не считается открытым архитектурным вопросом**. Для v1 фиксируется модель, которая одновременно:

- не использует глобальный interpreter lock;
- сохраняет простой кооперативный стиль `async`/`task.async` из ранних черновиков;
- остаётся компилируемой в bytecode VM и совместимой с дальнейшим AOT-profile;
- не требует делать все обычные объекты неявно thread-safe.

### 13.1. Три уровня исполнения

Amber v1 различает три уровня runtime:

1. **Worker** — системный поток ОС.
2. **Strand** — однопоточная последовательная область исполнения с собственной runnable-очередью.
3. **Task** — кооперативная fiber внутри strand.

Ключевое инвариантное правило:

- внутри одного **strand** одновременно исполняется **не более одной task**;
- разные **strand** могут исполняться **параллельно на разных worker'ах**;
- следовательно, у Amber **нет GIL**, но при этом обычная shared mutable state остаётся простой внутри одного strand.

Именно strand, а не весь процесс VM, является единицей последовательности и обычного «безопасного разделения по ссылке».

### 13.2. Surface API

Нормативные формы v1:

```amber
async |task|:
 rows = []

 committer = task.async |committer|:
 loop:
 committer.sleep 5.0
 commit rows
 rows.clear!

 parser = task.spawn |worker|:
 parse_big_file(path)

 task.async |consumer|:
 while msg = redis.brpop("some_queue"):
 rows << msg
 committer.resume if rows.count >= 10_000

 parser.wait()
```

Зафиксированные операции:

- `async |task|:` — создаёт корневой **strand scope** и root-task;
- `task.async |child|:` — создаёт дочернюю task **в том же strand**;
- `task.spawn |child|:` — создаёт дочернюю task **в новом strand**, который может быть выполнен на любом worker'е;
- `task.sleep(seconds)` — переводит текущую task в sleeping-state до дедлайна;
- `task.yield()` — добровольная передача управления планировщику;
- `handle.resume()` — делает sleeping/waiting task runnable;
- `handle.wait()` — дожидается завершения child-task и возвращает её результат;
- `handle.wait(timeout: seconds)` — то же, но бросает `TimeoutError`, если дедлайн истёк;
- `handle.cancel()` — кооперативный запрос на отмену.

`task.async` и `task.spawn` возвращают `TaskHandle`.

### 13.3. Нормативная разница между `task.async` и `task.spawn`

#### `task.async`

- child-task живёт в **том же strand**;
- может свободно видеть и менять обычные mutable-объекты, захваченные по ссылке;
- выполняется кооперативно вместе с sibling-task'ами;
- не даёт параллельного доступа к обычным объектам, потому что strand serializes execution.

Именно поэтому старый паттерн с общим `rows = []` считается валидным: обе task находятся в одном strand.

#### `task.spawn`

- child-task запускается в **другом strand**;
- новый strand может быть запущен на другом worker'е параллельно;
- прямой захват обычных mutable-объектов из родительского strand запрещён;
- пересекать границу strand могут только **shareable** значения и специальные synchronization objects.

Это и есть нормативная основа модели **without GIL**: параллелизм разрешён между strand'ами, а не через совместный небезопасный доступ ко всем объектам VM.

### 13.4. Shareable / non-shareable значения

Для v1 значения делятся на две группы.

#### Shareable

Могут свободно пересекать границы strand:

- `null`, `true`, `false`;
- числа;
- символы;
- frozen/immutable строки;
- frozen tuples/lists/maps;
- константные метаобъекты языка;
- closure-объекты, чьи captures целиком shareable;
- `TaskHandle`, `Channel`, `Mutex`, `Atomic`.

#### Strand-confined

По умолчанию не могут быть захвачены в `task.spawn` и не могут быть отправлены через cross-strand API без явного преобразования:

- обычные `Array` / `Map`;
- пользовательские объекты с mutable-состоянием;
- closure, захватывающие non-shareable ссылки.

Минимальное нормативное правило v1:

- если `task.spawn` или `Channel.send` получает non-shareable значение, реализация должна либо выдать compile-time error (когда это очевидно), либо бросить `IsolationError` на runtime.

В v1 **не вводится** скрытое копирование mutable-объектов и **не вводится** прозрачная thread-safe-обёртка для всех объектов языка.

### 13.5. Минимальная stdlib / runtime база для no-GIL модели

Чтобы модель была practically usable, в обязательный runtime contract v1 входят:

- `TaskHandle`
- `Channel`
- `Mutex`
- `Atomic`

Минимальные контракты:

```amber
ch = Channel.new(capacity: 0)
ch.send(value)
value = ch.recv()
ch.close()

m = Mutex.new()
m.lock()
m.unlock()
m.locked?()
m.owned?()
m.synchronize:
 critical_section()

a = Atomic.new(0)
a.get()
a.set(1)
a.compare_and_set(1, 2)
a.update |x|:
 x + 1
```

Нормативно:

- payload для `Channel` должен быть shareable;
- `Mutex` и `Atomic` могут разделяться между strand'ами;
- использование `Mutex`/`Atomic` не вводит GIL: это локальная синхронизация конкретных объектов, а не глобальный lock VM;
- `Channel` в v1 обязан иметь explicit `close()`;
- `send()` в закрытый channel обязан бросать `ChannelClosedError`;
- `recv()` из закрытого buffered-channel обязан вернуть уже поставленные элементы; `recv()` из закрытого и пустого channel обязан бросать `ChannelClosedError`;
- порядок элементов в одном channel — FIFO; очереди ожидающих `send`/`recv` на одном channel normative FIFO; более сильная глобальная fairness не гарантируется;
- `Mutex` в v1 non-reentrant; повторный `lock()` тем же владельцем без промежуточного `unlock()` обязан завершаться `DeadlockError`;
- `Mutex.unlock()` на незахваченном mutex или из не-владеющего task/strand/thread обязан завершаться `OwnershipError`;
- `Mutex.synchronize` эквивалентен `lock(); try body; finally unlock()`, возвращает результат блока и освобождает mutex при exception/cancellation unwind;
- atomic-compatible payload set v1: `null`, `Bool`, `Integer`, `Symbol`, константные метаобъекты языка и shareable heap references; попытка записать non-compatible replacement обязана завершаться `AtomicCompatibilityError`;
- `Atomic.compare_and_set(expected, replacement)` сравнивает primitive значения по значению, а heap references по identity; user code не исполняется внутри CAS comparison;
- `Atomic.update` эквивалентен CAS-loop `old = get(); replacement = block(old); compare_and_set(old, replacement)` до успешной записи, возвращает новое значение, abort/propagate при exception из блока и обязан предупреждать, что block может быть выполнен больше одного раза;
- `Atomic.get()`, `Atomic.set(...)`, `Atomic.compare_and_set(...)` и `Atomic.update(...)` в v1 обладают seq-cst semantics.

Эти решения сознательно выбирают простой reference contract для реализации и corpus; более слабые memory orders и альтернативные closed-channel profiles не являются частью v1.

### 13.6. Scheduling semantics

#### Состояния task

Минимальный state-machine v1:

- `new`
- `runnable`
- `running`
- `sleeping`
- `waiting`
- `done`
- `failed`
- `cancelled`

#### Правило планирования

- worker берёт runnable-strand из глобальной очереди;
- внутри strand исполняется одна runnable-task;
- переключение между task'ами внутри strand происходит только в scheduler points:
 - `sleep`
 - `yield`
 - `wait`
 - blocking `Channel` ops
 - acquisition `Mutex`
 - back-edge циклов
 - call boundary, если реализация использует safe-point poll.

Strand может мигрировать между worker'ами **только между task-steps**, но это не нарушает модель, потому что внутри strand всё равно не бывает параллельного исполнения.

### 13.7. Семантика `resume`

`resume` окончательно фиксируется так:

- это **не немедленный jump** в target-task;
- это **enqueue / wake signal** для target-task;
- текущая task продолжает исполняться до ближайшего scheduler point или конца своего текущего шага.

Алгоритм v1:

```text
resume(handle):
 if handle.state in {done, failed, cancelled}:
 return false
 if handle.wake_pending:
 return true
 handle.wake_pending = true
 mark_runnable(handle.task)
 enqueue(handle.strand)
 return true
```

Следствия:

- повторные `resume()` **коалесцируются** в один pending wake-token;
- `resume()` завершённой task возвращает `false`;
- `resume()` runnable/running task является допустимым no-op и возвращает `true`.

### 13.8. `wait`, exceptions, cancellation, timeout

#### `wait`

`handle.wait()`:

- если child уже завершён успешно — сразу возвращает cached result;
- если child `failed` — повторно бросает сохранённое исключение в текущей task;
- если child ещё работает — текущая task переводится в `waiting` и scheduler переключает strand дальше.

#### Исключения

Unhandled exception child-task:

- сохраняется в `TaskHandle`;
- переводит child в `failed`;
- поднимается при `wait()`;
- если parent выходит из structured-scope, не дождавшись failed-child явно, runtime обязан rethrow first unhandled child failure на выходе из scope.

#### Structured concurrency

Каждый `async`-scope и каждая task образуют **structured child set**:

- при выходе из task-body runtime автоматически join'ит незавершённых детей;
- если один child падает и ошибка не была обработана локально, siblings получают `cancel()`;
- после этого first failure rethrow'ится в parent.

Detached / orphan task в v1 не вводятся.

#### Cancellation

`handle.cancel()`:

- ставит cancellation flag;
- делает target runnable, если он sleeping/waiting;
- не прерывает task посреди произвольной инструкции;
- наблюдается только в safe-points.

Нормативно: при обнаружении cancellation в safe-point runtime бросает `CancelledError`, если код не завершился раньше естественным образом.

#### Timeout

`handle.wait(timeout: seconds)` эквивалентен ожиданию с дедлайном. Если deadline истёк до завершения child:

- текущая task получает `TimeoutError`;
- child автоматически **не отменяется**, если только вызывающий явно не сделал `cancel()`.

### 13.9. Память и happens-before

Для v1 фиксируется следующая минимальная модель видимости:

- внутри одного strand действует ordinary program order;
- между strand'ами happens-before возникает только через explicit synchronization edges.

Нормативные меж-strand edges:

- successful `Channel.send(v)` happens-before successful `recv()`, который возвращает именно это значение `v`;
- `Channel.close()` happens-before любое последующее наблюдение closed-state на том же channel;
- `Mutex.unlock()` happens-before следующий успешный `lock()`, который захватывает тот же mutex;
- успешное завершение task happens-before успешный `handle.wait()`, который наблюдает этот result или rethrow'ит сохранённую failure;
- все операции `Atomic.get/set/compare_and_set` участвуют в одном global seq-cst order данного исполнения.

Следствия:

- запись в ordinary non-shareable objects не становится меж-strand visible без одного из перечисленных edges;
- меж-strand races на ordinary confined objects остаются запрещены самой isolation model, а не «разрешены, но racy»;
- спецификация v1 не обещает более сильной fairness или более тонкой memory-order granularity, чем перечислено выше.

### 13.10. `$_` и лексические данные

Сохраняется более раннее решение:

- у каждого call frame есть собственный `last_result` slot;
- у каждой task/fiber — свой стек frame'ов;
- переключение между task'ами не смешивает `$_`.

Следовательно:

- `$_` не является global/thread-local переменной процесса;
- `$_` не протекает между sibling-task'ами даже внутри одного strand;
- `$_` naturally lowers to frame slot и не конфликтует с no-GIL execution.


### 13.11. Обязательный stdlib contract v1: chainable collections

Чтобы surface syntax языка опирался на единый нормативный runtime API, в фиксируется обязательный коллекционный профиль стандартной библиотеки.

Для `Array`, `Tuple`, `Range`, `Set` и `LazySeq` обязательны:

- `each`
- `each(size, step:)`
- `each_pair`
- `each_cons`
- `map`
- `flat_map`
- `select`
- `reject`
- `take_while`
- `reduce`
- `find`
- `any?`
- `all?`
- `none?`
- `first`
- `count`
- `contains?`
- `include?`
- `union`
- `intersection`
- `difference`
- `left_difference`
- `symmetric_difference`
- `subset?`
- `proper_subset?`
- `superset?`
- `proper_superset?`
- `disjoint?`
- `concat`
- `reverse`
- `sort`
- `uniq`
- `permutation`
- `combination`
- `group_by`
- `to_a`
- `lazy`

Нормативно:

- `each` возвращает receiver;
- `each(size, step:)` возвращает/yields `Array` окон длины `size` с шагом
 `step`; без `step:` шаг равен `size`;
- `each_pair` эквивалентен `each(2, step: 1)`;
- `each_cons(size)` эквивалентен `each(size, step: 1)`;
- `map`, `flat_map`, `select`, `reject` и `group_by` по умолчанию eager;
- `take_while`, `reverse`, `sort` и `uniq` возвращают `Array`, кроме
 `Set#uniq`, сохраняющего `Set`;
- `uniq |x|:` использует block как ключ дедупликации;
- `sort |a, b|:` ожидает integer comparator result;
- set-like операции возвращают `Set` для receiver-`Set` и `Array` для
 остальных последовательностей;
- операторные алиасы: `&` = `intersection`, `|` = `union`, `-` =
 `difference`, `^` = `symmetric_difference`, `<` / `<=` =
 proper/non-proper subset, `>` / `>=` = proper/non-proper superset, `+` =
 `concat`, `*` = repetition;
- `permutation(count)` и `combination(count)` возвращают `Array` массивов;
- open-ended `Range` и `LazySeq` не материализуют eager операции без явной
 конечной границы;
- `.lazy` переводит дальнейшую цепочку в lazy-profile;
- `to_a` материализует `LazySeq`.

`reduce` поддерживает две формы:

```amber
xs.reduce(init) |acc, x|:...
xs.reduce |acc, x|:...
```

Нормативно:

- форма с `init` всегда допустима;
- форма без `init` допустима только для непустой последовательности;
- на пустой последовательности без `init` должен выбрасываться `EmptyCollectionError`.

Для `Map` обязательны:

- `each |k, v|:`
- `map |k, v|:`
- `select |k, v|:`
- `reject |k, v|:`
- `transform |k, v|:` возвращает key/value tuple или list
- `transform_values |v, k|:` где `k` — опциональный второй аргумент блока
- `merge(other)` / `merge(other) |k, old, new|:`
- `keys`
- `values`
- `entries`
- `contains?`
- `include?`

Нормативно:

- `Map#map` возвращает `Array`;
- `Map#select` и `Map#reject` возвращают `Map`;
- `Map#transform` возвращает `Map` и может менять ключи;
- `Map#transform_values` возвращает `Map`;
- `Map#merge` возвращает `Map`, сохраняет порядок левого receiver, добавляет
 новые ключи справа в порядке правого аргумента и при конфликте использует
 правое значение либо значение, возвращённое block;
- `Map#+` и `Map#|` являются алиасами `Map#merge`;
- `Map#each_pair` является алиасом `Map#each`, а без block возвращает
 `entries`;
- `Map#contains?` и `Map#include?` проверяют наличие key.

С ordinary `Map` operations используют normalized key semantics. Для name keys canonical export — `Str`: `keys`, `entries`, `each`, `map`, `select`, `reject`, `transform`, `transform_values` и `merge` обязаны передавать/возвращать string key для `Symbol(:name)` / `Str("name")` entry. `Map#[]`, `Map#[]?`, `contains?` и `include?` нормализуют lookup key тем же правилом, что и литерал; unsupported key values дают `TypeError`, отсутствующие valid keys дают `KeyError` только для обязательного `Map#[]`.

`StrictMap` / `StrictHashMap` предоставляют тот же operation surface, но используют exact-key semantics и экспортируют фактический stored key value. `Map#merge` с ordinary maps схлопывает name-key duplicates, а strict merge схлопывает только exact-key duplicates.


## 14. Что входит в язык по намерению, но ещё не нормализовано до ядра


### 14.1. Amber/Notebook Watch Profile [закрыто]

Amber/Notebook вводит optional instrumentation layer для интерактивных kernel/runtime окружений: notebooks, BI notebooks, reactive dashboards, IDE scratchpads и другие hosts, которым нужна корректная invalidation зависимых блоков.

Профиль не меняет core language semantics и не является обязательной частью production runtime. Обычный `ambervm run`, frozen images, AOT/JIT profile и `.amberbc` loader могут не включать watch-инструментацию, если host/toolchain не заявил поддержку notebook profile.

#### Назначение

Surface API:

```amber
handle = Kernel.watch(target)
Kernel.unwatch(handle)
Kernel.watched?(target)
Kernel.revision(target)
Kernel.watch_state(target)
```

Главная форма:

```amber
Kernel.watch(x)
```

в notebook profile делает две вещи:

1. переводит binding `x` в watchable storage cell;
2. если текущее значение binding'а является live heap object, переводит этот объект в watchable representation для отслеживания изменений instance variables.

Пример:

```amber
user = User.new(name: "Ann", age: 20)
Kernel.watch(user)

label = "#{user.name}"
# cell dependency:
# binding:user@rev0
# object:user.@name@rev0

user.age = 21
# label-cell не инвалидируется

user.name = "Bob"
# label-cell инвалидируется
```

#### `Kernel.watch(...)` как intrinsic

`Kernel.watch(target)` имеет special semantics только если одновременно выполнены условия:

1. `Kernel` резолвится в builtin notebook kernel object;
2. `watch` резолвится в builtin intrinsic selector;
3. первый аргумент синтаксически является допустимым watch-target.

Если пользователь затенил `Kernel`, форма становится обычным method call и не получает доступа к binding-cell:

```amber
Kernel = MyKernel.new()
Kernel.watch(x) # ordinary send, не intrinsic
```

`Kernel.watch(...)` не является ordinary method call в полном смысле: runtime должен получить не только значение `x`, но и binding/ivar target. Поэтому frontend обязан распознавать эту форму после name resolution и понижать её в dedicated HIR/bytecode hook либо в эквивалентный VM intrinsic.

#### Допустимые watch-targets v1

В notebook profile допустимы только syntactic watch-targets:

```amber
Kernel.watch(x) # local / top-level binding
Kernel.watch(@x) # instance variable текущего self
Kernel.watch(@@x) # class variable текущего owner
```

В v1 запрещены:

```amber
Kernel.watch(foo()) # нет binding target
Kernel.watch(user.name) # неоднозначно: field read или method call
Kernel.watch(xs[0]) # indexing является protocol call
Kernel.watch(1 + 2) # expression value, не storage target
```

Такие формы обязаны давать compile-time diagnostic, если распознаны frontend'ом, либо runtime `WatchTargetError` в dynamic host path.

Наблюдение nested object'ов делается явно через отдельный binding:

```amber
address = user.address
Kernel.watch(address)
```

#### Binding watch

Watching a binding replaces its storage with watchable cell:

```text
LocalSlot<Value>
 -> LocalSlot<WatchCell>

WatchCell(
 value,
 binding_id,
 revision,
 flags,
 subscribers
)
```

Нормативная семантика:

```text
LOAD_LOCAL watched x during dependency capture:
 record_dependency(binding_id, revision)
 return cell.value

STORE_LOCAL watched x = value:
 old = cell.value
 cell.value = value
 cell.revision += 1
 emit_watch_event(binding_id, old, value)
```

Если новое значение является live heap object и watch handle был создан с object-tracking enabled, runtime может автоматически перевести новый объект в watchable representation.

Closure/upvalue semantics сохраняются: если binding уже был captured closure'ом, `Kernel.watch(x)` обязан заменить shared capture-cell на watchable cell, а не создать вторую независимую ячейку. Все closure, захватившие `x`, продолжают видеть одну и ту же storage cell.

#### Object watch

Если текущее значение watched binding'а является live heap object, runtime может перевести объект в watchable representation. User-visible identity не меняется:

```amber
Kernel.watch(user)
user.object_id == old_id # true
user.class == User # true
User === user # true
```

Реализация может использовать object-header flag, watched shape transition, side-table или эквивалентный механизм. Наблюдаемое требование одно: identity, class, equality и dispatch semantics не меняются.

Минимальное состояние:

```text
WatchObjectState(
 object_id,
 object_revision,
 field_revisions: Map<ivar_name, revision>,
 subscribers
)
```

Успешная запись в watched ivar:

```text
STORE_IVAR obj.@field = value:
 old = obj.@field
 obj.@field = value
 watch.object_revision += 1
 watch.field_revisions[field] += 1
 emit_watch_event(object_id, field, old, value)
```

Если write barrier, lifetime check, ownership/strand check или deallocation check падает, запись не происходит и watch event не публикуется.

#### Dependency capture для notebook cells

Notebook host может исполнять ячейку под dependency-capture mode:

```text
Kernel.begin_dependency_capture(cell_id)
execute cell
Kernel.end_dependency_capture() -> DependencySet
```

Во время capture:

- чтение watched binding'а записывает dependency на `(binding_id, revision)`;
- чтение watched ivar записывает dependency на `(object_id, ivar_name, field_revision)`;
- чтение object-wide state может записывать dependency на `(object_id, object_revision)`;
- method call сам по себе dependency не создаёт, но reads внутри метода создают dependencies, если проходят через watched cells/ivars;
- запись в watched binding/ivar bump'ает revision и публикует invalidation event.

Инвалидатор notebook host сравнивает recorded revision с текущей revision. Если revision изменилась, зависимый cell становится stale и должен быть перевычислен или помечен как invalidated в host UI.

#### Relation to world/frozen

Watching является data instrumentation, а не world mutation. `Kernel.watch(...)` не меняет:

- method tables;
- class graph;
- superclass relations;
- include/extend linearization;
- `method_missing` policy;
- loader state;
- dispatch-world freeze state.

Следовательно:

```text
world_epoch не меняется
watch_epoch может меняться
```

В frozen-world profile watch допустим только при включённой host-level notebook instrumentation. Frozen-world гарантирует стабильность dispatch-world, но не делает пользовательские данные immutable.

#### Lifetime, ownership, GC и barriers

Watch-hook является частью checked write path и выполняется после успешных runtime checks:

```text
STORE_IVAR watched_obj, field, value:
 check object not destroyed/deallocated
 check owner/strand permissions
 run write barrier
 write value
 bump watch revision
 emit watch event
```

Если объект находится в `destroyed` state, применяются обычные `DestroyedAccessError` rules. Если объект уже deallocated или handle stale, применяются обычные `UseAfterFreeError` rules. Cross-strand попытки watch/read/write confined object из чужого strand завершаются `IsolationError`.

GC обязан считать watch side-tables и subscriber lists обычными runtime roots или weak/ephemeron-indexed structures, чтобы watch-инструментация не удерживала пользовательские объекты дольше, чем это явно требует host policy.

#### Performance contract

Watchable mode заведомо медленнее ordinary execution:

- watched local/upvalue нельзя свободно scalar-replace'ить;
- watched ivar read/write должен проходить через dependency/revision hook;
- ivar inline cache должен учитывать `watched` flag или site mode;
- shape transition может уходить на slow path;
- dependency capture mode добавляет read barriers для watched targets.

Это не считается regression production runtime, потому что профиль является opt-in и host-level.

#### Minimal implementation contract

Reference VM может реализовать notebook watch через:

- `WatchCell` для locals/upvalues/top-level export cells;
- object-header `watched` flag + side-table `WatchObjectState`;
- `watch_epoch` для tooling/invalidation;
- dependency collector в текущем task/frame;
- watch event queue, потребляемую notebook host'ом.

Реализация вправе выбрать другой representation, если выполняет тот же observable contract: stable identity, корректные revisions, отсутствие world mutation и корректные invalidation events.

После закрытия implementation gate следующие вещи остаются за пределами минимального reference-профиля и относятся ко второй волне дизайна, toolchain или экосистемы:

- DSL-макросы в ruby-style;
- package manager / registry / artifact distribution / signing / hot reload;
- richer class-side composition DSL beyond уже зафиксированного declarative `extend`;
- расширенный reflection/introspection API поверх method tables, source locations и ancestor graph;
- full static checker, inference, variance, typing для `and/or`, typing для `$_` и точное type-narrowing через `case`;
- MIR/SSA, optimizer, native backend, JIT и frozen-image deployment.

# Часть II. Проектный слой компилируемого Amber [вынесен]

Детальная инженерная спецификация компилируемого Amber вынесена из основного документа в отдельный проектный слой:

- статус: reference implementation planning / executable engineering layer;
- покрытие: lexer/parser/AST/HIR, pattern compiler, bytecode ISA, VM/runtime ABI, object model, lifetime/GC/pinning/FFI, `.amberbc`, loader/verifier, no-GIL scheduler, stdlib/corpus/toolchain, backlog, milestone gates и implementation matrix.

Базовый pipeline, на который ссылается основной документ:

```text
source.am
 -> lexer/spans
 -> syntax-faithful AST `amber.ast.v1`
 -> binder + diagnostics `amber.diag.v1`
 -> HIR `amber.hir.v1`
 -> pattern IR + bytecode `amber.bc.v1`
 -> `.amberbc` verifier/loader
 -> register/slot VM runtime
```

Основная языковая спецификация не переоткрывает эти инженерные решения; для реализации, планирования репозитория, decomposition issues и release gates каноническим источником является отдельный project-layer файл.

# Часть III. Открытые вопросы и редакторские следы закрытых решений

Ниже собраны остаточные незакрытые зоны, а также несколько редакторских следов решений, закрытых уже к редакции, чтобы не потерять контекст дальнейших обсуждений.

## Q1. Политика стиля для имён с подчёркиванием [закрыто]

В политика закрывается так:

- `_`, `$_`, `_1`, `_2`,... — специальные формы языка;
- все остальные идентификаторы синтаксически допустимы;
- имена, слишком похожие на спец-формы, не становятся compile-time error и могут подпадать только под lint/tooling policy.

То есть вопрос закрывается не дополнительными запретами grammar-level, а разведением language core и style-level lint.

## Q2. Финальный модульный / импортный синтаксис [закрыто]

В source-level module syntax зафиксирован в static-profile:

- `package module.path` задаёт logical module id импортируемого файла;
- `import module.path [as Alias]` и `from module.path import Name [as Alias]` — единственные специальные формы загрузки/связывания;
- `export Name [as Public]` формирует export table исходного модуля;
- relative imports и star-import в v1 не вводятся;
- `require` не является loader-формой;
- `include` зафиксирован отдельно как mixin-composition form и не участвует в loader semantics.

В ecosystem/toolchain-level хвосты тоже закрываются: manifest/registry/signing/hot-reload policy стандартизованы отдельно, без изменения source-level module syntax.

## Q3. Глубина нормализации метапрограммирования [закрыто]

В зафиксирован **minimal MOP profile**:

- reopenable named classes;
- reflective `define_method`;
- builtin `send(...)`;
- `method_missing` fallback;
- world-mutation model и frozen-world boundary;
- named mixins;
- declarative `include` и linearized ancestor composition;
- жёсткое разведение namespace imports и behavioral composition.

Сознательно **не** входят в это решение:

- class-side mixins / `extend`;
- reflective remove/alias/visibility API;
- расширенный introspection/reflection API;
- позднее добавление clause к существующему методу через reopen/`define_method`;
- DSL/macros и hot reload.

То есть вопрос глубины MOP закрыт не «полным Ruby-MOP», а минимальным, компиляторно-дружелюбным профилем.

## Q4. Типовая система: grammar, semantics, inference [закрыто]

В типовая система закрывается как optional **Amber/Typed** profile поверх того же source language.

Принятые решения:

- typed profile включается на уровне package/build profile, а не новым source-keyword;
- exported `def`, `class_method def` и публичные constructor/boundary API в typed-package обязаны иметь явные parameter и return annotations;
- локалы, block params, внутренние/private callables и field-types допускают local inference;
- generics в считаются **invariant**;
- record-types остаются open by default, а exact-record пишется через `**Never`;
- `and` / `or` получают truthiness-aware flow typing (`false | null` — falsy-set Amber);
- `$_` имеет flow-type последнего выражения текущего scope; в начале scope его typed-view считается `Null`;
- `case` / `case!` делают pattern-based narrowing по subject; `case!` без `else` в typed profile требует exhaustiveness;
- reflective boundaries (`send(...)` с dynamic selector, `method_missing`, runtime `define_method`, reopen/`include`/`extend` через внешний open-world path) считаются `Any`-boundary, если сборка не находится в frozen typed profile.

Этим grammar/semantics/inference-вопросы закрываются на уровне спецификации. Дальше остаётся только реализация checker'а, flow engine и tooling.

## Q5. Concurrency после фиксации v1-core [закрыто]

Базовая no-GIL модель v1 сохраняется, а вторая волна закрывается следующими решениями:

- ownership transfer вводится через explicit `move(expr)` на cross-strand boundaries (`task.spawn`, `Channel.send`, `select` send-arm и другие ownership APIs);
- moved-from binding после успешного transfer больше не может читаться: статически очевидные случаи — compile error typed/lint-layer, иначе runtime `MovedValueError`;
- multi-channel wait вводится как expression `select:` с `when`, optional `timeout` и optional `else` arms;
- supervisor policy стандартизуется как keyword `policy:` для `async` и `task.spawn`; обязательные значения: `:cancel_scope` (default), `:one_for_one`, `:one_for_all`, `:rest_for_one`;
- async I/O интегрируется через standard awaitables/readiness tokens из `amber.io`, совместимые с `select`;
- distributed / multi-process runtime **не входит** в core language spec и остаётся library/host-level story поверх тех же message/ownership правил.

То есть concurrency-вопросы больше не открыты на языке: дальше остаётся только реализация scheduler/runtime и библиотек.

## Q6. Динамические pattern-objects [закрыто]

В dynamic pattern objects включены в v1, но только в **explicit-binding profile**.

Принятое решение:

- разрешить `pattern(expr)` и `pattern(expr) with MAP_PATTERN`;
- запретить скрытую инъекцию локалов;
- разрешить feature только в `case`, `case!` и clause-style `def`;
- оставить block params и pattern assignment вне v1 для этой формы.

Richer matcher protocols, typed bindings-map и library-level combinators остаются допустимой будущей библиотечной эволюцией, но больше не считаются незакрытым spec-level вопросом.

## Q7. Формальный статус `matcher expressions` вне `case` / `case!` [закрыто]

В это закрывается так:

- bare matcher expressions разрешены только в `case` / `case!`;
- в `def`-клаузаx, block params и pattern assignment они не вводятся;
- любые дальнейшие расширения этой формы возможны только отдельным RFC второй волны.

## Q8. Финальный раздел по стандартной библиотеке коллекций [закрыто]

В обязательный коллекционный профиль нормализован как часть спецификации:

- для `Array`, `Tuple`, `Range`, `Set` и `LazySeq` зафиксирован минимальный
 `Enumerable`-подобный contract плюс set-like операции, subset/superset
 predicates, membership checks, collection operator methods, windowed
 `each`, `take_while`, `reverse`, `sort`, `uniq`, `permutation` и
 `combination`;
- для `Map` зафиксированы `each/each_pair/map/select/reject/transform/transform_values/merge/keys/values/entries/contains?/include?`
 плюс `+` / `|` merge aliases;
- для `reduce` зафиксированы формы с `init` и без `init`, включая `EmptyCollectionError` на пустой коллекции без `init`;
- для обычного collection indexing зафиксированы `IndexError` и `KeyError`
 вместо неканонических nullable edge results.

Следующая волна может расширять stdlib, но старт reference implementation больше не зависит от незакрытого коллекционного API.

## Q9. Нужна ли строгая match-форма поверх безопасного `case` [закрыто]

Да. В принят `case!` как строгая surface-form поверх того же `case`-engine.

Принятое решение:

- `case` без `else` остаётся safe-form и возвращает `null`;
- `case!` без `else` бросает `MatchError`;
- grammar `when PATTERN if GUARD:` и lowering остаются общими;
- отдельный `match!` в v1 не вводится.

## Q10. Формальная матрица диагностик компилятора [закрыто]

В вводится обязательное трёхчастное разведение диагностик:

- `compile_error` — hard fail языка;
- `warning` — обязательное предупреждение компилятора, не останавливающее сборку;
- `lint` — tooling-level правила, не входящие в language acceptance.

Минимальный обязательный каталог `compile_error` охватывает pattern/binder, module/import/export и class/mixin/MOP placement rules; обязательным `warning` v1 считается чтение `@field` из default-expression при наличии позднего auto-assign в то же поле.


## Q11. Где проходит граница между fully dynamic Amber и native/AOT profile [закрыто]

Граница теперь зафиксирована так:

- обычный dynamic Amber может жить в dispatch-world состоянии `open` неограниченно долго;
- Amber/Frozen — build/runtime profile, в котором после loader/linker/module-init выполняется freeze transition;
- после freeze любая world mutation (`class`/`mixin` reopen, `define_method`, `include`, меняющий ancestor graph, поздняя Amber module load в тот же world) запрещена и даёт `WorldFrozenError`;
- `send(...)` и `method_missing` после freeze остаются легальными, но рассматриваются как reflective slow-path, а не как источник новых world mutations;
- язык не требует обязательного deopt-механизма: реализации вправе либо держать такие места на generic path, либо строить JIT/deopt поверх того же language contract.

В и backend/toolchain boundary тоже закрывается: canonical MIR/SSA, native/JIT/AOT profile и `.amberimg` фиксируются как отдельный post-v1 profile.


## Q12. Остаточные вопросы после фиксации collector/pinning/FFI profile [закрыто]

Reference profile по-прежнему закрывает: non-moving generational collector, pin tokens, pinned scopes, opaque-handle FFI boundary, safe-point handshake и запрет implicit GC-finalizer semantics для пользовательского `destroy!`.

В вторая волна памяти фиксируется так:

- weak refs и ephemerons стандартизуются как runtime/library types `WeakRef[T]` и `Ephemeron[K, V]` в пакете `amber.memory`;
- surface borrow annotations в source grammar **не добавляются**; borrowing остаётся API-level через block-scoped helpers вроде `memory.borrow(obj) |view|:...`;
- zero-copy typed buffers/slices стандартизуются как runtime classes (`Bytes`, `Buffer[T]`, `Slice[T]`) с pin-aware semantics;
- collector telemetry/tuning и host embedding API относятся к host/runtime profile, а не к core language syntax.

Этим memory/FFI second wave закрывается на уровне спецификации: дальше остаются только runtime и embedding implementation details.

## Q13. Остаточные вопросы после фиксации module format / loader / verifier profile [закрыто]

Reference profile по-прежнему закрывает: `.amberbc`-артефакт, section model, loader state machine, dependency manifest, export/import symbol tables и минимальный verifier contract.

В distribution/toolchain policy фиксируется так:

- package manifest стандартизуется как `amber.toml`;
- registry/publish unit — signed package bundle `.amberpkg`, который содержит manifest, compiled modules, export tables, digests и optional source/debug payload;
- source files внутри пакета обязаны иметь `package`, равный manifest package либо находящийся под тем же dotted-prefix;
- reproducible builds и content digests обязательны для publishable artifacts; trust chain строится на embedded Ed25519 signatures + lockfile digests;
- hot reload разрешён только в open-world dev profile и только как atomic package-swap; в frozen profile он запрещён;
- incompatible reload, меняющий public export surface или нарушающий manifest/ABI contract, обязан завершаться `ReloadIncompatibleError`.

Этим distribution ecosystem закрывается на уровне спецификации; дальше остаётся только реализация registry/client/publisher/tooling.

## Q14. Нужны ли полевые lifetime-аннотации (`owned`, `weak`, `borrowed`) [окончательно закрыто в ]

В это решение доводится до окончательного вида:

- source-level field modifiers `owned`, `weak`, `borrowed` **не будут добавляться** в Amber source grammar;
- lifecycle языка остаётся построенным вокруг ordinary object model, `destroy!`, `memory.dealloc`, weak/ephemeron wrappers и block-scoped borrow helpers;
- ownership/borrowing/weakness выражаются не модификаторами полей, а runtime/library objects и host-interop API.

То есть Amber окончательно закрывает memory story без field-level lifetime annotations.

## Q15. Class-side mixins / `extend` [закрыто]

В class-side composition фиксируется как отдельный declarative profile:

- `extend` разрешён только непосредственно внутри body `class` и её reopen-форм;
- каждый operand `extend` обязан резолвиться в mixin object;
- instance-methods mixin'а при `extend` становятся методами class object receiver'а;
- локальные `class_method def` доминируют над методами, пришедшими через `extend`;
- при нескольких `extend` действует то же правило, что и для `include`: later direct extend wins;
- `extend` является world mutation и подчиняется тем же freeze/invalidation правилам, что и `include`.

`extend` в `mixin` body и произвольный reflective class-side alias/remove API по-прежнему не входят в язык.

## Q16. Расширенный reflection / introspection API [закрыто]

В расширенная рефлексия стандартизуется как read-only stdlib/runtime package `amber.reflect`.

Принятые решения:

- reflection выдаёт immutable mirror objects (`ClassMirror`, `MixinMirror`, `MethodMirror`, `PackageMirror`, `WorldMirror`);
- mirror API покрывает name/kind/superclass/ancestors/includes-or-extends, method tables, selector ownership, source locations, parameter metadata и optional typed signature metadata;
- mirrors являются snapshot-views и не дают прав на world mutation;
- mutation path по-прежнему ограничен ранее зафиксированными механизмами (`class`/`mixin` reopen, `include`, `extend`, `define_method`).

То есть вопрос introspection закрывается без возврата к "полной Ruby-MOP".

## Q17. Native backend / JIT / frozen-image profile [закрыто]

В backend boundary фиксируется так:

- canonical optimizer/backend IR — `MIR` в SSA-форме поверх уже зафиксированного HIR;
- bytecode VM остаётся reference execution engine, а native/JIT backend — дополнительным профилем;
- native compilation допускается только для frozen-world artifacts/images;
- reflective sites (`SEND_DYN`, `method_missing`, open-world mutation paths) остаются runtime stubs/guards и не требуют обязательного deopt-механизма;
- deployable frozen image стандартизуется как `.amberimg`, bundling manifest, package table, code payload (bytecode and/or native), debug map и signatures.

После этого AOT/JIT вопрос тоже закрыт на уровне языка и artifact model: дальше остаётся только реализация MIR/backend/image-builder.


## Q18. Capability and sandbox profile [закрыто]

В фиксируется optional **Amber/Capabilities & Sandbox** profile.

Принятые решения:

- package, plugin, notebook cell, workflow step и Wasm component не получают host resources по умолчанию;
- filesystem, network, env, process, clock, random, FFI, GPU/device, database и secret-store доступы выдаются через manifest-declared capability grants;
- capability grant является host-issued token, а не ordinary user object, который можно подделать в Amber коде;
- `amber.toml` получает секцию `[capabilities]`, а `.amberbc`/`.amberpkg` могут хранить declared capability requirements в optional metadata section;
- отсутствие required capability является compile/load-time diagnostic, если доказуемо, и `CapabilityError`, если обнаружено только runtime;
- sandbox profile не меняет core language semantics и не считается world mutation.

Этим закрывается вопрос безопасного исполнения untrusted notebooks, BI snippets, plugins, serverless functions и AI-agent generated code.

## Q19. Effect and purity profile [закрыто]

В фиксируется optional **Amber/Effects** profile поверх Amber/Typed.

Принятые решения:

- callable может объявлять effect row через suffix `!{...}` после return type или после parameter list, если return type отсутствует;
- минимальные effect labels: `pure`, `alloc`, `mut`, `world`, `watch`, `async`, `strand`, `fs`, `net`, `env`, `time`, `random`, `ffi`, `db`, `gpu`, `unsafe`, `reflect`, `workflow`;
- пустая строка effects `!{}` означает deterministic/pure-without-observable-effects boundary, кроме allocation, если host profile явно считает allocation unobservable;
- effect rows используются checker'ом, optimizer'ом, notebook invalidator, sandbox loader и replay runtime;
- dynamic/reflective boundaries, которые невозможно проверить статически, поднимаются до conservative effect row `!{unsafe, reflect}` либо требуют explicit annotation.

Этим закрывается вопрос формального различения pure computation, data mutation, world mutation и host I/O без добавления checked exceptions.

## Q20. Observability and replay profile [закрыто]

В фиксируется optional **Amber/Observability & Replay** profile.

Принятые решения:

- runtime events получают canonical names, attributes, source spans, task/strand ids, world/watch epochs и optional trace context;
- обязательные event families: task, strand, channel, mutex, atomic, gc, loader, world, watch, ffi, capability, effect, schema, workflow;
- `trace.span "name":...` является library/intrinsic boundary, который может lower'иться в HIR event scope;
- deterministic execution scope виртуализирует clock, random, scheduler order и external input providers;
- replay trace `.ambertrace` хранит event log, dependency fingerprints, virtual sources, watch revisions и package/build digests;
- replay divergence является `ReplayDivergenceError`, а forbidden nondeterminism внутри deterministic scope — `DeterminismError`.

Этим закрывается вопрос воспроизводимых notebooks, BI refresh jobs, CI flakes и production debugging без изменения обычного scheduler contract.

## Q21. DataFrame / columnar BI profile [закрыто]

В фиксируется optional **Amber/DataFrame & Columnar BI** profile.

Принятые решения:

- stdlib получает normative table abstractions: `Table`, `Column[T]`, `Series[T]`, `Schema`, `LazyTable`, `QueryPlan`, `GroupedTable`;
- `Table` operations являются relational/vectorized operations, а не ordinary object iteration;
- lazy query plans обязаны быть inspectable, hashable по lineage fingerprint и совместимыми с notebook dependency capture;
- column-level revision keys расширяют watch profile: cell may depend on `table_id.column(:amount).revision`;
- columnar memory ABI может быть Arrow-compatible, но core Amber не зависит от конкретного external memory format.

Этим закрывается BI/story для аналитических notebooks, reactive dashboards и high-volume ETL без превращения `Array#map` в скрытый relational engine.

## Q22. Schema, serialization and API contracts profile [закрыто]

В фиксируется optional **Amber/Schema & API Contracts** profile.

Принятые решения:

- `schema Name vN:` является declarative profile form, сериализуемой в AST/HIR metadata и optional `.amberbc` section;
- schema fields имеют required/optional/default/deprecated/renamed metadata;
- schema evolution поддерживает explicit migration hooks и compatibility checks;
- codecs (`Json.codec(T)`, `Binary.codec(T)`, host codecs) обязаны выполнять boundary validation;
- API contract generator может эмитить OpenAPI-like descriptions для HTTP/RPC boundaries;
- schema violations дают `SchemaViolationError`.

Этим закрывается вопрос stable wire contracts для backend, services, plugin APIs и data pipelines.

## Q23. Wasm component and host plugin profile [закрыто]

В фиксируется optional **Amber/Wasm Component** profile.

Принятые решения:

- `.amberwasm` является deployable component artifact поверх frozen-world subset;
- imports/exports мапятся на WIT-like component interface descriptions;
- Amber capabilities мапятся на host/WASI-style resource permissions;
- raw FFI внутри Wasm profile запрещён по умолчанию;
- reflective world mutation после component instantiation запрещена;
- host plugin execution должен сочетать schema contracts, capabilities и effect rows.

Этим закрывается portable sandbox story для BI plugins, edge/serverless snippets и embeddable extensions.

## Q24. Accelerator / GPU / SIMD profile [закрыто]

В фиксируется optional **Amber/Accelerator** profile.

Принятые решения:

- `Tensor`, `DeviceBuffer[T]`, `Device`, `Kernel` и `DeviceStream` являются runtime/library types;
- accelerator kernels используют restricted closure subset: primitive numeric types, tensors, buffers, slices, constants и pure helper calls;
- arbitrary object access, dynamic dispatch, allocation, reflection, exceptions через host stack и hidden I/O внутри kernel запрещены;
- host/device transfer semantics explicit: copy, borrow/pin, map, unmap, synchronize;
- accelerator operations несут effect `gpu` или более конкретный device effect;
- нарушения kernel subset или device lifetime дают `AcceleratorError`.

Этим закрывается путь к GPU/SIMD/accelerator workloads без переноса полной dynamic object model на устройство.

## Q25. AI-agent tooling and provenance profile [закрыто]

В фиксируется optional **Amber/AI-Agent Tooling & Provenance** profile.

Принятые решения:

- compiler/toolchain обязан уметь эмитить machine-readable symbol graph, semantic spans, type/effect summaries и refactoring-safe anchors;
- agent patches должны применяться через structured patch protocol, а не только raw text diff, если host включил этот profile;
- patch transaction фиксирует author, tool id, prompt/request digest, changed symbols, tests run, diagnostics before/after и capability grants;
- provenance log может сериализоваться в `.amberprov` и/или optional package metadata;
- compiler обязан иметь `amber patch check` mode, который проверяет semantic patch до применения.

Этим закрывается вопрос безопасного AI-assisted maintenance без превращения source tree в неотслеживаемый набор textual edits.

## Q26. Contracts and property testing profile [закрыто]

В фиксируется optional **Amber/Contracts & Property Testing** profile.

Принятые решения:

- `require`, `ensure`, `invariant` и `old(expr)` являются contract-profile forms;
- contracts могут исполняться runtime, использоваться typed checker'ом и документироваться tooling'ом;
- property tests используют генераторы, shrinkers, seeds и replayable counterexamples;
- failed contract даёт `ContractViolationError`;
- contracts не являются optimizer assumptions в unsafe mode, если build не включил explicit `assume_contracts`.

Этим закрывается вопрос бизнес-инвариантов и AI-generated code validation без включения dependent types в core.

## Q27. Privacy, taint and data lineage profile [закрыто]

В фиксируется optional **Amber/Privacy, Taint & Lineage** profile.

Принятые решения:

- значения, поля schema, columns и tables могут иметь `DataLabel` / taint tags (`pii`, `secret`, `regulated`, custom labels);
- taint propagation работает через ordinary operations, table query plans, codecs and API exports;
- export boundary обязан проверять policy context;
- redaction protocol должен быть explicit и observable;
- lineage graph связывает source datasets, transformations, notebook cells, watch revisions и exported artifacts;
- policy violations дают `PolicyViolationError`.

Этим закрывается enterprise/BI вопрос: какие данные откуда пришли, какие ячейки/отчёты зависят от sensitive inputs и где запрещён export.

## Q28. Durable workflow profile [закрыто]

В фиксируется optional **Amber/Durable Workflow** profile.

Принятые решения:

- `workflow Name:` является profile-level declarative form для long-running, replayable, restart-safe computations;
- workflow состоит из named `step` blocks с explicit effects, retries, deadlines, idempotency keys and optional compensation;
- workflow history является append-only replay log;
- step body должен быть deterministic относительно recorded inputs или объявлять external effects;
- durable workflow не заменяет обычные `Task`/`Strand`; он строится поверх scheduler, effects, schema, capabilities and replay;
- нарушения durable history/compensation/retry contract дают `WorkflowError`.

Этим закрывается gap между runtime concurrency и production-grade ETL/backend/agentic workflows, которые должны переживать рестарт, deploy и retry.

# Часть IV-XV. Implementation blueprint, backlog и матрица [вынесены]

Бывшие разделы основного файла, посвящённые компилируемости, reference bytecode VM, lifetime/allocator, collector/pinning/FFI, `.amberbc`, minimal MOP/frozen-world implementation hooks, reference implementation blueprint, closure-профилям второй волны, детализированной матрице имплементации, backlog/milestone gating и tracking issues, перенесены в:


В этом месте намеренно оставлен только базовый reference, чтобы основной документ оставался языковой спецификацией, а проектная декомпозиция жила отдельно и могла обновляться без шума в language-core тексте.

# Часть XVI. Modern Pressure Profiles 

## 1. Назначение 

 не меняет уже зафиксированное ядро Amber. Его назначение — закрыть современные pressure-points, которые стали критичными для языков, запускаемых в notebooks, BI-платформах, sandboxed plugins, CI/CD, serverless/edge, AI-agent workflows, production observability и accelerator-heavy data workloads.

Главное правило:

```text
Новая возможность сначала оформляется как optional profile.
Core syntax/runtime меняются только если без этого нельзя выразить профиль через уже существующие AST/HIR/bytecode/tooling контракты.
```

 вводит следующие normative profile families:

```text
Amber/Capabilities & Sandbox
Amber/Effects
Amber/Observability & Replay
Amber/DataFrame & Columnar BI
Amber/Schema & API Contracts
Amber/Wasm Component
Amber/Accelerator
Amber/AI-Agent Tooling & Provenance
Amber/Contracts & Property Testing
Amber/Privacy, Taint & Lineage
Amber/Durable Workflow
```

Эти профили могут включаться независимо, но рассчитаны на композицию:

- capabilities ограничивают, что код имеет право делать;
- effects описывают, что код может сделать;
- observability/replay объясняют, что код сделал;
- schema/contracts описывают, какие значения пересекают boundaries;
- DataFrame/lineage/watch дают granular BI invalidation;
- Wasm/plugin profile делает execution portable and sandboxed;
- AI-agent provenance делает изменения source/tooling проверяемыми;
- durable workflow превращает replay/effects/capabilities в production orchestration model.

## 2. Общие правила профилей 

### 2.1. Профильность

Каждый profile является opt-in:

```toml
[profiles]
typed = true
notebook_watch = true
capabilities = true
effects = true
observability = true
replay = true
columnar_bi = true
schema = true
wasm_component = false
accelerator = false
agent_tooling = true
contracts = true
privacy = true
workflow = false
```

Host/toolchain может поддерживать subset профилей. Если source/package требует профиль, который toolchain не поддерживает, build/load обязан завершиться diagnostic до исполнения.

### 2.2. Неизменность core semantics

 profiles не меняют:

- truthiness;
- dispatch lookup;
- pattern matching semantics;
- no-GIL strand/task model;
- ordinary object identity;
- open/frozen world boundary;
- `.amberbc` mandatory sections;
- production behavior ordinary `ambervm run`, если профиль не включён.

Профиль может добавить metadata, validation, instrumentation или restricted subset, но не может silently изменить meaning обычного core-кода.

### 2.3. Metadata-first подход

Все новые profile facts должны быть представлены в одном или нескольких слоях:

```text
AST metadata — для IDE/formatter/refactor/source tools
HIR metadata — для checker/lowering/interpreter
bytecode metadata — для verifier/loader/runtime
package metadata — для registry/sandbox/provenance
trace metadata — для observability/replay/audit
```

Профили не должны требовать, чтобы production VM всегда несла весь overhead. Verifier обязан уметь отвергнуть неподдерживаемую optional section, если она помечена как required.

## 3. Amber/Capabilities & Sandbox Profile

### 3.1. Цель

Capability profile отвечает на вопрос:

```text
Какие host resources этот код имеет право использовать?
```

Это отличается от package signing. Подпись отвечает, кто произвёл artifact; capability profile отвечает, что artifact может делать во время исполнения.

### 3.2. Manifest contract

`amber.toml` получает секцию:

```toml
[capabilities]
fs.read = ["./data", "./config"]
fs.write = ["./out"]
net.connect = ["api.example.com:443"]
env.read = ["AMBER_ENV"]
time = true
random = true
ffi = false
process.spawn = false
secrets.read = ["analytics/api-token"]
gpu = false
```

Нормативно:

- отсутствующая capability означает deny by default;
- wildcard grants допустимы только если host policy их разрешает;
- package manifest объявляет requested capabilities;
- host launch policy выдаёт actual capabilities;
- runtime видит только intersection requested ∩ granted;
- попытка использовать невыданную capability даёт `CapabilityError`.

### 3.3. Runtime API

Минимальный runtime API:

```amber
caps = Kernel.capabilities()
caps.allowed?(:fs_read, path: "./data/orders.csv")

Kernel.with_capabilities(caps.subset(fs_read: ["./data"])):
 Csv.read("./data/orders.csv")
```

Capability token не является ordinary mutable object. Его нельзя сконструировать user code'ом, сериализовать в source literal или получить через reflection mirrors без host grant.

### 3.4. Capability taxonomy 

Минимальные canonical names:

```text
fs.read
fs.write
fs.metadata
net.connect
net.listen
env.read
env.write
process.spawn
process.signal
time.now
time.sleep
random.secure
random.pseudo
ffi.call
ffi.load
db.connect
secrets.read
device.gpu
device.accelerator
notebook.watch
trace.emit
workflow.persist
```

Toolchain может добавлять vendor-specific names только под reverse-DNS prefix:

```text
com.example.crm.read
cloud.vendor.queue.publish
```

### 3.5. Interaction with imports and packages

A module import не выдаёт capabilities. Capabilities принадлежат execution context, package grant или host plugin instance.

```amber
import net.http as http

http.get("https://example.com")
# legal only if current context has net.connect capability
```

Module namespace object не должен быть способен обходить capability checks.

### 3.6. Sandbox boundaries

Sandboxed execution units:

```text
notebook cell
BI query block
Wasm component instance
serverless function invocation
workflow step
AI-agent patch test run
plugin callback
```

Каждый boundary имеет:

```text
capability set
resource limits
effect allowance
trace context
schema contract
optional taint policy
```

## 4. Amber/Effects Profile

### 4.1. Цель

Effects profile отвечает на вопрос:

```text
Какие наблюдаемые действия может выполнить callable?
```

Capability profile является runtime permission model; effects profile является static/dynamic semantic contract.

### 4.2. Surface syntax

С return type:

```amber
def normalize(row as Row) -> Row !{}:
 row.trimmed()

def fetch_user(id as UserId) -> User !{net, async}:
 http.get("/users/#{id}").await()
```

Без return type:

```amber
def log_event(e) !{fs.write, time}:
 File.append("events.log", "#{clock.now()} #{e}")
```

Для block/callable types в typed profile:

```amber
Processor = Fn[Row -> Row !{}]
Fetcher = Fn[UserId -> User !{net, async}]
```

### 4.3. Canonical effect labels

Минимальный set:

```text
alloc allocation observable to host/profile
mut ordinary data mutation
world dispatch-world mutation
watch notebook watch/revision mutation
async creates/awaits task or awaitable
strand crosses strand boundary or uses synchronization
fs filesystem I/O
net network I/O
env environment read/write
time real or virtual clock
random random source
ffi native/foreign call
reflect dynamic reflection / SEND_DYN / mirrors
unsafe unchecked host/runtime escape hatch
db database or external storage
gpu accelerator/device operation
schema schema encode/decode/migration boundary
trace telemetry emission
workflow durable workflow persistence/replay
```

`pure` is represented by `!{}` in strict profile. Hosts may define `alloc` as unobservable for local pure computation, but must be consistent in checker diagnostics.

### 4.4. Sub-effecting

Effect row `A` is allowed at call site requiring `B` only if:

```text
A subset_of B
```

Example:

```amber
def p(x as Int) -> Int !{}: x + 1

def run(f as Fn[Int -> Int !{}]):
 f(1)

run(p) # ok
run(clocky) # error if clocky has !{time}
```

### 4.5. Dynamic boundaries

These constructs force conservative effects unless statically resolved:

- `send(receiver, selector_expr,...)` with non-literal selector;
- `method_missing` fallback;
- reflective mirrors crossing into dynamic lookup;
- FFI callbacks;
- host callbacks;
- `eval`-like functionality, if any host provides it;
- dynamic package/plugin loading in open-world profile.

A typed/effects build may require explicit annotation:

```amber
def dynamic_call(obj, sel) -> Any !{reflect, unsafe}:
 send(obj, sel)
```

### 4.6. Effects and optimizer

Optimizer may use effect rows for:

- common subexpression elimination of pure calls;
- memoization hints;
- parallelization of independent pure/dataflow nodes;
- notebook invalidation pruning;
- replay deterministic validation;
- capability preflight;
- warning about unused nondeterministic calls.

Optimizer must not erase or reorder operations with non-empty effect rows unless it proves equivalence under the active profile.

## 5. Amber/Observability & Replay Profile

### 5.1. Observability goals

Amber runtime has tasks, strands, channels, GC, loader, MOP, frozen-world, notebook watches, FFI, schemas and optional native/JIT. requires observability to be semantic, not purely logging.

Canonical event shape:

```text
AmberEvent(
 name,
 timestamp_or_virtual_time,
 trace_id?, span_id?, parent_span_id?,
 task_id?, strand_id?, worker_id?,
 module_id?, method_id?, source_span?,
 world_epoch?, watch_epoch?,
 attributes,
 severity?,
 causality_edges[]
)
```

### 5.2. Surface API

```amber
trace.span "parse.orders", attrs: {path: path}:
 rows = Csv.read(path)
 trace.metric("orders.count", rows.count)
 rows
```

Library spelling may remain ordinary calls; profile-enabled lowering may emit `HTraceSpan`/`HTraceEvent`.

### 5.3. Required event families

Minimum canonical names:

```text
task.started
task.blocked
task.resumed
task.cancelled
task.completed
task.failed
strand.enqueued
strand.migrated
channel.send
channel.recv
channel.close
mutex.wait
mutex.lock
mutex.unlock
atomic.cas
gc.cycle.start
gc.cycle.end
gc.pause.start
gc.pause.end
loader.module.load
loader.module.init
world.freeze
world.mutation
watch.read
watch.write
watch.invalidate
ffi.enter
ffi.exit
capability.check
capability.denied
effect.boundary
schema.decode
schema.encode
workflow.step.start
workflow.step.commit
workflow.step.retry
workflow.step.compensate
```

### 5.4. Trace context

When host integrates with distributed tracing, Amber trace context must propagate through:

- async task boundaries;
- channel send/recv payload metadata, if host policy allows;
- awaitables and async I/O;
- HTTP/RPC stdlib clients;
- workflow steps;
- Wasm component calls.

Trace context propagation must not make non-shareable values cross strands. It is metadata, not object sharing.

### 5.5. Replay trace `.ambertrace`

Replay profile records:

```text
package lock digests
compiled artifact digests
capability grants
schema versions
virtual clock values
random seeds/outputs
external input digests
scheduler decisions
channel operation order
watch revisions and invalidations
workflow step commits
stdout/stderr ordering
trace event stream
```

`.ambertrace` may be binary or canonical JSON in reference tools, but must have a stable content digest and version header.

### 5.6. Deterministic scope

```amber
Kernel.deterministic(seed: 42):
 report = run_pipeline()
 report.digest()
```

Inside deterministic scope:

- real clock is replaced by virtual clock;
- random source is seeded/recorded;
- scheduler choices are recorded or deterministic;
- external I/O must go through recorded providers;
- unordered map iteration must be canonicalized if observed.

Forbidden nondeterminism gives `DeterminismError`.

### 5.7. Replay

```amber
Kernel.replay("run-2026-04-26.ambertrace"):
 run_pipeline()
```

If execution diverges from trace, runtime raises `ReplayDivergenceError` with source span and first divergent event id.

## 6. Amber/DataFrame & Columnar BI Profile

### 6.1. Цель

Amber core collections are object/iterator oriented. BI workloads need relational, columnar, lazy and lineage-aware abstractions.

### 6.2. Normative types

```text
Table
Column[T]
Series[T]
Schema
Row
LazyTable
QueryPlan
GroupedTable
JoinPlan
Aggregation
ColumnStats
```

`Table` is not a subtype of `Array[Row]` in the normative model. It may expose row iteration, but query operations are columnar/relational.

### 6.3. Query API

```amber
orders.where: _1.status ==:paid.select(:country,:amount,:user_id).group_by(:country).agg(
 revenue: sum(:amount),
 users: count_distinct(:user_id)
 ).sort_by(:revenue, desc: true)
```

Required operations:

```text
select
rename
drop
where
with_column
group_by
agg
join
union
sort_by
limit
sample
collect
explain
schema
lineage
```

### 6.4. Lazy query plans

`LazyTable` represents a plan, not immediate materialized data.

```amber
plan = orders.lazy.where: _1.amount > 100
plan.explain()
rows = plan.collect()
```

Normative requirements:

- query plan is inspectable;
- plan has stable fingerprint over inputs, operations and schema versions;
- optimizer may reorder relationally safe operations;
- side-effecting UDFs require effect annotations and constrain optimizer reordering.

### 6.5. Columnar memory ABI

Reference implementation may use an Arrow-compatible ABI for zero-copy interchange, but Amber spec only requires:

```text
contiguous or chunked column buffers
null bitmap or equivalent validity representation
schema metadata
nested values support or explicit unsupported diagnostic
pin-aware export to native/FFI/accelerator profiles
```

### 6.6. Notebook watch integration

Columnar profile extends watch keys:

```text
binding:orders@rev
object:orders@rev
column:orders.amount@rev
table-plan:plan_fingerprint@rev
lineage:source_digest@rev
```

A cell depending only on `orders.amount` is not invalidated by changes to `orders.status`, unless the query plan uses both.

### 6.7. UDFs

UDFs in table operations must declare effects in typed/effects profile:

```amber
orders.with_column(:bucket) |row| -> Symbol !{}:
 if row.amount > 1000::large else::small
```

A UDF with `!{net}` or `!{time}` can be accepted only if host policy allows nondeterministic query plans.

## 7. Amber/Schema & API Contracts Profile

### 7.1. Schema syntax

```amber
schema User:
 id as UUID
 name as Str
 email as Str?
 created_at as Time
 deprecated legacy_id as Int?

schema Order v1:
 id as UUID
 user_id as UUID
 amount as Decimal
 status as Symbol =:new
```

`schema` is a profile-level declarative form. Parser that does not enable schema profile may reject it before ordinary expression parsing.

### 7.2. Field metadata

Supported field metadata:

```text
required / optional
nullable
default expression
deprecated
renamed_from
sensitive labels
wire_name
codec hint
version introduced/removed
```

Example:

```amber
schema Customer:
 id as UUID
 name as Str
 email as Str? @pii
 renamed_from(:full_name) display_name as Str
 deprecated legacy_id as Int?
```

### 7.3. Codecs

```amber
codec = Json.codec(User)
user = codec.decode(body)
body = codec.encode(user)
```

Normative codec behavior:

- unknown field policy is schema-configurable: reject, preserve, ignore;
- missing required fields give `SchemaViolationError`;
- deprecated fields decode but may warn;
- encode respects wire names and redaction policy;
- binary codecs must include schema/version fingerprint unless host profile waives it.

### 7.4. Migrations

```amber
migration User v1 -> |old|:
 {
 id: old.id,
 name: old.name,
 email: null,
 created_at: old.created_at,
 legacy_id: old.legacy_id,
 }
```

Migrations must be deterministic unless annotated otherwise.

### 7.5. API contracts

HTTP/RPC boundary can be declared:

```amber
api Users:
 get "/users/{id}" -> User !{db}
 post "/users" CreateUser -> User !{db}
```

Tooling may generate OpenAPI-like description, client stubs, schema validators and contract tests.

## 8. Amber/Wasm Component Profile

### 8.1. Artifact

`.amberwasm` is an optional deployable artifact for frozen, sandboxed component execution.

Required properties:

```text
frozen-world only
no runtime class/mixin reopen
no define_method world mutation
explicit imports/exports
capability-bound host calls
schema-defined boundary values
no raw pointer FFI by default
```

### 8.2. Interface mapping

Amber exports:

```amber
export normalize

def normalize(row as Row) -> Row !{}:
 row.cleaned()
```

map to component interface entries:

```text
normalize: func(row: Row) -> Row
```

Complex values crossing the component boundary must be schema-defined or mapped to a supported primitive/record/list type.

### 8.3. Capabilities and WASI-style worlds

Host provides capability world:

```text
world analytics-plugin {
 import fs.read("/data")
 import clock.virtual
 export normalize
}
```

Amber does not require source syntax for WIT; toolchain may generate or consume WIT-like interface files.

### 8.4. Component lifecycle

Component instance has lifecycle:

```text
compiled -> verified -> instantiated -> running -> stopped
```

Component cannot mutate host dispatch-world. Any dynamic feature that would require world mutation must be rejected at build/load time.

## 9. Amber/Accelerator Profile

### 9.1. Types

```amber
xs = Tensor.f32([1.0, 2.0, 3.0], device::gpu)
ys = gpu.map(xs) |x| -> F32 !{}:
 x * 2.0
```

Normative runtime/library types:

```text
Tensor[T]
DeviceBuffer[T]
Device
DeviceStream
Kernel
KernelModule
```

### 9.2. Kernel subset

Allowed inside accelerator kernel:

- primitive numeric operations;
- boolean comparisons;
- local variables;
- structured control flow;
- tensor/buffer indexing;
- pure helper functions compiled into same kernel module;
- constants and scalar parameters.

Forbidden inside accelerator kernel:

- arbitrary object allocation;
- dynamic dispatch;
- reflection;
- `send` with dynamic selector;
- ordinary heap object access;
- exceptions crossing device boundary;
- FFI;
- filesystem/network/env/time/random;
- notebook watch operations.

### 9.3. Device memory

Host/device movement is explicit:

```amber
buf = DeviceBuffer.copy_from(bytes, device: gpu0)
view = buf.map_read() |slice|:
 slice[0]
buf.copy_to(host_buffer)
buf.destroy!()
```

Device buffers participate in lifetime profile. Use-after-destroy/dealloc follows existing lifetime errors or `AcceleratorError` if the fault is device-specific.

### 9.4. Effects

Accelerator operations carry effects:

```text
!{gpu}
!{gpu, mut}
!{gpu, async}
```

A host can deny GPU usage via capability profile even if code typechecks.

## 10. Amber/AI-Agent Tooling & Provenance Profile

### 10.1. Goal

AI agents will edit Amber projects. The language/toolchain must make machine edits checkable, attributable and reversible.

### 10.2. Semantic index

Required command:

```text
amber symbols --json
```

Minimum symbol graph fields:

```text
symbol_id
name
kind
module
visibility
source_span
defined_in
references[]
type_summary?
effect_summary?
schema_summary?
doc_summary?
```

### 10.3. Explain API

```text
amber explain path/to/file.amber --span 120:8 --json
```

Output must include:

```text
AST node
HIR node if available
binding resolution
type/effect info if enabled
diagnostics touching span
profile requirements
possible refactor actions
```

### 10.4. Structured patch protocol

Patch file:

```json
{
 "format": "amber.patch.v1",
 "intent": "rename_symbol",
 "operations": [
 {"op": "rename", "symbol_id": "...", "new_name": "customer_id"}
 ],
 "provenance": {
 "tool": "agent-name",
 "request_digest": "...",
 "capabilities": ["refactor", "format"]
 }
}
```

Required commands:

```text
amber patch check patch.json
amber patch apply patch.json
amber audit provenance
```

`patch check` must run parser/binder/profile validators before modifying files.

### 10.5. Provenance log

`.amberprov` records:

```text
patch id
author/tool identity
input request digest
files changed
symbols changed
diagnostics before/after
tests/checks run
artifact digests
human approval marker if required
```

Package publishing may require provenance digests in `.amberpkg` metadata.

## 11. Amber/Contracts & Property Testing Profile

### 11.1. Function contracts

```amber
def withdraw(account as Account, amount as Money) -> Account !{mut}:
 require amount > 0
 require account.balance >= amount

 result = account.debit(amount)

 ensure result.balance == old(account.balance) - amount
 result
```

`require` checks preconditions. `ensure` checks postconditions. `old(expr)` captures value at function entry; for mutable objects, profile must specify shallow/deep/snapshot policy.

### 11.2. Invariants

```amber
class Account:
 invariant @balance >= 0
```

Invariant check points are profile-configurable but must include public method entry/exit in strict contract mode.

### 11.3. Property tests

```amber
property "reverse twice gives original" |xs as Array[Int]|:
 expect xs.reverse().reverse() == xs
```

Property engine must record:

```text
seed
generator parameters
shrinker path
minimal counterexample if found
profile set
dependency fingerprints
```

### 11.4. Failure

Failed `require`, `ensure`, invariant or property test raises `ContractViolationError` with source span and diagnostic payload.

## 12. Amber/Privacy, Taint & Lineage Profile

### 12.1. Data labels

```amber
type Email = Str @pii
type ApiToken = Str @secret
```

Schema/table fields can carry labels:

```amber
schema User v1:
 id as UUID
 email as Str @pii
 country as Str
```

### 12.2. Taint propagation

Labels propagate through:

- assignment;
- object fields;
- schema encode/decode;
- table columns;
- joins and aggregations;
- string interpolation;
- logs/traces, unless redacted;
- API exports.

Profile may allow conservative propagation where exact propagation is expensive.

### 12.3. Export policy

```amber
policy PublicExport:
 deny label:pii
 deny label:secret
 allow aggregate(:country, min_group: 10)
```

```amber
def export_public(rows as Table) -> Csv !{fs.write}:
 require_policy PublicExport, rows
 Csv.write(rows)
```

Policy violation gives `PolicyViolationError`.

### 12.4. Redaction

Redaction must be explicit:

```amber
safe = users.redact(:email, with::hash)
```

Implicit redaction by logging/tracing is allowed only as host safety net; source-level redaction must remain visible in lineage.

### 12.5. Lineage graph

Lineage node shape:

```text
LineageNode(
 id,
 kind: source | transform | join | aggregate | notebook_cell | export,
 inputs[],
 output,
 schema_fingerprint,
 labels,
 source_span?,
 watch_revisions?,
 trace_span?
)
```

Lineage must connect to notebook watch/dependency capture when both profiles are enabled.

## 13. Amber/Durable Workflow Profile

### 13.1. Goal

Runtime tasks are ephemeral. Durable workflows survive process restart, deploy, retry and partial failure.

### 13.2. Surface form

```amber
workflow ImportOrders:
 step fetch !{net} retry: {max: 3, backoff::exponential}:
 http.get(source)

 step normalize !{}:
 transform(fetch.result)

 step commit !{db} idempotency_key: normalize.result.digest():
 db.insert_many(normalize.result)

 compensate commit !{db}:
 db.delete_batch(commit.result.batch_id)
```

`workflow`, `step` and `compensate` are profile-level declarative forms.

### 13.3. History

Workflow history records:

```text
workflow id
workflow version
step start/commit/failure/retry/compensation events
input/output digests
schema versions
effect grants
idempotency keys
timeouts/deadlines
trace ids
```

History is append-only. Replay reconstructs workflow state from history.

### 13.4. Determinism

A step must be deterministic relative to recorded inputs unless it declares effects and records external results.

During replay:

- already committed steps are not re-executed;
- their recorded outputs are supplied to dependent steps;
- divergent code/schema version requires migration policy or workflow version bump.

### 13.5. Error handling

Workflow violations give `WorkflowError`. Ordinary task exceptions inside a step are recorded as step failure and then processed by retry/compensation policy.

## 14. Cross-profile interaction rules

### 14.1. Capabilities + Effects

Effects are semantic declarations. Capabilities are permissions.

```amber
def f() !{net}:
 http.get("https://example.com")
```

This typechecks under effects profile, but still fails with `CapabilityError` if host did not grant `net.connect`.

### 14.2. Effects + Replay

Replay profile requires nondeterministic effects to be virtualized, recorded or denied.

```text
!{time} -> virtual clock or recorded clock values
!{random} -> seeded/recorded random source
!{net} -> recorded response provider or denied in deterministic mode
```

### 14.3. Watch + DataFrame + Lineage

Notebook invalidation chooses the most precise active dependency key:

```text
binding-level
object-level
ivar-level
column-level
query-plan-level
lineage-source-level
```

Hosts may conservatively widen dependencies but must not miss invalidation.

### 14.4. Schema + Privacy

Schema labels feed taint propagation. Codec/export boundaries must preserve labels or explicitly redact/drop them.

### 14.5. Wasm + Capabilities + Schema

Wasm component boundary must be schema-described and capability-limited. If schema profile is disabled, component boundary is limited to primitive and simple aggregate types supported by component ABI.

### 14.6. Accelerator + Effects + Memory

Accelerator profile reuses `Buffer[T]`, `Slice[T]`, pinning and explicit lifetime model. Device kernels must have restricted effects and cannot access ordinary heap objects.

### 14.7. AI-agent tooling + all profiles

Semantic index and patch protocol must include active profile information so agent edits cannot accidentally erase capabilities, effects, privacy labels, contracts or schema versions.

## 15. HIR and bytecode additions 

### 15.1. HIR nodes

Optional HIR families:

```text
HCapabilityCheck(capability, attributes)
HCapabilityScope(grants, body)
HEffectBoundary(effect_row, body)
HTraceSpan(name, attrs, body)
HTraceEvent(name, attrs)
HDeterministicScope(options, body)
HReplayBoundary(trace_ref, body)
HSchemaDef(name, version, fields, metadata)
HSchemaCodec(schema_ref, codec_kind)
HTablePlan(op, inputs, args, effect_row?)
HColumnDependency(table_ref, column_ref)
HContractRequire(expr)
HContractEnsure(expr, old_captures)
HInvariant(expr)
HPropertyTest(name, generators, body)
HTaintLabel(expr, labels)
HPolicyCheck(policy_ref, value)
HLineageNode(kind, inputs, output, metadata)
HWasmExport(name, signature)
HWasmImport(name, signature, capability)
HAcceleratorKernel(params, body_ir, device_requirements)
HWorkflowDef(name, steps, compensation)
HWorkflowStep(name, effect_row, policy, body)
HAgentPatchMetadata(metadata)
```

### 15.2. Bytecode / `.amberbc` optional sections

Core bytecode добавляет `MAKE_MAP_DYN(dst, count, key_reg, value_reg,...)`
для map literals с non-symbol или expression keys. Legacy symbol-only literals
сохраняют `MAKE_MAP(dst, count, symbol_id, value_reg,...)`, чтобы старый
байткод и fast path оставались совместимыми.

Core lowering добавляет HIR-level формы для spread:

```text
HSpreadArg(expr)
HKwargSpreadArg(expr)
HSpreadElement(expr)
HMapSpreadEntry(expr)
```

Реализация может понижать их через новые helper opcodes/runtime calls
(`CALL_SPREAD`, `SEND_SPREAD`, `KWARGS_VIEW`, `KWARGS_MERGE`) или через
эквивалентную staged assembly поверх существующего call packet. Observable
contract: left-to-right evaluation, eager finite spread, validation-based
keyword conversion, duplicate keyword detection after normalization.

Core lowering также различает ordinary and strict map construction.
Ordinary `MAKE_MAP` / `MAKE_MAP_DYN` use name-key normalization for
`Symbol`/`Str` textual keys; strict map construction must preserve exact key
identity and may use a strict flag, strict helper opcode, or constructor-side
metadata. Existing bytecode without strict metadata remains ordinary-map
bytecode.

Optional sections:

```text
CAPS capability requirements and grants metadata
EFCT effect summaries per callable/site
OBSV event schemas and trace site table
RPLY replay/determinism metadata
SCMA schema definitions and migration table
TABL table/query-plan metadata
LINE lineage metadata anchors
PRIV taint labels and policy ids
CNTR contracts/property-test metadata
WASM component import/export mapping
ACCL accelerator kernel descriptors
WFLW durable workflow descriptors
PROV provenance/agent patch metadata
```

Verifier rules:

- unknown optional section with `required=false` may be ignored;
- unknown optional section with `required=true` must reject artifact;
- section digest participates in package artifact digest;
- profile-specific sections must reference stable symbol/source ids.

### 15.3. Runtime support table

Runtime advertises profile support:

```text
amber runtime profiles --json
```

Example:

```json
{
 "capabilities": true,
 "effects": "check+runtime",
 "observability": true,
 "replay": true,
 "columnar_bi": false,
 "schema": true,
 "wasm_component": false,
 "accelerator": {"gpu": false, "simd": true},
 "agent_tooling": true,
 "contracts": "runtime",
 "privacy": false,
 "workflow": false
}
```

## 16. CLI additions 

Required or recommended commands by profile:

```text
amber profiles list
amber capabilities check amber.toml
amber effects check src/
amber trace run --out run.ambertrace
amber replay run.ambertrace
amber schema check
amber schema emit-openapi
amber table explain query.amber
amber wasm build
amber accel check
amber symbols --json
amber explain path --span LINE:COL --json
amber patch check patch.json
amber patch apply patch.json
amber audit provenance
amber contract test
amber privacy audit
amber workflow run
amber workflow replay
```

Toolchains may group these under subcommands, but machine-readable JSON output is required for agent/tool integrations where applicable.

## 17. Conformance lanes 

 adds optional conformance lanes:

```text
tests/profiles/capabilities/
tests/profiles/effects/
tests/profiles/observability/
tests/profiles/replay/
tests/profiles/table/
tests/profiles/schema/
tests/profiles/wasm/
tests/profiles/accelerator/
tests/profiles/agent_tooling/
tests/profiles/contracts/
tests/profiles/privacy/
tests/profiles/workflow/
```

Minimum positive tests:

- capability grant allows permitted filesystem path and denies outside path;
- pure function accepted where `!{}` required;
- `trace.span` emits nested span events with task/strand metadata;
- deterministic scope replays same random/clock values;
- table query plan has stable fingerprint and column dependency keys;
- schema codec accepts valid value and rejects missing required field;
- Wasm component build rejects world mutation;
- accelerator checker rejects dynamic dispatch inside kernel;
- semantic patch rename preserves bindings;
- property test records seed and counterexample;
- taint label blocks public export without redaction;
- workflow replay skips already committed step.

Minimum negative tests:

- no capability -> `CapabilityError`;
- effect row mismatch -> compile diagnostic or `EffectViolationError`;
- real clock in deterministic scope without provider -> `DeterminismError`;
- replay event mismatch -> `ReplayDivergenceError`;
- schema incompatible migration -> `SchemaViolationError`;
- non-schema object crosses Wasm boundary in strict profile -> load diagnostic;
- GPU kernel captures ordinary heap object -> `AcceleratorError` or compile diagnostic;
- patch references stale symbol id -> patch rejection;
- failed `ensure` -> `ContractViolationError`;
- PII export without policy -> `PolicyViolationError`;
- workflow step re-execution conflicts with committed idempotency key -> `WorkflowError`.

## 18. Development matrix updates 

 extends the existing implementation matrix with P4/P5 tracks. These do not block P0/P1 dynamic runtime.

### P4 — platform safety, observability and data profiles

| Трек | Состояние | Ближайший шаг | Зависимости | Критерий выхода |
|---|---|---|---|---|
| G16. Capabilities & sandbox | Закрыто на уровне profile | Реализовать manifest parser, capability resolver, runtime checks and `CapabilityError` | G6e, G11 | Package/plugin/notebook contexts deny host resources by default |
| G17. Effects checker | Закрыто на уровне profile | Добавить effect rows в typed checker, HIR summaries and call-site validation | G10, G16 | Pure/effectful boundaries диагностируются воспроизводимо |
| G18. Observability & replay | Закрыто на уровне profile | Реализовать event schema, trace spans, `.ambertrace`, deterministic scheduler mode | G7, G13, G16 | CI может записать run и воспроизвести его до первого divergence |
| G19. Schema/API contracts | Закрыто на уровне profile | Реализовать `schema`, codecs, migrations and API description generator | G10, G17 | Encode/decode/API boundaries валидируются schema-first |
| G20. DataFrame/Columnar BI | Закрыто на уровне profile | Реализовать `Table`, `Column`, `LazyTable`, query fingerprints and watch integration | G9, G18, G19 | Notebook invalidation работает на column/query-plan granularity |
| G21. Privacy/Taint/Lineage | Закрыто на уровне profile | Реализовать labels, policy checks, lineage graph and export audit | G18, G19, G20 | Sensitive data export блокируется или требует explicit redaction |

### P5 — portability, accelerators, agent tooling and workflows

| Трек | Состояние | Ближайший шаг | Зависимости | Критерий выхода |
|---|---|---|---|---|
| G22. Wasm Component | Закрыто на уровне profile | Реализовать frozen subset checker, component interface mapping and capability host imports | G14, G16, G19 | `.amberwasm` plugin исполняется sandboxed без world mutation |
| G23. Accelerator | Закрыто на уровне profile | Реализовать kernel subset checker, tensor/device buffer runtime and CPU/SIMD fallback | G6d, G15, G17 | Kernel checker rejects dynamic Amber features and runs supported numeric kernels |
| G24. AI-agent tooling/provenance | Закрыто на уровне profile | Реализовать symbol graph, explain JSON, structured patch protocol and `.amberprov` | G1-G5, G10, G17 | Agent patches проверяются semantic-first before apply |
| G25. Contracts/property testing | Закрыто на уровне profile | Реализовать `require/ensure/invariant/property`, generators and shrinkers | G10, G18 | Failed contract/property yields replayable diagnostic |
| G26. Durable workflow | Закрыто на уровне profile | Реализовать workflow history, step replay, idempotency and compensation | G16-G19, G25 | Workflow survives restart and replays committed steps correctly |

## 19. Ненормативные внешние ориентиры

Этот список не является dependency Amber. Он фиксирует external design references, на которые consciously ориентируется концептуально:

- SLSA: supply-chain levels / provenance / artifact assurance — https://slsa.dev/
- OpenTelemetry: traces, metrics, logs and context propagation — https://opentelemetry.io/
- W3C Trace Context: standard HTTP headers for distributed trace context — https://www.w3.org/TR/trace-context/
- Apache Arrow: language-independent columnar memory format — https://arrow.apache.org/
- OpenAPI Specification: language-agnostic HTTP API description — https://swagger.io/specification/
- WebAssembly Component Model — https://component-model.bytecodealliance.org/
- WASI Preview 2 / WIT / component model direction — https://github.com/WebAssembly/WASI/blob/main/docs/Preview2.md
- NIST AI Risk Management Framework — https://www.nist.gov/itl/ai-risk-management-framework
- eBPF concept for low-level observability/security probes — https://ebpf.io/

Amber does not copy these specifications. It uses them as pressure tests for Amber's own profile boundaries.

## 20. Итоговый статус 

После Amber имеет три слоя зрелости:

```text
Core language and reference runtime contracts: closed for implementation.
Second-wave compiler/runtime profiles -: closed for implementation.
Modern platform profiles: closed as optional profile specifications, implementation order remains product/host-driven.
```

 делает Amber не только dynamic/typed/no-GIL/compiled language, но и platform-oriented language: безопасно запускаемый, объяснимый, воспроизводимый, пригодный для BI/data, переносимый в sandboxed components, готовый к AI-agent tooling and auditable enterprise workflows.

# Часть II. Интегрированные языковые решения

## Inline conditional expression и conditional collection elements


---

#### 0. Обзор решения

В Amber фиксируются две новые core-level surface-возможности:

1. **Inline conditional expression** как современная замена C-style ternary operator.
2. **Conditional collection element** для литералов `Array`, `Set` и `Map`.

Amber **не вводит** форму:

```amber
cond ? consequent: alternative
```

Вместо неё канонической inline-формой условного выражения становится:

```amber
if condition then consequent else alternative
```

Для коллекций фиксируется trailing-condition syntax:

```amber
[
 base,
 extra if enabled?,
]

{:read,:write if can_write?,
}

{
 a: 1,
 b: 2 if x == 3,
 c: 3,
}
```

Trailing condition управляет **наличием элемента в коллекции**, а не значением элемента.

---

#### 1. Мотивация

Amber уже использует Ruby-like имена методов с суффиксом `?`:

```amber
empty?
present?
valid?
question_method?
```

Поэтому классический тернарный оператор:

```amber
obj.question_method? ? x: y
```

создаёт плохую визуальную модель:

- рядом оказываются два `?` с разной семантикой;
- `?` уже является частью идентификатора метода;
- safe navigation уже использует отдельную форму `.?.`;
- C-style ternary выглядит чужеродно относительно expression-oriented `if`.

Так как `if` в Amber уже является выражением, inline conditional должен быть развитием той же модели, а не отдельным оператором из C/JS-семейства.

Conditional collection elements закрывают другой частый кейс: условное включение элемента или map-entry без промежуточного builder-кода.

До расширения приходилось писать:

```amber
payload = {
 a: 1,
 c: 3,
}

if x == 3:
 payload[:b] = 2

return payload
```

После расширения:

```amber
return {
 a: 1,
 b: 2 if x == 3,
 c: 3,
}
```

---

#### 2. Inline conditional expression

##### 2.1. Каноническая форма

```amber
if condition then consequent else alternative
```

Примеры:

```amber
status = if user.admin? then:admin else:user

label = if item.archived? then "archived" else "active"

price = if discount.present? then discounted_price else base_price

result = if obj.question_method? then x else y
```

##### 2.2. Запрещённая форма

Amber не поддерживает C-style ternary:

```amber
result = obj.question_method? ? x: y # invalid
```

Это должно диагностироваться как syntax error с рекомендацией использовать inline `if... then... else...`.

Suggested diagnostic:

```text
AMB-SYN-INLINE-TERNARY-CSTYLE
C-style ternary operator is not supported in Amber; use `if condition then consequent else alternative`.
```

##### 2.3. `then` как contextual keyword

`then` является contextual keyword только внутри inline conditional expression после `if <condition>`.

В обычных identifier-position использование имени `then` не меняется, если это уже разрешено базовыми правилами идентификаторов и не создаёт локального конфликта parser-а.

##### 2.4. `else` обязательно

Inline conditional expression всегда обязан иметь `else`.

Валидно:

```amber
value = if cond then a else b
```

Невалидно:

```amber
value = if cond then a
```

Причина: inline conditional предназначен для value-producing expression. Отсутствие `else` в компактной форме часто скрывает ошибку. Для условного выполнения без альтернативы следует использовать block-form `if`.

##### 2.5. Семантика вычисления

```amber
if condition then consequent else alternative
```

Вычисляется так:

1. вычисляется `condition`;
2. если `condition` truthy, вычисляется только `consequent`;
3. иначе вычисляется только `alternative`;
4. результатом выражения является значение выбранной ветки.

Truthiness совпадает с общей моделью Amber:

- falsy: только `false` и `null`;
- всё остальное truthy.

##### 2.6. Ассоциативность и приоритет

Inline conditional expression является `PrimaryExpr`/control expression family, как block-form `IfExpr`.

Рекомендуемое parser-правило:

```ebnf
InlineIfExpr::= "if" Expr "then" Expr "else" Expr
```

`InlineIfExpr` не является infix-оператором и не добавляет нового уровня precedence. Он парсится как отдельная expression-form, аналогично другим control expressions.

---

#### 3. Conditional collection elements

##### 3.1. Общая идея

Conditional collection element — это элемент литерала коллекции с trailing condition:

```amber
element if condition
element unless condition
```

Для `Map` условной единицей является entry:

```amber
key: value if condition
key: value unless condition
```

Если condition не проходит, элемент/entry отсутствует в результирующей коллекции.

---

#### 4. Conditional elements в `Array`

##### 4.1. Syntax

```amber
[
 base,
 extra if enabled?,
 fallback,
]
```

##### 4.2. Семантика

```amber
[
 a,
 b if cond,
 c,
]
```

эквивалентно последовательному построению:

```amber
tmp = []
tmp.push(a)

if cond:
 tmp.push(b)

tmp.push(c)
tmp
```

`b` не вычисляется, если `cond` falsy.

##### 4.3. Примеры

```amber
args = [
 "--verbose" if verbose?,
 "--dry-run" if dry_run?,
 input_path,
]

items = [
 required_item,
 optional_item if optional_item.present?,
]
```

---

#### 5. Conditional elements в `Set`

##### 5.1. Syntax

```amber
{:read,:write if can_write?,:admin unless restricted?,
}
```

Эта форма применяется к set-literal, то есть к `{...}`, где top-level элементы не являются `key: value` entries.

##### 5.2. Семантика

```amber
{
 a,
 b if cond,
 c,
}
```

эквивалентно:

```amber
tmp = Set.new()
tmp.add(a)

if cond:
 tmp.add(b)

tmp.add(c)
tmp
```

`b` не вычисляется, если `cond` falsy.

Повторные элементы схлопываются по обычной runtime-семантике `Set`.

##### 5.3. Примеры

```amber
permissions = {:read,:write if user.editor?,:admin if user.admin?,
}

features = {:base,:cache unless env.test?,
}
```

---

#### 6. Conditional entries в `Map`

##### 6.1. Syntax

```amber
{
 a: 1,
 b: 2 if x == 3,
 c: 3,
}
```

Допускается также `unless`:

```amber
{
 cache: true unless env.test?,
 debug: true if env.debug?,
}
```

##### 6.2. Семантика

```amber
{
 a: 1,
 b: 2 if cond,
 c: 3,
}
```

эквивалентно:

```amber
tmp = {}
tmp[:a] = 1

if cond:
 tmp[:b] = 2

tmp[:c] = 3
tmp
```

Если `cond` falsy:

- ключ `:b` отсутствует;
- value expression `2` не вычисляется.

##### 6.3. Conditional entry не равен conditional value

Эта форма:

```amber
{
 b: 2 if cond,
}
```

означает условное наличие key/value-entry.

Она не равна:

```amber
{
 b: if cond then 2 else null,
}
```

Во второй форме ключ `:b` присутствует всегда.

##### 6.4. Duplicate keys

Повторные ключи сохраняют существующее правило `Map`: более поздняя вставка заменяет более раннюю.

```amber
{
 mode::default,
 mode::debug if debug?,
}
```

Если `debug?` truthy, итоговое значение `mode` — `:debug`.

Если `debug?` falsy, итоговое значение `mode` — `:default`.

---

#### 7. `if` и `unless` в conditional elements

Для всех collection literal kinds поддерживаются оба suffix-а:

```amber
element if condition
element unless condition
key: value if condition
key: value unless condition
```

Семантика:

```amber
x unless cond
```

эквивалентно:

```amber
x if not cond
```

Однако AST должен сохранять исходный `kind = if | unless`, чтобы formatter, diagnostics и source maps могли воспроизвести surface-form.

---

#### 8. Порядок вычисления

Collection literal вычисляется слева направо.

Для conditional element:

```amber
[
 a(),
 b() if cond(),
 c(),
]
```

порядок такой:

1. вычислить `a()`;
2. добавить результат `a()`;
3. вычислить `cond()`;
4. если `cond()` truthy, вычислить `b()` и добавить результат;
5. вычислить `c()`;
6. добавить результат `c()`.

Важно: element/value expression не вычисляется, если condition не прошёл.

Для `Map`:

```amber
{
 a: f(),
 b: g() if cond(),
 c: h(),
}
```

порядок такой:

1. вычислить key `:a`;
2. вычислить `f()`;
3. вставить `:a => f()`;
4. вычислить condition `cond()`;
5. если condition truthy:
 - вычислить key `:b`;
 - вычислить `g()`;
 - вставить `:b => g()`;
6. вычислить key `:c`;
7. вычислить `h()`;
8. вставить `:c => h()`.

Для literal-symbol keys вида `a:` key construction считается compile-time/literal-level операцией, но наблюдаемый порядок value/condition evaluation остаётся как выше.

---

#### 9. Grammar sketch

##### 9.1. Inline conditional

```ebnf
PrimaryExpr::= Literal
 | Name
 | "@" Name
 | "@@" Name
 | "(" Expr ")"
 | ListLiteral
 | SetLiteral
 | MapLiteral
 | IfExpr
 | InlineIfExpr
 | UnlessExpr
 | CaseExpr
 | WhileExpr
 | UntilExpr
 | DoWhileExpr
 | LoopExpr
 | DefExpr
 | ClassExpr
 | MixinExpr

InlineIfExpr::= "if" Expr "then" Expr "else" Expr
```

Implementation note: parser may fold `InlineIfExpr` into existing `IfExpr` AST family with `form = inline`, but AST must preserve that the source used inline spelling.

##### 9.2. Collection literals

```ebnf
ListLiteral::= "[" [ CollectionElementList ] "]"

SetLiteral::= "{" SetElementList "}"

MapLiteral::= "{" [ MapEntryList ] "}"

CollectionElementList::= ConditionalElement { "," ConditionalElement } [ "," ]

SetElementList::= ConditionalElement { "," ConditionalElement } [ "," ]

ConditionalElement::= Expr CollectionCondition?

CollectionCondition::= "if" Expr
 | "unless" Expr

MapEntryList::= ConditionalMapEntry { "," ConditionalMapEntry } [ "," ]

ConditionalMapEntry::= MapKey ":" Expr CollectionCondition?

MapKey::= Name
 | SymbolLiteral
 | StringLiteral
 | Expr
```

##### 9.3. Disambiguation rule

Inside collection literals, a top-level trailing `if`/`unless` after an element or map value belongs to the collection element/entry.

Therefore:

```amber
{
 b: 2 if cond,
}
```

means:

```text
conditional map entry
```

not:

```text
map entry with conditional value
```

To express a conditional value, use explicit inline conditional:

```amber
{
 b: if cond then 2 else null,
}
```

or parenthesize a nested expression if the grammar later introduces any value-level postfix conditional form.

---

#### 10. AST extension

Recommended AST additions:

```text
AstInlineIfExpr(
 condition,
 consequent,
 alternative,
 span,
 form = "inline"
)

AstCollectionCondition(
 kind, # "if" | "unless"
 expr,
 span
)

AstArrayElement(
 expr,
 condition? # null | AstCollectionCondition
)

AstSetElement(
 expr,
 condition? # null | AstCollectionCondition
)

AstMapEntry(
 key_expr,
 value_expr,
 condition?, # null | AstCollectionCondition
 span
)
```

AST must remain syntax-faithful:

- inline conditional should not be parsed as a generic method call;
- conditional elements should not be flattened during parsing;
- `if` vs `unless` should be preserved;
- source spans should cover both the element/value and the trailing condition.

---

#### 11. HIR lowering

##### 11.1. Inline conditional

```amber
if cond then a else b
```

lowers to:

```text
HIfExpr(
 condition = lower(cond),
 then_expr = lower(a),
 else_expr = lower(b)
)
```

No new runtime primitive is required.

##### 11.2. Array conditional element

```amber
[
 a,
 b if cond,
 c,
]
```

lowers conceptually to:

```text
tmp = HArrayNew()
HArrayPush(tmp, lower(a))

HIf(
 lower(cond),
 HArrayPush(tmp, lower(b)),
 HNoop()
)

HArrayPush(tmp, lower(c))
tmp
```

##### 11.3. Set conditional element

```amber
{
 a,
 b if cond,
 c,
}
```

lowers conceptually to:

```text
tmp = HSetNew()
HSetAdd(tmp, lower(a))

HIf(
 lower(cond),
 HSetAdd(tmp, lower(b)),
 HNoop()
)

HSetAdd(tmp, lower(c))
tmp
```

##### 11.4. Map conditional entry

```amber
{
 a: 1,
 b: 2 if cond,
 c: 3,
}
```

lowers conceptually to:

```text
tmp = HMapNew()
HMapPut(tmp,:a, lower(1))

HIf(
 lower(cond),
 HMapPut(tmp,:b, lower(2)),
 HNoop()
)

HMapPut(tmp,:c, lower(3))
tmp
```

No dedicated bytecode opcode is required. Existing branch/jump instructions and collection mutation/build instructions are sufficient.

---

#### 12. Diagnostics

##### 12.1. C-style ternary rejected

```amber
x = cond ? a: b
```

Diagnostic:

```text
AMB-SYN-INLINE-TERNARY-CSTYLE
C-style ternary operator is not supported; use `if cond then a else b`.
```

##### 12.2. Missing `else` in inline conditional

```amber
x = if cond then a
```

Diagnostic:

```text
AMB-SYN-INLINE-IF-MISSING-ELSE
Inline conditional expression requires `else`.
```

##### 12.3. Dangling condition without collection element

```amber
[
 if cond,
]
```

Diagnostic:

```text
AMB-SYN-CONDITIONAL-ELEMENT-MISSING-VALUE
Conditional collection element requires a value before `if` or `unless`.
```

##### 12.4. Ambiguous conditional map value

This is not an error:

```amber
{
 b: 2 if cond,
}
```

It is specified as conditional entry.

Formatter and diagnostics should help users when intent appears to be conditional value:

```amber
{
 b: if cond then 2 else null,
}
```

---

#### 13. Formatter rules

Recommended formatting:

```amber
value = if cond then a else b
```

For long branches:

```amber
value =
 if cond then
 a
 else
 b
```

or, if the language formatter does not support multiline inline conditional, it should prefer the existing block-form:

```amber
value = if cond:
 a
else:
 b
```

Collection elements preserve trailing condition on the same line when short:

```amber
[
 base,
 extra if enabled?,
 fallback,
]
```

For long conditions, formatter may break after the value:

```amber
[
 extra
 if feature.enabled? and user.allowed?,
]
```

But the recommended v1 formatter policy is to keep conditional elements simple and line-local.

---

#### 14. Conformance tests

Minimum parser-positive cases:

```amber
x = if ready? then a else b

arr = [
 1,
 2 if include_two?,
 3,
]

set = {:read,:write if can_write?,
}

map = {
 a: 1,
 b: 2 if x == 3,
 c: 3,
}
```

Minimum parser-negative cases:

```amber
x = cond ? a: b

x = if cond then a

arr = [
 if cond,
]
```

Minimum runtime cases:

```amber
log = []

def mark(x):
 log.push(x)
 x

arr = [
 mark(1),
 mark(2) if false,
 mark(3),
]

### arr == [1, 3]
### log == [1, 3]
```

```amber
m = {
 a: 1,
 b: fail() if false,
 c: 3,
}

### m == {a: 1, c: 3}
### fail() is not called
```

```amber
m = {
 mode::default,
 mode::debug if true,
}

### m[:mode] ==:debug
```

---

#### 15. Acceptance decision

This section is accepted with the following final decisions:

1. C-style ternary `cond ? a: b` is not part of Amber.
2. Inline conditional expression is fixed as:

 ```amber
 if condition then consequent else alternative
 ```

3. `else` is mandatory in inline conditional expression.
4. Conditional collection element syntax is fixed for all collection literal kinds:
 - `Array`
 - `Set`
 - `Map`
5. The recommended syntax is trailing `if` / `unless`:

 ```amber
 element if condition
 element unless condition
 key: value if condition
 key: value unless condition
 ```

6. Conditional collection syntax controls presence of the element/entry, not conditional value.
7. Conditional element/value expression is not evaluated if the condition is not satisfied.
8. AST preserves the surface form; HIR lowers to ordinary branch-based construction.
9. No new VM or bytecode primitive is required.

## Type conversion sugar и string interpolation


---

#### 0. Обзор решения

This section is intended to be inserted into the main Amber specification as a small language-level extension after the existing sections on postfix expressions, callable class objects, `as TypeTerm`, and string/profile semantics.

The patch preserves the following existing design decisions:

1. `Class(args...)` is the preferred constructor-call form and is semantically equivalent to `Class.new(args...)`.
2. `expr as TypeTerm` is a runtime type assertion/check boundary, not a conversion operation.
3. Parser output remains syntax-faithful; surface forms such as string interpolation must not be erased in the parser.
4. HIR is the semantic-core representation and must lower interpolation and conversion sugar into explicit operations.
5. Deterministic diagnostics, stack traces, disassembly and golden outputs must not expose raw memory addresses.
6. Privacy/Taint profile labels propagate through string interpolation.

---

#### 1. Terminology

Amber distinguishes four related but separate operations:

| Operation | Surface form | Meaning |
|---|---|---|
| Type assertion | `expr as TypeTerm` | Checks that the value already satisfies the type term. Does not convert. |
| Constructor/conversion call | `TargetType(value)` | Ordinary class-object call through the existing constructor/callable path. |
| Cast protocol | `value.cast(TargetType)` | Explicit runtime conversion protocol. |
| Sugar conversion method | `value.to_int()`, `value.to_str()`, etc. | Stdlib convenience wrapper around the cast/stringification protocol. |

Normative rule:

```amber
x as Int # assertion/check only
Int(x) # explicit construction/conversion
x.cast(Int) # explicit protocol conversion
x.to_int() # stdlib sugar for x.cast(Int)
```

`as` must never parse, coerce, stringify, truncate or otherwise transform the value.

---

#### 2. Type assertion semantics

`expr as TypeTerm` performs a runtime type assertion. It lowers to a `TypeCheckProgram` and uses the normal type-check semantics for `TypeTerm`.

Examples:

```amber
x = "123"

x as Str # ok, result is x
x as Int # TypeError
x as Int? # TypeError unless x is Int or null
```

The result of a successful assertion is the original value, not a copy or converted value.

```amber
user2 = user as User
user2.object_id == user.object_id # true, subject to object_id availability/profile
```

---

#### 3. Explicit conversion: `TargetType(value)`

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
 class_method def cast(value):...

 class_method def parse(str as Str):...
```

Recommended convention:

| Method | Intended meaning |
|---|---|
| `TargetType.cast(value)` | General explicit conversion from an already materialized Amber value. |
| `TargetType.parse(str as Str)` | Text parsing with syntax/format validation. |
| `TargetType.new(...)` / `TargetType(...)` | Construction path, usually allowed to delegate to `cast` for single-argument cases. |

For builtin scalar types, `TargetType(value)` should delegate to `TargetType.cast(value)` when called with exactly one positional argument and no block, unless the type's constructor has a more specific normative behavior.

---

#### 4. Cast protocol

##### 4.1. Canonical form

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
tensor.to("cuda:0") # domain-specific device transfer
tensor.cast(Float32) # type/dtype conversion
edge.to(node) # graph/domain API
```

##### 4.2. Failure

If the target type cannot perform the conversion, the operation raises `TypeError` or a more specific subtype such as `ValueError` when the source type is acceptable but the value content is invalid.

Examples:

```amber
"abc".cast(Int) # ValueError or TypeError, depending on stdlib error taxonomy
[].cast(Int) # TypeError
```

Recommended distinction:

| Situation | Error |
|---|---|
| Source type/category unsupported | `TypeError` |
| Source category supported, content invalid | `ValueError` |
| Runtime bytes/string encoding invalid at boundary | `EncodingError` |

If the current runtime error registry does not include `ValueError`, this section adds it.

##### 4.3. Nullable/non-raising cast

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

#### 5. Sugar conversion methods for standard types

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

##### 5.1. Canonical mapping

| Sugar method | Canonical expansion |
|---|---|
| `value.to_str()` | `Amber.stringify(value, mode::display)` |
| `value.to_int()` | `value.cast(Int)` |
| `value.to_float()` | `value.cast(Float)` |
| `value.to_bool()` | `value.cast(Bool)` |
| `value.to_symbol()` | `value.cast(Symbol)` |
| `value.to_array()` | `value.cast(Array)` |
| `value.to_tuple()` | `value.cast(Tuple)` |
| `value.to_set()` | `value.cast(Set)` |
| `value.to_map()` | `value.cast(Map)` |

The sugar methods are ordinary methods and participate in normal dispatch, method lookup, open-world invalidation and frozen-world rules.

##### 5.2. Method naming

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

##### 5.3. Standard scalar conversions

###### `to_str`

`value.to_str()` uses display stringification.

It must return `Str`.

For `Str`, it returns the receiver unchanged.

```amber
"abc".to_str() # "abc"
42.to_str() # "42"
null.to_str() # "null"
```

###### `to_int`

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
3.0.to_int() # 3
3.5.to_int() # ValueError unless stdlib explicitly chooses truncation
"42".to_int() # 42
"04".to_int() # 4, if canonical parser accepts leading zeroes
"4.2".to_int() # ValueError
```

If truncation is desired, stdlib should expose a separate spelling:

```amber
x.trunc()
x.floor()
x.ceil()
x.round()
```

###### `to_float`

Recommended builtin behavior:

| Receiver | Result |
|---|---|
| `Float` | receiver unchanged |
| `Int` | exact or nearest representable Float |
| `Str` | parse canonical floating syntax |
| `Bool` | rejected |
| `null` | `TypeError` |

###### `to_bool`

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
0.to_bool() # TypeError
"".to_bool() # ValueError
"false".to_bool() # false
```

###### `to_symbol`

Recommended builtin behavior:

| Receiver | Result |
|---|---|
| `Symbol` | receiver unchanged |
| `Str` | symbol with the same name, if valid |
| Other | `TypeError` |

##### 5.4. Collection conversions

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
range.to_array() # ok for finite range
lazy_seq.to_array() # ok only if finite/realizable
infinite.to_array() # RuntimeError or specific InfiniteCollectionError if available
```

##### 5.5. computed-property aliases

As a back-update from the computed-property patch, also exposes
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

#### 6. Stringification protocol

String conversion for display is not identical to `Str(value)` construction in all cases. Amber therefore defines a stringification hook.

##### 6.1. `Amber.stringify`

Canonical operation:

```amber
Amber.stringify(value, mode::display)
```

Rules:

1. If `value` is `Str`, return it unchanged.
2. Else if `value` responds to `to_str`, call `value.to_str()` and require a `Str` result.
3. Else if `value` responds to `inspect`, call `value.inspect()` and require a `Str` result.
4. Else produce a deterministic object representation that does not include raw memory addresses.

The fallback representation should be stable for golden tests and stack traces. It may include class name and a deterministic object id only if that id is explicitly defined by the runtime and normalized in deterministic outputs.

##### 6.2. `to_str` versus `inspect`

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

#### 7. Interpolated string literals

##### 7.1. Syntax

Amber supports interpolation inside double-quoted string literals:

```amber
"Hello, #{name}"
"sum = #{a + b}"
"user = #{user.inspect()}"
```

Interpolation grammar:

```ebnf
InterpolatedString::= '"' StringPart* '"'
StringPart::= TextChunk
 | EscapeSequence
 | Interpolation
Interpolation::= "#{" Expr "}"
```

`Expr` is the ordinary Amber expression grammar. Newlines do not terminate the expression while inside interpolation delimiters.

Empty interpolation is invalid:

```amber
"#{}" # invalid
"#{ }" # invalid
```

##### 7.2. Escapes

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

##### 7.3. Evaluation order

Interpolation parts are evaluated left-to-right.

```amber
"#{a()} #{b()} #{c()}"
```

is observationally equivalent to evaluating `a()`, stringifying it, then `b()`, stringifying it, then `c()`, stringifying it, and finally concatenating/building the result.

Embedded expressions are ordinary expression evaluations in the current scope. They update `$_` according to the existing `$_` rules. The final string expression then updates `$_` to the completed string.

Example:

```amber
s = "#{1 + 2} #{$_}"
### second interpolation observes $_ == 3
### s == "3 3"
```

##### 7.4. Stringification in interpolation

Each embedded value is converted by:

```amber
Amber.stringify(value, mode::display)
```

Therefore:

```amber
"#{x}"
```

is equivalent to:

```amber
Amber.stringify(x, mode::display)
```

inside the string builder.

---

#### 8. AST and HIR

##### 8.1. AST

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

##### 8.2. HIR

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

##### 8.3. Bytecode

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
builder.append(Amber.stringify(name, mode::display))
builder.append("!")
builder.finish()
```

---

#### 9. Privacy, taint and lineage

If the Amber/Privacy, Taint & Lineage profile is enabled, string interpolation propagates labels from every interpolated value into the result string.

```amber
email as Str @pii
msg = "User email: #{email}"
### msg carries @pii
```

Literal text itself may contribute source/location metadata but normally carries no privacy label unless a host policy assigns one.

Logs/traces using interpolated strings must observe the same policy checks as any other labeled string. Policy violation raises `PolicyViolationError`.

---

#### 10. Diagnostics and runtime errors

##### 10.1. Compile-time diagnostics

This section adds or standardizes:

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

##### 10.2. Runtime errors

This section uses existing errors where possible:

| Error | Use |
|---|---|
| `TypeError` | Unsupported cast, wrong target type, `to_str`/`inspect` returning non-`Str`. |
| `EncodingError` | Invalid runtime string/buffer encoding at a boundary. |
| `PolicyViolationError` | Privacy/taint export/logging violation. |

This section adds, if absent:

```text
ValueError
```

Use `ValueError` when the source value has an acceptable type/category but invalid content for the requested conversion.

Examples:

```amber
"abc".to_int() # ValueError
[].to_int() # TypeError
```

---

#### 11. Standard library contract

##### 11.1. Required methods

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

Amber.stringify(value, mode::display)
```

A minimal implementation may place the `to_*` methods in a conversion mixin included by `Object`, provided ordinary method lookup observes them as object methods.

##### 11.2. Required class-side cast methods

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

##### 11.3. Optional aliases

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

#### 12. Examples

##### 12.1. Environment parsing

```amber
port = env["PORT"].cast?(Int) or 5432
debug = env["DEBUG"].cast?(Bool) or false
```

##### 12.2. User-defined conversion

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

##### 12.3. Display versus debug

```amber
class User:
 def init(@name):
 pass

 def to_str():
 @name

 def inspect():
 "#<User name=#{@name.inspect()}>"

u = User("Ada")

"hello #{u}" # "hello Ada"
u.inspect() # "#<User name=\"Ada\">", subject to exact Str#inspect escaping
```

##### 12.4. Avoiding `.to(...)` conflict

```amber
tensor.to("cuda:0") # device transfer, library-defined
tensor.to_float() # scalar conversion if tensor is scalar-like and library supports it
tensor.cast(Float32) # dtype/type conversion, target type explicit
```

---

#### 13. Conformance tests

##### 13.1. Parser tests

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

##### 13.2. HIR golden tests

Required HIR cases:

1. Plain string remains constant.
2. Interpolated string lowers to `HStringBuild`.
3. Each expression part is wrapped in `HStringify`.
4. Source spans inside interpolation point to the embedded expression.
5. Constant-only interpolated strings may be folded only after preserving equivalent diagnostics.

##### 13.3. Runtime tests

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

##### 13.4. Privacy profile tests

Required behavior:

```amber
email as Str @pii
msg = "email=#{email}"
### msg carries @pii
```

Attempting to export/log `msg` under a policy that denies `@pii` must raise `PolicyViolationError`.

---

#### 14. Patch summary

This section adds:

1. Explicit cast protocol: `value.cast(TargetType)`.
2. Nullable cast attempt: `value.cast?(TargetType)`.
3. Standard sugar methods: `to_str`, `to_int`, `to_float`, `to_bool`, `to_symbol`, `to_array`, `to_tuple`, `to_set`, `to_map`.
4. Stringification protocol: `Amber.stringify(value, mode::display)`.
5. Interpolated string literals using `#{ Expr }`.
6. AST/HIR contracts for interpolation.
7. Deterministic fallback string representation.
8. Privacy/taint propagation through interpolation.
9. Diagnostics for malformed strings/interpolation.
10. Optional `ValueError` runtime error for invalid conversion content.

The extension deliberately does not define `.to(...)` as a type conversion method.

## Computed property descriptors


---

#### 0. Обзор решения

This section introduces **computed property descriptors**.

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
8. This section does not add hidden side effects to bare ordinary identifiers.

Recommended insertion point: after the existing sections on functions/methods/classes and callable references, with cross-references from postfix member access, assignment semantics, HIR lowering, MOP/reflection and diagnostics.

---

#### 1. Motivation

Amber already distinguishes callable values, callable references and callable invocation:

```amber
fn = &Math.answer
fn()
```

This section does not weaken that distinction.

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

f # not f()
f() # ordinary call
&f # callable reference
```

Bare ordinary names remain value access, not hidden call sites.

---

#### 2. Terminology

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

f # ordinary binding access; not an implicit call
f() # ordinary callable call
&f # callable reference, when target is valid

g # property get; evaluates the getter
```

For object members:

```amber
obj.g # property get if `g` is a readable property
obj.g = x # property set if `g` is a writable property
```

---

#### 3. Surface syntax

##### 3.1. Compact getter shorthand

A property declaration may use the compact getter shorthand:

```amber
prop answer:
 42

answer # 42
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

##### 3.2. Grouped descriptor form

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

##### 3.3. Instance property

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

##### 3.4. Class-side property

A class-side property is declared with `class_prop`:

```amber
class Build:
 class_prop version:
 get:
 "20.3"

Build.version # "20.3"
```

Compact getter shorthand is also valid:

```amber
class Build:
 class_prop version:
 "20.3"
```

`class_prop` is intentionally separate from `class_method def`, mirroring the existing separation between instance methods and class methods.

##### 3.5. Mixin property

A mixin may declare instance-side properties:

```amber
mixin Timestamped:
 prop age_seconds:
 Clock.now() - @created_at
```

When the mixin is included into a class, property lookup participates in the same ancestor/linearization model as ordinary instance methods, subject to the conflict rules in this section.

Class-side properties inside mixins are not introduced by this section. If a future Amber revision wants mixin-provided class-side properties, it should extend the existing `extend`/class-side composition rules explicitly.

##### 3.6. Setter compact spelling not introduced

This section does not introduce Ruby-style declarations such as:

```amber
prop f=(value):...
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

`f=` is not an ordinary identifier and this section does not create public methods named `f=`.

---

#### 4. Grammar additions

This section adds `prop`, `class_prop`, `get` and `set` as contextual keywords in property declaration positions.

Reference grammar additions:

```ebnf
PropertyDef::= "prop" Identifier ":" PropertySuite
ClassPropertyDef::= "class_prop" Identifier ":" PropertySuite

PropertySuite::= CompactGetterSuite
 | INDENT PropertyArm+ DEDENT

CompactGetterSuite::= Suite
 | Expr

PropertyArm::= GetterArm
 | SetterArm

GetterArm::= "get" ":" Suite
 | "get" ":" Expr

SetterArm::= "set" "(" Identifier ")" ":" Suite
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

#### 5. Getter semantics

##### 5.1. Evaluation

A property get evaluates the getter arm each time the access is performed.

```amber
prop now:
 Clock.now()

a = now
b = now
### a and b may differ
```

Properties are not memoized by the language. A library may implement memoization explicitly inside the getter body.

##### 5.2. Result

A getter body returns according to ordinary Amber body result rules. If it finishes without explicit `return`, it returns the current `$_`.

```amber
prop normalized_name:
 @name.strip()
 $_.downcase()
```

##### 5.3. Arity

A getter arm declares no user-visible parameters.

Invalid:

```amber
prop scale:
 get(x):
 x * 2
```

Getter access is not a call expression and must not accept arguments:

```amber
obj.size # property get
obj.size() # call of property result only if `obj.size` first resolves to a callable value under ordinary expression rules; not a getter call syntax
```

Implementations must not reinterpret `obj.size()` as `obj.size` getter invocation with call punctuation. The property access happens first; any following `(...)` calls the resulting value.

---

#### 6. Setter semantics

##### 6.1. Assignment syntax

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
obj.f=(value) # invalid unless a future revision adds methods named `f=`
```

##### 6.2. Setter arity

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

Defaults, rest parameters, keyword parameters and block parameters are not allowed in setter arms in this section.

##### 6.3. Assignment result

A property assignment expression returns the original right-hand side value, not the setter body result.

```amber
class Box:
 prop value:
 get:
 @value

 set(v):
 @value = v:ok

box = Box()
x = (box.value = 10)
### x == 10, not:ok
```

This preserves the existing Amber assignment convention that assignment evaluates to the assigned value and updates `$_` with the assigned value.

##### 6.4. Right-hand side evaluation

The right-hand side of a property assignment is evaluated exactly once.

```amber
obj.f = expensive()
```

Lowering must be observationally equivalent to:

```amber
$tmp = expensive()
PROPERTY_SET(obj,:f, $tmp)
$tmp
```

except that `$tmp` is not user-visible.

##### 6.5. Receiver evaluation order

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

##### 6.6. Setter failure

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

#### 7. Readability and writability

##### 7.1. Read-only property

A property with a getter and no setter is readable but not assignable.

```amber
prop id:
 @id

obj.id # ok
obj.id = 10 # error
```

Assignment to a read-only property raises or diagnoses `ReadOnlyPropertyError` / `E_PROP_MISSING_SETTER`, depending on whether the error is detected statically or dynamically.

##### 7.2. Write-only property

A property with a setter and no getter is writable but not readable.

```amber
prop password:
 set(value):
 @password_hash = Password.hash(value)

user.password = "secret" # ok
user.password # error
```

Reading a write-only property raises or diagnoses `WriteOnlyPropertyError` / `E_PROP_MISSING_GETTER`.

Write-only properties are allowed in class and mixin object-body contexts. Top-level write-only properties are not introduced by this section.

##### 7.3. Read-write property

A property with both getter and setter is both readable and writable.

```amber
prop temperature_c:
 get:
 @temperature_c

 set(value):
 @temperature_c = Float(value)
```

---

#### 8. Top-level and local properties

##### 8.1. Top-level readable properties

Top-level readable properties are allowed:

```amber
prop version:
 "20.3"

version
```

They behave as computed bindings in their declaring module/scope.

##### 8.2. Top-level writable properties

Top-level writable properties are not introduced by this section.

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

##### 8.3. Local properties

Local properties inside function/block bodies are not introduced by this section.

Invalid:

```amber
def f():
 prop x:
 42
```

Rationale: local property declarations would require additional closure/capture and assignment-resolution rules. This section limits properties to module/object declaration contexts.

---

#### 9. Member lookup and conflicts

##### 9.1. Property namespace

A property descriptor occupies the external member name `name`.

For, a class or effective ancestor composition must not expose an ordinary method, stored public field accessor and property descriptor with the same external name unless an existing Amber override rule explicitly allows one declaration to replace another in a deterministic order.

Recommended rule:

> A property descriptor conflicts with ordinary methods and generated public field accessors of the same external name in the same effective owner. Implementations should emit a deterministic conflict diagnostic rather than choosing an implicit precedence order.

Invalid:

```amber
class User:
 prop name:
 @name

 def name():
 "other"
```

##### 9.2. Getter/setter descriptor merging

Within the same owner, grouped arms of a single `prop name:` declaration form one descriptor.

Multiple separate property declarations with the same name in the same owner are invalid in:

```amber
class User:
 prop name:
 get:
 @name

 prop name:
 set(value):
 @name = value
### invalid in 
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

##### 9.3. Mixin conflicts

If two included mixins contribute a property descriptor or method/field/property conflict for the same external name, the existing mixin linearization/conflict policy applies if it is explicit enough to resolve the conflict.

If the current composition rules cannot deterministically resolve the conflict, the owner must receive a `PropertyNameConflict` diagnostic.

---

#### 10. Callable references

##### 10.1. Ordinary functions and methods

No change:

```amber
def f():
 42

f() # call
&f # callable reference, when target is valid
```

##### 10.2. Property getter references

This section does not introduce a new top-level getter-reference syntax.

Invalid:

```amber
prop answer:
 42

&answer # invalid in 
```

For instance-side properties, `&Class#name` may refer to a readable property getter only if the implementation represents readable properties as property-compatible unbound member references and the spec profile explicitly enables this behavior.

The conservative baseline is:

```amber
&User#full_name # valid only for ordinary methods in; property getter references are reserved for a future RFC
```

Recommended diagnostic if attempted on a property:

```text
E_PROP_GETTER_REFERENCE_UNSUPPORTED
cannot take callable reference to property getter `full_name`; use an explicit closure/adapter
```

##### 10.3. Property setter references

This section does not introduce setter callable references.

Invalid:

```amber
&User#full_name=
```

Rationale: supporting setter references would require extending callable reference target grammar to selector names containing `=`, which this section intentionally avoids.

---

#### 11. AST and HIR requirements

##### 11.1. AST

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

##### 11.2. Binder

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

##### 11.3. HIR

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
HSend(receiver,:name, [], surface_kind = property_get)
HSend(receiver,:name_set, [value], surface_kind = property_set)
```

However, deterministic HIR dumps must distinguish property get/set from ordinary method calls.

##### 11.4. Property assignment lowering

Surface:

```amber
receiver.name = rhs
```

HIR-equivalent:

```text
r = eval(receiver)
v = eval(rhs)
HPropSet(r,:name, v)
result = v
```

The setter body result must not become the assignment expression result.

##### 11.5. Bytecode and VM

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

#### 12. Reflection and MOP

This section recommends, but does not require for P0, a reflective descriptor API:

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

`define_method` is not extended to define properties in this section. A future MOP extension may add:

```amber
define_property(:name, get:..., set:...)
```

but core syntax does not require it.

---

#### 13. Diagnostics

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
E_PROP_TOP_LEVEL_SETTER: top-level writable properties are not part of Amber
```

---

#### 14. Compatibility

This section is source-compatible with existing Amber code unless that code already uses `prop`, `class_prop`, `get` or `set` in the newly introduced declaration/arm-label positions.

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

#### 15. Examples

##### 15.1. Read-only computed property

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

##### 15.2. Read-write validating property

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

##### 15.3. Write-only property

```amber
class User:
 prop password:
 set(value):
 @password_hash = Password.hash(value)

user.password = "secret" # ok
user.password # error: write-only property
```

##### 15.4. Class-side property

```amber
class Build:
 class_prop version:
 "20.3"

Build.version
```

##### 15.5. Assignment result

```amber
x = (account.balance = 100)
### x == 100
```

---

#### 16. Conformance tests

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
 @value = v:ignored

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
### E_PROP_INVALID_HEADER
```

```amber
prop age:
 set():
 pass
### E_PROP_SETTER_ARITY
```

```amber
prop age:
 set(value = 0):
 @age = value
### E_PROP_SETTER_DEFAULT
```

```amber
prop f=(value):
 pass
### E_PROP_SETTER_CALL_SYNTAX or E_PROP_INVALID_HEADER
```

```amber
def f():
 prop x:
 42
### E_PROP_INVALID_CONTEXT
```

```amber
prop version:
 set(value):
 pass
### E_PROP_TOP_LEVEL_SETTER
```

```amber
class User:
 prop name:
 @name

 def name():
 "other"
### E_PROP_NAME_CONFLICT
```

```amber
class Box:
 prop id:
 @id

box = Box()
box.id = 10
### E_PROP_MISSING_SETTER or runtime ReadOnlyPropertyError
```

---

#### 17. Open extension points

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

#### 18. Summary

Amber adds property descriptors through:

```amber
prop f:
 get:...

 set(value):...
```

and keeps the compact read-only getter shorthand:

```amber
prop f:...
```

The compact form is defined normatively as:

```amber
prop f:
 get:...
```

The extension deliberately does not make ordinary `def` callable through bare access. Property get and property set are explicit descriptor semantics, with syntax-faithful AST, deterministic binder validation, explicit HIR lowering and assignment behavior that returns the original RHS value.

## Attribute property sugar


---

### 0. Обзор решения

This section introduces **attribute property sugar**.

The purpose of the feature is to provide a concise declaration form for exposing instance storage through standard property descriptors without requiring explicit getter/setter bodies.

The extension preserves all property semantics:

* `prop` remains the canonical property descriptor mechanism;
* `attr` is purely syntactic sugar;
* `attr` always lowers into a property descriptor;
* `attr` does not introduce public fields;
* `@field` remains internal instance storage;
* external member names remain property names;
* no implicit public accessor generation is introduced.

---

### 1. Motivation

Current code frequently requires boilerplate:

```amber
class User:
 def init(@email):
 noop

 prop email:
 get:
 @email
```

or

```amber
class Box:
 def init(@value):
 noop

 prop value:
 get:
 @value

 set(v):
 @value = v
```

The overwhelming majority of such declarations merely forward access to an instance field.

This section introduces a concise declaration form:

```amber
class User:
 attr email
```

and

```amber
class Box:
 attr var value
```

while preserving the full property model.

---

### 2. Design principles

#### 2.1. No automatic public fields

The following declaration:

```amber
def init(@x):
 noop
```

does not create a public member named `x`.

Therefore:

```amber
class A:
 def init(@x):
 noop

A(42).x
```

remains invalid unless a property or attribute declaration exists.

Instance storage and public API remain separate concepts.

---

#### 2.2. `attr` lowers to `prop`

Normative rule:

> Every `attr` declaration lowers into an equivalent property descriptor declaration.

There is no runtime distinction between:

```amber
attr email
```

and:

```amber
prop email:
 get:
 @email
```

---

#### 2.3. `attr` is not a field declaration

The following declarations occupy the same external member namespace:

```amber
attr email
```

```amber
prop email:...
```

Therefore they conflict.

---

### 3. Surface syntax

#### 3.1. Getter-only attribute

Canonical form:

```amber
attr name
```

Meaning:

```amber
prop name:
 get:
 @name
```

Example:

```amber
class User:
 def init(@email):
 noop

 attr email
```

---

#### 3.2. Read-write attribute

Canonical form:

```amber
attr var name
```

Meaning:

```amber
prop name:
 get:
 @name

 set(value):
 @name = value
```

Example:

```amber
class Box:
 def init(@value):
 noop

 attr var value
```

---

#### 3.3. Setter-only attribute

Canonical form:

```amber
attr set name
```

Meaning:

```amber
prop name:
 set(value):
 @name = value
```

Example:

```amber
class Credentials:
 attr set password
```

---

### 4. Explicit backing storage

#### 4.1. Motivation

Sometimes the public member name should differ from the internal storage name.

Example:

```amber
@raw_email
```

should be exposed as:

```amber
email
```

This section introduces explicit storage binding.

---

#### 4.2. Syntax

Getter-only:

```amber
attr email from @raw_email
```

Read-write:

```amber
attr var email from @raw_email
```

Setter-only:

```amber
attr set email from @raw_email
```

---

#### 4.3. Lowering

##### Getter-only

```amber
attr email from @raw_email
```

lowers to:

```amber
prop email:
 get:
 @raw_email
```

##### Read-write

```amber
attr var email from @raw_email
```

lowers to:

```amber
prop email:
 get:
 @raw_email

 set(value):
 @raw_email = value
```

##### Setter-only

```amber
attr set email from @raw_email
```

lowers to:

```amber
prop email:
 set(value):
 @raw_email = value
```

---

### 5. Default storage resolution

If no explicit storage is specified:

```amber
attr email
```

the backing storage defaults to:

```amber
@email
```

Likewise:

```amber
attr var value
```

defaults to:

```amber
@value
```

and:

```amber
attr set password
```

defaults to:

```amber
@password
```

Normative rule:

> Missing `from @field` is equivalent to `from @<attribute_name>`.

---

### 6. Restrictions

#### 6.1. Only instance fields are allowed

Valid:

```amber
attr email from @raw_email
```

Invalid:

```amber
attr email from @@raw_email
```

Invalid:

```amber
attr email from self.email
```

Invalid:

```amber
attr email from storage()
```

Invalid:

```amber
attr email from obj.field
```

The storage target must be a direct instance field token.

Reference grammar:

```ebnf
AttrStorage::= "from" InstanceFieldName

InstanceFieldName::= "@" Identifier
```

---

#### 6.2. Computed behavior requires `prop`

The following is intentionally unsupported:

```amber
attr full_name from "#{@first} #{@last}"
```

Any computed behavior requires an ordinary property:

```amber
prop full_name:
 "#{@first} #{@last}"
```

---

### 7. Name conflicts

#### 7.1. Conflict with property descriptors

Invalid:

```amber
class User:
 attr email

 prop email:
 @email
```

Required diagnostic:

```text
E_MEMBER_NAME_CONFLICT
external member `email` declared multiple times
```

---

#### 7.2. Conflict with another attribute

Invalid:

```amber
class User:
 attr email
 attr var email
```

Required diagnostic:

```text
E_MEMBER_NAME_CONFLICT
external member `email` declared multiple times
```

---

#### 7.3. Internal storage names do not conflict

Valid:

```amber
class User:
 attr email from @raw_email
```

The external member:

```amber
email
```

and the storage field:

```amber
@raw_email
```

belong to different namespaces.

---

### 8. Grammar additions

```ebnf
AttrDef::= "attr" AttrMode? Identifier AttrStorage?

AttrMode::= "var"
 | "set"

AttrStorage::= "from" InstanceFieldName

InstanceFieldName::= "@" Identifier
```

Interpretation:

```text
attr x
```

means getter-only.

```text
attr var x
```

means getter + setter.

```text
attr set x
```

means setter-only.

---

### 9. AST

Recommended AST node:

```text
AstAttrDef(
 name,
 mode,
 storage_field?,
 span
)
```

where:

```text
mode = GET_ONLY
 | GET_SET
 | SET_ONLY
```

Parser output must preserve the original `attr` declaration.

Parser must not immediately rewrite the node into `AstPropertyDef`.

---

### 10. HIR lowering

Before semantic lowering:

```amber
attr var email from @raw_email
```

After lowering:

```text
HPropertyDef(
 name = "email",
 getter = HFieldRead("@raw_email"),
 setter = HFieldWrite("@raw_email")
)
```

`attr` introduces no new runtime primitive.

All runtime behavior is inherited from property descriptors.

---

### 11. Diagnostics

#### Missing field name

Invalid:

```amber
attr from @email
```

Diagnostic:

```text
E_ATTR_EXPECTED_NAME
attribute declaration requires a member name
```

---

#### Missing storage target

Invalid:

```amber
attr email from
```

Diagnostic:

```text
E_ATTR_EXPECTED_STORAGE_FIELD
expected instance field after `from`
```

---

#### Invalid storage target

Invalid:

```amber
attr email from foo()
```

Diagnostic:

```text
E_ATTR_INVALID_STORAGE
attribute storage must be an instance field token
```

---

### 12. Conformance tests

Valid:

```amber
class User:
 attr email
```

Valid:

```amber
class User:
 attr var email
```

Valid:

```amber
class User:
 attr set password
```

Valid:

```amber
class User:
 attr email from @raw_email
```

Valid:

```amber
class User:
 attr var email from @raw_email
```

Invalid:

```amber
class User:
 attr email
 prop email:
 @email
```

Invalid:

```amber
attr email from foo()
```

Invalid:

```amber
attr email from self.email
```

Invalid:

```amber
attr email from @@shared
```

---

### 13. Acceptance decision

This section is accepted with the following final decisions:

1. `attr` is introduced as property sugar.
2. `attr name` defines a getter-only property.
3. `attr var name` defines a read-write property.
4. `attr set name` defines a setter-only property.
5. `from @field` specifies explicit backing storage.
6. Missing storage defaults to `@<name>`.
7. `attr` lowers into ordinary property descriptors.
8. `attr` does not create public fields.
9. `attr` and `prop` occupy the same external member namespace.
10. Duplicate external member declarations are errors.
11. Storage targets are restricted to direct instance field tokens.
12. Computed behavior remains the responsibility of `prop`.

## Range step, materialization, negative indexing и Int#times


---

### 0. Обзор решения

This section extends Amber's collection layer with a stricter and more Amber-native range model.

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

This section deliberately does **not** introduce `to_a()`.

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

### 1. Design principles

#### 1.1. Amber-native conversion names

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

#### 1.2. Infinite collections must fail explicitly

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

#### 1.3. Range step is explicit direction, not guessed direction

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
5..1 # empty
5..1:-1 # 5, 4, 3, 2, 1
```

The implementation must not infer `-1` from `start > end`.

Rationale:

* runtime endpoints should not make step direction implicit;
* `a..b` should have stable meaning regardless of values;
* explicit descending ranges are clearer;
* empty range is preferable to hidden direction switching.

---

#### 1.4. Float ranges require explicit step

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

### 2. Range syntax

#### 2.1. Inclusive range

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
1..5 # 1, 2, 3, 4, 5
1..5:2 # 1, 3, 5
5..1:-1 # 5, 4, 3, 2, 1
```

---

#### 2.2. Exclusive range

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
1...5 # 1, 2, 3, 4
1...5:2 # 1, 3
5...1:-1 # 5, 4, 3, 2
```

---

#### 2.3. Open-ended range

Open-ended ranges are supported:

```amber
start..
start..:step
```

Examples:

```amber
1.. # 1, 2, 3, 4,...
1..:2 # 1, 3, 5, 7,...
10..:-1 # 10, 9, 8, 7,...
0.0..:0.5 # 0.0, 0.5, 1.0, 1.5,...
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

#### 2.4. Beginless ranges are not introduced

This section does not introduce beginless ranges.

Invalid:

```amber..5...5..5:2
```

Rationale:

* beginless ranges complicate array slicing semantics;
* negative indexing already covers common tail-relative indexing cases;
* beginless syntax can be added later with a separate lowering model.

---

### 3. Range step semantics

#### 3.1. Step validity

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

#### 3.2. Integer range step

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

#### 3.3. Float range step

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

#### 3.4. Direction and emptiness

The sign of `step` determines range direction.

For ascending steps:

```amber
1..5:1 # non-empty
5..1:1 # empty
```

For descending steps:

```amber
5..1:-1 # non-empty
1..5:-1 # empty
```

If the step direction cannot reach the endpoint, the range is finite and empty. This is not an error.

Examples:

```amber
(5..1).array # []
(5..1:1).array # []
(1..5:-1).array # []
```

---

#### 3.5. Inclusive boundary

For inclusive ranges:

```amber
start..end:step
```

The range contains each value in the step sequence that does not pass `end`.

Examples:

```amber
(1..5:2).array # [1, 3, 5]
(1..6:2).array # [1, 3, 5]
(5..1:-2).array # [5, 3, 1]
(6..1:-2).array # [6, 4, 2]
```

---

#### 3.6. Exclusive boundary

For exclusive ranges:

```amber
start...end:step
```

The range contains each value in the step sequence that does not reach or pass the exclusive `end` boundary.

Examples:

```amber
(1...5:2).array # [1, 3]
(1...6:2).array # [1, 3, 5]
(5...1:-2).array # [5, 3]
(6...1:-2).array # [6, 4, 2]
```

---

#### 3.7. Float sequence definition

Float range values are defined by ordinal index, not by repeated mutation of an accumulator.

For each integer `n >= 0`:

```text
value_n = start + n * step
```

The range contains all `value_n` values that satisfy the range boundary rule.

This definition reduces implementation-dependent drift from repeated floating-point addition.

Implementations may still use optimized iteration internally, but observable behavior in conformance tests must match the ordinal-index definition within the standard Float comparison model.

---

### 4. Grammar

#### 4.1. Reference grammar

```ebnf
RangeExpr::= Expr RangeOperator Expr? RangeStep?

RangeOperator::= ".."
 | "..."

RangeStep::= ":" Expr
```

Interpretation constraints:

1. `RangeStep` is recognized only immediately after a range expression.
2. `RangeStep` is not a general postfix operator.
3. `:` does not gain new meaning outside range step position.
4. Open-ended range is allowed only for `..`, not for `...`, in this section.

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

#### 4.2. Formatter rules

Formatter should emit no whitespace before the step colon:

```amber
1..10:2
```

not:

```amber
1..10: 2
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

### 5. Range materialization

#### 5.1. `Range#to_array()`

Finite ranges implement:

```amber
range.to_array()
```

The method returns a new `Array` containing the range values in iteration order.

Examples:

```amber
(1..5).to_array() # [1, 2, 3, 4, 5]
(1...5).to_array() # [1, 2, 3, 4]
(1..5:2).to_array() # [1, 3, 5]
(5..1:-1).to_array() # [5, 4, 3, 2, 1]
```

Each call returns a fresh array.

```amber
a = (1..3).to_array()
b = (1..3).to_array()
a.object_id == b.object_id # false, subject to object_id profile availability
```

---

#### 5.2. `Range#array`

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
(1..5).array # [1, 2, 3, 4, 5]
(1...5).array # [1, 2, 3, 4]
(1..5:2).array # [1, 3, 5]
```

`array` is a property alias, not a method.

Invalid:

```amber
(1..5).array()
```

Unless the returned array is itself callable, this is an ordinary attempted call of the property result and must not be interpreted as a property getter call.

---

#### 5.3. No `to_a()`

Amber does not define:

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

#### 5.4. Open-ended materialization

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

### 6. `InfiniteCollectionError`

#### 6.1. Error class

This section introduces:

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

#### 6.2. Required uses

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
(1..).first(5) # [1, 2, 3, 4, 5]
(1..:2).first(3) # [1, 3, 5]
```

#### 6.3. Message guidance

Recommended messages:

```text
cannot materialize an open-ended range
cannot count an infinite collection without a bound
cannot convert an infinite collection to Array
```

Messages must be deterministic and must not include raw memory addresses.

---

### 7. Negative array indexing

#### 7.1. Single index normalization

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

xs[0] # "a"
xs[1] # "b"
xs[-1] # "d"
xs[-2] # "c"
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

#### 7.2. Negative index assignment

Negative indexing also applies to indexed assignment.

```amber
xs = ["a", "b", "c"]
xs[-1] = "z"
xs # ["a", "b", "z"]
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

### 8. Array slicing by `IntRange`

#### 8.1. Basic rule

Arrays accept finite or open-ended `IntRange` values as index operands.

```amber
xs[range]
```

returns a new `Array` containing elements selected by the range's integer iteration order.

Examples:

```amber
xs = ["a", "b", "c", "d", "e"]

xs[1..3] # ["b", "c", "d"]
xs[1...3] # ["b", "c"]
xs[0..:2] # ["a", "c", "e"]
xs[4..0:-2] # ["e", "c", "a"]
```

The result is always a new `Array`.

---

#### 8.2. Negative range endpoints

Range endpoints used for array slicing are normalized as array indices.

For an array of length `n`, each endpoint `i` is normalized as:

```text
normalized = if i < 0 then n + i else i
```

Examples:

```amber
xs = ["a", "b", "c", "d", "e"]

xs[-1] # "e"
xs[-2] # "d"
xs[-3..-1] # ["c", "d", "e"]
xs[-1..0:-1] # ["e", "d", "c", "b", "a"]
xs[-1...0:-1] # ["e", "d", "c", "b"]
```

---

#### 8.3. Open-ended range slices

Open-ended `IntRange` slices are bounded by the receiver array length.

Examples:

```amber
xs = ["a", "b", "c", "d", "e"]

xs[2..] # ["c", "d", "e"]
xs[2..:2] # ["c", "e"]
xs[-3..] # ["c", "d", "e"]
xs[-1..:-1] # ["e", "d", "c", "b", "a"]
```

Open-ended slice materialization is allowed because the receiver array supplies the finite bound.

This does not contradict `InfiniteCollectionError` for standalone range materialization.

```amber
(1..).array # InfiniteCollectionError
xs[1..] # valid, bounded by xs.length
```

---

#### 8.4. Out-of-bounds slice endpoints

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

#### 8.5. Empty slices

A slice may be empty when the normalized range direction cannot reach the boundary.

Examples:

```amber
xs = ["a", "b", "c"]

xs[2..0] # [] because default step is +1
xs[0..2:-1] # [] because step is -1 and end is ahead
```

This is not an error because the endpoints themselves are valid.

---

#### 8.6. Float ranges are invalid as array indices

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

### 9. `Int#times`

#### 9.1. Purpose

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

#### 9.2. `Int#times` without block

Without a block, `Int#times` returns a finite `Times` enumerable equivalent to the integer range:

```amber
0...n
```

Examples:

```amber
5.times.array # [0, 1, 2, 3, 4]
5.times.to_array() # [0, 1, 2, 3, 4]
0.times.array # []
(-3).times.array # []
```

For `n <= 0`, `n.times` is empty.

`Times` may be implemented as a specialized lightweight enumerable or as a lowered `IntRange`.

Observable behavior must match:

```amber
if n <= 0 then 0...0 else 0...n
```

---

#### 9.3. `Int#times` block form

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

result == null # true
```

Rationale:

* direct block `times` is effect-oriented;
* returning receiver is Ruby-specific and not Amber-style;
* result collection belongs to `.map`, not `.times`.

---

#### 9.4. `times.each`

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

#### 9.5. `times.map`

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
### []

(-3).times.map |i|:
 i
### []
```

`times.map` propagates exceptions raised by the block.

---

#### 9.6. Block arity

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

### 10. Enumerable behavior

#### 10.1. Range enumerable methods

Finite ranges participate in the standard Enumerable-like contract:

```amber
range.each |x|:...

range.map |x|:...

range.select |x|:...

range.to_array()
range.array
range.lazy()
```

`to_a()` is not part of the Amber contract.

---

#### 10.2. Open-ended range enumerable methods

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

### 11. AST and HIR notes

#### 11.1. AST

Recommended AST nodes:

```text
AstRangeExpr(
 start_expr,
 end_expr?,
 boundary_kind, # inclusive | exclusive
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

Exclusive open-ended ranges are invalid in this section.

---

#### 11.2. HIR

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

#### 11.3. `times` lowering

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

### 12. Diagnostics

#### 12.1. New diagnostics

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

#### 12.2. Runtime errors

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

### 13. Conformance tests

#### 13.1. Integer ranges

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

#### 13.2. Float ranges

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

#### 13.3. Infinite collection materialization

```amber
assert_raises(InfiniteCollectionError):
 (1..).to_array()

assert_raises(InfiniteCollectionError):
 (1..).array

assert((1..).first(3) == [1, 2, 3])
assert((1..:2).first(3) == [1, 3, 5])
```

---

#### 13.4. No `to_a`

```amber
assert_raises(NoMethodError):
 (1..3).to_a()
```

If the implementation uses `MethodMissingError` or `E_METHOD_NOT_FOUND`, the test should match the implementation's standard missing-method class.

---

#### 13.5. Negative indexing

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

#### 13.6. Array slicing

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

#### 13.7. `Int#times`

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

### 14. Compatibility and migration notes

#### 14.1. No Ruby `to_a`

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

#### 14.2. Descending ranges

Code expecting automatic descending behavior must specify negative step.

```amber
5..1 # []
5..1:-1 # [5, 4, 3, 2, 1]
```

---

#### 14.3. Float ranges

Code using float endpoints must specify step.

```amber
0.0..1.0 # invalid
0.0..1.0:0.1 # valid
```

---

### 15. Summary

Amber adds range and repetition ergonomics while preserving Amber-style explicitness:

```amber
(1..5).array # [1, 2, 3, 4, 5]
(1..5:2).array # [1, 3, 5]
(5..1:-1).array # [5, 4, 3, 2, 1]
(1..).first(3) # [1, 2, 3]

xs[-1] # last element
xs[-3..-1] # tail slice
xs[2..] # from index 2 to end

5.times |i|:
 print i # returns null

5.times.each |i|:
 print i # returns [0, 1, 2, 3, 4]

5.times.map |i|:
 i * 2 # returns [0, 2, 4, 6, 8]
```

The patch deliberately avoids compatibility-only aliases and introduces `InfiniteCollectionError` as the required explicit failure mode for eager materialization of infinite/open-ended collections.

## Array generation APIs и optional bracket access


---

### 0. Обзор решения

This section introduces two related improvements to Amber's collection ergonomics:

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
array[index] # may raise IndexError
map[key] # may raise KeyError
```

and adds an explicit nullable addressing mode for flow-dependent absence:

```amber
array[?index] # value or null
map[?key] # value or null
```

---

### 1. Design principles

#### 1.1. Array initialization must say whether values are repeated or generated

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
### [same_array, same_array, same_array]

Array.of(3):
 []
### [fresh_array, fresh_array, fresh_array]
```

The language must not hide cloning, copying or per-slot allocation behind a repeated-value API.

---

#### 1.2. Bracket access remains the canonical collection addressing form

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

#### 1.3. `?` means optional lookup, not C-style ternary

Amber does not introduce C-style ternary syntax:

```amber
cond ? a: b
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

#### 1.4. Optional bracket access is read-only

The patch does not introduce optional assignment:

```amber
xs[?i] = value # invalid
map[?key] = value # invalid
```

Reasoning:

* optional read has clear nullable semantics;
* optional array assignment is ambiguous: no-op, append, gap-fill or error;
* map insertion already has ordinary strict assignment syntax;
* suppressing assignment errors would hide bugs.

---

### 2. Array construction APIs

#### 2.1. `Array.of(length) |index|:`

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
### [0, 1, 4, 9, 16]
```

`index` is an `Int`.

The result is a new `Array` of exactly `length` elements.

---

#### 2.2. Shorthand block forms

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

#### 2.3. Fresh mutable values

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

#### 2.4. `Array.build` alias

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

#### 2.5. `Array.filled(length, value)`

`Array.filled` constructs a new array of `length` elements where every slot contains `value`.

```amber
Array.filled(5, 0)
### [0, 0, 0, 0, 0]

Array.filled(3, null)
### [null, null, null]
```

For object values, the same object reference is stored in every slot.

```amber
inner = []
xs = Array.filled(3, inner)

xs[0].object_id == xs[1].object_id
### true, subject to object_id profile availability
```

No implicit clone, copy, deep copy or per-slot allocation is performed.

---

#### 2.6. Relationship to `[value] * N`

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

### 3. Array construction errors

#### 3.1. Negative length

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

#### 3.2. Non-integer length

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

#### 3.3. Missing block for `Array.of` / `Array.build`

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

### 4. Optional bracket access

#### 4.1. Canonical form

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

#### 4.2. Array optional access

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

xs[0] # "a"
xs[-1] # "c"
xs[3] # IndexError
xs[-4] # IndexError

xs[?0] # "a"
xs[?-1] # "c"
xs[?3] # null
xs[?-4] # null
```

---

#### 4.3. Tuple optional access

If tuples support bracket indexing, optional bracket access applies analogously:

```amber
tuple[?index]
```

returns the tuple element or `null` when the index is invalid.

---

#### 4.4. Map optional access

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

user[:name] # "Ada"
user[:age] # KeyError

user[?:name] # "Ada"
user[?:age] # null
user[?:email] # null
```

---

#### 4.5. Stored `null` and absent value

Optional bracket access intentionally does not distinguish between:

1. present key with `null` value;
2. absent key;
3. invalid index.

Example:

```amber
m = {
 email: null,
}

m[?:email] # null
m[?:age] # null
```

Use explicit presence predicates when the distinction matters:

```amber
m.contains?(:email) # true
m.contains?(:age) # false
```

For arrays:

```amber
xs.has_index?(i)
```

returns whether `i` is a valid index after negative-index normalization.

---

#### 4.6. Evaluation order

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

#### 4.7. What optional access catches

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

### 5. Optional bracket assignment

#### 5.1. Assignment is invalid

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

### 6. Grammar

#### 6.1. Reference grammar

```ebnf
PostfixExpr::= PrimaryExpr PostfixPart*

PostfixPart::= CallSuffix
 | MemberSuffix
 | BracketSuffix
 | SafeNavigationSuffix
 |...

BracketSuffix::= "[" BracketMode? Expr "]"

BracketMode::= "?"
```

Interpretation constraints:

1. `?` is recognized as `BracketMode` only immediately after `[` in bracket access.
2. `?` is not a general prefix operator.
3. `?` does not become part of the address expression.
4. Existing method names ending in `?` remain unaffected.
5. C-style ternary remains invalid.

---

#### 6.2. Parsing examples

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

```amber:name
```

```amber
xs[?-1]
```

parses as optional access with address expression:

```amber
-1
```

---

### 7. Formatter rules

#### 7.1. Simple addresses

Formatter should preserve compact optional access for simple address expressions:

```amber
xs[?i]
xs[?-1]
map[?key]
map[?:name]
```

#### 7.2. Complex addresses

Formatter should parenthesize complex address expressions after `?`:

```amber
xs[?(i + 1)]
map[?(prefix + key)]
matrix[?(row * width + col)]
```

This avoids visual ambiguity around whether `?` applies to the whole address expression.

---

### 8. AST

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

### 9. HIR lowering

#### 9.1. Strict access

```amber
receiver[address]
```

lowers to ordinary strict index/key lookup:

```text
INDEX_GET(receiver, address, mode: STRICT)
```

#### 9.2. Optional access

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

### 10. Presence predicates

Optional access should be paired with explicit presence APIs.

#### 10.1. Array

```amber
xs.has_index?(index)
```

Returns `true` if `index` is valid after negative-index normalization, otherwise `false`.

Examples:

```amber
xs = ["a", "b", "c"]

xs.has_index?(0) # true
xs.has_index?(-1) # true
xs.has_index?(3) # false
xs.has_index?(-4) # false
```

#### 10.2. Map

```amber
map.contains?(key)
```

Returns `true` if `key` is present, even if the associated value is `null`.

Examples:

```amber
m = {
 email: null,
}

m.contains?(:email) # true
m.contains?(:age) # false
```

---

### 11. Diagnostics

#### 11.1. Optional assignment

```amber
xs[?i] = value
```

Required diagnostic:

```text
E_OPTIONAL_BRACKET_ASSIGNMENT
optional bracket access is read-only; use strict `receiver[key] = value`
```

---

#### 11.2. Missing block for `Array.of`

```amber
Array.of(10)
```

Required diagnostic:

```text
ArgumentError
Array.of requires a block
```

---

#### 11.3. Negative array length

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

#### 11.4. Non-Int array length

```amber
Array.filled("10", null)
```

Required diagnostic:

```text
TypeError
array length must be Int
```

---

### 12. Conformance examples

#### 12.1. `Array.of`

```amber
Array.of(4) |i|:
 i + 1
### [1, 2, 3, 4]
```

```amber
Array.of(3):
 _1 * 2
### [0, 2, 4]
```

```amber
Array.of(3):
 null
### [null, null, null]
```

```amber
xs = Array.of(3):
 []

xs[0].push(1)
xs
### [[1], [], []]
```

---

#### 12.2. `Array.build`

```amber
Array.build(3) |i|:
 "item-#{i}"
### ["item-0", "item-1", "item-2"]
```

`Array.build` must produce the same result as `Array.of` for the same block.

---

#### 12.3. `Array.filled`

```amber
Array.filled(4, false)
### [false, false, false, false]
```

```amber
inner = []
xs = Array.filled(2, inner)
xs[0].push(:x)
xs
### [[:x], [:x]]
```

---

#### 12.4. Optional array access

```amber
xs = ["a", "b", "c"]

xs[?0] # "a"
xs[?2] # "c"
xs[?3] # null
xs[?-1] # "c"
xs[?-4] # null
```

Strict access remains strict:

```amber
xs[3]
### IndexError
```

---

#### 12.5. Optional map access

```amber
m = {
 a: 1,
 b: null,
}

m[?:a] # 1
m[?:b] # null
m[?:c] # null
```

Presence remains distinguishable through `contains?`:

```amber
m.contains?(:b) # true
m.contains?(:c) # false
```

Strict access remains strict:

```amber
m[:c]
### KeyError
```

---

### 13. Recommended final surface

```amber
### Generated arrays
Array.of(N) |i|:
 expr

Array.of(N):
 expr_using(_1)

Array.build(N):
 expr

### Repeated-value arrays
Array.filled(N, value)

### Strict access
xs[i]
map[key]

### Optional access
xs[?i]
map[?key]

### Presence checks
xs.has_index?(i)
map.contains?(key)
```

---

### 14. Rationale summary

`Array.of` and `Array.build` make generated array construction explicit and per-slot.

`Array.filled` makes repeated-value construction explicit and does not hide copying semantics.

Optional bracket access keeps the dominant collection-addressing syntax while making nullable lookup flow visible at the exact site where absence is accepted.

The final design is intentionally explicit:

```amber
xs[i] # require presence
xs[?i] # accept absence
```

and:

```amber
map[key] # require key
map[?key] # accept missing key
```

This preserves strict defaults while supporting common flow-dependent indexing and keyed lookup without method-style detours.

## Value-keyed Map/Set, HashMap/HashSet и expression map keys


---

### 0. Обзор решения

This section changes Amber `Map` and `Set` from symbol-only / identity-adjacent key behavior into ordered, value-keyed collections.

`Map` and `Set` remain ordered-vector based. additionally introduces explicit hash-backed `HashMap` and `HashSet` types under a strict `Hashable` protocol. Hash-backed collections are opt-in and do not replace the default ordered collection literals.

The core change is:

```amber
m = {1: "int", "name": "str",:name: "sym"}

m[1] # "int"
m["name"] # "str"
m[:name] # "sym"
```

and:

```amber
s = {[1, 2], (1, 2)}
s.count() # 1
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
Map{a: 1} # explicit ordered Map
Set{1, 2, 3} # explicit ordered Set
HashMap{a: 1} # hash-backed map
HashSet{1, 2, 3} # hash-backed set
```

Plain `{...}` remains ordered; one-letter prefixes such as `u{...}` and `o{...}` are intentionally not introduced.

Spread expansion (`fn(*args, **kwargs)`, `[1, *items]`, `{a: 1, **other}`) is intentionally deferred to Amber so that remains focused on key semantics, hashability and typed collection literals.

---

### 1. Design principles

#### 1.1. `Map` and `Set` are ordered value collections

`Map` and `Set` preserve insertion order. This section changes only key/element comparison semantics.

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

#### 1.2. Value-key equality is explicit runtime behavior

`Map` and `Set` use a dedicated runtime key equality helper, not generic object identity and not host-language pointer equality.

The helper is frame-aware because user-defined object equality may execute Amber code:

```text
map_key_equal(vm, frame, stored_key, lookup_key) -> Bool | Error
```

For built-in scalar and structural keys, the helper uses Amber value semantics.

For instance objects, it uses the stored key's `==` method, subject to strict validation rules.

---

#### 1.3. User objects need meaningful value equality

Instance objects are accepted as `Map` keys and `Set` elements only when equality lookup resolves to a user-defined, non-default value equality implementation.

A default identity fallback such as `Object#==`, if present, is not sufficient.

Inherited user-defined equality is sufficient:

```amber
class A:
 def ==(other):
 other.is_a?(A)

class B < A:
 noop

{B(): 1} # valid, because meaningful equality is inherited
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

#### 1.4. Equality is stored-key dispatched

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

m = {a::stored_a}
m[b] #:stored_a, because a == b

m2 = {b::stored_b}
m2[a] # KeyError, because b == a is false
```

---

#### 1.5. Duplicate overwrite preserves the first stored key

When a duplicate key is inserted into a `Map`, only the value is updated. The stored normalized key is not replaced.

```amber
m = {1: "int", 1.0: "float"}

m[1] # "float"
m[1.0] # "float"
m.keys() # [1]
```

Rationale:

* insertion order remains stable;
* the first spelling/representation remains visible through `keys`, `entries`, `each` and `to_a`;
* duplicate insertion has minimal mutation surface.

For `Set`, duplicate insertion keeps the first stored normalized element.

---

#### 1.6. Composite keys are normalized snapshots

Array/list keys are normalized to immutable tuple snapshots before storage and lookup.

Nested arrays/lists inside tuple/list keys are normalized recursively.

```amber
xs = [1, 2]
m = {xs::ok}

xs[0] = 9

m[(1, 2)] #:ok
m[(9, 2)] # KeyError
```

This normalization is equivalent to the built-in Array-to-Tuple conversion for built-in arrays, but it is a runtime-internal key normalization operation. It must not dynamically dispatch to user-defined conversion hooks.

---

#### 1.7. Cyclic composite keys are rejected

Cyclic Array/Tuple structures are not supported as keys in this section.

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
* this section does not introduce graph isomorphism semantics for collection keys.

---

### 2. Runtime representation

#### 2.1. `MapEntry`

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

#### 2.2. Required key helper boundary

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

### 3. Supported key types

#### 3.1. Built-in scalar keys

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
 true: "bool",:name: "symbol",
 "name": "string",
 1: "int",
 1.5: "float",
}
```

Numeric key equality follows Amber numeric equality. If Amber equality says `1 == 1.0`, then `Map` and `Set` treat them as equal keys/elements.

---

#### 3.2. Float edge cases

`0.0` and `-0.0` compare according to Amber numeric equality. If Amber numeric equality treats them as equal, `Map` and `Set` must also treat them as equal.

```amber
m = {0.0::zero}
m[-0.0] #:zero, if 0.0 == -0.0 in Amber
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

#### 3.3. Range keys

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
m = {(1..)::open}

m[(1..)] #:open
m[(1..:2)] # KeyError
```

Float range validation remains governed by range construction rules. A float range without an explicit step is rejected before or during range construction, not by `Map` key validation.

Invalid:

```amber
{(1.0..2.0)::bad}
```

Required diagnostic remains:

```text
E_RANGE_FLOAT_STEP_REQUIRED
float ranges require an explicit step
```

---

#### 3.4. Tuple keys

Tuples are valid keys.

Tuple keys compare element-by-element using the same key equality rules recursively.

```amber
m = {(1, "x")::ok}

m[(1, "x")] #:ok
m[(1.0, "x")] #:ok, if 1 == 1.0
```

Nested arrays/lists inside tuple keys are normalized recursively to tuple snapshots.

---

#### 3.5. Array/list keys

Arrays/lists are accepted as keys only through normalization to tuple snapshots.

```amber
m = {[1, 2]::ok}

m[(1, 2)] #:ok
m[[1, 2]] #:ok
```

The original array identity is not part of the key.

---

#### 3.6. Instance object keys

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

### 4. Map literal syntax

#### 4.1. Bare identifier shorthand is preserved

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

#### 4.2. Explicit symbol keys

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

#### 4.3. Literal expression keys

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

m["name"] # 1
m[:name] # KeyError
m[?:name] # null
```

---

#### 4.4. Parenthesized expression keys

Any non-literal expression key must be parenthesized:

```amber
{(name): value}
{(user.id): value}
{(compute_key()): value}
{(if ok then a else b): value}
```

A bare identifier is never a variable key in map literal key position.

```amber
{name: value} # Symbol key:name
{(name): value} # value of variable/expression `name`
```

Rationale:

* preserves existing shorthand;
* makes expression keys explicit;
* avoids parser ambiguity with labels, conditional entries and future map-entry extensions.

---

#### 4.5. Range keys require parentheses

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

#### 4.6. Conditional map entries

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

### 5. Set literal behavior

`Set` construction uses the same key normalization and key equality as `Map`.

```amber
s = {[1, 2], (1, 2), 1, 1.0}

s.count() # 2, if 1 == 1.0
```

Array/list elements are normalized to tuple snapshots.

Duplicate elements collapse while preserving the first stored normalized element.

Conditional set elements continue to control element presence:

```amber
{:read,:write if can_write?,
 [1, 2] if include_pair?,
}
```

If the condition is falsy, the element expression is not evaluated.

---

### 6. Map operations

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
map.each |k, v|:...
map.map |k, v|:...
map.select |k, v|:...
map.reject |k, v|:...
map.transform |k, v|:
 (new_key, new_value)
map.transform_values |v, k|:...
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

### 7. Set operations

All `Set` operations that observe, lookup, create or transform elements must use the same normalization and equality rules as `Map` keys.

Required affected operations:

```amber
set.add(value)
set.delete(value)
set.include?(value)
set.contains?(value)
set.each |x|:...
set.map |x|:...
set.select |x|:...
set.reject |x|:...
set | other
set & other
set - other
set ^ other
items.to_set()
```

Array and Tuple membership outside `Set` remains governed by existing `value_equals` / collection equality behavior unless the operation explicitly constructs or operates through a `Set`.

---

### 8. Pattern matching and `deconstruct_keys`

Pattern matching remains symbol-key based.

Named-key map patterns observe only entries whose normalized stored key is `Symbol`.

```amber
m = {name: "Ada", "name": "string", 1: "one"}

case m
in {name: n}
 n # "Ada"
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

This section does not introduce expression-key map patterns.

Invalid / not introduced:

```amber
case m
in {"name": x}...
end
```

unless already valid under a separate pattern-matching rule outside this section.

---

### 9. Bytecode

#### 9.1. Existing `MakeMap`

Existing `MakeMap` remains valid for symbol-immediate maps and old bytecode/tests.

It constructs maps whose keys are `Symbol` values.

The existing bytecode format remains loadable.

---

#### 9.2. New `MakeMapDyn`

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

#### 9.3. Emitter strategy

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

#### 9.4. Verifier requirements

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

#### 9.5. Serializer, disassembler and quick analyses

The bytecode serializer, deserializer, disassembler, verifier, quick analyses and opcode registry must be updated for `MakeMapDyn`.

Disassembly should preserve the key/value register pair structure:

```text
MakeMapDyn r0, count=2, (r1 => r2), (r3 => r4)
```

or equivalent deterministic formatting.

---

### 10. Parser and HIR lowering

#### 10.1. AST/HIR distinction

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

#### 10.2. Recommended lowering

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

### 11. Diagnostics

#### 11.1. Invalid object key

```text
TypeError
object used as Map key must define value equality with `==`
```

```text
TypeError
object used as Set element must define value equality with `==`
```

---

#### 11.2. Non-Bool equality result

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

#### 11.3. Cyclic composite key

```text
TypeError
cyclic composite Map keys are not supported
```

```text
TypeError
cyclic composite Set elements are not supported
```

---

#### 11.4. NaN key

```text
TypeError
NaN cannot be used as a Map key
```

```text
TypeError
NaN cannot be used as a Set element
```

---

#### 11.5. Range key parentheses

```text
E_MAP_KEY_RANGE_PARENS_REQUIRED
range expression keys in map literals must be parenthesized
```

---

#### 11.6. Unsupported expression key spelling

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

### 12. Порядок реализации

#### 12.1. Phase 1: runtime key model

* Change `MapEntry` to `key: Value, value: Value`.
* Add key normalization, validation, equality, find and upsert helpers.
* Update `Map` lookup, assignment and construction paths.
* Update `Set` construction and membership to use the same helper.
* Preserve old symbol-key behavior through `make_symbol_map_value(...)`.

---

#### 12.2. Phase 2: compatibility and existing tests

* Keep existing `MakeMap` bytecode valid.
* Keep existing symbol-key map tests green.
* Keep pattern matching and `deconstruct_keys` symbol-key based.
* Ensure old bytecode deserializes and executes unchanged.

---

#### 12.3. Phase 3: `MakeMapDyn`

* Add opcode registry entry.
* Add serializer/deserializer support.
* Add verifier support.
* Add disassembler output.
* Add quick-analysis support.
* Add VM execution support.

---

#### 12.4. Phase 4: parser/HIR syntax

* Preserve `{name: value}` as symbol shorthand.
* Add literal expression map keys.
* Add parenthesized expression keys.
* Require parentheses for range keys.
* Ensure conditional map entries still control whole-entry presence.

---

#### 12.5. Phase 5: collection operation audit

Update and test:

```text
Map: [], [?], assignment, contains?, include?, keys, entries, each, map, select,
 reject, transform, transform_values, merge, +, |, casts from pairs

Set: construction, add, delete, include?, contains?, algebra, to_set
```

---

### 13. Conformance tests

#### 13.1. Parser/HIR tests

```amber
{name: v} # Symbol key
{:name: v} # Symbol key
{"name": v} # Str key
{1: v} # Int key
{1.5: v} # Float key
{null: v} # Null key
{true: v} # Bool key
{(name): v} # expression key
{[1, 2]: v} # Array key, normalized at runtime
{(1, 2): v} # Tuple key
{(1..5): v} # Range key
{(1..5:2): v} # stepped Range key
```

Invalid:

```amber
{1..5: v}
{1..5:2: v}
{user.id: v}
```

---

#### 13.2. Bytecode tests

* `MakeMap` still serializes, deserializes, verifies and disassembles.
* `MakeMapDyn` serializes, deserializes, verifies and disassembles.
* Malformed `MakeMapDyn` operand count fails verification.
* `MakeMapDyn` with uninitialized key register fails verification.
* `MakeMapDyn` with uninitialized value register fails verification.
* Old bytecode using symbol maps remains valid.

---

#### 13.3. Numeric key tests

```amber
m = {1: "a", 1.0: "b"}

assert(m[1] == "b")
assert(m[1.0] == "b")
assert(m.keys() == [1])
```

```amber
m = {0.0::zero}
assert(m[-0.0] ==:zero)
```

If `Float.nan` is available:

```amber
assert_raises(TypeError):
 {Float.nan: 1}
```

---

#### 13.4. String vs Symbol key tests

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

#### 13.5. Range key tests

```amber
m = {(1..3)::a}

assert(m[(1..3)] ==:a)
assert_raises(KeyError):
 m[(1...3)]
```

```amber
m = {(1..5:2)::odd}

assert(m[(1..5:2)] ==:odd)
assert_raises(KeyError):
 m[(1..5:3)]
```

```amber
m = {(1..)::open}

assert(m[(1..)] ==:open)
assert_raises(KeyError):
 m[(1..:2)]
```

---

#### 13.6. Tuple and Array normalization tests

```amber
m = {(1, 2)::tuple}

assert(m[[1, 2]] ==:tuple)
assert(m[(1.0, 2.0)] ==:tuple)
```

```amber
m = {[1, 2]::array}

assert(m[(1, 2)] ==:array)
assert(m[[1, 2]] ==:array)
```

Mutation snapshot:

```amber
xs = [1, 2]
m = {xs::ok}

xs[0] = 9

assert(m[(1, 2)] ==:ok)
assert_raises(KeyError):
 m[(9, 2)]
```

Nested normalization:

```amber
m = {([1, [2, 3]])::nested}

assert(m[(1, (2, 3))] ==:nested)
```

Cycle rejection:

```amber
a = []
a.push(a)

assert_raises(TypeError):
 {a: 1}
```

---

#### 13.7. Set tests

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

#### 13.8. Custom object equality tests

Working custom object key:

```amber
class Point:
 def init(@x, @y):
 noop

 attr x
 attr y

 def ==(other):
 other.is_a?(Point) and @x == other.x and @y == other.y

m = {Point(1, 2)::p}

assert(m[Point(1, 2)] ==:p)
```

Inherited equality:

```amber
class A:
 def ==(other):
 other.is_a?(A)

class B < A:
 noop

m = {B()::b}
assert(m[B()] ==:b)
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

m = {a::a}
assert(m[b] ==:a)

m2 = {b::b}
assert_raises(KeyError):
 m2[a]
```

---

#### 13.9. Optional bracket tests

```amber
m = {[1, 2]::ok}

assert(m[?(1, 2)] ==:ok)
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

#### 13.10. Pattern matching regression tests

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

#### 13.11. Regression tests

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

### 14. Compatibility notes

#### 14.1. Source compatibility

Existing symbol-key map literals remain valid:

```amber
{name: value}
```

continues to mean symbol key `:name`.

String-key maps change only if an implementation previously treated string literal keys as symbol-compatible. Under this section:

```amber
{"name": value}
```

is always a real `Str` key.

---

#### 14.2. Bytecode compatibility

Old bytecode using `MakeMap` remains valid.

New bytecode using expression keys must use `MakeMapDyn`.

---

#### 14.3. Pattern compatibility

Existing map patterns remain symbol-key based.

This section does not make string keys match symbol-name patterns.

---

### 15. Summary of normative decisions

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

### 16. Предпосылки

* No implementation edits were made in this planning document.
* Existing uncommitted parser/HIR/emitter/runtime/tests changes must be preserved and built on, not reverted.
* No `<=>` protocol is implemented for key equality in this change.
* No hash table is introduced in this change.
* No expression-key map patterns are introduced in this change.

---

### 17. `HashMap`, `HashSet` and strict `Hashable` protocol

#### 17.0. Обзор решения

This section is a normative addition to the value-keyed `Map` / `Set` patch.

`Map` and `Set` remain the default ordered, vector-backed, value-keyed collections described above. also introduces explicit hash-backed collection types for workloads that require average constant-time lookup under a stricter key protocol:

```amber
Map{a: 1} # explicit ordered Map
Set{1, 2, 3} # explicit ordered Set

HashMap{a: 1} # hash-backed map
HashSet{1, 2, 3} # hash-backed set
```

Plain collection literals remain ordered:

```amber
{a: 1} # Map
{1, 2, 3} # Set
```

No `u{...}` / `o{...}` one-letter literal prefixes are introduced.

---

#### 17.1. Type model

Amber has four core associative collection families:

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

#### 17.2. Performance contract

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

#### 17.3. `Hashable` protocol

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

#### 17.4. Built-in hashable values

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
h = HashMap{xs::ok}

xs[0] = 9

h[(1, 2)] #:ok
h[(9, 2)] # KeyError
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

#### 17.5. Hash normalization and equality

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

#### 17.6. Duplicate insertion rule

`HashMap` duplicate insertion follows `Map` semantics:

* the later value overwrites the earlier value;
* the first stored normalized key is retained;
* iteration order remains unspecified.

Example:

```amber
hm = HashMap{1: "int", 1.0: "float"}

hm[1] # "float"
hm[1.0] # "float"
hm.keys() # contains first stored key `1`; order unspecified
```

`HashSet` duplicate insertion keeps the first stored normalized element.

---

#### 17.7. HashMap operations

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
hm.each |k, v|:...
hm.map |k, v|:...
hm.select |k, v|:...
hm.reject |k, v|:...
hm.transform |k, v|:
 (new_key, new_value)
hm.transform_values |v, k|:...
hm.merge(other)
hm + other
hm | other
```

Any operation that inserts or transforms keys into a `HashMap` must validate the resulting keys against the `Hashable` protocol.

`hm[?key]` suppresses absence only. It must not suppress `TypeError`, invalid hash protocol errors, cyclic key errors, non-`Int` hash errors or equality protocol errors.

---

#### 17.8. HashSet operations

`HashSet` supports the same user-facing operation family as `Set` where the operation is meaningful:

```amber
hs.add(value)
hs.delete(value)
hs.contains?(value)
hs.include?(value)
hs.each |x|:...
hs.map |x|:...
hs.select |x|:...
hs.reject |x|:...
hs | other
hs & other
hs - other
```

Any operation that inserts elements into a `HashSet` must validate the inserted element against the `Hashable` protocol.

---

### 18. Explicit collection literal constructors

#### 18.1. Canonical forms

 introduces explicit collection literal constructors:

```amber
Map{a: 1}
Set{1, 2, 3}
HashMap{a: 1}
HashSet{1, 2, 3}
```

Plain `{...}` remains ordered and continues to auto-disambiguate between `Map` and `Set` according to existing map-entry vs set-element rules:

```amber
{a: 1} # Map
{1, 2, 3} # Set
{} # Map, unless the host specification already fixes another empty-literal rule
```

The explicit constructors are syntax, not ordinary method calls.

---

#### 18.2. Type restrictions

`Map{...}` and `HashMap{...}` require map entries:

```amber
Map{a: 1} # valid
HashMap{a: 1} # valid

Map{1, 2, 3} # invalid
HashMap{1, 2, 3} # invalid
```

`Set{...}` and `HashSet{...}` require set elements:

```amber
Set{1, 2, 3} # valid
HashSet{1, 2, 3} # valid

Set{a: 1} # invalid
HashSet{a: 1} # invalid
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

#### 18.3. Grammar

Reference grammar:

```ebnf
CollectionLiteral::= PlainCollectionLiteral
 | TypedCollectionLiteral

PlainCollectionLiteral::= "{" CollectionItems? "}"

TypedCollectionLiteral::= CollectionType "{" CollectionItems? "}"

CollectionType::= "Map"
 | "Set"
 | "HashMap"
 | "HashSet"
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

#### 18.4. Interaction with expression map keys

Typed map literals use the same key syntax rules as plain map literals:

```amber
HashMap{name: value} # Symbol key:name
HashMap{:name: value} # explicit Symbol key
HashMap{"name": value} # Str key
HashMap{1: value} # Int key
HashMap{(name): value} # expression key
HashMap{(1..5): value} # Range key
HashMap{(1..5:2): value} # stepped Range key
```

The additional `HashMap` constraint is that the normalized key must be `Hashable`.

---

#### 18.5. Interaction with conditional collection elements

Typed literals support conditional entries/elements using the existing trailing-condition model:

```amber
HashMap{
 a: 1,
 b: 2 if enabled?,
}

HashSet{:read,:write if user.editor?,
}
```

If a conditional entry/element is skipped, its key/value/element expression is not evaluated.

If it is included, `HashMap` / `HashSet` perform ordinary `Hashable` validation.

---

#### 18.6. Future spread compatibility

If a later patch introduces collection spread syntax, typed hash literals follow this rule:

```amber
HashMap{a: 1, **other}
HashSet{1, *items}
```

`HashMap{**other}` may accept ordered `Map` or `HashMap` inputs, but every inserted key must be hashable.

`HashSet{*items}` may accept ordered `Set`, `HashSet`, array, tuple or finite spreadable inputs, but every inserted element must be hashable.

This section does not introduce spread syntax; it only reserves the compatibility rule.

---

### 19. Rejected literal-prefix alternatives

#### 19.1. Rejected: one-letter collection prefixes

The following forms are intentionally not introduced:

```amber
u{a: 1} # rejected unordered/hash-backed map prefix
o{a: 1} # rejected ordered map prefix
u{1, 2} # rejected unordered/hash-backed set prefix
o{1, 2} # rejected ordered set prefix
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

### 20. Additional diagnostics

#### 20.1. Missing `hash`

```text
TypeError
HashMap key requires `hash`
```

```text
TypeError
HashSet element requires `hash`
```

#### 20.2. Non-Int hash result

```text
TypeError
`hash` must return Int
```

#### 20.3. Invalid hash key

```text
TypeError
value cannot be used as HashMap key because it is not Hashable
```

```text
TypeError
value cannot be used as HashSet element because it is not Hashable
```

#### 20.4. Invalid typed literal contents

```text
E_COLLECTION_LITERAL_KIND
Map literal constructor requires key-value entries
```

```text
E_COLLECTION_LITERAL_KIND
Set literal constructor requires elements, not key-value entries
```

#### 20.5. Rejected one-letter prefix

If the parser can recognize the likely intent, suggested diagnostic:

```text
E_COLLECTION_LITERAL_PREFIX
one-letter collection literal prefixes are not supported; use `HashMap{...}`, `HashSet{...}`, `Map{...}` or `Set{...}`
```

---

### 21. Additional conformance tests

#### 21.1. Typed literal construction

```amber
assert(Map{a: 1}.is_a?(Map))
assert(Set{1, 2}.is_a?(Set))
assert(HashMap{a: 1}.is_a?(HashMap))
assert(HashSet{1, 2}.is_a?(HashSet))
```

#### 21.2. Plain literals remain ordered

```amber
assert({a: 1}.is_a?(Map))
assert({1, 2}.is_a?(Set))
```

#### 21.3. Hashable custom object

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

m = HashMap{Point(1, 2)::ok}
assert(m[Point(1, 2)] ==:ok)
```

#### 21.4. Missing hash rejected

```amber
class EqOnly:
 def ==(other):
 true

assert_raises(TypeError):
 HashMap{EqOnly(): 1}

assert_raises(TypeError):
 HashSet{EqOnly()}
```

#### 21.5. Non-Int hash rejected

```amber
class BadHash:
 def ==(other):
 true

 def hash():
 "not int"

assert_raises(TypeError):
 HashMap{BadHash(): 1}
```

#### 21.6. Array key snapshot in HashMap

```amber
xs = [1, 2]
h = HashMap{xs::ok}

xs[0] = 9

assert(h[(1, 2)] ==:ok)
assert_raises(KeyError):
 h[(9, 2)]
```

#### 21.7. Duplicate numeric key overwrite

```amber
hm = HashMap{1: "int", 1.0: "float"}

assert(hm[1] == "float")
assert(hm[1.0] == "float")
```

#### 21.8. HashSet duplicate collapse

```amber
hs = HashSet{[1, 2], (1, 2)}
assert(hs.count() == 1)
```

#### 21.9. Typed literal kind diagnostics

```amber
assert_syntax_error:
 HashMap{1, 2, 3}

assert_syntax_error:
 HashSet{a: 1}
```

#### 21.10. Rejected one-letter prefixes

```amber
assert_syntax_error:
 u{a: 1}

assert_syntax_error:
 o{a: 1}
```

---

### 22. Updated normative decision summary

 now fixes the following additional decisions:

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
11. Spread expansion is deferred to.

## Spread expansion для calls и collection literals


---

### 0. Обзор решения

This section introduces spread expansion in Amber.

The accepted surface forms are:

```amber
fn(1, *args, **kwargs)

[1, *items, 9]

{1, *items, 9} # Set spread

{a: 1, **other, b: 2} # Map spread
```

`*` and `**` are contextual spread markers. They are not general prefix operators.

Spread expansion is intentionally placed in, not. defines value-keyed ordered `Map` / `Set`, explicit `HashMap` / `HashSet`, strict `Hashable`, and typed collection literals. builds on those collection semantics without changing them.

---

### 1. Design principles

#### 1.1. Spread markers are syntactic, not ordinary operators

The tokens `*` and `**` gain spread meaning only in these syntactic positions:

1. call argument lists;
2. array literal element lists;
3. set literal element lists;
4. map literal entry lists.

They are not valid as standalone prefix expressions:

```amber
*xs # invalid outside spread position
**opts # invalid outside spread position
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

#### 1.2. Evaluation is left-to-right

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

#### 1.3. Conditional spread follows conditional collection element semantics

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

#### 1.4. Infinite spread is forbidden

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

### 2. Positional call spread

#### 2.1. Surface form

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

#### 2.2. Accepted positional spread values

In, positional call spread accepts these finite values:

```text
Array
Tuple
finite Range
```

Future revisions may extend spreadability to a general finite iterable protocol. This section deliberately keeps the v1 rule narrow so expansion is deterministic and diagnostics are clear.

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

#### 2.3. No implicit `to_array()` / `to_tuple()` dispatch in core spread

Core spread does not silently invoke user-defined conversion methods such as `to_array()` or `to_tuple()`.

Rationale:

* spread should not hide arbitrary user code execution before argument assembly;
* conversion methods are ordinary dynamic dispatch and may have side effects;
* the core rule should remain predictable.

A future stdlib protocol may introduce explicit spread adapters.

---

### 3. Keyword call spread

#### 3.1. Surface form

```amber
fn(1, *args, mode::fast, **kwargs)
```

Example:

```amber
kwargs = {mode::fast, debug: true}
fn("build", **kwargs)
```

is equivalent to:

```amber
fn("build", mode::fast, debug: true)
```

---

#### 3.2. Accepted keyword spread values

In, keyword spread accepts `Map` and `HashMap` values whose keys are all `Symbol` keys.

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

#### 3.3. Duplicate keyword arguments are errors

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

#### 3.4. Positional and keyword ordering restrictions

Valid:

```amber
fn(1, *args, mode::fast, **opts)
```

Invalid:

```amber
fn(mode::fast, *args)
fn(**opts, 1)
fn(**opts, mode::fast)
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

### 4. Array literal spread

#### 4.1. Surface form

```amber
[1, *items, 9]
```

Example:

```amber
items = [2, 3]
[1, *items, 4]
### [1, 2, 3, 4]
```

---

#### 4.2. Lowering model

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

#### 4.3. Accepted array spread values

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

#### 4.4. Conditional array spread

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

### 5. Set literal spread

#### 5.1. Surface form

```amber
{1, *items, 9}
```

For explicit set literals:

```amber
Set{1, *items, 9}
HashSet{1, *items, 9}
```

---

#### 5.2. Lowering model

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

#### 5.3. Accepted set spread values

Set spread accepts:

```text
Array
Tuple
Set
HashSet
finite Range
```

Future revisions may allow any finite iterable.

For `Set{...}`, elements use value-key equality.

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

#### 5.4. Conditional set spread

Valid:

```amber
{:base,
 *extra_permissions if enabled?,
}
```

If `enabled?` is falsy, `extra_permissions` is not evaluated.

---

### 6. Map literal spread

#### 6.1. Surface form

```amber
{a: 1, **other, b: 2}
```

For explicit typed map literals:

```amber
Map{a: 1, **other, b: 2}
HashMap{a: 1, **other, b: 2}
```

---

#### 6.2. Lowering model

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

#### 6.3. Accepted map spread values

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

#### 6.4. Map spread accepts arbitrary map keys

Unlike keyword argument spread, map literal spread accepts arbitrary valid map keys.

Valid:

```amber
{**{1: "one", "name": "Ada",:age: 36}}
```

For `HashMap{...}`, all inserted keys must additionally be `Hashable`.

Valid only if every key is hashable:

```amber
HashMap{**{1: "one", "name": "Ada",:age: 36}}
```

---

#### 6.5. Duplicate map keys overwrite

Map literal spread uses ordinary map insertion/update semantics.

```amber
m = {a: 1, **{a: 2}}
m[:a] # 2

m2 = {**{a: 1}, a: 2}
m2[:a] # 2
```

This is intentionally different from call keyword spread, where duplicate keyword arguments are errors.

---

#### 6.6. Conditional map spread

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

### 7. Grammar additions

#### 7.1. Call arguments

Reference grammar:

```ebnf
CallArg::= Expr
 | Identifier ":" Expr
 | "*" Expr
 | "**" Expr
```

Ordering restrictions are semantic/parser validation rules, not precedence rules.

---

#### 7.2. Array and set elements

Reference grammar:

```ebnf
CollectionElement::= Expr CollectionCondition?
 | "*" Expr CollectionCondition?
```

Where:

```ebnf
CollectionCondition::= "if" Expr
 | "unless" Expr
```

---

#### 7.3. Map entries

Reference grammar:

```ebnf
MapEntry::= MapKey ":" Expr CollectionCondition?
 | "**" Expr CollectionCondition?
```

`MapKey` follows the expression-keyed map literal rules:

```amber
{name: v} # Symbol key:name
{(name): v} # expression key
{"name": v} # Str key
{:name: v} # explicit Symbol key
{(1..5): v} # parenthesized Range key
```

---

### 8. AST and HIR representation

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

### 9. Runtime helpers

Recommended runtime helper boundaries:

```text
spread_positional_values(vm, frame, value) -> Value[] | Error
spread_array_values(vm, frame, value) -> Value[] | Error
spread_set_values(vm, frame, value) -> Iterable<Value> | Error
spread_map_entries(vm, frame, value) -> Iterable<(Value, Value)> | Error
spread_keyword_entries(vm, frame, value) -> Iterable<(Symbol, Value)> | Error
```

For `HashMap` / `HashSet` targets, insertion helpers must enforce the `Hashable` protocol.

---

### 10. Bytecode strategy

This section does not require one opcode per spread form.

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

### 11. Diagnostics

#### 11.1. Spread outside valid position

```text
E_SPREAD_POSITION
`*` spread is only valid in call arguments and collection literals
```

```text
E_KWARG_SPREAD_POSITION
`**` spread is only valid in call arguments and map literals
```

#### 11.2. Invalid positional spread value

```text
TypeError
positional spread requires Array, Tuple, or finite Range
```

#### 11.3. Invalid array spread value

```text
TypeError
array spread requires Array, Tuple, or finite Range
```

#### 11.4. Invalid set spread value

```text
TypeError
set spread requires Array, Tuple, Set, HashSet, or finite Range
```

#### 11.5. Invalid map spread value

```text
TypeError
map spread requires Map or HashMap
```

#### 11.6. Invalid keyword spread value

```text
TypeError
keyword argument spread requires Map or HashMap with Symbol keys
```

#### 11.7. Duplicate keyword argument

```text
ArgumentError
duplicate keyword argument `name`
```

#### 11.8. Infinite spread

```text
InfiniteCollectionError
cannot spread an infinite/open-ended collection
```

#### 11.9. Hash target rejects non-hashable spread element/key

```text
TypeError
HashSet element must be Hashable
```

```text
TypeError
HashMap key must be Hashable
```

---

### 12. Formatter rules

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
 mode::fast,
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

### 13. Conformance corpus

#### 13.1. Positional call spread

```amber
args = [2, 3]
assert(fn(1, *args, 4) == fn(1, 2, 3, 4))
```

#### 13.2. Tuple call spread

```amber
args = (2, 3)
assert(fn(1, *args) == fn(1, 2, 3))
```

#### 13.3. Range call spread

```amber
assert(fn(*(1..3)) == fn(1, 2, 3))
```

#### 13.4. Open-ended range spread rejected

```amber
assert_raises(InfiniteCollectionError):
 fn(*(1..))
```

#### 13.5. Keyword spread with symbol keys

```amber
opts = {mode::fast, debug: true}
assert(fn(**opts) == fn(mode::fast, debug: true))
```

#### 13.6. Keyword spread rejects string key

```amber
assert_raises(TypeError):
 fn(**{"mode"::fast})
```

#### 13.7. Duplicate keyword rejected

```amber
assert_raises(ArgumentError):
 fn(mode::slow, **{mode::fast})
```

#### 13.8. Array spread

```amber
assert([1, *[2, 3], 4] == [1, 2, 3, 4])
assert([1, *(2..4), 5] == [1, 2, 3, 4, 5])
```

#### 13.9. Conditional array spread suppresses expression

```amber
called = false

def extra():
 called = true
 [2, 3]

xs = [1, *extra() if false, 4]
assert(xs == [1, 4])
assert(called == false)
```

#### 13.10. Set spread collapse

```amber
s = {1, *[1, 2, 3]}
assert(s.count() == 3)
```

#### 13.11. HashSet spread enforces Hashable

```amber
class NoHash:
 def ==(other):
 true

assert_raises(TypeError):
 HashSet{* [NoHash()]}
```

#### 13.12. Map spread with arbitrary keys

```amber
m = {a: 1, **{1: "one", "name": "Ada"}}
assert(m[:a] == 1)
assert(m[1] == "one")
assert(m["name"] == "Ada")
```

#### 13.13. Map spread duplicate overwrite

```amber
m = {a: 1, **{a: 2}}
assert(m[:a] == 2)
```

#### 13.14. HashMap spread enforces Hashable keys

```amber
class EqOnly:
 def ==(other):
 true

assert_raises(TypeError):
 HashMap{**{EqOnly(): 1}}
```

#### 13.15. Argument order diagnostics

```amber
assert_syntax_error:
 fn(mode::fast, *args)

assert_syntax_error:
 fn(**opts, 1)
```

---

### 14. Rejected alternatives

#### 14.1. General prefix spread operators

Rejected:

```amber
x = *items
x = **opts
```

Reason: spread has meaning only when a receiver context defines how expanded values are consumed. A general prefix operator would require standalone runtime values for “spread packs”, complicating evaluation and diagnostics.

---

#### 14.2. Implicit conversion through `to_array()` / `to_map()`

Rejected for core spread.

Reason: spread should not silently call user-defined conversion methods with arbitrary side effects. Future stdlib protocols may add explicit adapters.

---

#### 14.3. String-key keyword spread

Rejected:

```amber
fn(**{"name": "Ada"})
```

Reason: `{"name": value}` is a real `Str` key in. Keyword argument names are `Symbol`-like call names, not strings.

---

### 15. Normative decision summary

 fixes the following decisions:

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

## Name-indifferent maps, strict maps и kwargs view


---

#### 0. Обзор решения

This section revises the Amber `Map` / `HashMap` key model and the Amber keyword spread model.

The core decision is:

```amber
Map / HashMap # name-indifferent by default for Symbol/Str name keys
StrictMap / StrictHashMap # exact-key containers preserving Symbol and Str as distinct keys
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

m[:user_id] # "symbol key"
m["user_id"] # "string key"
```

Keyword spread also becomes application-friendly:

```amber
fn(**opts)
```

is valid when `opts` can produce a keyword-argument view and every produced key can be converted to a valid Amber keyword name. Conceptually, `**opts` behaves like spread over `opts.kwargs`, but implementations may lower this as an intrinsic/protocol operation rather than an ordinary property access.

---

#### 1. Background and prerequisites

Amber already has several interacting design choices that make this section important.

##### 1.1. Symbol-key shorthand

Amber source maps use Ruby-like shorthand:

```amber
{name: value}
```

which is syntax-faithfully represented as a symbol-key entry:

```amber
{:name: value}
```

This shorthand is idiomatic for application payloads, options, configuration objects, and pattern matching.

##### 1.2. External data usually has string keys

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

payload[:user_id] # KeyError or null in strict string-key maps
payload["user_id"] # works
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

##### 1.3. Exact Symbol/Str separation is rarer than accidental mismatch

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

#### 2. Rationale

##### 2.1. Default ergonomics should match application-data reality

Amber aims to be Ruby-inspired and expression-oriented while retaining deterministic, explicit runtime semantics. In Ruby ecosystems, a long-standing pain point is the distinction between symbol keys written in code and string keys returned by JSON. Rails addresses this with `HashWithIndifferentAccess`; however, requiring an opt-in wrapper at every boundary is easy to forget.

Amber can make the safer choice at the language-container level:

```amber
payload[:id]
payload["id"]
```

should address the same entry for ordinary maps.

##### 2.2. Pattern matching must not be fragile at serialization boundaries

Pattern matching is part of Amber's core model. The following should be robust:

```amber
payload = Json.parse(body)

case payload
in {id: id, email: email}
 User(id, email)
end
```

If ordinary maps distinguish `:id` and `"id"`, then map patterns become brittle around JSON and host interop. Name-indifferent maps make pattern matching usable for external payloads without boilerplate normalization.

##### 2.3. Strict maps remain available for precise modeling

The language should not remove expressiveness. Code that intentionally needs exact-key separation can say so:

```amber
StrictMap{name: 1, "name": 2}
```

This turns a rare, subtle distinction into an explicit type-level choice.

##### 2.4. Avoid global string-to-symbol interning for external data

This section does not require JSON or external strings to become globally interned symbols. Ordinary `Map` may store canonical name keys as strings or as an internal `NameKey` representation. This avoids unbounded symbol interning from untrusted input while preserving ergonomic access.

##### 2.5. Keyword spread should validate, not reject common maps upfront

Given:

```amber
opts = {"mode"::fast, "limit": 10}
fn(**opts)
```

rejecting this solely because keys originated as strings is unnecessarily strict. The relevant question is whether every key can become a valid Amber keyword name. Therefore `**opts` should validate keys and raise a precise error only when invalid keys occur.

---

#### 3. Terminology

| Term | Meaning |
|---|---|
| Name key | A key whose addressable name is a textual identifier-like name shared by `Symbol(:name)` and `Str("name")`. |
| Name-indifferent map | A map where `Symbol(:name)` and `Str("name")` address the same entry. |
| Exact-key map | A map where key type participates in identity/equality and `Symbol(:name)` is distinct from `Str("name")`. |
| Canonical name-key export | The value exposed by APIs such as `keys()` for a name key. In this section it is `Str`. |
| Keyword-convertible key | A key that can be converted to a valid Amber keyword argument name during `**` spread. |
| Kwargs view | A finite map-like view used by keyword spread after validation. |

---

#### 4. Design principles

##### 4.1. Ordinary maps are application-data maps

Plain map literals and `Map` constructors create name-indifferent maps:

```amber
{name: 1}
{"name": 1}
Map{name: 1}
HashMap{"name": 1}
```

All of these support both symbol and string lookup for the same textual name.

##### 4.2. Strictness is explicit

Exact-key behavior requires an explicit strict type:

```amber
StrictMap{name: 1, "name": 2}
StrictHashMap{name: 1, "name": 2}
```

##### 4.3. Pattern matching follows the matched map's semantics

For ordinary `Map` / `HashMap`, named-key patterns are name-indifferent.

For `StrictMap` / `StrictHashMap`, named-key patterns remain exact-symbol-key based unless the strict object explicitly exposes a different `deconstruct_keys` behavior.

##### 4.4. Keyword spread is validation-based

`**expr` obtains a kwargs view and validates keys. It is not limited to maps that physically store symbol keys.

##### 4.5. User objects may participate through `kwargs`, but the protocol is narrow

User objects may participate in keyword spread when they expose a readable `kwargs` property. This uses Amber's property descriptor model rather than implicit nullary method invocation.

The returned value must itself be a valid keyword-spread value or keyword-entry view.

---

#### 5. Surface syntax and container types

##### 5.1. Ordinary map literals

Plain map literals create ordinary `Map`:

```amber
m = {name: "Ada"}
```

Lookup is name-indifferent:

```amber
m[:name] # "Ada"
m["name"] # "Ada"
```

String-key literal entries behave the same way:

```amber
m = {"name": "Ada"}

m[:name] # "Ada"
m["name"] # "Ada"
```

##### 5.2. Explicit ordinary map constructors

```amber
Map{name: 1}
HashMap{"name": 1}
```

Both use name-indifferent key normalization for `Symbol`/`Str` name keys.

##### 5.3. Strict exact-key containers

Amber adds:

```amber
StrictMap
StrictHashMap
```

Examples:

```amber
m = StrictMap{name: 1, "name": 2}

m[:name] # 1
m["name"] # 2
m.keys() # [:name, "name"]
```

Strict containers preserve the current exact-key behavior for `Symbol` and `Str`.

---

#### 6. Ordinary `Map` / `HashMap` key normalization

##### 6.1. Name-key normalization

For ordinary `Map` and `HashMap`:

```text
Symbol(name) -> NameKey(name)
Str(name) -> NameKey(name)
```

`NameKey(name)` is an abstract runtime key. Implementations may store it as:

1. an internal tagged key object;
2. a canonical string key;
3. a compact symbol/string pair with canonical comparison;
4. another representation with identical observable behavior.

##### 6.2. Canonical export

Name keys export as `Str` through ordinary public key enumeration APIs:

```amber
m = {name: 1}

m.keys() # ["name"]
m.entries() # [["name", 1]] or equivalent entry representation
```

Rationale: external data formats, serialization, JSON interop, and diagnostics are better served by string export than by creating or preserving symbol provenance.

Implementations may expose provenance through debug/reflection APIs, but ordinary semantics must not depend on provenance.

##### 6.3. Duplicate overwrite

Duplicate name keys overwrite the value and preserve the first normalized position:

```amber
m = {name: 1, "name": 2}

m[:name] # 2
m["name"] # 2
m.keys() # ["name"]
```

Insertion order is based on the first occurrence of the normalized key.

##### 6.4. Other key types

Non-`Symbol`/`Str` keys continue to follow ordinary value-keyed map semantics:

```amber
m = {1: "int", 1.0: "float"}
```

The exact equality relationship between numeric or structural values remains governed by the existing runtime key equality rules for the corresponding map type.

This section only changes the default treatment of `Symbol` and `Str` textual name keys in ordinary maps.

---

#### 7. Strict exact-key map semantics

`StrictMap` and `StrictHashMap` preserve exact `Symbol` versus `Str` key identity/equality.

```amber
m = StrictMap{name: "symbol", "name": "string"}

m[:name] # "symbol"
m["name"] # "string"
```

##### 7.1. Duplicate overwrite in strict maps

Duplicates are determined by exact-key semantics:

```amber
m = StrictMap{name: 1,:name: 2, "name": 3}

m[:name] # 2
m["name"] # 3
m.keys() # [:name, "name"]
```

The first exact key position is preserved for duplicates of that exact key.

##### 7.2. Use cases for strict maps

Strict maps are intended for:

1. language tooling and AST metadata where symbolic keys and external field names must be distinguished;
2. protocol bridges that must preserve exact source key types;
3. debugging, migration, compatibility layers, and conformance tests;
4. advanced metaprogramming where key type has semantic meaning;
5. security-sensitive adapters that must reject ambiguous name-key collapse.

---

#### 8. JSON and external data integration

##### 8.1. Default JSON behavior

`Json.parse` should return ordinary `Map` by default:

```amber
payload = Json.parse(body)

payload[:user_id]
payload["user_id"]
```

This means deserialized object keys are accessible through both symbol and string addressing.

##### 8.2. Pattern matching over JSON payloads

```amber
payload = Json.parse(body)

case payload
in {user_id: id, name: name}
 User(id, name)
end
```

The pattern must match JSON objects with string keys.

##### 8.3. Exact preservation option

Exact preservation may be requested explicitly:

```amber
payload = Json.parse(body, map: StrictMap)
```

or, if the JSON package prefers key-mode terminology:

```amber
payload = Json.parse(body, keys::strict)
```

This section recommends the constructor-oriented spelling:

```amber
Json.parse(body, map: StrictMap)
```

because it composes with custom map implementations.

##### 8.4. No default symbolization of external strings

JSON keys should not be converted into global symbols by default. Ordinary maps may expose name-indifferent access without symbol interning.

---

#### 9. Pattern matching semantics

##### 9.1. Ordinary maps

Named-key map patterns use ordinary lookup semantics of the matched map.

For ordinary `Map` / `HashMap`, lookup is name-indifferent:

```amber
case {"user_id": 123}
in {user_id: id}
 id # 123
end
```

The following forms are equivalent for matching purposes in ordinary maps:

```amber
{user_id: 123}
{:user_id: 123}
{"user_id": 123}
```

##### 9.2. Strict maps

For strict maps, named-key patterns request exact symbol keys unless the object exposes a custom `deconstruct_keys` implementation.

```amber
case StrictMap{"user_id": 123}
in {user_id: id}
 id
else
 null
end
### => null
```

```amber
case StrictMap{user_id: 123}
in {user_id: id}
 id
end
### => 123
```

##### 9.3. Rest capture

For ordinary maps, rest capture observes canonical exported keys:

```amber
case {"id": 1, "name": "Ada"}
in {id: id, **rest}
 rest.keys() # ["name"]
end
```

For strict maps, rest capture preserves exact keys.

##### 9.4. `deconstruct_keys`

`Map#deconstruct_keys(keys)` and `HashMap#deconstruct_keys(keys)` must use name-indifferent lookup.

`StrictMap#deconstruct_keys(keys)` and `StrictHashMap#deconstruct_keys(keys)` use exact-symbol lookup for named-key patterns unless overridden by a user-defined/custom implementation.

---

#### 10. Keyword spread semantics

##### 10.1. Surface form

Keyword spread remains:

```amber
fn(**opts)
```

##### 10.2. Conceptual lowering

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

##### 10.3. Evaluation order

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

##### 10.4. Accepted operands

Keyword spread accepts:

1. `Map`;
2. `HashMap`;
3. `StrictMap`;
4. `StrictHashMap`;
5. objects exposing a readable `kwargs` property whose result is itself a valid keyword-spread value or keyword-entry view.

Future revisions may extend this protocol to dedicated keyword-entry view types. This section deliberately does not accept arbitrary arrays of pairs, generic enumerables, or `each_pair`-style protocols.

##### 10.5. Keyword-convertible keys

A key is keyword-convertible iff it is one of:

1. `Symbol(name)` where `name` is a valid Amber keyword argument name;
2. `Str(name)` where `name` is a valid Amber keyword argument name;
3. ordinary `Map` / `HashMap` `NameKey(name)` where `name` is a valid Amber keyword argument name.

Valid:

```amber
fn(**{mode::fast})
fn(**{"mode"::fast})
fn(**{:mode::fast})
```

Invalid:

```amber
fn(**{"user-id": 1})
fn(**{"first name": "Ada"})
fn(**{"1st": true})
fn(**{1: "one"})
fn(**{null: "x"})
```

##### 10.6. Keyword name grammar

Keyword argument names use Amber parameter-name identifier rules.

If the base language permits parameter identifiers with `?` or `!` suffixes, then such names are valid for keyword spread. If parameter identifiers exclude those suffixes, keyword spread must reject them.

This section does not independently expand identifier grammar.

##### 10.7. Duplicate keyword detection

After key conversion, duplicate keyword names are an error unless the duplicate was already collapsed by the source map before spread.

Ordinary `Map` collapses name duplicates during construction:

```amber
opts = {name: 1, "name": 2}
fn(**opts) # ok; keyword name receives 2
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

##### 10.8. Invalid key diagnostics

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

##### 10.9. Non-spreadable operands

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

#### 11. User-defined `kwargs` property protocol

##### 11.1. Motivation

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

##### 11.2. Property-based protocol

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

##### 11.3. Result validation

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

##### 11.4. Error attribution

Errors raised while evaluating the `kwargs` property propagate normally, but validation errors after property evaluation are attributed to the `**` spread site.

##### 11.5. No implicit nullary method call

The protocol uses a property, not an implicit call to a method named `kwargs`.

If a class declares:

```amber
def kwargs():
 {mode::fast}
```

then `fn(**obj)` does not implicitly call `obj.kwargs()` unless the language's property/method model separately defines such behavior. To participate, the class should declare:

```amber
prop kwargs:
 {mode::fast}
```

or expose an equivalent property descriptor.

---

#### 12. HIR and runtime lowering

##### 12.1. Map literal lowering

Ordinary map literals lower to construction of ordinary `Map` with name-key normalization:

```amber
{name: 1, "name": 2}
```

HIR must preserve syntax-faithful source information for diagnostics and formatting but the runtime construction operation normalizes both keys to the same `NameKey("name")`.

##### 12.2. Strict map literal lowering

```amber
StrictMap{name: 1, "name": 2}
```

lowers to strict map construction. No name-key collapse occurs between `Symbol(:name)` and `Str("name")`.

##### 12.3. Lookup lowering

For ordinary maps:

```amber
m[:name]
m["name"]
```

both lower to keyed lookup whose runtime key normalization resolves to `NameKey("name")`.

For strict maps, lookup uses exact-key semantics.

##### 12.4. Pattern lowering

Named-key map pattern lowering must call `deconstruct_keys` or the equivalent pattern lookup helper using the matched object's semantics.

For ordinary maps, the helper uses name-indifferent lookup.

For strict maps, the helper uses exact-symbol lookup unless a custom implementation is provided.

##### 12.5. Keyword spread lowering

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

##### 12.6. Bytecode considerations

Implementations may add helper opcodes or runtime calls such as:

```text
KWARGS_VIEW
KWARGS_VALIDATE
KWARGS_MERGE
CALL_KW
```

or fold these into existing `CALL` metadata. Observable behavior must remain deterministic.

---

#### 13. Diagnostics

Suggested diagnostic/error names:

| Code / Error | Situation |
|---|---|
| `KeywordArgumentError` | Runtime class for invalid keyword spread conversion or duplicate keyword after spread. |
| `E_KWARG_SPREAD_KEY` | Key cannot be converted to a valid keyword name. |
| `E_KWARG_SPREAD_DUPLICATE` | Duplicate keyword produced after spread key conversion. |
| `E_KWARG_SPREAD_OPERAND` | Operand cannot produce a kwargs view. |
| `E_KWARG_SPREAD_RESULT` | User object's `kwargs` property produced an invalid spread value. |
| `E_STRICT_MAP_DUPLICATE_DEBUG` | Optional debug/lint diagnostic for exact keys that become duplicate keyword names under spread. |

##### 13.1. Invalid key

```amber
fn(**{"user-id": 1})
```

```text
KeywordArgumentError
map key `"user-id"` cannot be used as a keyword argument name
```

##### 13.2. Duplicate after strict spread

```amber
fn(**StrictMap{name: 1, "name": 2})
```

```text
KeywordArgumentError
duplicate keyword argument `name`
```

##### 13.3. Non-spreadable operand

```amber
fn(**42)
```

```text
KeywordArgumentError
object of type Int cannot be used as keyword spread operand
```

##### 13.4. Invalid `kwargs` result

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

#### 14. Совместимость и модель поведения

##### 14.1. Изменения относительно предыдущей модели 

The behavior where ordinary `Map` treats `:name` and `"name"` as distinct keys is replaced.

Old ordinary map behavior moves to:

```amber
StrictMap
StrictHashMap
```

##### 14.2. Changes to map pattern matching

Old behavior:

```amber
case {"id": 1}
in {id: id}
 id
else
 null
end
### old result: null
```

New behavior for ordinary maps:

```amber
### new result: 1
```

Strict maps preserve old behavior:

```amber
case StrictMap{"id": 1}
in {id: id}
 id
else
 null
end
### null
```

##### 14.3. Изменения относительно предыдущей модели keyword spread

Keyword spread is no longer restricted to maps with physical symbol keys. It validates name-convertible keys instead.

```amber
fn(**{"mode"::fast}) # now valid
```

Invalid keys still raise:

```amber
fn(**{"user-id"::bad}) # KeywordArgumentError
```

##### 14.4. Рекомендации по применению

Code that intentionally relied on `:name` and `"name"` being separate in ordinary maps should migrate to `StrictMap`:

```amber
### old
m = {name: 1, "name": 2}

### new
m = StrictMap{name: 1, "name": 2}
```

Code that performed manual JSON key normalization can often remove it:

```amber
### old
payload = Json.parse(body).symbolize_keys()

### new
payload = Json.parse(body)
payload[:id]
```

---

#### 15. Security and robustness notes

##### 15.1. Avoid symbol-interning denial of service

Name-indifferent lookup must not require turning every external string key into a globally interned symbol. Implementations should prefer canonical string/name-key storage for ordinary maps.

##### 15.2. Keyword spread validates keys at call boundaries

External maps may be used as keyword spread operands only if every key is a valid keyword name. This prevents invalid external field names such as `"user-id"` from silently becoming call keywords.

##### 15.3. Explicit strict containers for ambiguous data

When key provenance matters, use `StrictMap` or `StrictHashMap`.

##### 15.4. User-defined `kwargs` should be narrow

The `kwargs` property protocol is intentionally narrower than generic enumeration. This avoids surprising calls from arbitrary pair-like objects and keeps call diagnostics deterministic.

---

#### 16. Conformance tests

##### 16.1. Ordinary map name-indifferent lookup

```amber
m = {name: "Ada"}
assert_equal("Ada", m[:name])
assert_equal("Ada", m["name"])
```

##### 16.2. String literal key lookup by symbol

```amber
m = {"name": "Ada"}
assert_equal("Ada", m[:name])
assert_equal("Ada", m["name"])
```

##### 16.3. Duplicate collapse

```amber
m = {name: 1, "name": 2}
assert_equal(2, m[:name])
assert_equal(2, m["name"])
assert_equal(["name"], m.keys())
```

##### 16.4. Strict map preserves distinction

```amber
m = StrictMap{name: 1, "name": 2}
assert_equal(1, m[:name])
assert_equal(2, m["name"])
assert_equal([:name, "name"], m.keys())
```

##### 16.5. JSON pattern matching

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

##### 16.6. Strict pattern mismatch

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

##### 16.7. Keyword spread from symbol-key map

```amber
def f(mode:):
 mode

assert_equal(:fast, f(**{mode::fast}))
```

##### 16.8. Keyword spread from string-key map

```amber
def f(mode:):
 mode

assert_equal(:fast, f(**{"mode"::fast}))
```

##### 16.9. Keyword spread invalid string key

```amber
def f(**kwargs):
 kwargs

assert_raises(KeywordArgumentError):
 f(**{"user-id": 1})
```

##### 16.10. Keyword spread invalid non-name key

```amber
def f(**kwargs):
 kwargs

assert_raises(KeywordArgumentError):
 f(**{1: "one"})
```

##### 16.11. Strict duplicate keyword after spread

```amber
def f(name:):
 name

assert_raises(KeywordArgumentError):
 f(**StrictMap{name: 1, "name": 2})
```

##### 16.12. User object `kwargs` property

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

##### 16.13. Invalid user object `kwargs` result

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

#### 17. Reference implementation checklist

##### Parser / AST

- Preserve map literal key surface form for formatter and diagnostics.
- Preserve `StrictMap{...}` / `StrictHashMap{...}` typed literal constructor nodes.
- Preserve `**expr` spread nodes.

##### HIR

- Lower ordinary map literals to name-indifferent map construction.
- Lower strict map constructors to exact-key construction.
- Lower keyword spread to explicit kwargs-view and validation stages or equivalent intrinsic nodes.

##### Runtime

- Add `NameKey` normalization for ordinary `Map` / `HashMap`.
- Add `StrictMap` / `StrictHashMap` types.
- Implement canonical string export for ordinary name keys.
- Implement `Map#deconstruct_keys` with name-indifferent lookup.
- Implement strict map exact-key `deconstruct_keys` behavior.
- Implement keyword-spread validation and duplicate detection.
- Implement `kwargs` property participation for user objects.

##### Diagnostics

- Add deterministic `KeywordArgumentError` messages.
- Attribute validation errors to the spread site.
- Avoid memory addresses or host-specific formatting in diagnostics.

##### Stdlib / JSON

- Make `Json.parse` return ordinary `Map` by default.
- Add explicit strict preservation option, preferably `map: StrictMap`.
- Document that JSON keys are not globally symbolized by default.

---

#### 18. Оставшиеся варианты реализации

The following are intentionally left to implementation, provided observable behavior matches this section:

1. whether ordinary map `NameKey` is represented as a tagged internal value or canonical string;
2. whether keyword names are represented internally as symbols, strings, or call-site keyword IDs;
3. whether `KWARGS_VIEW` is a bytecode opcode, HIR intrinsic, runtime helper, or folded into `CALL`;
4. whether reflection APIs expose key provenance for name-indifferent map entries;
5. whether a future dedicated `Kwargs` view type is introduced.

---

#### 19. Summary

This section makes ordinary Amber associative containers safer and more useful for the most common application-data workflows.

The default becomes:

```amber
m = {name: "Ada"}

m[:name] # "Ada"
m["name"] # "Ada"
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
fn(**{"mode"::fast}) # ok
fn(**{"user-id": 1}) # KeywordArgumentError
```

Exact distinction remains available when it is truly needed:

```amber
StrictMap{name: "symbol", "name": "string"}
```

The resulting model favors the common, less error-prone path while preserving precise low-level semantics through explicit strict containers.

# Часть III. Standard library и runtime-facing API

## Stdlib: план и приоритеты слоя


### Контекст

Язык Amber уже зафиксирован. Следующий слой развития — стандартная библиотека и runtime-facing API вокруг уже существующих решений:

- базовые структуры данных и их методы;
- `task` / threading / async primitives;
- `Kernel.watch` / notebook watch profile;
- `io` и networking;
- error registry и conformance corpus.

Главный принцип: не делать `networking` раньше, чем закрыты collections, task/sync и IO resource contracts.

---

### Рекомендуемый порядок развития

```text
collections
 -> task/sync
 -> watch profile
 -> io foundation
 -> networking
 -> HTTP client
 -> advanced concurrency
```

---

## S1. Core collections stdlib

### Приоритет

**Самый высокий.**

Collections должны идти первыми, потому что ими будут пользоваться почти все остальные части stdlib: async, networking, watch, loader diagnostics, test runner и notebook runtime.

### Цель

Закрыть минимальный chainable API для:

- `Array`
- `Tuple`
- `Range`
- `Set`
- `Map`
- `LazySeq`

### Базовый Enumerable-like contract

```amber
collection.each |x|:...

collection.map |x|:...

collection.flat_map |x|:...

collection.select |x|:...

collection.reject |x|:...

collection.reduce(init) |acc, x|:...

collection.find |x|:...

collection.any? |x|:...

collection.all? |x|:...

collection.none? |x|:...

collection.first()
collection.count()
collection.to_a()
collection.lazy()
```

### Map contract

```amber
map.each |k, v|:...

map.map |k, v|:...

map.select |k, v|:...

map.reject |k, v|:...

map.transform |k, v|:
 (new_key, new_value)

map.transform_values |v, k|:...

map.keys()
map.values()
map.entries()
```

### Error classes

Минимально нужны:

```text
EmptyCollectionError
IndexError
KeyError
ArgumentError
TypeError
```

### DoD

- green corpus для `Array`, `Tuple`, `Range`, `Set`, `Map`, `LazySeq`;
- тесты на пустые коллекции;
- тесты на mutation during iteration;
- тесты на lazy materialization;
- тесты на block arity;
- тесты на exception propagation из блока.

---

## S2. Task / threading / async modules

### Приоритет

**Второй после collections.**

Это не просто «удобная библиотека», а surface API поверх no-GIL runtime: strands, tasks, worker pool, structured concurrency, cancellation, channels, mutexes и atomics.

### Namespace proposal

```amber
import task
from sync import Channel, Mutex, Atomic
```

### Task API

```amber
handle = task.async:
 compute()
```

```amber
handle = task.spawn:
 compute_shareable()
```

```amber
task.sleep(0.1)
task.yield()
```

```amber
handle.wait()
handle.wait(timeout: 1.0)
handle.cancel()
handle.cancelled?()
handle.done?()
handle.result()
handle.failure()
```

### Semantic split

| API | Семантика |
|---|---|
| `task.async` | дочерняя task в том же strand |
| `task.spawn` | дочерняя task в новом strand |
| `task.sleep` | suspend current task |
| `task.yield` | cooperative yield |
| `handle.wait` | join/wait |
| `handle.cancel` | request cancellation |

### Channel API

```amber
ch = Channel.new(capacity: 16)

ch.send(value)
value = ch.recv()

ch.close()
ch.closed?()
```

### Channel rules

- FIFO;
- `send` cross-strand требует shareable payload;
- `recv` из closed empty channel бросает `ChannelClosedError`;
- `close()` idempotent или clearly specified;
- send после close бросает `ChannelClosedError`.

### Mutex API

```amber
m = Mutex.new()

m.lock()
m.unlock()

m.synchronize:
 critical_section()
```

### Mutex rules

- mutex non-reentrant;
- повторный `lock` той же task/strand бросает `DeadlockError`;
- unlock не-владельцем бросает `OwnershipError` или `RuntimeError`-подкласс;
- `synchronize` гарантирует unlock через unwind.

### Atomic API

```amber
a = Atomic.new(0)

a.get()
a.set(1)
a.compare_and_set(1, 2)

a.update |x|:
 x + 1
```

### Atomic rules

- seq-cst semantics в reference profile;
- payload должен быть atomic-compatible;
- `update` — stdlib sugar поверх CAS loop.

### DoD

- scheduler corpus;
- parallel strand tests;
- isolation violation tests;
- cancellation propagation tests;
- timeout tests;
- channel FIFO tests;
- non-reentrant mutex tests;
- atomic compare-and-set tests.

---

## S3. Watch profile

### Приоритет

**После task/sync, но до networking.**

Watch — это profile stdlib / notebook runtime layer, а не обычный core object method.

### Canonical API

Canonical spelling лучше оставить таким:

```amber
Kernel.watch(x)
Kernel.watch(@x)
Kernel.watch(@@x)
```

Не стоит вводить `object.watch` как core API на этом этапе.

### Почему не `object.watch`

`watch` выглядит как обычный method call, но на самом деле требует:

- compiler/kernel intrinsic recognition;
- restricted target grammar;
- special lowering;
- watch-cell replacement;
- revision tracking;
- notebook dependency capture.

Поэтому `object.watch` создаст ложное ожидание, что это обычный dynamic method.

### Допустимые targets

```amber
Kernel.watch(x)
Kernel.watch(@x)
Kernel.watch(@@x)
```

### Недопустимые targets

```amber
Kernel.watch(foo())
Kernel.watch(user.name)
Kernel.watch(xs[0])
Kernel.watch(1 + 2)
```

### Optional ergonomic alias

В Notebook profile можно добавить:

```amber
from notebook import watch

watch(x)
watch(@x)
watch(@@x)
```

Но lowering всё равно должен идти в canonical watch intrinsics.

### Internal runtime objects

```text
WatchCell
WatchObjectState
WatchEvent
WatchSubscriber
```

### Minimal WatchCell

```text
WatchCell(
 value,
 revision,
 subscribers
)
```

### Minimal WatchObjectState

```text
WatchObjectState(
 object_id,
 object_revision,
 field_revisions,
 subscribers
)
```

### Watch rules

- `Kernel.watch` не bump’ает `world_epoch`;
- watch не является world mutation;
- watch не меняет production semantics;
- object identity не меняется;
- class/equality/dispatch semantics не меняются;
- failed write не публикует watch event;
- successful watched ivar write bump’ает revision.

### Priority внутри watch

1. local/top-level binding watch;
2. ivar/class-var watch;
3. dependency capture для notebook cells;
4. object field revision tracking;
5. subscriber/event API;
6. debug inspection API.

### Не делать в первой итерации

- deep watch object graph;
- watch arbitrary expression;
- watch indexing;
- watch method call result;
- watch production semantics;
- `Object#watch` как обычный method.

---

## S4. IO foundation

### Приоритет

**Перед networking.**

Networking нельзя нормально сделать без общего IO/resource layer.

### Suggested module

```amber
import io
```

### Core protocols

```amber
reader.read(max_bytes:)
writer.write(bytes)
resource.close()
resource.closed?()
```

### Suggested interfaces/classes

```text
io.Reader
io.Writer
io.Closeable
io.Buffer
io.Bytes
io.ByteArray
io.Error
```

### Resource rules

- close должен быть idempotent или строго specified;
- read/write after close бросают `ClosedResourceError`;
- blocking operations должны иметь cancellation points;
- timeout должен быть согласован с `task` runtime;
- native handles не должны протекать как raw pointer.

### Minimal errors

```text
IOError
ClosedResourceError
TimeoutError
CancelledError
WouldBlockError
```

### DoD

- read/write/close corpus;
- cancellation during IO tests;
- timeout tests;
- close during pending read/write tests;
- native handle lifetime tests.

---

## S5. Low-level networking

### Приоритет

**После IO foundation.**

Первый networking layer должен быть низкоуровневым: TCP + DNS. HTTP лучше делать позже.

### Suggested modules

```amber
from net import TcpListener, TcpStream
```

### TcpListener API

```amber
listener = TcpListener.bind("127.0.0.1", 8080)

loop:
 conn = listener.accept()
 task.async:
 handle(conn)
```

### TcpStream API

```amber
stream = TcpStream.connect("example.com", 80, timeout: 5.0)

stream.write(bytes)
chunk = stream.read(max_bytes: 4096)

stream.close()
```

### DNS API

```amber
from net import dns

addresses = dns.resolve("example.com")
```

### Minimal networking errors

```text
NetworkError
ConnectionError
ConnectionRefusedError
ConnectionResetError
AddressInUseError
DnsError
TimeoutError
CancelledError
ClosedResourceError
```

### Rules

- all blocking operations are cancellation points;
- all blocking operations can support timeout;
- TCP streams implement `io.Reader`, `io.Writer`, `io.Closeable`;
- no raw socket handle exposure in safe stdlib;
- OS-specific errors normalize into Amber error classes.

### DoD

- connect success/failure tests;
- accept loop tests;
- cancellation during connect/read/write;
- timeout during connect/read/write;
- close during blocked read;
- DNS success/failure;
- deterministic error normalization.

---

## S6. HTTP client

### Приоритет

**После TCP + IO.**

HTTP should be client-only first.

### Suggested module

```amber
from net.http import Client, Request
```

### Minimal API

```amber
client = Client.new(timeout: 10.0)

res = client.get("https://example.com")

res.status
res.headers
res.body_bytes()
res.body_text()
```

### Request API

```amber
req = Request.new(
 method: "POST",
 url: "https://example.com/api",
 headers: {"content-type": "application/json"},
 body: bytes
)

res = client.send(req)
```

### HTTP response

```text
Response(
 status,
 headers,
 body
)
```

### Rules

- redirects off by default or explicitly configured;
- body reading is cancellation-aware;
- timeout covers connect + read unless split later;
- no server framework in first HTTP layer;
- TLS can be feature-gated if implementation host is not ready.

### DoD

- GET success;
- POST success;
- timeout;
- cancellation;
- connection failure;
- invalid URL;
- header normalization;
- body streaming later, not first.

---

## S7. Advanced concurrency

### Приоритет

**После basic task/sync/networking.**

Это уже не минимальный stdlib layer, а production-grade concurrency profile.

### Candidates

```amber
task.select:
 case ch1.recv():...
 case ch2.recv():...
```

```amber
task.with_timeout(5.0):
 operation()
```

```amber
supervisor = task.Supervisor.new(policy::one_for_one)
```

```amber
moved = move(value)
```

### Не включать в первый stdlib release

- `select`;
- `move`;
- supervisor policies;
- async file IO;
- stream backpressure protocol;
- structured service runtime;
- actor framework.

---

## Roadmap matrix

| Этап | Слой | Что входит | Почему сейчас |
|---|---|---|---|
| S1 | collections core | `Array/Tuple/Range/Set/LazySeq/Map`, `Enumerable`, errors | база для всего stdlib |
| S2 | sync/task runtime | `task`, `TaskHandle`, `Channel`, `Mutex`, `Atomic` | обязательная no-GIL поверхность |
| S3 | watch profile | `Kernel.watch`, watch cells, watchable object state, dependency capture | optional notebook/IDE слой без изменения production semantics |
| S4 | io foundation | `Reader/Writer/Closeable`, bytes/buffer, resource close, timeout/cancel hooks | мост к networking |
| S5 | networking low-level | TCP, DNS minimal, TLS later or feature-gated | требует async readiness |
| S6 | HTTP client | request/response, headers, body APIs | первый полезный net API |
| S7 | advanced concurrency | `select`, `move`, supervisor policies, async I/O polishing | за пределами минимального v1 runtime |

---

## Concrete backlog

### Collections

- `STD-001` Collection protocol conformance suite.
- `STD-002` Array/Tuple/Range/Set eager methods. Done: finite
 `Range`, exclusive-end `Range`, and open-ended `Range` edge cases are
 covered by the VM collection dispatch and stdlib collection tests.
- `STD-003` LazySeq pipeline and materialization. Done: lazy wrappers now
 defer `map` / `flat_map` / `select` / `reject`, terminal operations
 materialize or short-circuit the pipeline, and open-ended `Range.lazy`
 supports bounded `first(count)` while rejecting unbounded materialization.
- `STD-004` Map iteration and transform contract. Done: `Map` preserves
 insertion order for `keys` / `values` / `entries` / `to_a`, supports
 symbol/string key lookup, passes `k, v` to iteration/filter/map blocks,
 returns `Array` from `map`, returns `Map` from `select` / `reject`, supports
 key-changing `transform`, and lets `transform_values` blocks read `k` as an
 optional second argument while preserving keys.
- `STD-005` Suitable collections operations. Done: eager finite
 collections now support `union`, `intersection`, `difference`,
 `left_difference`, `symmetric_difference`, subset/superset/disjoint
 predicates, `contains?` / `include?`, `permutation(count)`, and
 `combination(count)`, plus operator aliases `&`, `|`, `-`, `^`,
 `<` / `<=` / `>` / `>=`, `+` concatenation, `*` repetition, `concat`,
 `take_while`, `reverse`, `sort`, and `uniq` with an optional block;
 `each(size, step:)`, `each_pair`, and `each_cons(size)` provide
 Ruby-friendly window iteration; `Set` preserves set result shape for
 set-like operations, finite `LazySeq` materializes these operations, and
 `Map#merge` / `Map#+` / `Map#|` preserve insertion order with right-wins or
 block-resolved conflicts.
- Ruby-friendly and common aliases are covered for collections: `collect`,
 `collect_concat`, `filter`, `find_all`, `detect`, `inject`, `member?`,
 `includes?`, `each_slice`, `entries`, and `count` / `length` / `size`; the
 size trio is also available as read-only collection properties.
- `STD-006` Collection error registry and edge cases. Done: the collection
 error surface is pinned to `EmptyCollectionError`, `IndexError`, `KeyError`,
 `ArgumentError`, and `TypeError`; ordinary sequence/range/lazy indexing now
 reports `IndexError` on out-of-bounds access, `Map#[]` reports `KeyError`
 for absent keys, `Map#contains?` / `Map#include?` provide non-raising key
 presence checks, and the stdlib collections suite covers these negative
 edges.

### Task (Async or Threading) / Synchronization primitives


- `STD-010` Task module public API. Done: `RuntimeTaskModule` exposes
 `async`, `spawn`, `sync`, `sleep`, and `yield_current` over the existing
 scheduler, returning `RuntimeTaskHandle` values with task/strand ids,
 `wait(timeout)`, non-blocking `result`, `failure`, `cancel`, `resume`, and
 state predicates;
 `sync` creates a non-yielding synchronous block for future `sync:` /
 `task.sync:` lowering inside async scopes; `stdlib_task_tests` covers success,
 timeout without auto-cancel, cooperative cancellation, sync-block yield
 suppression, and failure surfacing.
- `STD-011` TaskHandle state/result/failure API. Done:
 `RuntimeTaskHandleState`, `RuntimeTaskHandleSnapshot`, `state()`, and
 `snapshot()` expose inactive/running/terminal states; `wait`, `result`, and
 `failure` carry state and canonical `TaskNotDoneError`, `TaskFailedError`,
 `CancelledError`, `TimeoutError`, and `LifetimeError` surfaces; task tests
 cover inactive handles, unfinished non-blocking reads, success, failure, and
 cancellation.
- `STD-012` Channel API and FIFO corpus. Done: `RuntimeChannel` exposes
 bounded buffered and rendezvous construction, `send`, move-aware `send`,
 `recv`, `close`, `closed`, and `stats`; checked sends reject non-shareable
 payloads with `IsolationError`; close/empty recv and send-after-close report
 `ChannelClosedError`; `stdlib_task_tests` covers buffered FIFO, waiting
 sender FIFO, waiting receiver FIFO, close idempotency, closed-channel drain
 semantics, send/recv timeouts, receive cancellation, and isolation
 rejection.
- `STD-013` Mutex API plus `synchronize`. Done: `RuntimeMutex` exposes
 `lock`, `unlock`, `locked`, `owned`, `synchronize`, and `stats`; same-owner
 double lock reports `DeadlockError`, unlocked/non-owner unlock reports
 `OwnershipError`, waiting lockers acquire FIFO, `synchronize` returns the
 block value and unlocks on exception unwind, and cancellation-aware lock waits
 are covered by `stdlib_task_tests`.
- `STD-014` Atomic API plus `update`. Done: `RuntimeAtomic` keeps the existing
 integer `get` / `set` / `compare_and_set` facade and adds value-level
 `get_value`, `set_value`, `compare_and_set_value`, and `update`; atomic
 payload writes reject non-compatible confined values with
 `AtomicCompatibilityError`, heap CAS compares by identity, `update` returns
 the replacement value after a seq-cst CAS loop and can re-run the block on
 contention, and `stdlib_task_tests` covers guard failures, deterministic
 retry, and a cross-strand counter.
- `STD-015` Inter-thread communication: send/receive, barrier,
 scatter-gather/map-reduce. Done: `RuntimeBarrier` adds reusable generation
 barriers with timeout/cancellation-aware waits and stats;
 `RuntimeFlowModule` adds ordered `gather`, `scatter`, `scatter_map`,
 `scatter_reduce`, and `broadcast` over `RuntimeTaskHandle` workers with
 checked/unchecked isolation modes, default first-failure cancellation,
 collect/ignore failure policies, shareability validation for partitions and
 worker results, flow stats, and `stdlib_task_tests` coverage for ordered
 gather, reduce, broadcast, failure collection, and isolation edges.
- `STD-016` Auto-parallel collections iteration/combination/permutation
 methods. Done: `RuntimeThreadedCollection` provides the runtime-facing
 facade for `[1, 2, 3].threaded(3).map:...` style lowering, backed by
 `RuntimeFlowModule`; `.parallel(...)` is an alias for `.threaded(...)`.
 It supports ordered `each`, `map`, `select`, `reject`, `flat_map`,
 `combination(count)`, and `permutation(count)`, preserves checked isolation by
 default, supports explicit unchecked mode via `RuntimeFlowOptions`, surfaces
 worker failures through the existing flow failure policies, and records
 threaded collection and flow stats. Collection threading now accepts
 `scatter:` policy selection: `:atomic` / `:dynamic` is the default and runs up
 to worker-count tasks over a shared atomic item index; `:chunks` / `:fixed`
 runs fixed contiguous chunks with one task per worker; `:items` keeps the
 legacy per-item task mode for compatibility and diagnostics. Runtime logging
 keeps the native `thread=` id in task log context, alongside annotation and
 `task=` when present, so threaded/parallel workers remain attributable in
 stderr and buffered logger output. `stdlib_task_tests` covers ordered
 transforms, auto-parallel each, generated combinations/permutations,
 scatter-policy task counts, `.parallel` source-level lowering,
 checked/unchecked isolation, result shareability rejection, and failure
 collection.

### Watch

- `STD-020` Notebook watch target diagnostics.
- `STD-021` WatchCell storage replacement.
- `STD-022` WatchObjectState and ivar revision events. Done:
 `Kernel.watch(@ivar)` lowers/emits through `HWatchIvar` / `WATCH_IVAR`;
 runtime instances carry `RuntimeWatchObjectState` with stable object ids,
 object revisions, field revisions, and field subscribers; successful watched
 ivar writes publish `watch.ivar.write` events with old/new values plus
 field/object revision deltas without bumping `world_epoch`; failed writes do
 not publish events; `hir_tests`, `emitter_tests`, and `vm_tests` cover the
 path.
- `STD-023` Dependency capture API for notebook host.
 Done: `RuntimeWorld` exposes `begin_dependency_capture(cell_id)`,
 `end_dependency_capture()`, and `dependency_capture_snapshot()`; watched
 binding reads through locals/upvalues and watched ivar reads publish
 deterministic `RuntimeDependencySet` entries keyed by watch cell or
 object/field revision without bumping `watch_epoch` or `world_epoch`;
 `vm_tests` covers binding reads, repeated-read de-duplication, nested method
 ivar reads, and capture shutdown.

### IO

- `STD-030` IO resource protocol.
- `STD-031` Byte buffer / immutable bytes story.
- `STD-032` Scheduler readiness bridge.

### Networking

- `STD-040` TCP listener/stream.
- `STD-041` DNS resolution.
- `STD-042` HTTP client minimal.

---

## Hard recommendation

Не начинать с networking.

Правильный порядок:

```text
collections
 -> task/sync
 -> watch
 -> io
 -> networking
```

`object.watch` как user-facing method лучше не вводить в core stdlib сейчас. Он будет выглядеть как обычный method call, но по семантике требует compiler/kernel intrinsic, restricted target grammar и notebook-profile lowering.

Canonical form:

```amber
Kernel.watch(target)
```

Ergonomic alias допустим только в Notebook profile:

```amber
from notebook import watch

watch(x)
```

Но не как `Object#watch` в core.

## Text output, debug print и pretty print


---

## 0. Обзор решения

This section defines a minimal but explicit text-output layer for Amber stdlib.

It introduces three user-facing output helpers:

```amber
print value
p value
pp value
```

and their canonical call forms:

```amber
print(value)
p(value)
pp(value)
```

`print` is for ordinary user-facing string/display output.

`p` is for compact debug output, inspired by Ruby's `p`.

`pp` is for structured pretty debug output, primarily useful for collections, maps, nested objects and diagnostic state.

All three write to the current logical stdout by default, but accept an explicit output sink.

In the `iamber` interactive console / notebook profile, writes to the logical stdout/stderr streams are forcibly routed to the active cell output sink so that the complete execution log of a cell can be preserved and inspected independently.

---

## 1. Design principles

### 1.1. Output helpers are stdlib, not syntax

`print`, `p` and `pp` are ordinary stdlib functions exposed through `Kernel` and imported by the default prelude.

The command form:

```amber
p x
pp xs
print "hello"
```

uses Amber's existing command-call syntax.

This section does not introduce a new language-level print statement.

---

### 1.2. Logical streams, not raw global OS handles

`stdout` and `stderr` are logical streams.

In a CLI process, they normally map to the host process stdout/stderr.

In an embedded environment, test runner, iamber cell, IDE console or sandbox, they may be dynamically rebound.

Therefore this:

```amber
p value
```

means:

```amber
Kernel.p(value, to: io.current_stdout())
```

not:

```amber
Kernel.p(value, to: io.host_stdout())
```

---

### 1.3. Explicit sinks are supported

All output helpers accept an explicit sink:

```amber
print "message", to: io.stderr()
p value, to: io.stderr()
pp config, to: file
```

The sink must implement the text writer protocol or be adaptable to it.

---

### 1.4. iamber must capture logical output

In the `iamber` execution profile, all writes to `io.current_stdout()` and `io.current_stderr()` during cell evaluation are routed to the active cell output sink.

This preserves the cell execution log even if the rendered notebook output is cleared, collapsed or re-ordered by UI.

---

## 2. Module and namespace placement

### 2.1. Canonical placement

Canonical definitions live on `Kernel`:

```amber
Kernel.print(...)
Kernel.p(...)
Kernel.pp(...)
```

The prelude exposes them as bare functions:

```amber
print "hello"
p value
pp value
```

### 2.2. IO module support

The `io` module provides logical stream accessors:

```amber
io.current_stdout()
io.current_stderr()

io.stdout()
io.stderr()
```

Recommended equivalence:

```amber
io.stdout() == io.current_stdout()
io.stderr() == io.current_stderr()
```

The shorter names are ergonomic aliases. The `current_*` names emphasize dynamic rebinding.

Host/raw streams, if exposed at all, must be profile-gated:

```amber
io.host_stdout()
io.host_stderr()
```

These are not available in the safe notebook profile unless explicitly enabled by the host.

---

## 3. Text writer protocol

### 3.1. Minimal protocol

Any object accepted as `to:` by `print`, `p` or `pp` must support:

```amber
writer.write_str(str as Str)
```

Recommended optional operations:

```amber
writer.write_line(str as Str = "")
writer.flush()
writer.closed?()
writer.close()
```

### 3.2. Byte writer adaptation

A byte-oriented `io.Writer` may be adapted into a text writer through UTF-8 encoding:

```amber
text_writer = io.TextWriter.new(byte_writer, encoding::utf8)
```

If a function receives a byte writer as `to:`, the stdlib may either:

1. reject it with `TypeError`; or
2. adapt it using the default UTF-8 text encoding.

Recommended v1 rule:

> `print`, `p` and `pp` require a text writer. Byte writer adaptation must be explicit.

This avoids hidden encoding policy at output call sites.

### 3.3. Closed writer behavior

Writing to a closed writer raises:

```text
ClosedResourceError
```

If the underlying resource fails, the implementation raises:

```text
IOError
```

or a more specific subtype.

---

## 4. `print`

### 4.1. Purpose

`print` is ordinary user-facing display output.

It converts each argument to display text and writes each argument followed by a newline.

This intentionally differs from Ruby's `print`, which does not append a newline.

Amber's `print` follows Python-like line-oriented behavior.

---

### 4.2. Surface forms

Canonical call form:

```amber
print(value)
print(a, b, c)
print(value, to: io.stderr())
```

Command-call form:

```amber
print value
print a, b, c
print value, to: io.stderr()
```

---

### 4.3. Semantics

```amber
print(a, b, c, to: writer)
```

is observationally equivalent to:

```amber
writer.write_str(Amber.stringify(a, mode::display))
writer.write_str("\n")

writer.write_str(Amber.stringify(b, mode::display))
writer.write_str("\n")

writer.write_str(Amber.stringify(c, mode::display))
writer.write_str("\n")
```

Every argument is printed on its own line.

Examples:

```amber
print "hello"
```

Output:

```text
hello
```

```amber
print "a", "b", 3
```

Output:

```text
a
b
3
```

---

### 4.4. String handling

For `Str`, `print` writes the string contents without debug quotes.

```amber
print "hello"
```

Output:

```text
hello
```

This is different from `p`:

```amber
p "hello"
```

Output:

```text
"hello"
```

---

### 4.5. Return value

`print` returns `null`.

Rationale:

* `print` is an effect-oriented user-output operation.
* Returning the printed value would make `print` too close to `p`.
* Python-like behavior maps naturally to Amber's `null`.

Examples:

```amber
x = print "hello"
## x == null
```

---

### 4.6. Zero arguments

`print()` writes a single newline and returns `null`.

```amber
print()
```

Output:

```text

```

That is equivalent to:

```amber
io.current_stdout().write_str("\n")
```

---

## 5. `p`

### 5.1. Purpose

`p` is compact debug output.

It writes the inspect representation of each argument followed by a newline.

It is intended for fast debugging in scripts, REPL sessions, tests and notebook cells.

---

### 5.2. Surface forms

Canonical call form:

```amber
p(value)
p(a, b, c)
p(value, to: io.stderr())
```

Command-call form:

```amber
p value
p a, b, c
p value, to: io.stderr()
```

---

### 5.3. Semantics

```amber
p(a, b, c, to: writer)
```

is observationally equivalent to:

```amber
writer.write_str(Amber.stringify(a, mode::inspect))
writer.write_str("\n")

writer.write_str(Amber.stringify(b, mode::inspect))
writer.write_str("\n")

writer.write_str(Amber.stringify(c, mode::inspect))
writer.write_str("\n")
```

Examples:

```amber
p "hello"
```

Output:

```text
"hello"
```

```amber
p [1, 2, 3]
```

Output:

```text
[1, 2, 3]
```

---

### 5.4. Return value

For one positional value:

```amber
p(value)
```

returns `value`.

For multiple positional values:

```amber
p(a, b, c)
```

returns:

```amber
Tuple(a, b, c)
```

For zero positional values:

```amber
p()
```

writes nothing and returns `null`.

Rationale:

* single-value `p` is useful in expression pipelines;
* multi-value `p` preserves the argument set without inventing an `Array`;
* zero-value `p` has no natural inspected value.

Examples:

```amber
result = p compute()
## result is the result of compute()

a, b = p left(), right()
## returns Tuple(left_result, right_result), subject to ordinary destructuring rules
```

---

## 6. `pp`

### 6.1. Purpose

`pp` is structured pretty debug output.

It is primarily intended for:

* nested arrays, tuples, sets and maps;
* objects with meaningful field/property state;
* diagnostics;
* AST/HIR/runtime structures;
* notebook and REPL inspection.

`pp` should produce stable, deterministic and readable output.

---

### 6.2. Surface forms

Canonical call form:

```amber
pp(value)
pp(value, max_width: 80)
pp(value, max_depth: 20, max_items: 100)
pp(value, to: io.stderr())
```

Command-call form:

```amber
pp value
pp value, max_width: 100
pp value, to: io.stderr()
```

---

### 6.3. Semantics

```amber
pp(value, to: writer, max_width: 80, max_depth: 20, max_items: 100)
```

creates a pretty-printer configured with the given options, renders `value`, writes the rendered text, then writes a trailing newline.

Conceptually:

```amber
printer = PrettyPrinter.new(
 max_width: max_width,
 max_depth: max_depth,
 max_items: max_items
)

writer.write_str(printer.render(value))
writer.write_str("\n")
```

---

### 6.4. Return value

`pp` follows `p` return semantics.

For one positional value:

```amber
pp(value)
```

returns `value`.

For multiple positional values:

```amber
pp(a, b, c)
```

returns:

```amber
Tuple(a, b, c)
```

For zero positional values:

```amber
pp()
```

writes nothing and returns `null`.

---

### 6.5. Pretty output for collections

Example:

```amber
pp {
 name: "Ada",
 roles: [:admin,:editor],
 settings: {
 theme: "dark",
 retries: 3,
 },
}
```

Possible output:

```text
{
 name: "Ada",
 roles: [:admin,:editor,
 ],
 settings: {
 theme: "dark",
 retries: 3,
 },
}
```

The exact layout is implementation-defined within the constraints below.

Required constraints:

1. output must be deterministic;
2. output must not contain raw memory addresses;
3. map order must follow the standard `Map` iteration order;
4. set order must follow the standard `Set` iteration order if the implementation defines one;
5. cycles must not recurse forever;
6. infinite or open-ended lazy sequences must not be silently exhausted.

---

## 7. Stringification modes

### 7.1. Canonical entrypoint

The stdlib stringification operation is:

```amber
Amber.stringify(value, mode: mode)
```

where `mode` is one of:

```text:display:inspect:pretty
```

### 7.2. Display mode

Used by:

```amber
print
string interpolation
value.to_str()
```

Display mode is human-facing.

For `Str`, it returns the receiver unchanged.

Examples:

```amber
Amber.stringify("hello", mode::display) # "hello"
Amber.stringify(42, mode::display) # "42"
Amber.stringify(null, mode::display) # "null"
```

---

### 7.3. Inspect mode

Used by:

```amber
p
```

Inspect mode is debug-facing and should make type/structure visible.

Examples:

```amber
Amber.stringify("hello", mode::inspect) # "\"hello\""
Amber.stringify(:name, mode::inspect) # ":name"
Amber.stringify([1, 2], mode::inspect) # "[1, 2]"
```

Resolution order:

```text
1. If value is Str, produce a quoted escaped string literal representation.
2. Else if value responds to inspect(), call it and require Str.
3. Else if value is a known builtin collection/scalar, use the builtin inspect formatter.
4. Else produce deterministic fallback object representation.
```

Fallback object representations must not include raw memory addresses.

---

### 7.4. Pretty mode

Used by:

```amber
pp
```

Pretty mode may be implemented through `PrettyPrinter`.

Resolution order:

```text
1. If value responds to pretty(pp), call it with the active PrettyPrinter.
2. Else if value is a known collection/scalar, use the builtin pretty formatter.
3. Else use inspect mode.
```

`pretty(pp)` writes into the supplied pretty-printer and does not need to return `Str`.

---

## 8. PrettyPrinter protocol

### 8.1. Class

Recommended stdlib class:

```amber
class PrettyPrinter:
 attr var max_width
 attr var max_depth
 attr var max_items
 attr var sort_keys

 def text(str as Str)
 def line()
 def group:...
 def indent:...
 def render(value)
```

The exact internal layout algorithm is implementation-defined.

The public contract is deterministic output under equal input and equal options.

---

### 8.2. User-defined pretty printing

A user type may define:

```amber
def pretty(pp):...
```

Example:

```amber
class User:
 def inspect():
 "#<User name=#{@name.inspect()}>"

 def pretty(pp):
 pp.text("#<User")
 pp.indent:
 pp.line()
 pp.text("name: ")
 pp.render(@name)
 pp.line()
 pp.text("email: ")
 pp.render(@email)
 pp.line()
 pp.text(">")
```

`pretty(pp)` must not assume that it writes to stdout. It only writes into the provided printer.

---

### 8.3. Cycle handling

The pretty-printer must detect recursive structures.

Example:

```amber
xs = []
xs.push(xs)
pp xs
```

Valid output shape:

```text
[
 #<cycle Array>
]
```

The exact cycle marker is implementation-defined but must be deterministic.

---

### 8.4. Depth and item limits

`pp` accepts:

```amber
max_depth:
max_items:
```

Recommended defaults:

```amber
max_depth: 20
max_items: 100
```

If a value exceeds `max_depth`, the printer must emit a deterministic elision marker.

Example:

```text
#<max-depth Array>
```

If a collection exceeds `max_items`, the printer must emit a deterministic truncation marker.

Example:

```text
[
 1,
 2,
 3,... 97 more
]
```

---

### 8.5. Lazy and infinite collections

`pp` must not silently exhaust a lazy or infinite sequence.

Required behavior:

* finite eager collections may be fully printed subject to `max_items`;
* finite lazy collections may be materialized up to `max_items`;
* open-ended or infinite lazy collections must be previewed only up to `max_items`;
* unbounded materialization is invalid.

Example:

```amber
pp (1..).lazy(), max_items: 5
```

Possible output:

```text
#<LazySeq [
 1,
 2,
 3,
 4,
 5,...
]>
```

---

## 9. Sinks and dynamic output contexts

### 9.1. Current output context

The `io` module maintains a dynamic output context.

```amber
io.current_stdout()
io.current_stderr()
```

These are task-local or dynamic-scope-local, depending on the runtime implementation.

They must be safe under Amber's task/strand execution model.

---

### 9.2. Rebinding output

Recommended API:

```amber
io.with_output(stdout: writer, stderr: err_writer):...
```

Example:

```amber
buffer = io.Buffer.new()

io.with_output(stdout: buffer):
 print "hello"
 p [1, 2, 3]

buffer.to_str()
```

Expected buffer content:

```text
hello
[1, 2, 3]
```

---

### 9.3. Explicit sink precedence

An explicit `to:` argument overrides the current stdout default.

```amber
io.with_output(stdout: buffer):
 p value, to: io.stderr()
```

This writes to the current logical stderr, not to `buffer`.

If `io.stderr()` is also dynamically rebound, it writes to that rebound stream.

---

## 10. iamber output capture profile

### 10.1. Cell output sink

The `iamber` profile defines a cell-local sink:

```text
iamber.CellOutputSink
```

It implements the text writer protocol.

It records ordered cell output events.

Recommended event model:

```text
CellOutputEvent(
 stream,
 text,
 timestamp,
 order,
 source_span?
)
```

where:

```text
stream =:stdout |:stderr
```

Future rich-display protocols may add:

```text
stream =:display |:html |:markdown |:json
```

but this section only defines text output.

---

### 10.2. Cell evaluation rule

During cell evaluation, iamber installs:

```amber
io.with_output(
 stdout: cell_sink.stream(:stdout),
 stderr: cell_sink.stream(:stderr),
):
 eval_cell()
```

Therefore:

```amber
print "hello"
p value
pp data
p warning, to: io.stderr()
```

all enter the active cell log.

---

### 10.3. Forced capture of logical streams

Normative rule:

> In the iamber profile, writes to logical stdout and logical stderr during cell evaluation are captured by the active cell output sink.

This includes:

```amber
io.stdout().write_str("hello\n")
io.stderr().write_str("warning\n")
print "hello"
p value
pp data
```

All of the above are captured.

---

### 10.4. Explicit non-standard resources

Explicit user-created resources are not redirected to the cell log.

Example:

```amber
file = io.File.open("debug.log", mode::write)
p value, to: file
file.close()
```

This writes to `debug.log`, not to the cell output sink.

---

### 10.5. Raw host streams

If the host exposes raw process streams:

```amber
io.host_stdout()
io.host_stderr()
```

then use of those streams inside iamber is host-policy controlled.

In the safe notebook profile, these APIs should be unavailable or require explicit capability access.

---

### 10.6. Cell log preservation

The iamber runtime must preserve the ordered text log independently from the UI rendering state.

This allows:

* inspecting previous cell output;
* replaying output;
* exporting execution logs;
* debugging hidden/collapsed cells;
* separating stdout and stderr streams.

---

## 11. Privacy, taint and policy

`print`, `p` and `pp` are output/export operations.

If the Privacy/Taint/Lineage profile is enabled, stringification and output must apply the same policy checks as other text-export boundaries.

Examples:

```amber
email as Str @pii

print email
p email
pp {email: email}
```

Depending on the active policy, these may raise:

```text
PolicyViolationError
```

`pp` must not bypass policy checks by recursively inspecting object internals.

If a field is not permitted to be exported, the printer must either:

1. raise `PolicyViolationError`; or
2. emit a policy-approved redaction marker.

The choice is profile-defined.

---

## 12. Error behavior

### 12.1. Invalid sink

If `to:` does not implement the required text writer protocol:

```amber
p value, to: 123
```

raises:

```text
TypeError
```

### 12.2. Closed sink

If the sink is closed:

```amber
file.close()
print "hello", to: file
```

raises:

```text
ClosedResourceError
```

### 12.3. Writer failure

If the sink fails during output, the underlying error propagates as:

```text
IOError
```

or a more specific subtype.

### 12.4. Invalid stringification result

If `to_str()`, `inspect()` or a related hook returns a non-`Str` value, raise:

```text
TypeError
```

### 12.5. Pretty hook failure

If `pretty(pp)` raises, the exception propagates.

Partial writes before the failure are allowed unless the sink provides transactional semantics.

---

## 13. Evaluation order

For:

```amber
p a(), b(), to: sink()
```

evaluation order is:

1. evaluate `a()`;
2. evaluate `b()`;
3. evaluate `sink()`;
4. stringify and write the first value;
5. stringify and write the second value;
6. return `Tuple(a_result, b_result)`.

For:

```amber
print a(), b(), to: sink()
```

evaluation order is the same, but the result is `null`.

For:

```amber
pp value(), max_items: n(), to: sink()
```

evaluation order is:

1. evaluate `value()`;
2. evaluate `n()`;
3. evaluate `sink()`;
4. configure the pretty-printer;
5. render and write;
6. return `value_result`.

---

## 14. Command form

Because Amber supports command-call syntax, these are valid:

```amber
print "hello"
p value
pp config
```

They are equivalent to:

```amber
print("hello")
p(value)
pp(config)
```

With multiple arguments:

```amber
print a, b, c
p a, b, c
pp a, b, c
```

equivalent to:

```amber
print(a, b, c)
p(a, b, c)
pp(a, b, c)
```

With keyword arguments:

```amber
p value, to: io.stderr()
pp value, max_width: 100
print value, to: file
```

equivalent to:

```amber
p(value, to: io.stderr())
pp(value, max_width: 100)
print(value, to: file)
```

---

## 15. Examples

### 15.1. Basic print

```amber
name = "Ada"

print "Hello, #{name}"
```

Output:

```text
Hello, Ada
```

---

### 15.2. Multiple print arguments

```amber
print "alpha", "beta", 42
```

Output:

```text
alpha
beta
42
```

---

### 15.3. Debug print

```amber
p "alpha", [:beta, 42]
```

Output:

```text
"alpha"
[:beta, 42]
```

Return value:

```amber
Tuple("alpha", [:beta, 42])
```

---

### 15.4. Pretty print

```amber
pp {
 user: {
 name: "Ada",
 roles: [:admin,:editor],
 },
 ok: true,
}
```

Output:

```text
{
 user: {
 name: "Ada",
 roles: [:admin,:editor,
 ],
 },
 ok: true,
}
```

---

### 15.5. stderr

```amber
p error, to: io.stderr()
```

Writes inspect output to the current logical stderr.

In iamber, this appears in the cell log as a stderr event.

---

### 15.6. Capturing output

```amber
buffer = io.Buffer.new()

io.with_output(stdout: buffer):
 print "hello"
 p [1, 2, 3]

buffer.to_str()
```

Result:

```text
hello
[1, 2, 3]
```

---

## 16. Conformance tests

### 16.1. `print`

Required tests:

```amber
print "hello"
## stdout == "hello\n"
## result == null
```

```amber
print "a", "b"
## stdout == "a\nb\n"
## result == null
```

```amber
print()
## stdout == "\n"
## result == null
```

```amber
print "hello", to: io.stderr()
## stderr == "hello\n"
```

### 16.2. `p`

Required tests:

```amber
x = p "hello"
## stdout == "\"hello\"\n"
## x == "hello"
```

```amber
x = p 1, 2
## stdout == "1\n2\n"
## x == Tuple(1, 2)
```

```amber
x = p()
## stdout == ""
## x == null
```

### 16.3. `pp`

Required tests:

```amber
x = pp [1, 2, 3]
## stdout contains structured representation
## x == [1, 2, 3]
```

```amber
xs = []
xs.push(xs)
pp xs
## must terminate
## output contains deterministic cycle marker
```

```amber
pp (1..).lazy(), max_items: 5
## must not exhaust infinite sequence
## output contains deterministic truncation marker
```

### 16.4. Explicit sink

Required tests:

```amber
buffer = io.Buffer.new()
p "x", to: buffer
## buffer.to_str() == "\"x\"\n"
```

### 16.5. Dynamic output

Required tests:

```amber
buffer = io.Buffer.new()

io.with_output(stdout: buffer):
 print "x"
 p "y"

## buffer.to_str() == "x\n\"y\"\n"
```

### 16.6. iamber capture

Required tests:

```amber
## inside iamber cell
print "x"
p "y"
pp [1, 2]
```

Expected:

```text
cell log contains three stdout events in execution order
```

```amber
## inside iamber cell
p "warning", to: io.stderr()
```

Expected:

```text
cell log contains one stderr event
```

### 16.7. Closed sink

Required tests:

```amber
buffer = io.Buffer.new()
buffer.close()

print "x", to: buffer
## raises ClosedResourceError
```

### 16.8. Invalid sink

Required tests:

```amber
p "x", to: 123
## raises TypeError
```

---

## 17. Open questions

### 17.1. Should `print` support separators?

This section deliberately does not add Python-style `sep:` or `end:`.

Current rule:

```amber
print a, b
```

means:

```text
a
b
```

If needed, a future extension may add:

```amber
print a, b, sep: " ", end: "\n"
```

but the v1 surface keeps each argument line-oriented.

### 17.2. Should `pp` support colors?

Not in this section.

Terminal color and notebook styling should be a separate display/styling layer.

### 17.3. Should `p` use `inspect()` or `Amber.stringify(..., mode::inspect)`?

Normative answer:

```amber
p value
```

uses:

```amber
Amber.stringify(value, mode::inspect)
```

`inspect()` remains the user-definable hook used by that stringification mode.

### 17.4. Should `print` call `to_str()` directly?

Normative answer:

```amber
print value
```

uses:

```amber
Amber.stringify(value, mode::display)
```

This keeps interpolation, display conversion and print output aligned.

---

## 18. Summary

This section standardizes:

```amber
print value # display string, newline after each argument, returns null
p value # inspect string, newline after each argument, returns value
pp value # pretty inspect string, newline, returns value
```

Output goes to logical stdout by default:

```amber
io.current_stdout()
```

but can be routed explicitly:

```amber
p value, to: io.stderr()
pp value, to: file
```

The `iamber` profile captures logical stdout/stderr into a cell-local output sink during cell evaluation, preserving a structured execution log.

The design keeps output as a stdlib/runtime concern, composes with IO resource contracts, respects privacy/export policy, and avoids introducing new core syntax.

## Threading / async / flow concurrency API


**Проектный слой для разработки `task` / `sync` / flow concurrency API Amber** 

---

### 0. Статус документа

Этот документ является проектным слоем для разработки threading/async API Amber поверх уже зафиксированной спецификации Amber и проектного слоя компилируемого Amber.

Документ не меняет базовую семантику языка, не вводит новый общий синтаксис выражений и не переоткрывает закрытые решения core language. Его задача — превратить уже зафиксированную runtime-модель `Worker -> Strand -> Task`, no-GIL scheduler, structured concurrency, `Channel`, `Mutex`, `Atomic`, cancellation и shareability/isolation rules в подробный инженерный контракт для реализации.

Дополнительно этот документ вводит два расширения к базовому task/sync слою:

1. **MPI-like scatter/gather flow API** для data-parallel threading workflows.
2. **Explicit unchecked isolation mode** для настоящего no-GIL threading без автоматического `IsolationError`, когда пользователь явно принимает ответственность за synchronization и data races.

Каноническая граница:

- Amber language spec определяет язык, syntax, object model, pattern matching, callable refs, modules, profiles.
- Amber compilable project layer определяет VM, bytecode, loader, verifier, scheduler/runtime ABI и implementation matrix.
- Этот документ определяет stdlib/runtime-facing API для threading/async, включая user API, runtime hooks, verifier requirements, errors, conformance corpus и backlog.

---

### 1. Цели слоя

Threading/async layer должен дать Amber:

- cooperative async tasks внутри одного strand;
- настоящую параллельность через `task.spawn` и несколько worker threads;
- safe default на основе strand isolation;
- explicit unsafe escape hatch для performance-critical/system code;
- structured concurrency как default;
- cancellation и timeout semantics;
- bounded/unbounded synchronization primitives;
- MPI-like scatter/gather flows;
- deterministic diagnostics, traces и conformance tests;
- основу для последующих слоёв: Watch, IO, networking, HTTP, advanced concurrency.

Минимальный успешный результат:

```text
Amber source
 -> HIR task/sync/flow nodes
 -> bytecode concurrency opcodes / intrinsic sends
 -> verifier checks
 -> no-GIL VM scheduler
 -> deterministic runtime behavior under conformance corpus
```

---

### 2. Non-goals для v1

В первый релиз не входят:

- `task.select`;
- `move(value)` ownership transfer;
- supervisor trees / policies;
- actor framework;
- async file IO;
- TCP/HTTP implementation;
- stream backpressure protocol;
- distributed multi-process MPI;
- deterministic replay scheduler как обязательный production mode;
- automatic data-race detection в release runtime.

Эти возможности могут быть реализованы позже поверх стабильного `task/sync/flow` слоя.

---

### 3. Базовая execution model

Amber использует три уровня исполнения:

```text
Worker = системный поток ОС
Strand = последовательная область исполнения с собственной runnable-очередью
Task = кооперативная fiber/coroutine внутри strand
```

Основной инвариант:

```text
В одном strand одновременно исполняется не более одной task.
Разные strand могут исполняться параллельно на разных worker threads.
```

Следствия:

- VM не имеет global interpreter lock.
- Внутри одного strand ordinary mutable state безопасна без lock, потому что нет параллельного исполнения двух task одного strand.
- Параллельность возникает между разными strand.
- Cross-strand доступ в checked mode разрешён только для shareable values и sync objects.
- В unchecked mode isolation gate отключается явно, но VM memory safety остаётся обязательной.

---

### 4. Namespaces

Канонический импорт:

```amber
import task
from sync import Channel, Mutex, Atomic
```

Flow API:

```amber
from task.flow import scatter, gather, scatter_map, scatter_reduce, broadcast
```

Runtime-visible types:

```amber
task.TaskHandle
task.CancelToken
task.Timeout
task.Scheduler

task.flow.Flow
task.flow.Partition
task.flow.ScatterPlan
task.flow.GatherResult
task.flow.WorkerGroup

sync.Channel
sync.Mutex
sync.Atomic
```

`TaskHandle`, `Channel`, `Mutex`, `Atomic` являются sync/shareable runtime objects и могут пересекать strand boundary.

---

### 5. Normative decisions summary

1. `task.async` создаёт child task в том же strand.
2. `task.spawn` создаёт child task в новом strand.
3. Ordinary mutable objects являются strand-confined by default.
4. Cross-strand boundaries в checked mode принимают только shareable values и sync objects.
5. Нарушение isolation в checked mode даёт compile-time diagnostic или runtime `IsolationError`.
6. `isolation::unchecked` явно отключает isolation checks для конкретного spawn/flow/channel boundary.
7. Unchecked mode не отключает lifetime checks, GC checks, verifier checks, write barriers, root maps или object validity checks.
8. Unchecked mode не создаёт happens-before edge; synchronization остаётся обязанностью пользователя.
9. `Channel` payloads требуют shareability by default.
10. `Mutex` non-reentrant.
11. `Atomic` seq-cst only в v1.
12. `wait(timeout:)` не отменяет child автоматически.
13. `cancel()` cooperative и idempotent.
14. Structured concurrency является default.
15. Public orphan tasks не входят в v1.
16. Scatter/gather flow API предоставляет MPI-like high-level threading flows.
17. Scatter/gather сохраняет input order by default.
18. Flow failure policy defaults to `fail::first`.
19. Artifact с unchecked concurrency должен иметь `unsafe_concurrency` feature flag.
20. Host policy может запретить unsafe concurrency при compile/load/runtime.

---

### 6. Root async scope

#### 6.1. Basic form

```amber
async |task|:
 # root task body...
```

Семантика:

- создаёт root strand scope;
- создаёт root task внутри этого strand;
- передаёт в блок task-context object;
- по выходу из блока выполняет structured join всех дочерних task;
- если child task упала, root scope получает failure propagation;
- если root scope cancelled, все structured children получают cancellation request.

Результат async scope:

```amber
result = async |task|:
 1 + 2
## result == 3
```

Если внутри scope есть незавершённые children, scope не завершается до auto-join или cancellation-unwind.

---

### 7. `task.async` — same-strand cooperative task

```amber
handle = task.async |child|:
 compute()
```

Семантика:

- создаёт child task в том же strand;
- child task разделяет sequential execution domain с parent и siblings;
- ordinary mutable objects можно захватывать по ссылке;
- параллельного доступа к таким объектам нет, потому что strand исполняет только одну task за раз;
- scheduling cooperative: переключение происходит на safepoints.

Пример:

```amber
async |task|:
 rows = []

 producer = task.async |child|:
 rows << read_row()

 consumer = task.async |child|:
 if rows.count() > 0:
 commit rows
```

`rows` остаётся ordinary mutable Array, потому что обе tasks находятся в одном strand.

---

#### 7.1. Synchronous block inside async scopes

```amber
async |task|:
 task.async |child|:...

 sync:
 critical_without_task_switching()
```

Equivalent explicit spelling:

```amber
async |task|:
 task.sync:
 critical_without_task_switching()
```

Semantics:

- `sync:` / `task.sync:` does not create a new task;
- the block runs inline in the current task and strand;
- cooperative task switching is suppressed while the block is active;
- nested sync blocks keep switching suppressed until the outermost block exits;
- cancellation and lifetime checks remain valid at explicit runtime checks;
- blocking OS calls inside the block remain blocking OS calls and do not become
 reactor/cooperative waits.

The construct is intended for small critical sections that must not be
interleaved with other same-strand async tasks. It is not a replacement for
`Mutex` across strands.

---

### 8. `task.spawn` — new-strand parallel task

#### 8.1. Checked default

```amber
handle = task.spawn |child|:
 compute_shareable()
```

Семантика:

- создаёт child task в новом strand;
- новый strand может быть выполнен любым worker;
- task реально может исполняться параллельно с parent;
- closure captures должны быть shareable;
- direct capture ordinary mutable objects запрещён;
- нарушение даёт compile-time diagnostic, если видно статически, или `IsolationError` runtime.

Valid:

```amber
path = "/tmp/input.txt".freeze()

async |task|:
 worker = task.spawn |child|:
 parse_file(path)

 result = worker.wait()
```

Invalid in checked mode:

```amber
async |task|:
 rows = []

 worker = task.spawn |child|:
 rows << 1
## IsolationError или compile-time diagnostic
```

---

### 9. Explicit unchecked isolation mode

#### 9.1. Motivation

Amber VM рассчитана на настоящий no-GIL threading. Safe default через strand isolation нужен для обычного пользовательского кода, но low-level, system, HPC и performance-critical code должен иметь явный способ отключить isolation gate.

Unchecked mode означает:

```text
Пользователь явно разрешает cross-strand access к non-shareable объектам
и принимает ответственность за synchronization, data races и nondeterministic interleavings.
```

---

#### 9.2. Syntax

Default:

```amber
task.spawn:
 work()
```

Эквивалентно:

```amber
task.spawn(isolation::checked):
 work()
```

Unchecked:

```amber
task.spawn(isolation::unchecked):
 work()
```

Допустимый ergonomic alias:

```amber
task.safe_spawn:
 work()
task.unsafe_spawn:
 work()
```

Каноническая форма документации:

```amber
task.spawn(isolation::unchecked):
 work()
```

---

#### 9.3. Allowed isolation modes

```text:checked — default safe mode:unchecked — disable shareability/isolation gate for this boundary
```

Future modes, not v1:

```text:borrowed:moved:shared_lock
```

---

#### 9.4. Semantics of `isolation::unchecked`

При `isolation::unchecked`:

- `task.spawn` не требует shareable captures;
- closure может захватывать strand-confined objects;
- runtime не бросает `IsolationError` только за факт cross-strand capture/access;
- ordinary object operations выполняются как обычные VM operations;
- data races становятся возможными;
- memory visibility гарантируется только explicit synchronization edges;
- GC/lifetime safety остаётся обязательной;
- destroyed/deallocated/pin/object-header violations остаются runtime errors.

Ключевой инвариант:

```text
Unchecked isolation disables ownership/isolation checks.
It does not disable lifetime checks, verifier checks, GC root maps, write barriers or object validity checks.
```

---

#### 9.5. Happens-before in unchecked mode

Unchecked mode не создаёт happens-before edge.

Между strand'ами видимость появляется только через:

```text
Channel.send / Channel.recv
Channel.close
Mutex.unlock / Mutex.lock
Task completion / TaskHandle.wait
Atomic operations
flow/gather join
```

Если два strand одновременно пишут в ordinary object без synchronization:

```text
Amber-level data race with unspecified interleaving
```

Но VM не имеет права допустить:

```text
memory corruption
dangling pointer
invalid object header
collector unsoundness
write barrier omission
root-map loss
```

Иными словами:

```text
Amber-level state may be racy.
VM-level memory safety must remain intact.
```

---

#### 9.6. Example

Checked violation:

```amber
rows = []

task.spawn:
 rows << 1
## compile-time diagnostic or IsolationError
```

Explicit unchecked:

```amber
rows = []

task.spawn(isolation::unchecked):
 rows << 1
## allowed; module marked unsafe_concurrency
```

Recommended synchronized unchecked sharing:

```amber
m = Mutex.new()
shared = []

task.spawn(isolation::unchecked):
 m.synchronize:
 shared << 1
```

---

#### 9.7. Artifact markers

Любой `.amberbc`, содержащий unchecked spawn/flow/channel, должен иметь module flag:

```text
feature_flags: ["unsafe_concurrency"]
```

Code/debug metadata:

```text
unsafe_regions:
 - kind: "unchecked_spawn"
 span: source_span
 captures: [...]
```

Verifier обязан:

- разрешить unchecked regions только при наличии feature flag;
- сохранить stack/root maps;
- проверить handler tables;
- проверить safepoint metadata;
- не доказывать shareability captures внутри unchecked region;
- не отключать lifetime/GC/write-barrier validation.

Loader может запретить такой module host policy'ей:

```text
UnsafeConcurrencyDeniedError
```

---

#### 9.8. Runtime implementation strategy

Не рекомендуется рекурсивно retag'ать object graph в `unchecked_shared` для v1.

Reference VM approach:

```text
Frame.flags += UNSAFE_CONCURRENCY_REGION
```

Ownership checks:

```text
if current_frame.unsafe_concurrency:
 skip IsolationError check
else:
 enforce owner_token
```

Lifetime checks always run.

Rationale:

- recursive graph retagging expensive;
- object graph may be cyclic;
- hidden ownership mutation complicates diagnostics;
- frame-level unsafe flag is traceable and reversible.

---

#### 9.9. CLI / package policy

Compiler/runtime flags:

```text
amberc --disallow-unsafe-concurrency
ambervm --disallow-unsafe-concurrency
ambertest --disallow-unsafe-concurrency
```

Package manifest:

```toml
[features]
unsafe_concurrency = false
```

Default:

```text
unsafe_concurrency = true
```

If artifact contains `unsafe_concurrency`, but policy denies it:

```text
UnsafeConcurrencyDeniedError
```

---

### 10. TaskHandle API

#### 10.1. Construction

`TaskHandle` не создаётся напрямую пользовательским кодом.

Invalid:

```amber
h = TaskHandle.new()
```

Valid factory paths:

```amber
h1 = task.async: work()
h2 = task.spawn: work()
```

---

#### 10.2. Public methods

```amber
handle.wait()
handle.wait(timeout: seconds)

handle.cancel()
handle.cancelled?()
handle.done?()
handle.running?()
handle.failed?()

handle.result()
handle.failure()

handle.resume()
handle.strand_id()
handle.task_id()
```

---

#### 10.3. `wait()`

```amber
value = handle.wait()
```

Semantics:

- if task completed successfully, return result;
- if task failed, re-raise failure in waiter;
- if task cancelled, throw `CancelledError`;
- if current task is cancelled while waiting, throw `CancelledError`;
- `wait()` is a cancellation point;
- `wait()` is a scheduler safepoint.

---

#### 10.4. `wait(timeout:)`

```amber
value = handle.wait(timeout: 1.0)
```

Semantics:

- timeout is seconds;
- deadline uses scheduler clock;
- if deadline expires first, throw `TimeoutError`;
- timeout does not cancel child automatically;
- user may call `handle.cancel()` explicitly.

Example:

```amber
handle = task.spawn:
 long_job()

try:
 value = handle.wait(timeout: 2.0)
catch TimeoutError:
 handle.cancel()
```

---

#### 10.5. `cancel()`

```amber
handle.cancel()
```

Semantics:

- sets cancellation flag;
- wakes sleeping/waiting task if possible;
- does not kill task preemptively;
- task observes cancellation at safepoints;
- repeated `cancel()` is idempotent;
- returns `true` if request was new, otherwise `false`.

---

#### 10.6. `result()`

```amber
value = handle.result()
```

Semantics:

- if task done successfully, returns result;
- if task not done, throws `TaskNotDoneError`;
- if task failed, throws `TaskFailedError` or re-raises original failure depending on profile;
- if task cancelled, throws `CancelledError`.

Normative distinction:

```text
result() is non-blocking.
wait() is blocking/cooperative waiting.
```

---

#### 10.7. `failure()`

```amber
err = handle.failure()
```

Semantics:

- if task failed, returns error object;
- if task done successfully, returns `null`;
- if task cancelled, returns `CancelledError` object or `null` depending on internal representation;
- if task not done, throws `TaskNotDoneError`.

---

#### 10.8. `resume()`

```amber
handle.resume()
```

Semantics:

- makes sleeping/waiting task runnable;
- idempotent if task already runnable/running/done;
- does not override cancellation;
- does not migrate task to another strand.

---

### 11. Task context API

Task block receives a context object:

```amber
async |task|:
 task.sleep(0.1)
 task.yield()
```

Methods:

```amber
task.async |child|:...
task.spawn |child|:...

task.sleep(seconds)
task.yield()

task.cancelled?()
task.check_cancelled!()

task.current()
task.current_handle()
task.strand_id()
task.task_id()
```

#### 11.1. `sleep(seconds)`

```amber
task.sleep(0.1)
```

Semantics:

- puts current task into sleeping state;
- registers timer in scheduler;
- cancellation point;
- `seconds <= 0` behaves like `yield()`;
- `NaN`, invalid negative values and non-numbers produce `ArgumentError` / `TypeError`.

#### 11.2. `yield()`

```amber
task.yield()
```

Semantics:

- voluntarily yields to scheduler;
- current task returns to runnable queue;
- cancellation point;
- does not guarantee another task runs first if runnable queue is empty.

#### 11.3. `check_cancelled!()`

```amber
task.check_cancelled!()
```

Semantics:

- if cancellation flag is set, throws `CancelledError`;
- otherwise returns `null`;
- intended for CPU-bound loops.

---

### 12. Structured concurrency

#### 12.1. Parent-child ownership

Each task has:

```text
parent?
children_set
state
result_or_failure
cancel_flag
```

Rules:

- `task.async` and `task.spawn` create structured child tasks;
- child belongs to lexical/root async scope;
- parent scope does not complete until children done/failed/cancelled;
- public orphan tasks are not v1.

#### 12.2. Failure propagation

If child failed:

1. failure is stored in `TaskHandle`;
2. sibling tasks receive cancellation request;
3. parent observes failure during structured join;
4. stack trace includes task boundary;
5. diagnostics are deterministic.

#### 12.3. Cancellation propagation

If parent cancelled:

```text
parent.cancel()
 -> mark parent cancel_flag
 -> request cancellation for all structured children
 -> wake sleeping/waiting children
```

Child may finish as:

- `cancelled`, if it observes cancellation;
- `done`, if it completes before safepoint;
- `failed`, if it throws another error.

---

### 13. Ownership and shareability

Runtime owner modes:

```text
shareable
confined(strand_id)
sync
```

Optional internal mode for debug/accounting:

```text
unchecked_shared
```

#### 13.1. Shareable values

Shareable:

```text
null
true
false
numbers
symbols
frozen strings
frozen tuples/lists/maps
constant language metaobjects
closures with only shareable captures
TaskHandle
Channel
Mutex
Atomic
```

#### 13.2. Strand-confined by default

```text
Array
Map
mutable String
user objects with mutable state
capture cells
closures capturing non-shareable references
```

#### 13.3. Boundary checks

Runtime checks required at:

```text
task.spawn capture verification
Channel.send payload boundary
SEND/CALL receiver ownership check
LOAD_IVAR / STORE_IVAR
indexing fast paths
collection builtin fast paths
```

Violation in checked mode:

```text
IsolationError
```

No implicit behavior in checked mode:

- no hidden deep clone;
- no transparent thread-safe wrapper;
- no automatic ownership transfer;
- no copy-on-send;
- no move semantics.

---

### 14. Channel API

#### 14.1. Construction

```amber
ch = Channel.new()
ch = Channel.new(capacity: 0)
ch = Channel.new(capacity: 16)
```

Defaults:

```text
capacity: 0
isolation::checked
```

Meaning:

- `capacity: 0` — rendezvous/unbuffered channel;
- `capacity > 0` — bounded buffered channel;
- `capacity < 0` — `ArgumentError`;
- non-integer capacity — `TypeError`.

Unchecked channel:

```amber
ch = Channel.new(capacity: 16, isolation::unchecked)
```

---

#### 14.2. Methods

Required v1:

```amber
ch.send(value)
value = ch.recv()

ch.close()
ch.closed?()
```

Recommended staged methods:

```amber
ch.capacity()
ch.count()

ch.try_send(value)
ch.try_recv()

ch.send(value, timeout: seconds)
ch.recv(timeout: seconds)
```

---

#### 14.3. `send(value)`

Rules:

- if channel open and receiver waiting, deliver directly;
- if channel open and buffer has space, enqueue;
- if channel open and buffer full, current task waits;
- if channel closed, throw `ChannelClosedError`;
- in checked mode, payload must be shareable;
- operation is FIFO per channel;
- operation is cancellation point.

Reference v1 rule:

```text
Channel payload must be shareable unless channel isolation is explicitly:unchecked.
```

---

#### 14.4. Unchecked channel

```amber
ch = Channel.new(capacity: 16, isolation::unchecked)
```

Rules:

- channel is marked unsafe;
- values transferred without shareability gate;
- FIFO/wake semantics unchanged;
- send/recv still provide synchronization edge;
- sender and receiver may still race if both keep aliases and mutate shared object concurrently.

---

#### 14.5. `recv()`

Rules:

- if buffer non-empty, returns next FIFO value;
- if sender waiting on unbuffered channel, receives directly and wakes sender;
- if channel open and no value, current task waits;
- if channel closed and buffer non-empty, drains buffered values;
- if channel closed and empty, throws `ChannelClosedError`;
- operation is cancellation point.

---

#### 14.6. `close()`

Rules:

- idempotent;
- marks channel closed;
- wakes waiting receivers;
- wakes waiting senders with `ChannelClosedError`;
- does not discard buffered values;
- successful `recv()` after close can still return buffered values;
- `send()` after close always throws `ChannelClosedError`.

---

#### 14.7. Fairness

Normative v1 fairness:

```text
Per-channel waiting sender queue is FIFO.
Per-channel waiting receiver queue is FIFO.
Element order is FIFO.
Global scheduler fairness is not guaranteed.
```

---

### 15. Mutex API

#### 15.1. Construction

```amber
m = Mutex.new()
```

#### 15.2. Methods

```amber
m.lock()
m.unlock()
m.locked?()
m.owned?()
m.synchronize:
 critical_section()
```

Optional staged methods:

```amber
m.try_lock()
m.lock(timeout: seconds)
```

#### 15.3. `lock()`

Rules:

- if unlocked, current task becomes owner;
- if locked by another task, current task waits FIFO;
- if locked by same task, throw `DeadlockError`;
- operation is cancellation point while waiting.

Owner identity:

```text
(task_id, strand_id)
```

#### 15.4. `unlock()`

Rules:

- if unlocked, throw `OwnershipError`;
- if caller is not owner, throw `OwnershipError`;
- otherwise release lock;
- wake next waiter FIFO if any;
- return `null`.

#### 15.5. `synchronize`

```amber
m.synchronize:
 critical_section()
```

Lowering-equivalent semantics:

```amber
m.lock()
try:
 critical_section()
finally:
 m.unlock()
```

Rules:

- guarantees unlock during normal return, exception and cancellation unwind;
- returns block result;
- propagates block exception;
- non-reentrant rule still applies.

---

### 16. Atomic API

#### 16.1. Construction

```amber
a = Atomic.new(0)
```

Atomic-compatible v1 values:

```text
null
Bool
Integer
Symbol
shareable object reference
```

Optional host-supported values:

```text
Float, if host guarantees atomic representation or boxes safely
frozen String
```

#### 16.2. Methods

```amber
a.get()
a.set(value)
a.compare_and_set(expected, replacement)
a.update |x|:
 x + 1
```

Optional staged methods:

```amber
a.swap(value)
a.fetch_update |x|:...
```

#### 16.3. Memory ordering

Reference profile:

```text
Atomic.get seq-cst load
Atomic.set seq-cst store
Atomic.compare_and_set seq-cst CAS
Atomic.update CAS loop using seq-cst CAS
```

No weaker memory orders in v1 public API.

#### 16.4. `compare_and_set`

```amber
ok = a.compare_and_set(1, 2)
```

Returns:

```text
true if value changed
false if current value != expected
```

Comparison semantics:

- primitive values compare by value;
- heap references compare by identity;
- user code must not execute inside CAS comparison.

#### 16.5. `update`

```amber
a.update |x|:
 x + 1
```

Semantics:

```text
loop:
 old = a.get()
 new = block(old)
 if a.compare_and_set(old, new):
 return new
```

Rules:

- block may be re-executed;
- block must be side-effect-safe or user accepts repeated side effects;
- if block raises, update aborts and propagates exception;
- replacement must be atomic-compatible.

Mandatory documentation warning:

```text
Atomic.update block can run more than once.
```

---

### 17. MPI-like scatter/gather flow API

#### 17.1. Purpose

Flow API provides data-parallel high-level operations:

```text
scatter -> parallel workers -> gather
scatter -> parallel workers -> reduce
broadcast -> parallel workers -> gather
```

This is not distributed MPI. It is MPI-like local no-GIL threading over Amber strands/workers.

---

#### 17.2. Namespace

```amber
from task.flow import scatter, gather, scatter_map, scatter_reduce, broadcast
```

---

#### 17.3. Core runtime types

```text
Flow(
 flow_id,
 parent_task_id,
 worker_handles[],
 input_partitions[],
 result_policy,
 failure_policy,
 cancellation_policy,
 isolation_mode
)

Partition(
 index,
 value,
 size_hint?,
 metadata?
)

GatherResult(
 values[],
 failures[],
 cancelled?,
 completed_count,
 failed_count
)
```

---

#### 17.4. `scatter`

```amber
flow = task.flow.scatter(items, workers: 4) |part, ctx|:
 process(part)
```

Semantics:

- `items` split into partitions;
- each partition handled by child task via `task.spawn` semantics;
- each worker task gets `part` and flow-local context `ctx`;
- result stored by partition index;
- parent performs structured gather.

Example:

```amber
from task.flow import scatter

parsed = scatter(files, workers: 8) |file, ctx|:
 parse_file(file)
```

Result:

```text
Array[result] in original partition order
```

---

#### 17.5. `scatter_map`

```amber
parsed = task.flow.scatter_map(files, workers: 8) |file|:
 parse_file(file)
```

Equivalent user-level sketch:

```amber
handles = files.map |file|:
 task.spawn:
 parse_file(file)

parsed = handles.map |h|:
 h.wait()
```

Runtime receives additional structured metadata:

- workers count;
- partition boundaries;
- ordered gather;
- cancellation policy;
- trace metadata;
- possible auto-batching optimization.

---

#### 17.6. `scatter_reduce`

```amber
total = task.flow.scatter_reduce(
 items,
 init: 0,
 workers: 8,
 map: |x|: expensive_score(x),
 reduce: |acc, x|: acc + x
)
```

One-block form:

```amber
total = task.flow.scatter_reduce(items, init: 0, workers: 8) |partition|:
 partition.reduce(0) |acc, x|:
 acc + expensive_score(x)
```

Semantics:

1. input split into partitions;
2. each partition computed in parallel;
3. partial results gathered;
4. reduce performed in parent strand or tree-reduce mode;
5. final result returned.

Numerics warning:

```text
Parallel floating-point reduction may change operation order and produce slightly different low-level numeric results.
```

---

#### 17.7. `broadcast`

```amber
flow = task.flow.broadcast(config, workers: 8) |cfg, ctx|:
 worker_loop(cfg)
```

Semantics:

- one value sent to all workers;
- in checked mode, value must be shareable;
- in unchecked mode, mutable broadcast is allowed but unsafe;
- each worker receives same value/reference according to isolation mode.

---

#### 17.8. `gather`

```amber
handles = items.map |x|:
 task.spawn:
 process(x)

values = task.flow.gather(handles)
```

API:

```amber
task.flow.gather(handles)
task.flow.gather(handles, timeout: 5.0)
task.flow.gather(handles, ordered: true)
task.flow.gather(handles, fail::first)
task.flow.gather(handles, fail::collect)
```

Failure policies:

```text:first — first failure cancels siblings and is rethrown:collect — gather successes and failures in GatherResult:ignore — return only successes; failures accessible in result.failures
```

Default:

```text
fail::first
ordered: true
```

---

#### 17.9. Partitioning policy

```amber
scatter_map(items, workers: 8, partition::chunks) |x|:...
```

Supported v1 policies:

```text:items — one logical work item per input item; runtime may batch:chunks — contiguous chunks:stride — round-robin distribution:custom — user partitioner block
```

Custom partitioner:

```amber
parts = task.flow.scatter(items, partition: |items, workers|:
 items.group_by: _1.customer_id.hash() % workers
) |group|:
 process_group(group)
```

In checked mode, custom partitions must be shareable.

---

#### 17.10. Flow cancellation

If parent flow cancelled:

```text
flow.cancel()
 -> cancel all worker handles
 -> wake blocked gather
 -> return/raise CancelledError according to caller context
```

If worker failed under default `fail::first`:

1. store first failure;
2. cancel siblings;
3. gather rethrows failure in parent;
4. stack trace contains flow boundary.

---

#### 17.11. Ordering

Default result order preserves input order:

```amber
out = scatter_map([a, b, c], workers: 2) |x|:
 f(x)

## out[0] corresponds to a
## out[1] corresponds to b
## out[2] corresponds to c
```

Future streaming API:

```amber
task.flow.gather_each(handles) |result|:
 consume(result)
```

`gather_each` may return in completion order and must not be default.

---

#### 17.12. Flow isolation modes

Checked mode:

```amber
task.flow.scatter_map(items, workers: 8) |x|:
 process(x)
```

Rules:

- input partitions must be shareable;
- closure captures must be shareable;
- worker results crossing back to parent must be shareable;
- non-shareable input gives diagnostic or `IsolationError`.

Unchecked mode:

```amber
task.flow.scatter_map(
 items,
 workers: 8,
 isolation::unchecked
) |x|:
 mutate_shared_state(x)
```

Rules:

- flow marked unsafe;
- partition/capture shareability gate skipped;
- lifetime/GC/verifier checks still mandatory;
- user must synchronize shared mutation manually.

---

### 18. Task state machine

Minimal task states:

```text
new
runnable
running
sleeping
waiting
done
failed
cancelled
```

Transitions:

```text
new -> runnable
runnable -> running
running -> runnable on yield
running -> sleeping on sleep
running -> waiting on wait/channel/mutex
sleeping -> runnable on timer/resume/cancel wake
waiting -> runnable on dependency ready/resume/cancel wake
running -> done on normal return
running -> failed on unhandled exception
running -> cancelled on observed cancellation
```

Illegal internal transitions produce `InvalidTaskStateError`.

---

### 19. Scheduler design

#### 19.1. Worker pool

```text
WorkerPool(
 workers[],
 global_inject_queue,
 timer_wheel_or_heap,
 io_reactor?,
 shutdown_flag
)
```

Worker:

```text
Worker(
 worker_id,
 local_strand_queue,
 current_strand?,
 stats
)
```

#### 19.2. Strand

```text
Strand(
 strand_id,
 owner_worker_hint,
 runnable_tasks,
 sleeping_tasks,
 waiting_tasks,
 current_task?,
 status,
 epoch,
 local_alloc_cache
)
```

Invariants:

- at most one running task per strand;
- runnable queue local to strand;
- strand may migrate between workers only when no task is running;
- migration preserves task order inside strand.

#### 19.3. Reference scheduling algorithm

1. Worker pops runnable strand from local queue.
2. Worker runs one task from strand until safepoint, block, return, failure or budget expiration.
3. If strand still has runnable tasks, requeue strand.
4. If strand blocks completely, remove from runnable queues.
5. Timers/channel/mutex/resume events re-enqueue strand.
6. Work stealing may move runnable strand between workers.

Budget:

```text
cooperative_budget = instruction_count or time_slice
```

---

### 20. Safepoints

Required safepoints:

```text
task.sleep
task.yield
TaskHandle.wait
Channel.send blocking path
Channel.recv blocking path
Mutex.lock blocking path
loop back-edge
function/method call boundary
explicit SAFEPOINT bytecode
IO awaitable boundary, later
```

At safepoint:

1. poll cancellation;
2. process pending wake/resume;
3. allow GC root scanning;
4. allow scheduler preemption budget check;
5. update trace events if observability profile enabled.

---

### 21. HIR lowering

Surface forms remain ordinary AST send/block syntax unless builtin/intrinsic resolution proves canonical identity.

HIR nodes:

```text
HAsyncRoot
HSpawnSameStrand
HSpawnNewStrand
HSpawnUncheckedNewStrand
HTaskSleep
HTaskYield
HWait
HCancel
HResume
HTaskCheckCancelled
HChannelSend
HChannelRecv
HMutexLock
HMutexUnlock
HAtomicGet
HAtomicSet
HAtomicCAS
HFlowScatter
HFlowGather
HFlowScatterMap
HFlowScatterReduce
HFlowBroadcast
```

Lowering examples:

```text
async |task|: body
 -> HAsyncRoot(task_param, body)

task.async |child|: body
 -> HSpawnSameStrand(task_context, child_param, body)

task.spawn |child|: body
 -> HSpawnNewStrand(isolation=:checked, child_param, body)

task.spawn(isolation::unchecked) |child|: body
 -> HSpawnNewStrand(isolation=:unchecked, child_param, body)

task.flow.scatter_map(items, workers: n) |x|: body
 -> HFlowScatterMap(isolation=:checked, items, workers, body)

task.flow.scatter_map(items, workers: n, isolation::unchecked) |x|: body
 -> HFlowScatterMap(isolation=:unchecked, items, workers, body)
```

If `task`, `flow`, `spawn`, `sleep`, `yield`, `wait`, `cancel`, `resume`, `Channel`, `Mutex` or `Atomic` are shadowed by user bindings, lowering must not treat them as intrinsics. They remain ordinary dynamic calls.

---

### 22. Bytecode / VM opcodes

Minimum concurrency opcode family:

```text
SPAWN_SAME
SPAWN_NEW
SPAWN_NEW_UNCHECKED
TASK_CURRENT
TASK_SLEEP
TASK_YIELD
TASK_WAIT
TASK_CANCEL
TASK_RESUME
TASK_CHECK_CANCELLED

CHANNEL_NEW
CHANNEL_NEW_UNCHECKED
CHANNEL_SEND
CHANNEL_RECV
CHANNEL_CLOSE
CHANNEL_CLOSED

MUTEX_NEW
MUTEX_LOCK
MUTEX_UNLOCK
MUTEX_SYNCHRONIZE_ENTER
MUTEX_SYNCHRONIZE_EXIT

ATOMIC_NEW
ATOMIC_GET
ATOMIC_SET
ATOMIC_CAS
ATOMIC_UPDATE_LOOP

FLOW_SCATTER
FLOW_GATHER
FLOW_REDUCE
FLOW_CANCEL

SAFEPOINT
```

Implementation may also dispatch via ordinary `SEND`/`CALL`, but intrinsic opcodes are recommended for verifier checks and performance.

---

### 23. Verifier rules

Verifier must check:

```text
SPAWN_NEW closure operand exists and is callable
SPAWN_NEW closure capture map is encoded
SPAWN_NEW checked regions have shareability metadata or dynamic guards
SPAWN_NEW_UNCHECKED appears only with unsafe_concurrency feature flag
unchecked regions have source spans and unsafe metadata
TASK_WAIT target register has TaskHandle-compatible value or dynamic guard
CHANNEL_SEND has safepoint metadata
MUTEX_LOCK has safepoint metadata
loop back-edges include SAFEPOINT or equivalent call-boundary poll
handler tables cover synchronize unwind regions
root maps include blocked task frames
shareable sections contain no raw pointers
FLOW_* sections preserve partition/gather metadata
FLOW_* unchecked mode requires unsafe_concurrency feature flag
```

Verifier must not:

- skip GC root-map validation in unsafe regions;
- skip write-barrier validation;
- allow unsafe concurrency without artifact marker;
- execute user code.

---

### 24. Runtime ABI

#### 24.1. Task

```text
Task(
 task_id,
 strand_id,
 parent_task_id?,
 state,
 frame_stack,
 result?,
 failure?,
 cancel_flag,
 wake_pending,
 structured_children,
 waiting_on?,
 deadline?,
 trace_context?
)
```

#### 24.2. TaskHandle

```text
TaskHandle(
 target_task_id,
 target_strand_id,
 sync_header,
 result_cell,
 failure_cell,
 state_cell
)
```

Owner mode:

```text
sync
```

#### 24.3. Channel

```text
Channel(
 sync_header,
 capacity,
 closed_flag,
 isolation_mode,
 buffer_queue,
 waiting_senders,
 waiting_receivers
)
```

Waiters:

```text
SenderWaiter(task_id, strand_id, value, deadline?)
ReceiverWaiter(task_id, strand_id, target_register, deadline?)
```

#### 24.4. Mutex

```text
Mutex(
 sync_header,
 owner_task_id?,
 owner_strand_id?,
 recursion_guard,
 waiting_tasks
)
```

#### 24.5. Atomic

```text
Atomic(
 sync_header,
 atomic_cell,
 compatibility_tag
)
```

#### 24.6. Flow

```text
Flow(
 flow_id,
 parent_task_id,
 worker_group?,
 worker_handles[],
 partitions[],
 result_cells[],
 failure_cells[],
 isolation_mode,
 failure_policy,
 ordered,
 deadline?
)
```

---

### 25. Interaction with `$_`

Each task has its own frame stack and `last_result` slot.

Rules:

- `$_` is task-local through current frame;
- same-strand sibling tasks do not share `$_`;
- spawned tasks do not share `$_`;
- flow worker tasks do not share `$_`;
- root async scope result is final `$_` of root task unless explicit return/break rules apply.

---

### 26. Interaction with exceptions

Rules:

- unhandled exception in task marks task `failed`;
- `handle.wait()` re-raises failure in waiter;
- structured parent observes child failure during auto-join;
- sibling cancellation follows structured policy;
- stack trace includes async/task/flow boundary;
- stack trace is deterministic.

Example trace shape:

```text
Unhandled ParseError: invalid record
 at parse_file(path) input.am:12
 in task.spawn child task #stable-task-3
 in flow.scatter worker partition #4
 awaited by main.am:44
 in async root task #stable-task-1
```

Task IDs in diagnostics must be normalized/stable for tests, not raw memory addresses.

Unsafe boundary trace example:

```text
DataRaceDetectedError: unsynchronized ivar write
 at update_shared(state) worker.am:18
 in unsafe concurrency region task.spawn(isolation::unchecked) worker.am:14
 in async root task #stable-task-1
```

---

### 27. Interaction with IO / networking

`task/sync/flow` must precede IO/networking.

Future IO rules:

- blocking read/write are cancellation points;
- timeout uses same scheduler deadline model;
- close during pending read/write wakes blocked tasks;
- native handles never leak as raw pointers;
- TCP/HTTP reuse `TimeoutError` and `CancelledError`.

VM-internal hooks needed:

```text
register_awaitable(task, readiness_source, deadline?)
wake_task(task_id)
cancel_wait(task_id)
```

---

### 28. Observability events

Minimum task/sync events:

```text
task.started
task.blocked
task.resumed
task.cancelled
task.completed
task.failed

strand.enqueued
strand.migrated

channel.send
channel.recv
channel.close

mutex.wait
mutex.lock
mutex.unlock

atomic.cas
```

Flow events:

```text
flow.scatter.start
flow.scatter.partition
flow.worker.start
flow.worker.done
flow.worker.failed
flow.gather.start
flow.gather.done
flow.reduce.start
flow.reduce.done
flow.cancel
```

Unsafe events:

```text
unsafe.region.enter
unsafe.region.exit
unsafe.spawn
unsafe.channel
unsafe.policy.denied
```

Tracing must not change shareability rules or scheduling result unless deterministic scheduler profile is explicitly enabled.

---

### 29. Error registry

Required errors:

```text
TaskError
TaskNotDoneError
TaskFailedError
CancelledError
TimeoutError
IsolationError

ChannelError
ChannelClosedError

MutexError
DeadlockError
OwnershipError

AtomicError
AtomicCompatibilityError

FlowError
FlowCancelledError
FlowPartitionError
FlowGatherError

SchedulerError
InvalidTaskStateError

UnsafeConcurrencyError
UnsafeConcurrencyDeniedError
DataRaceDetectedError
```

Inheritance proposal:

```text
RuntimeError
 TaskError
 TaskNotDoneError
 TaskFailedError
 CancelledError
 TimeoutError
 IsolationError
 ChannelError
 ChannelClosedError
 MutexError
 DeadlockError
 OwnershipError
 AtomicError
 AtomicCompatibilityError
 FlowError
 FlowCancelledError
 FlowPartitionError
 FlowGatherError
 SchedulerError
 InvalidTaskStateError
 UnsafeConcurrencyError
 UnsafeConcurrencyDeniedError
 DataRaceDetectedError
```

Notes:

- `TimeoutError` shared by task, IO, networking.
- `CancelledError` shared by task, IO, networking.
- `IsolationError` shared by ownership runtime and task/sync.
- `DataRaceDetectedError` optional sanitizer/debug profile; release VM need not detect all races.

---

### 30. API examples

#### 30.1. Same-strand producer/consumer

```amber
async |task|:
 rows = []

 producer = task.async |child|:
 100.times |i|:
 rows << i
 child.yield()

 consumer = task.async |child|:
 loop:
 child.yield()
 if rows.count() >= 100:
 break rows.reduce(0) |acc, x|: acc + x

 consumer.wait()
```

#### 30.2. Cross-strand computation

```amber
async |task|:
 input = [1, 2, 3].freeze()

 worker = task.spawn |child|:
 input.reduce(0) |acc, x|:
 acc + x

 worker.wait()
```

#### 30.3. Checked channel pipeline

```amber
from sync import Channel

async |task|:
 ch = Channel.new(capacity: 16)

 producer = task.spawn |child|:
 [1, 2, 3].freeze().each |x|:
 ch.send(x)
 ch.close()

 consumer = task.async |child|:
 total = 0
 loop:
 try:
 total = total + ch.recv()
 catch ChannelClosedError:
 break total

 consumer.wait()
```

#### 30.4. Unchecked shared state with explicit mutex

```amber
from sync import Mutex

async |task|:
 m = Mutex.new()
 shared = []

 workers = (1..4).map |i|:
 task.spawn(isolation::unchecked) |child|:
 m.synchronize:
 shared << i

 workers.each |h|:
 h.wait()

 shared.count()
```

#### 30.5. Atomic counter

```amber
from sync import Atomic

async |task|:
 counter = Atomic.new(0)

 workers = (1..4).map |i|:
 task.spawn |child|:
 1000.times:
 counter.update |x|: x + 1

 workers.each |h|:
 h.wait()

 counter.get()
```

#### 30.6. Scatter map

```amber
from task.flow import scatter_map

async |task|:
 files = discover_files().freeze()

 parsed = scatter_map(files, workers: 8) |file|:
 parse_file(file)

 parsed.count()
```

#### 30.7. Scatter reduce

```amber
from task.flow import scatter_reduce

total = scatter_reduce(items, init: 0, workers: 8) |partition|:
 partition.reduce(0) |acc, x|:
 acc + score(x)
```

#### 30.8. Unchecked scatter map

```amber
from task.flow import scatter_map
from sync import Mutex

m = Mutex.new()
shared = []

scatter_map(items, workers: 8, isolation::unchecked) |x|:
 value = compute(x)
 m.synchronize:
 shared << value
```

---

### 31. Conformance corpus

#### 31.1. Scheduler corpus

```text
task_async_same_strand_mutable_capture_ok
task_spawn_mutable_capture_isolation_error
task_spawn_shareable_capture_ok
task_yield_allows_sibling_progress
task_sleep_timer_resume
task_wait_returns_result
task_wait_reraises_failure
task_wait_timeout
task_cancel_sleeping_child
task_cancel_waiting_child
structured_auto_join
structured_sibling_cancel_on_failure
task_stack_trace_deterministic
```

#### 31.2. Channel corpus

```text
channel_unbuffered_send_recv
channel_buffered_fifo
channel_waiting_senders_fifo
channel_waiting_receivers_fifo
channel_close_idempotent
channel_send_after_close_error
channel_recv_closed_buffered_drains
channel_recv_closed_empty_error
channel_send_non_shareable_isolation_error
channel_recv_cancellation
channel_send_timeout
channel_recv_timeout
```

#### 31.3. Mutex corpus

```text
mutex_lock_unlock
mutex_non_owner_unlock_error
mutex_double_lock_same_task_deadlock_error
mutex_waiter_fifo
mutex_synchronize_returns_block_value
mutex_synchronize_unlocks_on_exception
mutex_lock_wait_cancellation
```

#### 31.4. Atomic corpus

```text
atomic_get_set
atomic_compare_and_set_success
atomic_compare_and_set_failure
atomic_update_returns_new_value
atomic_update_retries
atomic_rejects_non_compatible_payload
atomic_cross_strand_counter
```

#### 31.5. Ownership corpus

```text
spawn_capture_array_rejected
spawn_capture_map_rejected
spawn_capture_user_object_rejected
spawn_capture_frozen_tuple_ok
spawn_capture_closure_with_confined_capture_rejected
cross_strand_send_confined_object_rejected
cross_strand_sync_object_ok
foreign_strand_ivar_load_isolation_error
foreign_strand_method_send_isolation_error
```

#### 31.6. Flow corpus

```text
flow_scatter_map_ordered_result
flow_scatter_reduce_sum
flow_scatter_first_failure_cancels_siblings
flow_scatter_collect_failures
flow_scatter_timeout
flow_scatter_parent_cancellation
flow_broadcast_shareable_value
flow_rejects_non_shareable_partition_safe_mode
flow_unchecked_allows_non_shareable_partition
flow_trace_events_deterministic
flow_partition_chunks
flow_partition_stride
flow_partition_custom
```

#### 31.7. Unsafe concurrency corpus

Checked-mode tests:

```text
spawn_capture_mutable_array_checked_rejected
spawn_capture_user_object_checked_rejected
channel_send_mutable_checked_rejected
flow_mutable_partition_checked_rejected
```

Unchecked-mode tests:

```text
spawn_capture_mutable_array_unchecked_allowed
spawn_capture_user_object_unchecked_allowed
unchecked_spawn_sets_module_feature_flag
unchecked_spawn_requires_loader_permission
unchecked_channel_allows_non_shareable_payload
unchecked_flow_allows_non_shareable_partition
unchecked_region_still_checks_destroyed_access
unchecked_region_still_checks_use_after_free
unchecked_region_still_runs_write_barriers
unchecked_region_stack_trace_marks_unsafe_boundary
```

Optional sanitizer tests:

```text
race_sanitizer_detects_unsynchronized_array_mutation
race_sanitizer_detects_unsynchronized_ivar_write
```

---

### 32. Implementation backlog

#### TASK-001 — Runtime object model for Task / Strand / Worker

Deliverables:

- `Task` struct;
- `TaskHandle` object;
- `Strand` runnable queue;
- worker pool;
- stable task IDs for diagnostics;
- state transition helpers.

DoD:

- root task can run to completion;
- deterministic task state dump;
- no raw pointer IDs in diagnostics.

---

#### TASK-002 — `async` root scope

Deliverables:

- parser/HIR acceptance for root `async |task|:`;
- HIR `HAsyncRoot`;
- VM root strand creation;
- structured auto-join.

DoD:

- root async returns block result;
- root async auto-joins children;
- root async propagates child failure.

---

#### TASK-003 — `task.async`

Deliverables:

- same-strand child creation;
- cooperative scheduling;
- same-strand mutable capture allowed;
- `TaskHandle` return.

DoD:

- sibling tasks interleave at yield/sleep/wait;
- shared Array within same strand works;
- no parallel access within same strand.

---

#### TASK-004 — `task.spawn` checked mode

Deliverables:

- new strand creation;
- worker queue integration;
- capture shareability verification;
- `IsolationError` path;
- cross-worker execution.

DoD:

- spawned tasks can run in parallel;
- mutable captures rejected;
- shareable captures accepted.

---

#### TASK-005 — unchecked spawn mode

Deliverables:

- `isolation::unchecked` parsing/lowering;
- artifact feature flag `unsafe_concurrency`;
- frame unsafe region flag;
- loader policy checks;
- diagnostics/warnings.

DoD:

- mutable captures allowed only when explicit;
- unsafe artifact denied without permission;
- lifetime checks remain active;
- stack trace marks unsafe boundary.

---

#### TASK-006 — Cancellation

Deliverables:

- cancellation flag;
- wake sleeping/waiting task on cancel;
- `CancelledError`;
- cancellation polling at safepoints;
- structured propagation.

DoD:

- cancellation visible in sleep/wait/channel/mutex;
- CPU loop can observe via `check_cancelled!`;
- sibling cancellation on failure.

---

#### TASK-007 — Timeouts

Deliverables:

- scheduler deadline model;
- timer queue;
- `wait(timeout:)`;
- channel timeout hooks;
- mutex timeout hooks if included.

DoD:

- timeout deterministic under test clock;
- timeout does not automatically cancel child;
- timeout error class shared with IO later.

---

#### TASK-008 — Channel

Deliverables:

- bounded buffer;
- unbuffered rendezvous;
- FIFO sender/receiver queues;
- close semantics;
- shareability check;
- unchecked channel mode.

DoD:

- all channel corpus green;
- no send after close;
- closed buffered channel drains before error;
- unchecked channel requires unsafe feature flag.

---

#### TASK-009 — Mutex

Deliverables:

- non-reentrant mutex;
- owner tracking;
- waiter FIFO;
- `synchronize` unwind guard.

DoD:

- same owner double lock gives `DeadlockError`;
- non-owner unlock gives `OwnershipError`;
- `synchronize` unlocks during exception/cancellation unwind.

---

#### TASK-010 — Atomic

Deliverables:

- seq-cst cell;
- `get`;
- `set`;
- `compare_and_set`;
- `update` CAS loop;
- atomic-compatible payload guard.

DoD:

- atomic counter across spawned strands works;
- incompatible mutable payload rejected;
- `update` retry behavior tested.

---

#### TASK-011 — Flow API

Deliverables:

- `scatter`;
- `scatter_map`;
- `scatter_reduce`;
- `broadcast`;
- `gather`;
- partitioning policies;
- ordered gather;
- failure policies.

DoD:

- flow corpus green;
- ordered result stable;
- failure propagation deterministic;
- cancellation propagates to flow workers.

---

#### TASK-012 — HIR/bytecode/verifier hooks

Deliverables:

- concurrency HIR nodes;
- flow HIR nodes;
- concurrency opcodes or intrinsic sends;
- safepoint metadata;
- root maps for blocked tasks;
- handler table validation;
- unsafe region verification.

DoD:

- malformed concurrency bytecode rejected before execution;
- safepoints present on loops/blocking ops;
- stack/root maps valid during blocked task GC;
- unsafe concurrency denied without explicit feature flag.

---

#### TASK-013 — Diagnostics and error registry

Deliverables:

- canonical error classes;
- diagnostic codes for static isolation violations;
- warnings for unchecked concurrency;
- deterministic task/flow stack traces;
- fixture normalizer support.

DoD:

- negative corpus stable;
- no raw addresses;
- stable task/strand/flow labels in golden output.

---

#### TASK-014 — Observability hooks

Deliverables:

- event emission points;
- trace context metadata propagation;
- event normalization for tests;
- unsafe boundary trace events.

DoD:

- task/channel/mutex/atomic/flow event corpus green;
- tracing does not change semantics unless deterministic scheduler profile is enabled.

---

### 33. Release gates

#### Gate A — single-strand async

Must pass:

```text
async root
task.async
yield
sleep
wait
same-strand mutable capture
structured auto-join
```

Exit criterion:

```text
Cooperative async works without cross-strand parallelism.
```

---

#### Gate B — no-GIL spawn checked mode

Must pass:

```text
task.spawn
worker pool
new strands
shareability checks
IsolationError
parallel strand smoke test
```

Exit criterion:

```text
Multiple strands execute on multiple workers without global interpreter lock under safe default rules.
```

---

#### Gate C — sync primitives

Must pass:

```text
Channel
Mutex
Atomic
cancellation while blocked
timeouts
FIFO guarantees
```

Exit criterion:

```text
User code can safely coordinate spawned strands.
```

---

#### Gate D — unchecked concurrency

Must pass:

```text
unchecked spawn
unchecked channel
unsafe_concurrency artifact flag
loader policy denial
lifetime checks inside unsafe region
stack traces with unsafe boundary
```

Exit criterion:

```text
System/performance-critical code can explicitly bypass isolation while VM safety remains intact.
```

---

#### Gate E — scatter/gather flow

Must pass:

```text
scatter_map
scatter_reduce
broadcast
gather ordered results
failure policy
flow cancellation
flow unchecked mode
```

Exit criterion:

```text
MPI-like local threading workflows are available on top of no-GIL strands.
```

---

#### Gate F — full conformance

Must pass:

```text
scheduler corpus
channel corpus
mutex corpus
atomic corpus
ownership corpus
flow corpus
unsafe concurrency corpus
deterministic stack traces
runtime error registry
```

Exit criterion:

```text
task/sync/flow layer is stable enough for Watch, IO, networking and HTTP layers.
```

---

### 34. Documentation warning for unchecked APIs

Every unchecked API page must contain this warning:

```text
`isolation::unchecked` disables Amber's strand isolation checks for this operation.
It does not make ordinary objects thread-safe.
Use Mutex, Atomic, Channel, or another explicit synchronization mechanism to establish happens-before edges.
Programs using unchecked concurrency can observe data races and nondeterministic Amber-level state.
VM memory safety, GC correctness and lifetime checks remain mandatory.
```

---

### 35. Оставшиеся варианты реализации

The following are implementation choices, not language-level semantic changes:

1. Whether `async |task|:` is parsed as special AST node or ordinary call before intrinsic resolution.
2. Whether flow API lowers to dedicated `FLOW_*` opcodes or to `SPAWN_NEW` + `WAIT` + runtime library calls.
3. Whether `task.unsafe_spawn` alias is included in v1 or only `task.spawn(isolation::unchecked)`.
4. Whether `Channel.new(isolation::unchecked)` is v1 or staged after unchecked spawn.
5. Whether debug race sanitizer ships with reference VM or as profile-specific tool.
6. Whether flow worker-group reuses strands or always spawns fresh strands.

Recommended v1 decisions:

```text
Use canonical keyword form: task.spawn(isolation::unchecked)
Include Channel.new(isolation::unchecked)
Use HIR flow nodes even if bytecode lowers to primitive spawn/wait
Do not recursively retag object graphs for unsafe sharing
Use frame-level unsafe region flag
Keep race sanitizer optional
```

---

### 36. Immediate first issues

Recommended issue order:

1. `ASYNC-001` Task/Strand/Worker runtime structs.
2. `ASYNC-002` Root async scope and task context.
3. `ASYNC-003` Same-strand `task.async`.
4. `ASYNC-004` Checked `task.spawn` and shareability gate.
5. `ASYNC-005` `TaskHandle.wait/result/failure/cancel`.
6. `ASYNC-006` Scheduler safepoints and cancellation polling.
7. `SYNC-001` Channel checked mode.
8. `SYNC-002` Mutex and `synchronize` unwind safety.
9. `SYNC-003` Atomic seq-cst API.
10. `UNSAFE-001` `isolation::unchecked` spawn mode.
11. `UNSAFE-002` unsafe artifact flag and loader policy.
12. `FLOW-001` `gather(handles)`.
13. `FLOW-002` `scatter_map` ordered result.
14. `FLOW-003` `scatter_reduce`.
15. `FLOW-004` flow failure/cancellation policy.
16. `TEST-001` scheduler/channel/mutex/atomic corpus.
17. `TEST-002` flow/unsafe concurrency corpus.
18. `DOC-001` unsafe concurrency warnings and examples.

---

### 37. Definition of done

The threading/async API layer is done when:

1. Public API is documented with examples.
2. HIR lowering is deterministic.
3. Bytecode/verifier accepts valid task/sync/flow programs.
4. Malformed bytecode is rejected before execution.
5. Scheduler runs multiple strands on multiple workers without GIL.
6. Checked mode reliably enforces `IsolationError`.
7. Unchecked mode is explicit and artifact-marked.
8. Lifetime/GC safety is preserved inside unsafe regions.
9. Channel FIFO and close semantics pass corpus.
10. Mutex non-reentrant semantics pass corpus.
11. Atomic seq-cst semantics pass corpus.
12. Scatter/gather preserves order by default.
13. Flow failure/cancellation policies pass corpus.
14. Stack traces are deterministic.
15. Diagnostics contain no raw pointer values.
16. Host policy can deny unsafe concurrency.
17. Documentation clearly separates safe default from unsafe escape hatch.

---

### 38. Final implementation stance

Amber should keep safe strand isolation as the default because it gives ordinary users a predictable no-GIL programming model. However, because the VM is explicitly designed for real parallel execution without GIL, the runtime must also expose an explicit `isolation::unchecked` mode for advanced/system code.

The correct split is:

```text
Default user code:
 checked isolation + IsolationError + shareable/sync boundaries

Performance/system/HPC code:
 explicit unchecked isolation + manual synchronization + artifact policy flag
```

MPI-like scatter/gather belongs above this model as a structured high-level API. In checked mode it is safe and shareability-enforced. In unchecked mode it becomes a powerful low-level threading tool for code that deliberately wants shared mutable state and accepts synchronization responsibility.

This preserves Amber's design goals:

- no global interpreter lock;
- safe default concurrency;
- real parallelism;
- explicit unsafe escape hatch;
- deterministic conformance;
- strong VM memory safety.

# Часть IV. Компилируемый Amber: implementation blueprint

**Проектный слой компилируемого Amber** 
Самодостаточный engineering blueprint для reference implementation 
14 мая 2026; обновлено compile-closure patch 14 мая 2026

## 0. Статус документа

Этот файл является отдельным проектным слоем поверх основной языковой спецификации Amber. Он не вводит новый surface syntax и не переоткрывает закрытые решения языка. Его задача — перевести уже зафиксированный Amber в набор инженерных контрактов, артефактов, этапов и acceptance-критериев, достаточных для реализации компилятора, байткодной VM, loader/verifier, no-GIL runtime и conformance suite.

Каноническая граница такая:

- этот файл отвечает за то, как построить компилируемый Amber: pipeline, ABI, форматы, runtime-подсистемы, тесты, backlog и implementation matrix;
- любые изменения здесь не должны менять наблюдаемое поведение языка без отдельного spec-format bump в основном документе.

Дополнение **compile-closure patch** в конце файла закрывает оставшиеся инженерные лакуны, которые мешали бы двум независимым реализациям получить совместимый компилируемый Amber: source/literal completion, prelude/builtin registry, slot/allocation rules, call ABI, operator lowering, exception/error registry, bytecode binary encoding, verifier dataflow, stack/root maps, build graph, bootstrap и расширенную implementation matrix.

## 1. Цель проектного слоя

Компилируемый Amber должен иметь воспроизводимый путь:

```text
source.am
 -> tokens/spans
 -> amber.ast.v1
 -> amber.diag.v1
 -> amber.hir.v1
 -> pattern decision program
 -> amber.bc.v1
 ->.amberbc
 -> verifier
 -> loader/linker/init
 -> register/slot VM
 -> optional MIR/SSA/native/frozen image
```

Минимальный успешный результат P0/P1: программа Amber компилируется в `.amberbc`, проходит verifier, загружается loader'ом, исполняется в reference VM, даёт стабильные diagnostics/stack traces и подтверждается corpus/golden tests.

## 2. Архитектурные инварианты

1. **AST syntax-faithful.** Parser не имеет права прятать surface-формы в обычные вызовы: block suffix, `.?.`, `&target`, `case!`, `include`, `extend`, `package/import/export`, `Kernel.watch(...)` должны быть представлены явно.
2. **HIR semantic-core.** HIR обязан устранить surface-sugar: safe-nav раскладывается в null-guards, block suffix становится closure, many-def становится clause-style def, `$_` становится frame slot, auto-assign — post-dispatch commit step.
3. **Bytecode register/slot.** Reference VM не стековая; locals, temporaries, `last_result`, captures и handler-state живут в регистрово-слотовой модели.
4. **No-GIL через strand isolation.** Параллелизм даётся несколькими strand'ами на worker threads; ordinary mutable objects остаются strand-confined, cross-strand идут только shareable/sync values.
5. **Open-world до freeze.** `class` reopen, `mixin` reopen, `define_method`, `include/extend` и loader world-mutations легальны до freeze barrier и запрещены после неё.
6. **Verifier before execution.** `.amberbc` не исполняется в reference profile без verifier-pass.
7. **Diagnostics deterministic.** Все compiler diagnostics, disasm, stack traces и golden outputs не зависят от raw pointer values, абсолютных путей и случайного порядка map-итерации.
8. **Runtime safety observable.** Destroyed/deallocated/tombstone/pin/ownership ошибки должны доходить до канонических runtime errors, а не превращаться в undefined behavior.

## 3. Минимальные внешние артефакты

| Артефакт | Формат | Владелец | Назначение | Обязателен для P0/P1 |
|---|---|---:|---|---:|
| Token stream dump | internal/debug | lexer | отладка spans, INDENT/DEDENT, `CHAIN_DOT` | да |
| AST dump | `amber.ast.v1` JSON | parser | golden parser tests, IDE, formatter | да |
| Diagnostic dump | `amber.diag.v1` JSON | binder/checker | negative corpus, CI gates | да |
| HIR dump | `amber.hir.v1` JSON | lowering | semantic golden tests | да |
| Pattern IR dump | `amber.pattern.v1` JSON/disasm | pattern compiler | проверка decision programs | да |
| Bytecode container | `amber.bc.v1` inside `.amberbc` | bytecode emitter | исполнимый модуль | да |
| Bytecode disasm | deterministic text | disassembler | golden compile tests | да |
| Loader trace | deterministic optional text/json | loader | dependency/init/frozen failures | да для loader tests |
| Runtime stack trace | deterministic text | VM | unhandled exceptions | да |
| MIR dump | `amber.mir.v1` | native lane | SSA/native/JIT профили | нет, P3 |
| Frozen image | `.amberimg` | frozen lane | deployable artifact | нет, P3/P4 |

## 4. Reference repository layout

```text
amber/
 crates-or-packages/
 amber_lexer/
 amber_parser/
 amber_ast/
 amber_binder/
 amber_hir/
 amber_patterns/
 amber_bytecode/
 amber_vm/
 amber_loader/
 amber_stdlib/
 amber_test/
 amber_cli/
 spec/
 tests/
 parser/
 binder/
 hir/
 bytecode/
 runtime/
 loader/
 scheduler/
 stdlib/
 notebook/
 profiles/
 corpus/
 positive/
 negative/
 golden/
 tools/
 fixture_normalizer/
 disasm_diff/
 diag_lint/
```

Этот layout не предписывает язык реализации. Он фиксирует ответственность модулей и границы контрактов, чтобы Rust/Zig/C++/Go/другая реализация могла сохранять те же внешние артефакты.

## 5. Compiler pipeline: детальный контракт стадий

### 5.1. F0 — lexer, spans, indentation

**Вход:** UTF-8 source. 
**Выход:** token stream + stable source spans.

Обязательные детали:

- `INDENT`/`DEDENT` как structural tokens;
- `case!` как отдельный keyword token;
- `.?.` как отдельный safe-navigation token;
- `CHAIN_DOT` только в режиме one-liner block body при глубине скобок 0 и пробеле слева;
- contextual treatment для `pattern` и `as` не в lexer, а в parser/binder;
- spans line/column + byte offsets, пригодные для diagnostics и source maps.

Acceptance:

- token golden покрывает indentation, `.?.`, `CHAIN_DOT`, `$_`, `_1`, `&Class#method`;
- token dump deterministic;
- malformed indentation даёт compiler diagnostic, а не panic.

### 5.2. F1 — parser и AST

**Вход:** token stream. 
**Выход:** `amber.ast.v1`.

Обязательные детали:

- Pratt parser для expression/postfix зоны;
- syntax-faithful nodes для callable refs, safe-nav tails, block suffix, package/import/export, class/mixin/include/extend;
- `case`/`case!` сохраняют `strict` flag;
- `AstPatDynamic` хранит `matcher_expr` и optional `export_map_pattern`;
- parser не понижает `Kernel.watch`, `send`, lifecycle calls или async calls в intrinsics.

Acceptance:

- `amberc parse --json` стабилен;
- positive parser corpus green;
- invalid surface forms (`fn.()`, `map(_1 * 2)`, `&foo()`, `&(expr)`) дают ожидаемые syntax diagnostics.

### 5.3. F2 — binder, scopes, signatures, diagnostics

**Вход:** AST. 
**Выход:** resolved AST metadata + `amber.diag.v1`.

Обязательные детали:

- scope graph для locals/top-level/imports/classes/mixins;
- read-only imported aliases;
- signature validation: duplicate params, keyword conflicts, default references to right-side params, self-reference defaults;
- object-body placement checks: `include`, `extend`, `class_method def` inside mixin;
- pattern pre-checks: duplicate binds, OR binding set equality, rest position;
- callable reference target validation;
- notebook watch target validation when profile enabled.

Acceptance:

- negative corpus сходится по diagnostic codes;
- diagnostics include `code`, `severity`, `span`, `message`, optional `notes`;
- binder не выполняет user code.

### 5.4. F3 — HIR lowering и pattern compiler

**Вход:** AST + binder metadata. 
**Выход:** `amber.hir.v1` + pattern IR.

Обязательные lowering rules:

- block suffix -> explicit `HClosure`;
- `_1.._N` -> explicit closure params with dense arity;
- safe-nav -> `HSafe*` or explicit null-guard HIR, but never raw postfix magic below HIR;
- `$_` -> `HLastGet/HLastSet`;
- `case`/`case!` -> `HMatchDispatch` with `fail_mode`;
- clause-style `def` -> `HMethod` with bind/dispatch/commit/body stages;
- `send` builtin -> `HSend` or `HSendDyn` only when binding resolves to prelude builtin;
- `Kernel.watch(...)` -> watch intrinsics only when Notebook profile and builtin resolution match;
- callable refs -> `HCallableRef` or `HUnboundMethodRef`;
- constructor calls stay `HCall`, unless static rewrite to `HSend(:new)` is proven observationally equivalent.

Acceptance:

- `amberc lower --json` stable;
- HIR golden covers safe-nav, `case!`, pattern assignment, block suffix, callable refs, constructor-call, watch profile;
- failed pattern compilation never leaves partial bindings committed.

### 5.5. V0 — bytecode emitter, `.amberbc`, disassembler

**Вход:** HIR + pattern IR. 
**Выход:** `.amberbc` + deterministic disasm.

Обязательные детали:

- `BcModule`, `BcMethod`, `BcCode`, constant/symbol tables;
- versioned bytecode format with section table;
- no raw pointers in serialized module;
- bytecode includes source map/debug sections sufficient for stack traces;
- disasm line-stable and suitable for golden diff.

Acceptance:

- compile -> disasm -> golden passes;
- `.amberbc` read/write round-trip preserves code/debug sections;
- verifier rejects malformed bytecode before VM execution.

### 5.6. V1 — register/slot VM core

**Вход:** verified `BcModule`. 
**Выход:** executed program or deterministic runtime failure.

Обязательные детали:

- frame stack with code pointer, local registers, captures/upvalues, `last_result`, handlers, current task;
- dispatch opcodes: `SEND`, `SEND_DYN`, `CALL`, `RETURN`, `RAISE`, `JUMP`, `JUMP_IF_*`;
- `CALL` handles ordinary callable objects and class objects via constructor path;
- closures capture by upvalue cells;
- exceptions unwind frames and structured children correctly;
- stack trace does not leak memory addresses.

Acceptance:

- single-worker runtime corpus green;
- `$_` semantics confirmed across function/block/task frames;
- reflective dispatch and method_missing behavior match language spec.

### 5.7. V2 — object model, lifetime, allocator, collector boundary

Obligatory runtime structures:

```text
ObjectHeader(
 class_ptr,
 shape_ptr,
 flags,
 owner_strand,
 lifetime_state,
 gc_bits,
 pin_count,
 object_id?
)

Shape(shape_id, ivar_slots, parent_shape?, shape_version)

DispatchOwnerRuntime(
 method_table,
 method_version,
 direct_includes,
 owner_flags,
 ivar_schema?,
 superclass?
)
```

Mandatory behavior:

- `destroy!` and `memory.dealloc` lower to lifecycle intrinsics/opcodes when builtin identity is known;
- live -> destroying -> destroyed -> deallocated state machine;
- tombstone header remains safe to inspect through lifetime intrinsics;
- `DeadShape` invalidates ivar/method fast paths;
- non-moving generational collector with remembered sets and safe-points;
- per-worker allocation fast path + remote-free queues;
- pinning blocks reclamation/deallocation and exposes only opaque handles or buffer spans.

Acceptance:

- use-after-free/destroyed access corpus green;
- double destroy/dealloc idempotence tested;
- collector does not move objects in reference profile;
- pinning tests cover opaque and buffer modes.

### 5.8. V3 — no-GIL scheduler, strands, sync primitives

Mandatory behavior:

- worker pool executes runnable strands;
- one task runs per strand at a time;
- `task.async` stays same-strand, `task.spawn` creates new strand;
- shareability checks on spawn/channel sends;
- structured concurrency: auto-join, failure propagation, sibling cancellation;
- `Channel`, `Mutex`, `Atomic` semantics match spec;
- safe-points include sleep/yield/wait/channel/mutex/back-edge/call-boundary polling.

Acceptance:

- multi-worker tests demonstrate parallel strands without global interpreter lock;
- isolation violations produce `IsolationError`;
- FIFO channel behavior and non-reentrant mutex `DeadlockError` confirmed.

### 5.9. V4 — loader/linker/verifier

Mandatory behavior:

- module states: unloaded/loading/linking/initializing/initialized/failed;
- static deps from `DEPS` section;
- `EXPT` checks for from-import;
- cyclic initialization surfaces `ModuleInitError` on early export observation;
- loader participates in `world_epoch` and freeze barrier;
- load-time verifier checks register bounds, jump targets, handler tables, pattern slots, safe-points and shareable sections.

Acceptance:

- loader fixtures cover dependency linking, missing exports, cyclic init, verifier errors, frozen-loader barrier;
- VM can run `.amberbc` without compiler process.

### 5.10. V5 — stdlib, corpus, conformance

Mandatory behavior:

- chainable collections API for `Array`, `Tuple`, `Range`, `Set`, `LazySeq`, `Map`;
- runtime errors canonical names;
- `ambertest` runner reads `meta.json`, selects phase, compares golden outputs;
- corpus includes parser/binder/HIR/bytecode/runtime/loader/scheduler/notebook/profile lanes.

Acceptance:

- full dynamic P0/P1 corpus green;
- no conformance test depends on host-specific absolute paths or pointer values.

## 6. Runtime ABI minimum

| Area | Required structure | Key fields | Fast path | Slow/error path |
|---|---|---|---|---|
| Frame | `Frame` | code, locals, upvalues, last_result, handlers, task | direct register access | unwind/exception handler |
| Closure | `Closure` | code, captures, arity metadata | direct call if arity known | `CALL` protocol error |
| Class | `ClassObject` | method table, superclass, includes, versions | monomorphic `CallIC` | lookup + method_missing |
| Mixin | `MixinObject` | method table, direct includes, version | linearized lookup cache | include cycle/type error |
| Instance | `ObjectHeader + slots` | class, shape, owner, lifetime | shape/ivar IC | shape transition/dead check |
| Module | `ModuleRecord` | state, exports, deps, code | initialized export cell | `ImportError`/`ModuleInitError` |
| Task | `Task` | id, strand, state, stack, result/failure, cancel flag | same-strand resume | cancellation/timeout/failure propagation |
| Strand | `Strand` | run queue, timers, waiting handles, worker hint | local run queue | migration/wake from other worker |
| Pin | `PinToken` | object, mode, generation, active flag | active pin guard | stale/double-unpin/lifetime error |
| Watch | `WatchCell/WatchObjectState` | revision, subscribers, field revisions | revision compare | dependency invalidation event |

## 7. Bytecode ISA minimum

The reference ISA must contain at least these families:

| Family | Instructions | Purpose |
|---|---|---|
| Data/frame | `LOADK`, `LOADNULL`, `MOVE`, `GETLAST`, `SETLAST`, `LOAD_LOCAL`, `STORE_LOCAL` | locals and expression results |
| Object state | `LOAD_IVAR`, `STORE_IVAR`, `LOAD_CVAR`, `STORE_CVAR`, `LOAD_CONST` | class/object/module state |
| Calls | `SEND`, `SEND_DYN`, `CALL`, `MAKE_CLOSURE`, `RETURN` | method/callable/class-object calls |
| Control | `JUMP`, `JUMP_IF_TRUE`, `JUMP_IF_FALSE`, `JUMP_IF_NULL`, `RAISE`, `TRY_BEGIN`, `TRY_END`, `SAFEPOINT` | branching/exceptions/scheduler |
| Pattern | `P_MATCH_*`, `P_BIND`, `P_COMMIT`, `P_FAIL`, `P_GUARD` | compiled pattern decisions |
| Lifecycle | `OBJ_DESTROY`, `OBJ_DEALLOC` | explicit lifetime operations |
| Concurrency | `SPAWN_SAME`, `SPAWN_NEW`, `SLEEP`, `YIELD`, `WAIT`, `RESUME`, `CANCEL`, `CHANNEL_*`, `MUTEX_*`, `ATOMIC_*` | no-GIL runtime |
| Watch optional | `WATCH_BINDING`, `WATCH_IVAR`, `WATCH_CVAR`, `WATCH_REVISION`, `WATCH_EVENT` | notebook profile |
| Module/loader helper | implementation-specific | init/export cells, loader barriers | may be VM-private |

Safe-nav does not need dedicated bytecode opcodes; it must be lowered to null checks and ordinary sends/calls before or during bytecode emission.

## 8. Implementation matrix 

### 8.1. Priority bands

| Band | Meaning | Goal |
|---|---|---|
| P0 | Frontend semantic core | parse/check/lower to stable AST/HIR/diag |
| P1 | Executable VM core | compile `.amberbc`, verify, run in VM |
| P2 | Full dynamic runtime | memory, scheduler, loader, stdlib, corpus |
| P3 | Profiles after dynamic core | typed, packages, reflection, advanced concurrency |
| P4 | Optimizing/frozen/native | MIR/SSA, JIT/AOT, `.amberimg` |
| P5 | Modern pressure profiles | capabilities/effects/replay/data/schema/wasm/accelerator/AI/contracts/privacy/workflows |

### 8.2. Work-package matrix

| WP | Priority | Scope | Deliverables | Dependencies | Tests / DoD |
|---|---:|---|---|---|---|
| W0 | P0 | Repo/tooling baseline | CLI skeleton, fixture normalizer, golden runner, format-version registry | none | `amberc --version`, `ambertest` smoke, deterministic fixture normalization |
| W1 | P0 | Lexer/parser/AST | tokens/spans, Pratt parser, `amber.ast.v1`, parser diagnostics | W0 | parser positive/negative corpus green |
| W2 | P0 | Binder/signatures/diagnostics | scope graph, imports/exports, signature/default checks, `amber.diag.v1` | W1 | binder negative corpus exact codes |
| W3 | P0 | Patterns/HIR/lowering | pattern compiler, `amber.hir.v1`, block/safe-nav/`$_`/callable-ref lowering | W1-W2 | HIR golden + pattern behavior tests |
| W4 | P1 | Bytecode artifacts | `BcModule/BcMethod/BcCode`, `.amberbc`, verifier skeleton, disasm | W3 | compile/disasm golden, verifier rejects malformed modules |
| W5 | P1 | VM core/dispatch | frame loop, registers, call/send/callable/class call, exceptions, inline caches | W4 | single-worker runtime green |
| W6 | P2 | Object/memory/lifetime/GC/pinning | headers, shapes, allocator, tombstones, non-moving GC, `PinToken`, FFI boundary | W5 | lifetime/GC/pinning corpus green |
| W7 | P2 | Scheduler/concurrency | workers, strands, tasks, `async/spawn`, `Channel/Mutex/Atomic`, cancellation | W5-W6 | parallel strand tests + isolation tests |
| W8 | P2 | Loader/stdlib/full corpus | loader state machine, deps/exports, chainable stdlib, full conformance runner | W4-W7 | modules/stdlib/full dynamic corpus green |
| W9 | P3 | Typed/open-world/packages | optional typed checker, mirrors, package manifest, signing, hot reload, class-side `extend` runtime path | W8 | typed corpus does not alter dynamic semantics |
| W10 | P3/P4 | Advanced concurrency/native/frozen | `move`, `select`, async I/O, MIR/SSA, JIT/AOT, `.amberimg` | W8-W9 | frozen/native smoke + MIR validation |
| W11 | P5 | Modern profile runtime | capabilities, effects, replay, schema, dataframe, wasm, accelerators, AI tooling, contracts, privacy, workflows | W8, then W9/W10 as needed | profile-specific conformance lanes green |
| W12 | cross | Documentation/spec sync | generated anchor map, changelog, migration notes, implementation status dashboard | all active WPs | docs match artifacts; no stale references |

### 8.3. Subpackage decomposition

| WP | Subtasks |
|---|---|
| W0 | W0.1 repo skeleton; W0.2 CI smoke; W0.3 golden layout; W0.4 format registry; W0.5 contribution rules |
| W1 | W1.1 lexer; W1.2 Pratt core; W1.3 module/class/mixin grammar; W1.4 pattern grammar; W1.5 AST serializer |
| W2 | W2.1 scope graph; W2.2 signature pipeline; W2.3 import/export checks; W2.4 object placement checks; W2.5 diagnostic renderer |
| W3 | W3.1 pattern prechecker; W3.2 pattern IR; W3.3 AST->HIR; W3.4 builtin intrinsic resolution; W3.5 HIR serializer |
| W4 | W4.1 bytecode schema; W4.2 verifier; W4.3 disasm; W4.4 emitter; W4.5 `.amberbc` round-trip |
| W5 | W5.1 VM loop; W5.2 call frames; W5.3 send/call caches; W5.4 closures/upvalues; W5.5 exceptions/unwind |
| W6 | W6.1 object headers/shapes; W6.2 lifetime ops; W6.3 allocator; W6.4 collector; W6.5 pinning/FFI |
| W7 | W7.1 task/strand state machines; W7.2 worker pool; W7.3 structured concurrency; W7.4 channels/sync; W7.5 cancellation/timeouts |
| W8 | W8.1 loader graph; W8.2 module init/export cells; W8.3 stdlib collections; W8.4 full corpus; W8.5 notebook watch optional |
| W9 | W9.1 typed checker; W9.2 mirrors/reflection; W9.3 package/signing; W9.4 hot reload; W9.5 open-world transactions |
| W10 | W10.1 `move`; W10.2 `select`; W10.3 async I/O; W10.4 MIR/SSA; W10.5 native/JIT; W10.6 frozen image |
| W11 | W11.1 capabilities; W11.2 effects; W11.3 replay; W11.4 dataframe/schema; W11.5 wasm/accelerator; W11.6 AI/contracts/privacy/workflow |

## 9. Milestone gates

| Milestone | Closes | Gate criteria | Release meaning |
|---|---|---|---|
| M0 | W0 | repo, CLI stubs, corpus layout, CI smoke | implementation can start without format drift |
| M1 | W1-W2 | parser + binder + diagnostics golden green | stable frontend contract |
| M2 | W3-W4 | HIR + pattern IR + bytecode/disasm/verifier skeleton green | source compiles to verified artifacts |
| M3 | W5 + W6.1-W6.2 | single-worker VM, object basics, lifetime baseline | executable dynamic subset |
| M4 | W6.3-W7 | allocator/collector/pinning + scheduler | no-GIL reference runtime |
| M5 | W8 | loader, modules, stdlib, full dynamic corpus | P0/P1 reference implementation green |
| M6 | W9 | typed/profile packaging/open-world transactions | second-wave profile baseline |
| M7 | W10.1-W10.3 | advanced concurrency | production-grade async/concurrency profile |
| M8 | W10.4-W10.6 | MIR/native/frozen | native/frozen path exists |
| M9 | W0-W12 | full corpus + docs + reproducibility | release-grade reference baseline |
| M10 | W11 | modern profiles conformance | platform profile suite usable |

Rule: typed/native/profile lanes must not block M5. Dynamic reference runtime must become green before optional checker/native constraints can become normative for implementation.

## 10. Parallel lanes

| Lane | Work packages | Can start | Blocks | Notes |
|---|---|---|---|---|
| A Frontend | W1-W3 | after W0 | W4-W11 | critical first lane |
| B Artifacts/corpus | W0, W4 parts, W8 corpus | immediately | all gates | prevents manual golden drift |
| C VM/object/memory | W5-W6 | after W4 skeleton | W7-W8 | main runtime critical path |
| D Scheduler/loader/stdlib | W7-W8 | after VM/lifetime baseline | M5 | cannot skip verifier/collector boundary |
| E Typed/packages/open-world | W9 | after M5 | M6 | must not redefine dynamic semantics |
| F Native/frozen | W10.4-W10.6 | after freeze-aware W9 | M8 | native only on frozen boundary |
| G Modern profiles | W11 | after M5, profile by profile | M10 | metadata-first, opt-in |
| H Docs/spec-sync | W12 | always | release | keeps main spec and project layer aligned |

## 11. First implementation sequence

### Cycle A — frontend bootstrap

1. W0.1 repository skeleton.
2. W0.2 CLI stubs: `amberc`, `ambervm`, `ambertest`.
3. W0.3 fixture/golden layout.
4. W1.1 lexer/spans.
5. W1.2 Pratt expression parser.
6. W1.3 module/class/mixin parser.
7. W1.5 AST serializer.

### Cycle B — semantic frontend

1. W2.1 scope graph.
2. W2.2 signatures/defaults.
3. W2.3 import/export checks.
4. W2.4 include/extend/class_method placement checks.
5. W3.1 pattern prechecker.
6. W3.2 pattern IR.
7. W3.3 HIR node set.

### Cycle C — compiled artifact

1. W3.4 lowering rules for `$_`, safe-nav, block suffix, callable refs, constructor-call, `send`, watch intrinsics.
2. W3.5 HIR serializer.
3. W4.1 `.amberbc` schema.
4. W4.2 verifier.
5. W4.3 deterministic disassembler.
6. W4.4 bytecode emitter baseline.

### Cycle D — executable VM baseline

1. W5.1 VM loop.
2. W5.2 frame/register ABI.
3. W5.3 `SEND/SEND_DYN/CALL` and caches.
4. W5.4 closures/upvalues.
5. W5.5 exceptions/unwind.
6. W6.1 object headers/shapes.
7. W6.2 `destroy!`/`memory.dealloc` baseline.

### Cycle E — real runtime

1. W6.3 allocator and tombstones.
2. W6.4 non-moving collector and barriers.
3. W6.5 pinning/FFI boundary.
4. W7.1-W7.5 scheduler/concurrency.
5. W8.1-W8.4 loader/stdlib/full corpus.

After Cycle E the project should have a usable P0/P1 reference implementation.

## 12. Acceptance checklist by subsystem

| Subsystem | Must prove |
|---|---|
| Parser | source spans stable; one-liner block boundary correct; invalid callable refs rejected |
| Binder | imports read-only; defaults ordered; pattern binds checked; diagnostics canonical |
| HIR | all surface sugar explicit; `case` and `case!` share engine; auto-assign commit after clause choice |
| Bytecode | no raw pointers; safe-points on back-edges; verifier catches invalid registers/jumps/handlers |
| VM | `CALL` handles class objects; `SEND_DYN` and `method_missing` legal after freeze; stack traces deterministic |
| Object model | shape caches invalidated; open class/mixin transactions atomic; include linearization stable |
| Lifetime | dead/destroyed/deallocated states observable with canonical errors; tombstone safe |
| GC | non-moving; remembered sets; no ownership-mode mutation by collector |
| Pinning/FFI | opaque handles for objects; spans only for supported buffers; stale pins rejected |
| Scheduler | no global interpreter lock; one task per strand; cross-strand isolation enforced |
| Loader | `.amberbc` verifier before execution; dependency/init states deterministic; frozen barrier enforced |
| Stdlib | chainable collection contracts pass; concurrency primitives match FIFO/seq-cst rules |
| Notebook | watch does not bump `world_epoch`; dependency capture invalidates by revisions |
| Profiles | opt-in metadata-first behavior; core semantics unchanged |

## 13. Risk matrix

| Risk | Impact | Early warning | Mitigation |
|---|---|---|---|
| Parser lowers too early | AST/IDE/golden instability | AST lacks block/safe/callable metadata | enforce syntax-faithful AST tests |
| HIR too close to surface syntax | VM emitter becomes ad hoc | bytecode emitter special-cases parser nodes | require all sugar elimination at HIR gate |
| Dynamic dispatch hidden in native assumptions | frozen/native unsoundness | native path touches open-world sites | native only after freeze + reflective stubs |
| Verifier too weak | VM crashes on malformed modules | fuzzed `.amberbc` panics runtime | load-time verifier mandatory |
| GC before ownership model | no-GIL heap races | collector changes object visibility | finish owner/strand fields before GC |
| Pinning exposes layout | FFI ABI freezes object internals | native code reads ivar offsets | only opaque handle except explicit buffer spans |
| Typed checker starts too early | checker changes language semantics | dynamic tests fail only with checker disabled | typed profile starts after M5 |
| Corpus not canonical | CI drift and flaky tests | golden differs by machine/path | normalize paths/order and stable serializers |
| Watch treated as world mutation | notebook profile invalidates optimizer incorrectly | `Kernel.watch` bumps `world_epoch` | separate `watch_epoch` and `world_epoch` |
| Modern profiles leak into core | surface complexity explosion | profile syntax required for ordinary code | profiles opt-in and metadata-first |

## 14. Definition of done for a work package

A work package is not done until all of these are true:

1. Public artifact format, if any, is versioned.
2. Positive and negative tests exist.
3. Diagnostics and stack traces are deterministic.
4. The corresponding golden fixtures are updated through the fixture normalizer, not by hand-editing unstable output.
5. The implementation has no generic internal-error fallback for spec-defined failures.
6. Documentation names the exact spec/project anchors used.
7. Format-impact is declared: no change, compatible extension, or explicit format bump.
8. Cross-lane dependencies are reflected in the milestone board.

## 15. Project-local coding contracts

These are implementation contracts, not language semantics:

- No stage may consume a later-stage representation directly. Parser cannot emit HIR; HIR cannot directly mutate bytecode module sections.
- Every serialized artifact must include `format`, `version`, `source_digest`, and `feature_flags` when relevant.
- Compiler phases must be deterministic under the same source and feature flags.
- Runtime slow paths must preserve the same observable errors as fast paths.
- Inline caches must be invalidated by `method_version`, `shape_version`, and `world_epoch` as applicable.
- All host/profile features must be behind explicit feature flags in artifacts and CLI.

## 16. Immediate next actions

The next concrete implementation move is to open the first repo issues in this order:

1. `ISS-001` repo skeleton and CLI crates/packages.
2. `ISS-002` canonical fixture layout and `ambertest` smoke runner.
3. `ISS-003` format-version registry for AST/HIR/diag/bytecode.
4. `ISS-004` lexer tokens/spans including indentation, `.?.`, `case!`, `CHAIN_DOT`.
5. `ISS-005` Pratt parser for expressions/postfix/calls/block suffix.
6. `ISS-006` parser for module/class/mixin/include/extend/signature forms.
7. `ISS-007` `amber.ast.v1` serializer and golden normalizer.
8. `ISS-008` scope graph and imported read-only aliases.
9. `ISS-009` signature/default binding pipeline.
10. `ISS-010` diagnostic engine with canonical code catalog.

These ten issues close the practical bootstrap gap from document to working frontend.

---

# Приложение A. Перенесённый P0 implementation package из основного файла


# Часть II. Пакет реализации P0

Этот раздел продолжает документ не как обзор истории, а как **реализационный пакет** для первого исполнимого ядра: парсер -> binder -> dispatch -> pattern runtime -> conformance suite.

## 15. Нормативная модель парсинга выражений

### 15.1. Лексические предпосылки

Для парсера Amber фиксируются следующие базовые токены и барьеры:

- `NEWLINE`, `INDENT`, `DEDENT` — как в Python-подобной модели блоков;
- `.` — обычная postfix-точка;
- `.?.` — отдельный токен safe-navigation;
- `:` — открытие блока или one-liner block suffix;
- `(` `)` `[` `]` `{` `}` `,`;
- `&` — prefix marker callable reference;
- `#` — separator внутри unbound callable reference target `&Class#method`;
- ключевые слова `not`, `and`, `or`, `in`, `if`, `else`, `elif`, `elsif`, `unless`, `case`, `case!`, `when`, `def`, `class`, `class_method`, `mixin`, `include`, `extend`, `while`, `until`, `do`, `loop`, `break`, `pass`, `noop`, `package`, `import`, `from`, `export`;
- `NEWLINE` не разрывает выражение внутри `()`, `[]`, `{}` и внутри интерполяции строк.
- `case!` лексируется как отдельная keyword-form, а не как `case` + postfix `!`;
- последовательность `class_method def` образует отдельную parser-level declarative form и не редуцируется к обычному send/call spelling;
- `pattern` не является глобально зарезервированным словом: это contextual keyword только в pattern-position при синтаксисе `pattern(...)`;
- `as` остаётся contextual keyword в type-position и в alias-позициях `import` / `from` / `export`.

Дополнительный лексический механизм v1:

- внутри **one-liner block body** при глубине скобок `0` токен `.` с хотя бы одним пробелом слева лексируется как `CHAIN_DOT`;
- `CHAIN_DOT` существует только внутри разбора one-liner блока;
- обычная точка без пробела слева остаётся `.` и относится к внутреннему выражению блока;
- `.?.` никогда не разбивается на `CHAIN_DOT` + `?.`: safe-nav лексируется раньше.

Именно это реализует правило:

```amber
numbers.map: _1.email.downcase().uniq()
# ^ внутренняя точка блока
# ^ продолжение внешней цепочки
```

### 15.2. Приоритеты выражений v1

Нормативный порядок приоритетов для parser core:

1. postfix: member access, member send, call, safe-nav, indexing, safe-indexing, block suffix;
2. callable reference `&target` и префиксные unary `+`, `-`, `not`;
3. мультипликативные `* / %`;
4. аддитивные `+ -`;
5. сравнения и membership: `== != < <= > >= in`;
6. `and`;
7. `or`;
8. присваивание `=`.

Ассоциативность:

- postfix — слева направо;
- арифметика и сравнения — слева направо;
- `not` — префиксный;
- `and` / `or` — слева направо, short-circuit;
- `=` — справа налево.

### 15.3. Reference grammar для выражений

Ниже — рабочая reference grammar. Для Amber v1 она является **нормативной**, но для postfix/bare-call допускает небольшой parser note, потому что часть синтаксиса контекстна и удобнее задаётся не чистой CFG, а Pratt-правилами.

```ebnf
Expr::= AssignExpr

AssignExpr::= Assignable "=" AssignExpr
 | OrExpr

OrExpr::= AndExpr { "or" AndExpr }

AndExpr::= NotExpr { "and" NotExpr }

NotExpr::= "not" NotExpr
 | CompareExpr

CompareExpr::= AddExpr { CompareOp AddExpr }

CompareOp::= "==" | "!=" | "<" | "<=" | ">" | ">=" | "in"

AddExpr::= MulExpr { ("+" | "-") MulExpr }

MulExpr::= PrefixExpr { ("*" | "/" | "%") PrefixExpr }

PrefixExpr::= ("+" | "-") PrefixExpr
 | CallableRefExpr
 | PostfixExpr

CallableRefExpr::= "&" CallableRefTarget

CallableRefTarget::= RefPath
 | RefPath "." MethodName
 | RefPath "#" MethodName

RefPath::= Name { "." Name }

PostfixExpr::= PrimaryExpr { PostfixSuffix }

PrimaryExpr::= Literal
 | Name
 | "@" Name
 | "@@" Name
 | "(" Expr ")"
 | ListLiteral
 | SetLiteral
 | MapLiteral
 | IfExpr
 | UnlessExpr
 | CaseExpr
 | WhileExpr
 | UntilExpr
 | DoWhileExpr
 | LoopExpr
 | DefExpr
 | ClassExpr
 | MixinExpr

PostfixSuffix::= CallSuffix
 | IndexSuffix
 | MemberSuffix
 | SafeCallSuffix
 | SafeIndexSuffix
 | SafeMemberSuffix

CallSuffix::= ParenArgs BlockSuffix?
 | BareArgs BlockSuffix? (* parser note: only if callee can accept bare args *)

IndexSuffix::= "[" ExprList? "]"

MemberSuffix::= "." MethodName MemberTail?

MemberTail::= ParenArgs BlockSuffix?
 | BareArgs BlockSuffix?
 | BlockSuffix
 | ε

SafeCallSuffix::= ".?.(" ArgList? ")" BlockSuffix?

SafeIndexSuffix::= ".?.[" Expr "]"

SafeMemberSuffix::= ".?." MethodName SafeMemberTail?

SafeMemberTail::= ParenArgs BlockSuffix?
 | BareArgs BlockSuffix?
 | BlockSuffix
 | ε

ParenArgs::= "(" ArgList? ")"

BareArgs::= BareArg { "," BareArg } [ "," ]
BareArg::= Expr | KeywordArg

ArgList::= Arg { "," Arg } [ "," ]

Arg::= Expr
 | KeywordArg

KeywordArg::= Name ":" Expr

ExprList::= Expr { "," Expr } [ "," ]

BlockSuffix::= "|" PatternList? "|" ":" BlockBody
 | ":" BlockBody

BlockBody::= INDENT Statement+ DEDENT
 | OneLineBlockBody

PatternList::= Pattern { "," Pattern } [ "," ]
```

`MethodName` в этой грамматике означает обычный идентификатор метода, включая суффиксы `?` и `!`. `CallableRefTarget` является restricted syntactic target: он не допускает call-tail, index-tail, parenthesized expression или ordinary receiver expression. `PrimaryExpr` перечислен укрупнённо: точная grammar литералов и statement-like expressions задаётся соответствующими разделами Части I. Для v1 parser core `UnlessExpr` обязателен как отдельная surface-form, а `DefExpr` / `ClassExpr` / `MixinExpr` обозначают expression-position тех же декларативных syntactic families; AST не имеет права терять этот факт и понижать их в ordinary call/control-flow узлы уже на parser-уровне.

### 15.4. Parser note: where bare args are legal

`BareArgs` не вводятся как полностью свободная CFG-конструкция. Для v1 действует практическое правило:

- bare-call разрешён только если текущий callee syntactically является
 - простым именем функции;
 - member send (`obj.method`);
 - safe member send (`obj.?.method`);
- bare-call не открывается сразу после `]`, `)` или уже завершённого `BlockSuffix`;
- bare-call не может пересекать `NEWLINE` на глубине скобок `0`;
- если после `obj.field` нет call-tail и нет block suffix, это field access, а не вызов.

Это даёт ожидаемое поведение:

```amber
puts x
task.sleep 5.0
numbers.reduce 0: _1 + _2
obj.field
obj.method(arg)
```

### 15.5. Parser note: one-liner block boundary

`OneLineBlockBody` задаётся не чистой EBNF, а правилом разбора:

1. парсер входит в режим `parse_inline_block_body`;
2. выражение блока разбирается обычным Pratt-parser'ом;
3. на глубине скобок `0` токен `CHAIN_DOT` завершает внутреннее выражение блока;
4. после этого `CHAIN_DOT` возвращается во внешний postfix-loop как продолжение внешней цепочки;
5. `NEWLINE` на глубине `0` завершает one-liner блок.

Следствие:

```amber
users.map: _1.email.downcase().strip().uniq()
```

парсится как:

```amber
(users.map { _1.email.downcase().strip() }).uniq()
```

### 15.6. Assignable и pattern assignment

В expression grammar `Assignable` — это только lvalue:

```ebnf
Assignable::= Name
 | "@" Name
 | "@@" Name
 | PostfixExpr "[" Expr "]"
 | PostfixExpr "." Name
```

Деструктурирующее присваивание:

```amber
PATTERN = expr
```

является отдельной statement-form, а не частью общего `Assignable "="...`. То есть у языка есть два вида assignment:

1. обычное lvalue-присваивание;
2. pattern assignment.

Это снимает конфликт между parser core и pattern grammar.

### 15.7. Surface grammar параметров v1

Для параметров и return-boundaries здесь используется тот же `TypeTerm`, который уже нормативно зафиксирован в §12.2; отдельной parser-local type grammar в этой части не вводится. Surface grammar сигнатур v1:

```ebnf
ParamListDef::= [ Param { "," Param } [ "," ] ]

Param::= PosParam
 | KwParam

PosParam::= LocalName
 | LocalName "=" Expr
 | AutoName
 | AutoName "=" Expr
 | LocalName "as" TypeTerm
 | LocalName "as" TypeTerm "=" Expr
 | AutoName "as" TypeTerm
 | AutoName "as" TypeTerm "=" Expr

KwParam::= LocalName ":"
 | LocalName ":" Expr
 | AutoName ":"
 | AutoName ":" Expr
 | LocalName "as" TypeTerm ":"
 | LocalName "as" TypeTerm ":" Expr
 | AutoName "as" TypeTerm ":"
 | AutoName "as" TypeTerm ":" Expr

AutoName::= "@" Name
 | "@@" Name

LocalName::= Name
TypeTerm::= <тот же rule-set, что и в §12.2; локальных отклонений нет>
```

Нормативные следствия:

- у `@x` внешнее имя аргумента всё равно `x`;
- `def f(@x, x):...` — compile-time error, потому что локальное имя параметра дублируется;
- rich multi-clause `def` обязаны иметь одну и ту же base signature в AST-эквивалентной форме;
- rest-pos / rest-kw параметры не входят в surface grammar v1; соответствующие enum-values AST считаются зарезервированными для будущего format bump и не могут появляться в `amber.ast.v1`;
- `include`-операнды не могут ссылаться на имена, вводимые тем же syntactic class/mixin body.

### 15.8. Surface grammar package/import/export/mixin/include/extend v1

Эти формы не являются обычными выражениями. `package` / `import` / `export` живут на module/top-level и участвуют в dependency graph. `mixin`, `include` и `extend` живут в declarative body-позициях object model.

```ebnf
ModulePath::= Name { "." Name }

PackageDecl::= "package" ModulePath

ImportDecl::= ImportModuleDecl
 | FromImportDecl

ImportModuleDecl::= "import" ModulePath [ "as" Name ]

FromImportDecl::= "from" ModulePath "import" ImportName { "," ImportName } [ "," ]

ImportName::= Name [ "as" Name ]

ExportStmt::= "export" ExportName { "," ExportName } [ "," ]

ExportName::= Name [ "as" Name ]

MixinDef::= "mixin" Name ":" NEWLINE INDENT MixinBody DEDENT

MixinBody::= { MixinItem }

MixinItem::= DefStmt
 | ClassDef
 | MixinDef
 | IncludeStmt
 | PassStmt

IncludeStmt::= "include" IncludePath { "," IncludePath } [ "," ]

IncludePath::= Name { "." Name }
```

Class-side composition добавляет отдельную parser-level form:

```ebnf
ExtendStmt::= "extend" IncludePath { "," IncludePath } [ "," ]
```

Нормативные parser/binder rules:

- `PackageDecl` допустим только как первая non-empty top-level форма;
- `ImportDecl` допустим только в contiguous import zone сразу после optional `PackageDecl`;
- `ExportStmt` допустим только на top-level;
- `ModulePath` v1 всегда absolute; relative spellings и `*` не являются частью grammar;
- `mixin` допускается на top-level и внутри declarative class/mixin body;
- `include` допустим только внутри declarative class/mixin body;
- `extend` допустим только внутри declarative body `class` и её reopen-форм; в `mixin` body он является compile-time error;
- для пути `M1..M2` `extend` обязателен как parser/binder artifact с placement checks и AST/HIR serialization; его runtime invalidation path сознательно остаётся в более позднем `W9`;
- `import` / `from` / `export` / `mixin` / `include` / `extend` не могут lower'иться в обычные call-expression узлы.

## 16. Binder и вызов функций/методов

### 16.1. Compile-time проверки сигнатуры

До runtime компилятор обязан проверить:

- дубликаты локальных имён параметров после снятия `@` / `@@`;
- дубликаты external keyword-имён;
- self-reference в default (`x = x`);
- ссылку из default на параметр справа по сигнатуре;
- совместимость base signature во всех клаузаx одного multi-clause `def`;
- невозможные комбинации вроде одновременного positional и keyword-binding одного и того же параметра в одной сигнатуре.

Рекомендуемые обязательные warning'и v1:

- в default используется `@x`, при том что в сигнатуре есть auto-assign параметр `@x`; это почти всегда означает путаницу между старым полем и локальным аргументом.

### 16.2. Runtime-модель вызова: фазы

Для обычного `def`:

1. evaluate caller side: receiver и фактические аргументы слева направо;
2. preflight: arity, keywords, неизвестные ключи, дубли;
3. bind explicit args;
4. evaluate defaults слева направо по сигнатуре;
5. typecheck параметров с `as TypeTerm`;
6. commit auto-assign;
7. execute body.

Для multi-clause `def`:

1. evaluate caller side;
2. preflight;
3. bind explicit args;
4. defaults;
5. typecheck;
6. dispatch по `when`-клаузам;
7. commit auto-assign **только после выбора ветки**;
8. execute chosen body.

### 16.3. Контекст вычисления default

Внутри default-выражения доступны:

- `self`;
- `@field` / `@@field` как **старое состояние** на входе в вызов;
- локальные имена параметров, уже вычисленных левее по сигнатуре.

Недоступны:

- параметры правее;
- ещё не вычисленные локальные имена;
- «будущие» значения auto-assign полей.

Нормативные примеры:

```amber
def init(@x, @y = x):
 pass
```

Здесь `y` зависит от локального `x`.

```amber
def update(@timeout = @timeout):
 pass
```

Здесь default читает старое поле `@timeout`, после чего commit-проход кладёт итоговое значение обратно.

### 16.4. Reference pseudocode: ordinary def

```text
bind_call(signature, passed_pos, passed_kw, self):

 validate_shape(signature, passed_pos, passed_kw)

 locals = {}
 pending_auto = []

 # explicit positional
 pos_i = 0
 kw = copy(passed_kw)

 for param in signature.params in order:
 if param.kind == POS:
 if pos_i < len(passed_pos):
 locals[param.local_name] = passed_pos[pos_i]
 pos_i += 1
 else:
 locals[param.local_name] = MISSING

 elif param.kind == KW:
 if param.external_name in kw:
 locals[param.local_name] = kw[param.external_name]
 remove kw[param.external_name]
 else:
 locals[param.local_name] = MISSING

 if param.has_auto_assign:
 pending_auto.append(param)

 if pos_i != len(passed_pos):
 raise ArgumentError("too many positional")
 if kw not empty:
 raise ArgumentError("unknown keyword(s)")

 for param in signature.params in order:
 if locals[param.local_name] is MISSING:
 if param.has_default:
 locals[param.local_name] = eval_default(param.default_expr, self, locals)
 else:
 raise ArgumentError("missing required parameter")

 for param in signature.params in order:
 if param.has_type:
 check_type(locals[param.local_name], param.type_term)

 return (locals, pending_auto)
```

Исполнение обычного `def`:

```text
call_def(defn, passed_pos, passed_kw, self):
 locals, pending_auto = bind_call(defn.signature, passed_pos, passed_kw, self)

 for param in pending_auto in order:
 commit_auto_assign(self, param, locals[param.local_name])

 return exec_body(defn.body, locals, self)
```

### 16.5. Reference pseudocode: multi-clause def

```text
call_multiclause(defn, passed_pos, passed_kw, self):
 locals, pending_auto = bind_call(defn.base_signature, passed_pos, passed_kw, self)

 subject_map = build_args_map(defn.base_signature, locals)
 subject_tuple = build_args_tuple(defn.base_signature, locals)

 chosen = NONE

 for clause in defn.clauses in source order:
 match_ok, clause_bindings = clause_match(clause.pattern, subject_map, subject_tuple, defn.base_signature)
 if not match_ok:
 continue

 clause_env = merge(locals, clause_bindings)

 if clause.has_guard and not truthy(eval_expr(clause.guard, clause_env, self)):
 continue

 chosen = (clause, clause_env)
 break

 if chosen is NONE:
 if defn.else_body exists:
 chosen = (ELSE, locals)
 else:
 raise MatchError()

 for param in pending_auto in order:
 commit_auto_assign(self, param, locals[param.local_name])

 return exec_body(chosen.body, chosen.env, self)
```

### 16.6. Subject selection в multi-clause `def`

Алгоритм `clause_match` использует форму паттерна:

- map pattern `{...}` -> матчим против `ArgsMap`;
- tuple pattern `(... )` -> матчим против `ArgsTuple`;
- иной pattern -> разрешён только если у base signature ровно один positional parameter; тогда матчим значение этого единственного позиционного параметра;
- если positional-параметров больше одного и паттерн не `{...}` / `(... )` -> compile-time error `ambiguous clause subject`.

### 16.7. Semantics of `$_` in call frames

Каждый call frame и каждый fiber frame содержит скрытый слот `last_result` для `$_`.

Правила:

- после каждого expression-statement слот обновляется;
- обычное присваивание кладёт в `$_` присвоенное значение;
- pattern assignment кладёт в `$_` правое значение при успехе;
- `pass` / `noop` кладут `null`;
- неявный return функции/блока возвращает текущее `$_` кадра;
- `$_` не делится между соседними вызовами и не протекает между fiber'ами.

Для реализации это означает, что `$_` не требует особой динамической магии: достаточно одного скрытого слота на frame.


### 16.8. Callable references, `HCall` и class-object call

Callable reference создаётся только из restricted reference target, описанного в §4.6 и §15.3. Binder обязан резолвить target до одного из следующих видов:

- callable binding / module export;
- class-side send-reference `&Class.method`;
- unbound instance send-reference `&Class#method`.

`&target` не исполняет target и не читает результат вызова. Он создаёт immutable callable reference object. Если target статически не существует или очевидно имеет неподходящую форму, это compile-time error. Если target зависит от dynamic/module loader state, runtime использует обычные `TypeError` / `NoMethodError` правила.

Reference call нормализуется как ordinary callable call:

```amber
fn = &Geometry.distance
value = fn(a, b)
```

Lowering:

```text
&Geometry.distance -> HCallableRef(kind = static_callable, target = Geometry.distance)
fn(a, b) -> HCall(fn, [a, b], {}, null)
```

Class-side reference:

```amber
finder = &User.find
user = finder(42)
```

наблюдаемо эквивалентен:

```amber
user = User.find(42)
```

Unbound instance method reference:

```amber
name_fn = &User#full_name
name = name_fn(user)
```

имеет call contract:

```text
call_unbound(owner = User, selector =:full_name, args = [receiver, *rest], block):
 raise TypeError if receiver is missing
 ok = User === receiver
 raise TypeError unless ok is Bool and ok == true
 return SEND receiver,:full_name, rest, block
```

`&Class#method` является unbound **send reference**, а не raw pointer на конкретное машинное тело метода. Он участвует в ordinary dispatch lookup, `method_missing`, method-table versioning, open-world invalidation и frozen-world guard rules так же, как соответствующий send-site.

`HCall` по class object является constructor-call path:

```text
HCall(callee = ClassObject, args, kwargs, block)
 -> HSend(receiver = ClassObject, selector =:new, args, kwargs, block)
```

Реализация может выполнить это как special branch внутри `CALL` или понизить в `SEND:new`, если сохраняется полная наблюдаемая эквивалентность: порядок вычисления callee/args, errors, keyword handling, block forwarding, `method_missing` и world-version checks.

## 17. Pattern runtime v1: reference implementation model

### 17.1. Compile-time предобработка паттерна

До runtime компилятор вычисляет для каждого паттерна:

- множество новых биндингов;
- наличие pin-ссылок;
- необходимость `deconstruct()` / `deconstruct_keys(keys)`;
- флаги `strict_map` для `**null` и `capture_rest` для `**rest`.

Обязательные compile-time ошибки:

- дубликаты имён в одном паттерне;
- разные наборы биндингов в альтернативах OR-pattern;
- `*rest` / `**rest` вне конца соответствующего паттерна;
- `**rest`, `**_`, `**null` более одного раза;
- wildcard `_` в качестве читаемого имени;
- bare matcher expression в `def`-клаузаx, block params и pattern assignment;
- dynamic pattern object в block params и pattern assignment;
- ссылка из `pattern(expr)` на имя, вводимое тем же enclosing pattern.

### 17.2. Runtime contract

Reference runtime использует функцию:

```text
match(pattern, value, env) -> MatchResult
```

где `MatchResult` имеет вид:

```text
MatchResult(success: Bool, bindings: Map<Name, Value>)
```

Контракт:

- при `success = false` `bindings` пусты и не коммитятся наружу;
- `bindings` коммитятся только после полного успеха всего паттерна;
- частичные биндинги в провалившейся альтернативе OR-паттерна отбрасываются.

### 17.3. Reference pseudocode: core dispatcher

```text
match(p, value, env):

 case p.kind of

 WILDCARD:
 return ok({})

 BIND_NAME:
 return ok({ p.name: value })

 LITERAL:
 return ok({}) if value_equals(value, p.literal) else fail()

 PIN:
 return ok({}) if value_equals(value, env[p.name]) else fail()

 AS_PATTERN:
 r = match(p.inner, value, env)
 return fail() unless r.success
 return ok(merge({ p.name: value }, r.bindings))

 OR_PATTERN:
 for alt in p.alternatives:
 r = match(alt, value, env)
 if r.success:
 return r
 return fail()

 TUPLE_PATTERN:
 seq = coerce_sequence_for_tuple_match(value)
 return fail() unless seq.available
 return match_tuple_items(p.items, seq.value, env)

 LIST_PATTERN:
 seq = coerce_sequence_for_list_match(value)
 return fail() unless seq.available
 return match_list_items(p.items, seq.value, env)

 MAP_PATTERN:
 mp = coerce_map_for_map_match(value, p.requested_keys, p.needs_full_map)
 return fail() unless mp.available
 return match_map_items(p, mp.value, env)

 DYNAMIC_PATTERN:
 matcher = eval_dynamic_matcher(p.matcher_expr, env)
 raise TypeError unless responds_to(matcher, match)

 dm = matcher.match(value)
 raise TypeError unless is_dynamic_match_result(dm)
 raise TypeError unless is_bool(dm.success)
 raise TypeError unless is_map(dm.bindings)

 if not dm.success:
 raise TypeError unless empty_map(dm.bindings)
 return fail()

 if p.export_map_pattern is null:
 raise TypeError unless empty_map(dm.bindings)
 return ok({})

 return match(p.export_map_pattern, dm.bindings, env)

 CONST_PATTERN:
 b = call_triple_eq(p.const, value)
 raise TypeError unless is_bool(b)
 return ok({}) if b else fail()

 TYPED_DESTRUCTURE:
 b = call_triple_eq(p.head, value)
 raise TypeError unless is_bool(b)
 return fail() unless b

 if p.mode == POSITIONAL:
 seq = call_deconstruct(value)
 return fail() if seq is null
 raise TypeError unless is_sequence(seq)
 return match_tuple_items(p.items, seq, env)

 if p.mode == KEYS:
 keys_arg = null if p.needs_full_map else p.requested_keys
 mp = call_deconstruct_keys(value, keys_arg)
 return fail() if mp is null
 raise TypeError unless is_map(mp)
 return match_map_items(p.key_items, mp, env)
```

### 17.4. Tuple/list matching

```text
match_tuple_items(pattern_items, seq, env):

 if no rest-pattern:
 return fail() unless len(seq) == len(pattern_items)

 if rest-pattern exists:
 return fail() unless len(seq) >= fixed_prefix_len

 acc = {}

 for each fixed item i in order:
 r = match(pattern_items[i], seq[i], env_plus(acc))
 return fail() unless r.success
 acc = merge(acc, r.bindings)

 if rest-pattern exists:
 rest_value = slice(seq, fixed_prefix_len, end)
 if rest binder is not "_":
 acc[rest_name] = rest_value

 return ok(acc)
```

Для tuple и list семантика одинаковая; различается только способ начального `coerce_*`.

### 17.5. Map matching

```text
match_map_items(map_pattern, mp, env):

 acc = {}

 for each declared key k -> subpattern in source order:
 return fail() unless has_key(mp, k)

 r = match(subpattern, mp[k], env_plus(acc))
 return fail() unless r.success
 acc = merge(acc, r.bindings)

 extra_keys = keys(mp) - declared_keys(map_pattern)

 if map_pattern.rest_kind == STRICT_NULL:
 return fail() unless extra_keys is empty

 if map_pattern.rest_kind == CAPTURE:
 acc[map_pattern.rest_name] = project_map(mp, extra_keys)

 return ok(acc)
```

Без `**...` лишние ключи разрешены.

### 17.6. Coercion rules

Нормативные helper-правила:

```text
coerce_sequence_for_tuple_match(value):
 if value is native Tuple/List/Array: return available(value)
 if responds_to(value, deconstruct):
 seq = value.deconstruct()
 if seq is null: return unavailable
 raise TypeError unless is_sequence(seq)
 return available(seq)
 return unavailable

coerce_sequence_for_list_match(value):
 if value is native List/Array: return available(value)
 if responds_to(value, deconstruct):
 seq = value.deconstruct()
 if seq is null: return unavailable
 raise TypeError unless is_sequence(seq)
 return available(seq)
 return unavailable

coerce_map_for_map_match(value, requested_keys, needs_full_map):
 if value is native Map/Hash: return available(value)
 if responds_to(value, deconstruct_keys):
 keys_arg = null if needs_full_map else requested_keys
 mp = value.deconstruct_keys(keys_arg)
 if mp is null: return unavailable
 raise TypeError unless is_map(mp)
 return available(mp)
 return unavailable
```

### 17.7. Поведение по контекстам

#### `case`

- no match + `else` -> выполнить `else`;
- no match без `else` -> `null`.

#### `case!`

- no match + `else` -> выполнить `else`;
- no match без `else` -> `MatchError`.

#### Pattern assignment

- no match -> `MatchError`;
- success -> коммит биндингов в текущий scope, `$_ = rhs_value`.

#### Block params

- no match -> `MatchError`.

#### Multi-clause `def`

- no match ни одной `when` и нет `else` -> `MatchError`;
- auto-assign до выбора ветки не коммитится.

### 17.8. Dynamic pattern objects (explicit-binding profile)

Surface form:

```amber
pattern(expr)
pattern(expr) with {id:, meta: {role::admin}, **null}
```

Evaluation order:

1. `expr` вычисляется в окружающем лексическом окружении до коммита любых новых биндингов текущего enclosing pattern.
2. `expr` вычисляется каждый раз, когда соответствующая clause реально проверяется; он не обязан быть compile-time constant.
3. Runtime вызывает `matcher.match(value)` на результате `expr`.
4. Возвращаемое значение обязано удовлетворять контракту `DynamicMatchResult`.
5. При `success = false` dynamic pattern проваливается и не даёт биндингов.
6. При `success = true`:
 - если `with` отсутствует, `bindings` обязаны быть пустыми, иначе `TypeError`;
 - если `with MAP_PATTERN` присутствует, returned `bindings` матчится против `MAP_PATTERN` по обычным правилам map-pattern matching.
7. Наружу коммитятся только биндинги, произведённые `MAP_PATTERN`.

Protocol contract:

```text
matcher.match(value) -> DynamicMatchResult

DynamicMatchResult(
 success: Bool,
 bindings: Map
)
```

Protocol errors:

- matcher object не поддерживает `match(value)`;
- returned value не является `DynamicMatchResult`;
- `success` не является `Bool`;
- `bindings` не является `Map`;
- `success = false` и `bindings` не пуст;
- `success = true`, `with` отсутствует, но `bindings` не пуст.

Все protocol violations обязаны бросать `TypeError`.

Dynamic pattern objects разрешены в `case`, `case!` и clause-style `def`.
Dynamic pattern objects запрещены в block params и pattern assignment.
`pattern` является contextual keyword только в pattern-position.

### 17.9. Matcher expressions только в `case` / `case!`

В `case` и `case!` допускается fallback-форма:

```amber
case x:
 when 1..10::small
 when String::str
```

Если `when...` не разбирается как `Pattern`, запись трактуется как `MatcherExpr`, и runtime делает `MatcherExpr === value`.

Эта форма отличается от dynamic pattern objects из §17.8 тем, что:

- не экспортирует bindings;
- не использует `with`;
- остаётся pure fallback, если `when...` не разбирается как `Pattern`.

Во всех остальных pattern-контекстах bare matcher expressions запрещены.

## 18. Conformance suite v1

### 18.1. Минимальная структура набора тестов

Реализация Amber v1 должна сопровождаться как минимум такими каталогами тестов:

```text
tests/
 parser/
 expressions/
 postfix/
 block_suffix/
 safe_nav/
 case/
 binder/
 defaults/
 auto_assign/
 multiclause/
 diagnostics/
 runtime/
 patterns/
 case/
 mixins/
 block_params/
 last_result/
 golden/
 ast/
 hir/
```

### 18.2. Обязательные группы позитивных тестов

Минимум:

- postfix chaining с несколькими block suffix;
- `.?.` для method/field/index/call;
- `CHAIN_DOT` boundary и вложенный чейнинг внутри one-liner блока;
- callable references `&NameSpace.some_fn`, `&Class.method`, `&Class#method` и вызов через `fn(args...)`;
- constructor-call sugar `Class(args...)` и dynamic factory `klass(args...)`;
- ordinary def: positional + keyword + defaults + auto-assign;
- static module syntax: `package`, `import`, `from... import...`, `export`, explicit re-export;
- multi-clause `def`: map subject / tuple subject / single-arg subject;
- `case` / `case!` со структурными паттернами и bare matcher expressions;
- dynamic pattern objects: `pattern(expr)` и `pattern(expr) with {...}`;
- pin / as-pattern / OR-pattern / `**null` / `**rest`;
- named `mixin` и `include` с linearized lookup order;
- open class и open mixin через повторные `class Name:...` / `mixin Name:...`-формы;
- `define_method(Target,:name) |...|:...`, где `Target` может быть классом или mixin'ом;
- reflective `send(receiver, selector,...)` с literal и dynamic selector;
- `method_missing` fallback;
- freeze transition и запрет world mutation после неё;
- `$_` в обычной функции, блоке и fiber frame.
- `Kernel.watch(x)` в notebook profile: binding dependency, ivar dependency, field-level invalidation и отсутствие `world_epoch` bump;

### 18.3. Обязательные негативные тесты

Минимум:

- `map(_1 * 2)` как invalid v1;
- `_1` вне implicit-block;
- `fn.()` как invalid callable-call spelling;
- `&foo()`, `&(foo + bar)`, `&obj.method` как invalid callable reference targets в v1;
- `&User#missing` без resolvable instance-side method target, если это статически очевидно;
- вызов `&User#method` без receiver или с receiver, не удовлетворяющим `User === receiver`, -> `TypeError`;
- дубликаты имён в паттерне;
- разные наборы биндингов в OR-pattern;
- `**rest` вне конца map-pattern;
- ambiguous clause subject;
- duplicate `package`;
- `import` после первого non-import top-level item;
- `from x import *`;
- relative import spelling;
- экспорт неизвестного имени или duplicate public export;
- присваивание импортированному alias;
- ссылка в default на параметр справа;
- использование `@x` в ожидании «нового значения» должно хотя бы предупреждаться;
- `===` / `deconstruct*` / dynamic matcher protocol с неправильным типом возвращаемого значения -> `TypeError`;
- `case!` без совпадения и без `else` -> `MatchError`;
- dynamic pattern object в block params;
- dynamic pattern object в pattern assignment;
- non-empty `bindings` без `with`;
- ссылка из `pattern(expr)` на имя, вводимое тем же enclosing pattern;
- `include` вне class/mixin body;
- `include` non-mixin target;
- `include` cycle;
- `class_method def` внутри mixin body;
- `define_method` с target не типа class/mixin;
- `define_method` с selector не типа `Symbol`/`Str`;
- конфликт `block suffix` и явного callable-аргумента в `define_method`;
- reopen mixin через binding другого типа;
- reopen с несовместимым superclass;
- world mutation после freeze barrier;
- поздняя загрузка Amber-модуля в frozen dispatch-world.
- `Kernel.watch(foo())`, `Kernel.watch(user.name)`, `Kernel.watch(xs[0])` и `Kernel.watch(1 + 2)` в notebook profile -> compile-time diagnostic или runtime `WatchTargetError`;
- watched-object write, который падает на lifetime/ownership/write-barrier check, не публикует watch event.

### 18.4. Golden representation policy

Для стабильной реализации рекомендуются три слоя golden-представлений:

1. **AST golden** — проверяет чисто синтаксический разбор;
2. **HIR golden** — проверяет нормализацию:
 - simple many-def sugar -> clause-style `def`;
 - safe-nav -> explicit null-guard nodes;
 - declarative `include` -> explicit `HInclude` body item;
 - implicit block -> explicit block-arity node;
3. **runtime golden** — проверяет финальные значения/ошибки.


### 18.5. Нормативная роль AST

Для Amber фиксируются **два разных frontend-слоя**:

1. **AST** — максимально syntax-faithful представление;
2. **HIR** — нормализованное исполнимое представление, на которое уже можно опирать VM / bytecode compiler.

Инвариант:

- parser обязан строить AST без скрытого lowering'а control-flow и dispatch semantics;
- HIR обязан убрать surface-sugar и сделать явными все runtime-critical шаги.

AST нужен для:

- точных диагностик и source spans;
- IDE / formatter / refactoring;
- golden-test'ов синтаксического уровня.

HIR нужен для:

- интерпретатора;
- bytecode compiler;
- optimizer / AOT pipeline в будущем.

### 18.6. Нормативный минимальный AST

AST v1 должен содержать по меньшей мере следующие семейства узлов.

#### Модульный уровень

```text
AstModule(items[])
AstPackageDecl(module_path)
AstImportStmt(kind, module_path, alias?, names[])
AstImportName(source_name, local_name)
AstExportStmt(items[])
AstExportItem(local_name, public_name)
AstClassDef(name, superclass?, body[])
AstMixinDef(name, body[])
AstDefStmt(name, signature, body)
AstClassMethodDef(name, signature, body)
AstClauseDef(name, base_signature, clauses[], else_body?)
AstIncludeStmt(paths[])
AstExtendStmt(paths[])
AstExprStmt(expr)
```

#### Сигнатуры

```text
AstSignature(params[])
AstParam(
 kind, # positional / keyword
 external_name?,
 local_name,
 default_expr?,
 type_expr?,
 auto_assign_kind? # none / @ / @@
)
AstClause(pattern, guard_expr?, body)
```

`AstClassDef`, `AstMixinDef` и `AstDefStmt` допускаются и в expression-position; serializer обязан сохранять placement metadata (`item` / `expr`) либо эквивалентную наблюдаемую информацию, не понижая эти формы в ordinary call-expression узлы.

#### Выражения и control-flow

```text
AstLiteral(value)
AstName(name)
AstIvar(name)
AstCvar(name)
AstConst(path)
AstCallableRef(target_kind, path, selector?)
AstInterpString(parts[])
AstUnary(op, expr)
AstBinary(op, left, right)
AstAssign(target, value)
AstPatternAssign(pattern, value)
AstIf(cond, then_body, else_body?)
AstUnless(cond, then_body, else_body?)
AstWhile(cond, body)
AstUntil(cond, body)
AstDoWhile(body, cond)
AstLoop(body)
AstBreak(value?)
AstCase(scrutinee, arms[], else_body?, strict = false)
AstBlock(params?, body, implicit_placeholders?)
AstPostfixChain(base, tails[])
```

#### Postfix-tail узлы

AST обязан сохранять surface-shape postfix-цепочки, а не разбирать её сразу в обычные call-узлы:

```text
AstTailDotMember(name)
AstTailSafeMember(name)
AstTailCall(args, kw_args, block? = null)
AstTailSafeCall(args, kw_args, block? = null)
AstTailIndex(index_expr)
AstTailSafeIndex(index_expr)
AstTailBlockSuffix(block)
```

Это требуется для корректного lowering'а:

- `.?.`
- block suffix
- `CHAIN_DOT` boundary
- bare-call / method-call distinction.

#### Паттерны

```text
AstPatWildcard
AstPatBind(name)
AstPatPin(name)
AstPatLiteral(value)
AstPatConst(path)
AstPatTuple(items[])
AstPatList(items[], rest?)
AstPatMap(fields[], rest_mode)
AstPatHead(head_expr, pos_items[], kw_fields[])
AstPatAs(bind_name, inner)
AstPatOr(alternatives[])
AstPatDynamic(matcher_expr, export_map_pattern? = null)
```

Где `rest_mode` для map-pattern обязан различать:

- `extra_ok` (по умолчанию)
- `bind_rest(name)`
- `ignore_rest`
- `strict_null`

`AstCase` обязан сохранять, использовал ли исходный код `case` или `case!`.
`AstPatDynamic` обязан сохранять исходный `matcher_expr` и optional `export_map_pattern` syntax-faithfully; parser не имеет права понижать его в обычный call-node на уровне AST.

### 18.7. Что AST сознательно НЕ делает

AST не имеет права:

- опускать `AstTailBlockSuffix` как будто это обычный positional-аргумент;
- понижать `$_` в обычное имя;
- понижать `&target` в ordinary closure/call без сохранения callable-reference target metadata;
- понижать `package` / `import` / `export` / `include` / `mixin` в обычные call-узлы;
- группировать unrelated `def` без стадии нормализации;
- превращать `safe-nav` в `if` на уровне parser;
- выполнять pattern lowering.

Parser должен быть максимально честным к surface syntax.

### 18.8. Нормативный HIR

HIR — это уже исполнимый semantic core. Для v1 он обязан нормализовать язык как минимум до следующих семейств узлов.

#### Модуль / классы / методы

```text
HModule(module_name?, imports[], exports[], items[])
HImportModule(module_id, local_name)
HImportNames(module_id, names[])
HExport(local_name, public_name)
HClass(name, superclass?, body[])
HMixin(name, body[])
HInclude(paths[])
HExtend(paths[])
HMethod(
 name,
 dispatch_side, # instance / class
 signature,
 clauses[],
 else_body?,
 auto_assign[]
)
HClosure(params[], captures[], body)
```

#### Базовые операции

```text
HConst(value)
HLoadLocal(slot)
HStoreLocal(slot, expr)
HLoadIvar(name)
HStoreIvar(name, expr)
HLoadCvar(name)
HStoreCvar(name, expr)
HLoadConst(path)
HCallableRef(kind, target?, receiver?, selector?)
HUnboundMethodRef(owner_class, selector)
HLastGet
HLastSet(expr)
```

#### Control-flow

```text
HSeq(items[])
HIf(cond, then_body, else_body)
HLoop(kind, cond?, body)
HBreak(value?)
HTry(body, handlers[], ensure?)
```

#### Вызовы и postfix

```text
HSend(receiver, selector, pos_args[], kw_args[], block?)
HSendDyn(receiver, selector_expr, pos_args[], kw_args[], block?)
HCall(callable, pos_args[], kw_args[], block?)
HIndex(receiver, index_expr)
HSafeSend(receiver, selector, pos_args[], kw_args[], block?)
HSafeCall(callable, pos_args[], kw_args[], block?)
HSafeIndex(receiver, index_expr)
```

#### Matching и dispatch

```text
HMatchDispatch(scrutinee, arms[], else_body?, fail_mode)
HPatternAssign(scrutinee, pattern, fail_mode)
HCompiledPattern(pattern_ir)
HClause(subject_kind, pattern, guard?, body)
```

Где `subject_kind` принимает значения:

- `single_positional`
- `positional_tuple`
- `named_args_map`

Для обычного `case` без `else` `fail_mode = null`; для `case!` без `else` `fail_mode = MatchError`.
`pattern_ir` обязан допускать узел `PatDynamic(matcher_expr, export_map_pattern?)`.

#### Async / strands / no-GIL runtime intrinsics

```text
HSpawnSameStrand(block)
HSpawnNewStrand(block)
HSleep(expr)
HYield
HResume(handle_expr)
HWait(handle_expr, timeout_expr?)
HCancel(handle_expr)
```

#### Notebook watch intrinsics

```text
HWatchBinding(binding_ref, options)
HWatchIvar(receiver_ref, ivar_name, options)
HWatchCvar(owner_ref, cvar_name, options)
HUnwatch(handle_expr)
HWatchRevision(target_ref)
HBeginDependencyCapture(cell_id_expr)
HEndDependencyCapture
```

Эти HIR-узлы допустимы только в Amber/Notebook Watch Profile или в host-instrumented build. Ordinary production lowering может отклонить их как unsupported profile feature до bytecode emission.


### 18.9. Нормативная форма HIR для `def`

HIR видит метод уже не как «синтаксический def», а как единый callable object с тремя стадиями.

#### Стадия 1. bind

- preflight shape-check;
- defaults left-to-right;
- type hooks;
- формирование locals;
- **без** auto-assign commit.

#### Стадия 2. dispatch

 - map-subject
 - tuple-subject
 - single-positional subject;
- клаузы проверяются сверху вниз;
- первая clause с successful pattern match и truthy guard побеждает.

#### Стадия 3. commit + body

- только после выбора clause исполняется `auto_assign[]`;
- затем исполняется body победившей clause;
- если clause не найдена — `else_body` либо `MatchError`.

Именно эта трёхфазная HIR-модель сохраняет раннее требование атомарности `@x = x`: авто-assign не должен происходить, если ни одна клауза не подошла.

### 18.10. Lowering rules AST -> HIR

#### 0. `package` / `import` / `export`

Module directives никогда не lower'ятся как обычные expressions.

Нормативно:

- `AstPackageDecl` заполняет `HModule.module_name`;
- contiguous `AstImportStmt*` нормализуются в `HImportModule` / `HImportNames` и формируют статический dependency set модуля;
- `AstExportStmt*` нормализуются в список `HExport`;
- binder обязан до expression-lowering проверить top-level-only placement, alias-collisions, duplicate public exports и read-only статус импортированных имён.

#### 0a. `mixin` / `include` / `extend` / `class_method def`

`AstMixinDef`, `AstIncludeStmt`, `AstExtendStmt` и `AstClassMethodDef` никогда не lower'ятся в обычные expressions.

Нормативно:

- `AstMixinDef` нормализуется в `HMixin(name, body[])`;
- `AstIncludeStmt` допустим только внутри `AstClassDef` / `AstMixinDef` и lower'ится в `HInclude(paths[])`;
- `AstExtendStmt` допустим только внутри `AstClassDef` и lower'ится в `HExtend(paths[])`; parser/binder path обязан стабилизировать его уже к `M1`, даже если runtime invalidation реализуется позже в `W9`;
- `AstClassMethodDef` нормализуется в `HMethod(dispatch_side = class,...)`; ordinary `AstDefStmt` внутри class/mixin body lower'ится в `HMethod(dispatch_side = instance,...)`;
- binder обязан до expression-lowering проверить placement `include` / `extend`, запрет `class_method def` внутри mixin body, явные type-mismatch'и include/extend-target'ов и циклы, которые можно доказать статически;
- object-body lowering обязан сохранять source order direct includes/extends, потому что later include/extend доминирует в linearization на соответствующей dispatch-side.

#### 0b. `unless` и `do... while`

- `AstUnless` lower'ится в `HIf` с инвертированным условием, но AST обязан сохранять исходную surface-form отдельно от `AstIf`;
- `AstDoWhile` lower'ится в `HLoop(kind = do_while,...)` и не может быть потерян как ordinary `while` уже на AST-уровне.

#### 1. `$_`

```text
AST: $_
HIR: HLastGet
```

Каждое expression-statement, которое наблюдаемо по правилам языка, завершает шагом `HLastSet(result)`.

#### 2. Block suffix

```amber
numbers.map: _1 * 2
```

AST:

```text
AstPostfixChain(
 AstName("numbers"),
 [AstTailDotMember("map"), AstTailBlockSuffix(AstBlock(implicit_placeholders=1,...))]
)
```

HIR:

```text
HSend(
 receiver = HLoadLocal(numbers),
 selector = "map",
 pos_args = [],
 kw_args = [],
 block = HClosure(params=[p1], body=...)
)
```

`map(_1 * 2)` не lower'ится, потому что в v1 это невалидная surface-form.

#### 2a. Reflective `send`

Когда expression `send(recv, selector,...)` резолвится именно к builtin prelude-binding `send`, lowering обязан идти так:

- если `selector` — compile-time literal `Symbol`/`Str`, форма может быть понижена в обычный `HSend`;
- иначе форма обязана понижаться в `HSendDyn(receiver, selector_expr,...)`.

Shadowing имени `send` отключает этот lowering и возвращает обычный `HCall`.

#### 2b. Callable references и constructor-call sugar

Callable reference lowering обязан сохранять различие между static callable binding, class-side bound send-reference и unbound instance send-reference:

```text
&Geometry.distance -> HCallableRef(kind = static_callable, target = Geometry.distance)
&User.find -> HCallableRef(kind = bound_send, receiver = HLoadConst(User), selector =:find)
&User#full_name -> HUnboundMethodRef(owner_class = HLoadConst(User), selector =:full_name)
```

Call-site после этого остаётся обычным `HCall`:

```text
fn(args...) -> HCall(fn, args, kwargs, block?)
```

Constructor-call sugar не становится отдельным AST/HIR control-flow node. `Point(1, 2)` и `factory(1, 2)` — это обычный `HCall` по callee. Runtime/bytecode `CALL` обязан распознать class object и выполнить constructor path через `:new`. Compiler может понизить статически доказанный `Point(...)` в `HSend(Point,:new,...)`, но только если это не меняет observable semantics.

#### 3. Safe-nav

```amber
user.?.address.?.city
```

HIR-понижение обязано быть наблюдаемо эквивалентным следующему шаблону:

```text
t0 = <user>
if t0 == null:
 null
else:
 t1 = send(t0, "address")
 if t1 == null:
 null
 else:
 send(t1, "city")
```

То есть `HSafeSend/HSafeCall/HSafeIndex` — это допустимые HIR-узлы только как short-hand. На более низком IR они обязаны раскладываться в явные null-guards.

#### 4. Clause-style `def`

```amber
def area(shape):
 when Point(x, y): x * y
 when Rect(w:, h:): w * h
 else: 0
```

HIR:

```text
HMethod(
 name = "area",
 signature = [shape],
 clauses = [
 HClause(single_positional, PatHead(Point, [x, y]), null, body1),
 HClause(single_positional, PatHead(Rect, kw=[w, h]), null, body2),
 ],
 else_body = body3,
 auto_assign = []
)
```

#### 5. Many-def sugar

Несколько простых `def` с одним именем и совместимой base-signature обязаны нормализоваться к одному `HMethod`.

#### 6. `case` / `case!`

`case` и `case!` lower'ятся в `HMatchDispatch`, который использует тот же pattern-engine, что и `HMethod.clauses`. Различается только `fail_mode`: `null` для обычного `case` без `else` и `MatchError` для `case!` без `else`.

#### 7. `task.async` / `task.spawn`

Если lowering распознаёт intrinsic runtime selector на текущем task-context:

- `task.async {... }` -> `HSpawnSameStrand(...)`
- `task.spawn {... }` -> `HSpawnNewStrand(...)`
- `handle.resume()` -> `HResume(handle)`
- `handle.wait(...)` -> `HWait(...)`

Это не меняет surface syntax: на AST это всё ещё обычные postfix/send-конструкции.

#### 8. `Kernel.watch(...)`

Если lowering распознаёт builtin notebook `Kernel.watch` и первый аргумент является syntactic watch-target, форма понижается в dedicated watch intrinsic:

```text
Kernel.watch(x) -> HWatchBinding(binding_ref(x), options)
Kernel.watch(@x) -> HWatchIvar(self, "x", options)
Kernel.watch(@@x) -> HWatchCvar(current_owner, "x", options)
```

Если `Kernel` или `watch` затенены пользовательским binding'ом, форма остаётся ordinary `HSend/HCall`. Если target не является допустимым watch-target, frontend обязан выдать compile-time diagnostic в notebook profile; dynamic host path может бросить `WatchTargetError`.

`HWatchBinding` и родственные узлы не являются world mutation и не должны вставлять dispatch invalidation. Они могут обновлять `watch_epoch`, watch side-tables и notebook dependency metadata.


### 18.11. Компилируемая layout-модель frame / closure / task

Чтобы HIR реально работал как база для VM, фиксируется следующий минимальный ABI-контур.

#### Frame

Каждый call frame содержит:

- ссылку на code object;
- массив local slots;
- upvalue/capture table;
- `last_result` slot для `$_`;
- exception handler stack;
- ссылку на current task.

#### Closure

Каждый closure object содержит:

- code object;
- captured environment;
- metadata по arity / implicit placeholder lowering.

#### Task

Каждая task содержит:

- `task_id`
- `strand_id`
- state
- call-stack / current frame
- result or failure
- cancellation flag
- wake_pending flag
- structured children set.

#### Strand

Каждый strand содержит:

- run queue
- sleeping timers index
- waiting handles index
- owner worker pointer (не эксклюзивный навсегда; strand может мигрировать)
- mailbox / wake signal state.

Этот ABI-контур уже достаточен, чтобы строить:

- AST interpreter;
- HIR interpreter;
- bytecode VM;
- в дальнейшем MIR/SSA без смены surface semantics.

### 18.12. Практический порядок реализации AST/HIR

Теперь рекомендуемый план уже не общий, а технически конкретный:

1. parser строит **syntax-faithful AST**;
2. frontend pass делает:
 - name resolution metadata;
 - placeholder arity inference;
 - def-group normalization;
3. lowering AST -> HIR делает явными:
 - block suffix
 - safe-nav
 - `$_`
 - clause dispatch
 - async intrinsics;
4. pattern compiler превращает `AstPattern`/`HCompiledPattern` в decision program;
5. только потом HIR идёт либо в bytecode compiler, либо во временный tree-walk interpreter.

Именно это теперь считается рекомендуемой архитектурой Amber frontend.


---

# Приложение B. Перенесённые разделы IV-XV из основного файла

Ниже сохранены перенесённые разделы о матрице дальнейшей разработки, компилируемости, VM/runtime ABI, lifetime/collector/loader/MOP/frozen-world, reference implementation blueprint, backlog, milestone gating и tracking issues. Новая матрица в верхней части этого файла является редакторски доработанным входом для практической реализации; эти перенесённые разделы остаются source anchors и детальными нормативными следами.

# Часть IV. Матрица дальнейшей разработки

## 1. Принцип приоритизации

После текущей редакции порядок разработки уточняется так:

1. parser -> syntax-faithful AST;
2. AST normalization -> HIR lowering;
3. pattern compiler + binder + dispatch;
4. bytecode compiler + code objects + verifier;
5. register VM + object/frame/closure ABI + inline caches;
6. allocator/lifetime runtime: tombstones, `destroy!`, `memory.dealloc`, shape transitions;
7. collector + pinning + opaque FFI boundary;
8. no-GIL scheduler: worker pool + strands + task runtime;
9. compiled-module loader / linker / `.amberbc` distribution profile;
10. conformance suite;
11. только после этого — реализовывать Amber/Typed checker, distribution/registry, reflection mirrors, advanced concurrency и native/frozen profiles.

## 2. Матрица

### P0 — frontend и semantic core

| Трек | Состояние | Ближайший шаг | Зависимости | Критерий выхода |
|---|---|---|---|---|
| G1. Parser core / expressions | Закрыто на уровне спецификации | Реализовать lexer + Pratt parser с `CHAIN_DOT`, `.?.`, block suffix и bare-call rules | Нет | Все примеры из grammar-раздела дают стабильный syntax-faithful AST |
| G2. AST schema | Закрыто на уровне архитектуры | Собрать node set, source spans и AST golden corpus | G1 | Parser выдаёт один и тот же AST на одинаковом surface syntax |
| G3. Binder / signatures / defaults | Закрыто на уровне спецификации | Реализовать `bind_call`, preflight, default-eval, typecheck hooks и delayed auto-assign commit | G1, G2 | Ordinary def и multi-clause def воспроизводимы по golden tests |
| G4. Pattern runtime v1 | Закрыто на уровне спецификации | Реализовать `match()` + протокол `===` / `deconstruct*` + context-specific commit semantics | G1, G2, G3 | Все примеры pattern matching из спеки исполняются без расхождений |
| G5. HIR и lowering | Закрыто на уровне архитектуры | Зафиксировать и реализовать AST->HIR lowering для safe-nav, implicit block, many-def sugar, `$_`, `mixin`/`include` и async intrinsics | G1–G4 | Есть стабильный HIR для интерпретатора и байткодного компилятора |

### P1 — исполнимая VM

| Трек | Состояние | Ближайший шаг | Зависимости | Критерий выхода |
|---|---|---|---|---|
| G6. Bytecode VM core | Закрыто на уровне архитектуры, не реализовано | Реализовать `BcModule/BcMethod/BcCode`, register VM loop, pattern opcodes, exceptions и call/ivar caches | G3–G5 | Язык выполняется не через AST-walk, а через VM с фиксированным ISA |
| G6b. Heap ownership / lifetime / collector ABI | Reference-profile закрыт, не реализован | Реализовать non-moving generational collector, object headers, tombstone states, `destroy!`, `memory.dealloc`, root scanning, write barriers, remembered sets и safe-point protocol | G6, G7 | Illegal cross-strand access и use-after-free ловятся корректно, а VM выдерживает параллельное исполнение |
| G6c. Object layout / allocator / shapes | Закрыто на reference-уровне, не реализовано | Реализовать per-worker arenas, remote-free queues, large object space, shape transitions, `DeadShape` и storage growth/shrink path | G6, G6b, G7 | Объекты растут/умирают без GIL и без UB, immediate dealloc работает наблюдаемо корректно |
| G6d. Pinning / native interop boundary | Reference-profile закрыт, не реализован | Реализовать `PinToken`, pinned scopes, opaque handles, pinned buffer views, native cancel-poll hooks и dealloc/pin guards | G6b, G6c, G7 | Native interop не ломает no-GIL semantics и не создаёт dangling pointers |
| G6e. Compiled module format / loader / verifier | Reference-profile закрыт, не реализован | Реализовать `.amberbc` reader/writer, verifier, dependency linker, init state machine, export tables и debug sections | G6, G13 | Precompiled modules воспроизводимо загружаются и дают корректные stack traces |
| G7. No-GIL scheduler / strands | Закрыто на уровне модели, не реализовано | Реализовать worker pool, strand queues, task states, wake tokens, timers, blocking points и structured cancellation | G5, G6 | Несколько strand'ов реально исполняются параллельно без global lock |
| G8. Conformance suite | Частично закрыто как структура | Собрать executable spec и golden corpus для AST/HIR/runtime/scheduler/loader | G1–G7, G6e | Любая реализация Amber прогоняется единым набором тестов |
| G9. Stdlib collections & concurrency base | Закрыто на уровне спецификации | Реализовать зафиксированный chainable API коллекций и concurrency primitives (`Channel`, `Mutex`, `Atomic`) | G5, G7 | Runtime API стабилен для пользовательского кода |
| G9b. Notebook watch instrumentation | Закрыто в как optional profile | Реализовать `WatchCell`, watched object side-tables, dependency capture, `watch_epoch`, HIR/bytecode watch hooks и notebook invalidation events | G5, G6, G6b, G8 | `Kernel.watch(x)` корректно инвалидирует cells по binding/ivar revisions и не bump'ит `world_epoch` |

### P2 — вторая волна реализации и профилей

| Трек | Состояние | Ближайший шаг | Зависимости | Критерий выхода |
|---|---|---|---|---|
| G10. Типовая система v1 | Спецификация закрыта в; реализация не начата | Реализовать Amber/Typed checker: flow engine, invariance rules, exhaustiveness, reflective `Any`-boundary и typed-package tooling | G1–G9 | Typed profile воспроизводим и не меняет dynamic semantics языка |
| G11. Modules & metaprogramming | Спецификация закрыта в; реализация частично не начата | Реализовать open-class/open-mixin transactions, `define_method`, builtin `send`, `method_missing`, `include`/`extend` linearization, world freeze state, manifest/registry/client path и hot-reload guards | G1, G5, G6e, G9 | Dispatch-world, distribution и class-side composition реализуются без двусмысленности |
| G12. Advanced concurrency extensions | Спецификация закрыта в; реализация не начата | Реализовать `move(expr)` boundaries, `select`, supervisor policies и async-I/O awaitables поверх уже зафиксированного scheduler core | G7, G9 | Beyond-v1 concurrency работает без слома core no-GIL model |

### P3 — компилируемость и оптимизация

| Трек | Состояние | Ближайший шаг | Зависимости | Критерий выхода |
|---|---|---|---|---|
| G13. Bytecode compiler pipeline | Закрыто на уровне архитектуры, не реализовано | Эмитить bytecode из HIR: method prologues, pattern dispatch, safepoints, debug spans, task intrinsics и lifecycle intrinsics (`OBJ_DESTROY`, `OBJ_DEALLOC`) | G5–G7 | Один и тот же фронтенд обслуживает интерпретатор и компилятор |
| G14. Dynamicity boundary for AOT | Концептуально закрыто: frozen-world profile зафиксирован | Реализовать `world_epoch`/freeze transition, `SEND_DYN`, loader freeze mode и invalidation tests | G11, G13 | Понятно, какие модули можно компилировать нативно и какие места обязаны оставаться reflective |
| G15. Native backend / JIT | Архитектурно закрыто в; реализация не начата | Реализовать MIR/SSA pipeline, native/JIT codegen, frozen-image builder и runtime stubs для reflective sites | G13, G14 | Появляется путь к нативному профилю без слома динамического языка |

## 3. Рекомендуемый дорожный порядок

### Этап 1. Закрыть frontend как инженерную систему
- G1
- G2
- G3
- G4
- G5

### Этап 2. Собрать исполнимую no-GIL VM
- G6
- G6b
- G6c
- G6d
- G6e
- G7
- G8
- G9
- G9b

### Этап 3. Реализовать уже закрытые profile-возможности второй волны
- G10
- G11
- G12

### Этап 4. Открыть оптимизирующую и нативную ветку
- G13
- G14
- G15

## 4. Что я рекомендую делать следующим немедленно

Самая практичная последовательность на ближайший цикл теперь такая:

1. собрать `BcModule/BcMethod/BcCode`, `.amberbc` serializer/deserializer и bytecode verifier;
2. реализовать register VM loop с `SEND/SEND_DYN/CALL/JUMP/RETURN/GETLAST/SETLAST/MAKE_CLOSURE/OBJ_DESTROY/OBJ_DEALLOC`;
3. эмитить bytecode из HIR для `safe-nav`, clause dispatch, pattern assignment, reflective `send(...)`, lifecycle intrinsics и `task.async` / `task.spawn`;
4. подключить method tables, class/shape versions, `DeadShape`, `world_epoch`, freeze-state и inline caches для send/ivar access;
5. для notebook profile добавить `WatchCell`, watched object side-tables, `watch_epoch`, dependency capture и `Kernel.watch(...)` lowering;
6. реализовать open-class/open-mixin transaction path, `define_method`, `include` linearization, `method_missing` fallback и `WorldFrozenError` guards поверх уже фиксированного ISA;
6. реализовать non-moving collector, remembered sets, ownership checks, tombstone checks, per-worker allocator и pin tables;
7. реализовать loader/linker/init state machine, dependency tests и frozen-mode loader barrier для precompiled modules.

Именно этот путь даёт самый короткий маршрут к работающему Amber runtime без возврата назад по архитектуре.


## 5. Implementation gate [закрыт]

Implementation phase считается открытой для **reference implementation P0/P1**.

В качестве блокеров старта больше не остаются открытыми:

1. scope freeze первой реализации как **dynamic core + bytecode VM**;
2. minimal type envelope (`-> TypeTerm`, grammar `TypeTerm`, runtime hooks);
3. обязательный stdlib contract для chainable collections и concurrency base;
4. формальная матрица диагностик (`compile_error` / `warning` / `lint`);
5. minor closures: underscore policy, boundary bare matcher expressions и отказ от field lifetime annotations в v1.

После на уровне спецификации больше не остаётся незакрытых вопросов второй волны.

Остаются только инженерные треки реализации:

- Amber/Typed checker, flow engine и typed-package tooling;
- registry/client/publisher, signing pipeline и hot-reload implementation;
- reflection mirrors и class-side `extend` в runtime/loader;
- `move`, `select`, supervisor policies и async-I/O awaitables;
- `WeakRef`/`Ephemeron`/buffers/borrow helpers и host embedding API;
- MIR/SSA pipeline, native/JIT backend и `.amberimg` builder.

# Часть V. Возможность сделать Amber компилируемым

## 1. Вывод

Да: Amber **можно сделать компилируемым**. Причём в двух разных смыслах:

1. **компилируемым в байткод для собственной VM** — это реалистичный и рекомендуемый путь v1;
2. **компилируемым в native code / JIT / AOT** — тоже возможно, но только как второй этап и только после фиксации границ динамичности.

Для проекта в его текущем состоянии надо считать целевым именно первый вариант.

## 2. Почему язык уже достаточно "компиляторопригоден"

После закрытия parser core у Amber есть хорошие свойства для компиляции:

- детерминированный синтаксис выражений и postfix-цепочек;
- явный desugaring для many-def sugar, safe-nav и implicit-block;
- pattern matching сводится к конечному набору runtime primitives;
- `$_` естественно понижается до скрытого frame-slot;
- `case` и multi-clause `def` сводятся к одной dispatch-модели;
- block suffix компилируется в closure object или inline-closure node;
- `async`, `task.async` и `task.spawn` понижаются до strand/task scheduler primitives без введения GIL.

То есть Amber уже хорошо ложится на модель:

```text
source -> lexer -> parser -> AST -> HIR -> bytecode -> VM
```

## 3. Что лучше выбрать как первый исполнимый таргет

### 3.1. Рекомендуемый путь: bytecode VM

Для Amber v1 оптимальный target — стековая или register-based VM.

Практические причины:

- язык динамический;
- типовая система ещё не закрыта;
- minimal MOP, mixin/`include` profile и frozen-world boundary уже нормализованы, но расширенный introspection слой ещё открыт;
- базовая concurrency-semantics уже закрыта как no-GIL strand model;
- байткод позволяет быстро получить работающий runtime и не замораживать дизайн слишком рано.

Рекомендуемая минимальная архитектура:

```text
AST
 -> HIR
 -> Bytecode
 -> VM
 - call frames
 - closure objects
 - class objects
 - method tables
 - inline caches
 - scheduler hooks
```

### 3.2. Почему не стоит начинать сразу с native AOT

Прямой native backend сейчас упрётся не в синтаксис, а в динамические свойства языка, даже несмотря на уже зафиксированную frozen-boundary:

- open classes и `define_method` до freeze barrier;
- `method_missing`;
- `send` с динамическим именем метода;
- gradual typing;
- matcher protocol через `===`;
- reflective/import hot-load вне frozen profile;
- дальнейшие concurrency-расширения сверх зафиксированного no-GIL core.

Сделать native backend можно, но до реализации frozen-loader path, reflective slow-path и optimizer infrastructure он будет постоянно упираться в runtime contract.

## 4. Что именно компилировать в Amber

### 4.1. Фронтенд

Фронтенд единый для всех таргетов:

- lexer;
- parser;
- AST;
- HIR lowering;
- signature binder metadata;
- pattern metadata.

### 4.2. Обязательный HIR-слой

Чтобы язык был действительно компилируемым, HIR должен сделать явными вещи, которые в surface syntax скрыты:

- `CHAIN_DOT` и границы inline-block;
- `safe-nav` как последовательность null-guards;
- implicit placeholder-block как explicit closure node;
- many-def sugar как clause-style `def`;
- `$_` как frame-local slot;
- auto-assign как post-dispatch commit step;
- `case` и `def when` как один `match-dispatch` primitive.

Без HIR компилятор быстро превратится в набор ad hoc исключений.

### 4.3. Байткодные примитивы, которые реально понадобятся

Минимальный instruction set должен уметь:

- загрузку литералов и локалов;
- load/store `@field` и `@@field`;
- обычный call и send;
- safe-send / safe-call / safe-index;
- создание closure/block object;
- jump / conditional jump;
- `match` / `deconstruct` / `deconstruct_keys` / `===`;
- enter/leave frame;
- `last_result_get` / `last_result_set`;
- raise / rescue boundary;
- fiber/task/strand primitives как отдельный runtime API;
- lifecycle primitives `destroy!` / `memory.dealloc` с tombstone checks.

## 5. Native/AOT: возможен, но только как профиль

### 5.1. Рекомендуемая формулировка

Amber стоит проектировать как:

- **dynamic core language**;
- **bytecode-compiled runtime by default**;
- **optional frozen/typed profile** для aggressive optimization и native compilation.

### 5.2. Что уже зафиксировано для AOT, а что ещё нужно реализовать

Language boundary для native/AOT уже описан:

- frozen-world boundary зафиксирован;
- invalidation model для `open class`, `open mixin`, `include`, `define_method`, `send`, `method_missing` задан;
- reflective slow-path отделён от world mutation;
- package/module loading phase ограничена freeze barrier.

Остаётся реализовать и/или дополнительно нормализовать:

- ABI объектов, closure-ов и frame-ов на уровне backend;
- policy для generics/type metadata;
- concrete MIR/SSA and optimizer pipeline;
- frozen-image packaging/deployment story;
- optional deopt/JIT strategy, если реализация вообще захочет её иметь.

### 5.3. Практичный компромисс

Самый реалистичный вариант — двухрежимная модель:

1. **обычный Amber**:
 - компиляция в байткод;
 - полная динамичность;
 - reflective features разрешены;

2. **Amber/Frozen** или **Amber/AOT profile**:
 - loader/linker/module-init работают в `open`-состоянии;
 - затем host/toolchain переводит dispatch-world в `frozen`;
 - после этого запрещены world mutations:
 - late open class,
 - dynamic `define_method`,
 - поздняя Amber module load в тот же world;
 - `send(...)` и `method_missing` не запрещаются, но становятся явно reflective slow-path;
 - код может пойти в JIT или native AOT.

Это позволяет не урезать основной язык ради компилятора.

## 6. Какие решения текущей спеки помогают компиляции, а какие мешают

### 6.1. Помогают

- `$_` как отдельный слотовый регистр, а не магическая динамика;
- clause-style `def` как единый dispatch;
- отказ от `map(_1 * 2)` как скрытой второй формы блока;
- bare matcher expressions только в `case` / `case!`, а не везде;
- поздний commit auto-assign после dispatch;
- `case` / `case!` и patterns через ограниченный protocol set.

### 6.2. Мешают или требуют slow-path

- open classes и `define_method` до freeze barrier;
- `method_missing`;
- `send(...)` с dynamic selector;
- runtime import/load вне frozen profile;
- незакрытая типизация `and/or`;
- необходимость реализовать strand-isolation и runtime sync primitives без отката к GIL.

## 7. Рекомендуемая архитектура реализации

### Вариант A — правильный базовый путь

```text
Amber source
 -> Lexer
 -> Parser
 -> AST
 -> HIR lowering
 -> Bytecode compiler
 -> Amber VM
```

Это надо считать **основной веткой проекта**.

### Вариант B — будущая оптимизирующая ветка

```text
Amber source
 -> Lexer
 -> Parser
 -> AST
 -> HIR
 -> MIR / SSA
 -> JIT or Native AOT
```

Её имеет смысл открывать только после того, как:

- есть стабильный HIR и ABI frame/task/strand;
- есть conformance suite;
- закрыты Q3–Q5 и Q11, а mixin/`include` profile доводит MOP-часть до законченного v1-ядра.

## 8. Практический вывод для следующего цикла

Если цель — сделать язык реально исполнимым и не закрыть дорогу к компилируемости, правильный следующий шаг такой:

1. реализовать parser -> AST -> HIR;
2. собрать bytecode VM;
3. заморозить runtime ABI для frames / closures / classes / tasks / strands;
4. реализовать no-GIL scheduler без global lock;
5. только потом проектировать AOT-profile.

То есть ответ на вопрос "можно ли сделать Amber компилируемым?" — **да, и уже сейчас стоит проектировать его как bytecode-compiled language с возможным нативным профилем позже**.


# Часть VI. Reference bytecode VM и runtime ABI v1

## 1. Статус и граница нормативности

Эта часть фиксирует **reference execution profile** для Amber v1. Она не отменяет языковую семантику из предыдущих частей, а делает её исполнимой в одном конкретном архитектурном профиле:

- frontend обязан идти через `source -> AST -> HIR -> bytecode`;
- code objects, constant pool и signature metadata считаются неизменяемыми и shareable;
- ordinary mutable heap-объекты остаются strand-confined;
- bytecode ISA задаётся **семантически**, а не как окончательная бинарная кодировка.

Следовательно, другая реализация Amber может использовать не этот exact VM layout, но обязана быть наблюдаемо эквивалентной этому профилю.

## 2. Исполнимый профиль v1: register/slot VM

Для Amber v1 фиксируется **register/slot bytecode VM**, а не стековая машина.

Причины выбора:

- HIR уже выражает locals, captures, dispatch и control-flow явно;
- `$_` естественно опускается в frame slot, а не в неявную operand-stack магию;
- pattern matching удобнее компилировать в decision blocks с временными регистрами;
- no-GIL scheduler проще соединять с frame/task ABI, когда нет скрытого operand stack между safe-points;
- этот же профиль легче поднять дальше в MIR/SSA для AOT/JIT.

Нормативный вывод:

- bytecode compiler обязан компилировать из HIR в фиксированный регистровый IR-байткод;
- `HSafeSend/HSafeCall/HSafeIndex` не должны доживать до bytecode как отдельные инструкции: они обязаны быть уже разложены в ветвления и null-guards;
- surface-sugar (`block suffix`, `many-def sugar`, implicit placeholders) к моменту bytecode emission уже обязан быть устранён.

## 3. Module / method / code object ABI

### 3.1. `BcModule`

Минимальный сериализуемый unit исполнения v1:

```text
BcModule(
 const_pool[],
 methods[],
 code_objects[],
 nested_dispatch_owners[],
 source_map?,
 debug_strings?,
 feature_flags
)
```

`BcModule` обязан быть immutable и shareable между strand'ами и worker'ами.

### 3.2. `BcMethod`

Метод в reference VM — это не просто code pointer, а связка метаданных и entry-code:

```text
BcMethod(
 selector,
 owner_dispatch,
 signature_desc,
 default_thunks[],
 type_hooks[],
 clause_table[],
 auto_assign_desc[],
 entry_code,
 method_flags
)
```

Где:

- `owner_dispatch` указывает на class или mixin owner метода;
- `signature_desc` задаёт canonical binding shape;
- `default_thunks[]` вычисляются слева направо по правилам binder;
- `type_hooks[]` исполняют `as TypeTerm` checks;
- `clause_table[]` описывает pattern/guard dispatch для clause-style `def`;
- `auto_assign_desc[]` коммитятся только после выбора победившей clause;
- `entry_code` — основной bytecode метода.

Это делает метод компилируемым без потери поздней динамичности языка.

### 3.3. `BcCode`

`BcCode` — неизменяемый code object:

```text
BcCode(
 code_id,
 kind, # module / method / block / ensure / rescue / default-thunk
 reg_count,
 local_layout,
 capture_layout,
 instructions[],
 handler_table[],
 call_site_table[],
 ivar_site_table[],
 source_spans[],
 safepoint_table[],
 flags
)
```

Нормативно:

- `instructions[]` immutable;
- `handler_table[]` задаёт protected ranges и точки входа в `rescue/ensure`;
- `call_site_table[]` и `ivar_site_table[]` резервируют места под inline caches;
- `source_spans[]` нужны для диагностик, stack traces и tooling;
- `safepoint_table[]` делает явными места, где разрешены cancellation / GC / scheduler hand-off.

### 3.4. Constant pool

Constant pool v1 может содержать только shareable/immutable сущности:

- литералы (`null`, bool, numbers, symbols, frozen strings);
- пути констант и селекторы;
- shape/method descriptors;
- code object refs;
- key sets для map-pattern matching.

Non-shareable объекты не могут быть зашиты в constant pool.

## 4. Frame ABI

### 4.1. Структура кадра

Минимальный runtime frame:

```text
Frame(
 caller,
 return_pc,
 code,
 self,
 lexical_owner,
 block,
 task,
 last_result,
 regs[code.reg_count],
 capture_cells,
 handler_cursor,
 flags
)
```

Поля имеют такой статус:

- `self` — текущий receiver;
- `lexical_owner` — класс/модуль/лексический контейнер, нужный для constant lookup и class-context;
- `block` — переданный closure либо `null`;
- `task` — текущая task для no-GIL scheduler и structured child set;
- `last_result` — canonical slot для `$_`;
- `regs[]` — регистровое окно метода.

### 4.2. Роль регистров

Bytecode работает не с именами, а со слотами/регистрами. Рекомендуемое разбиение v1:

- аргументы и связанные параметры — фиксированные ранние слоты;
- обычные локалы — следующие слоты;
- compiler-temporaries — хвост регистрационного окна;
- captures читаются через `capture_cells`, а не как обычные locals.

Точная нумерация слотов implementation-defined, но `BcCode.local_layout` обязан её описывать.

### 4.3. Семантика `$_`

`$_` в байткоде больше не является специальным именем. Нормативно используются две операции:

- `GETLAST dst`
- `SETLAST src`

Правила совпадают с Частью II:

- expression-statement наблюдаемо завершает `SETLAST`;
- неявный return кадра возвращает `last_result`;
- переключение между task'ами не смешивает `last_result`, потому что slot живёт в frame, а frame — внутри task stack.

### 4.4. Вызов и возврат

Для reference VM фиксируется такой call convention:

1. caller вычисляет receiver/callee и фактические аргументы слева направо;
2. `SEND`/`CALL` резолвит target method/callable; для class object в позиции `CALL` выбирается constructor path `:new`; для callable reference object выполняется его reference descriptor;
3. binder/prologue создаёт новый frame и canonical locals по `signature_desc`;
4. body исполняется в собственном `regs[]`;
5. `RETURN src` возвращает значение в caller;
6. caller сам решает, нужно ли обновить свой `last_result` в соответствии с surface semantics.

То есть `last_result` — это не скрытый аккумулятор VM, а явная часть frame ABI.

## 5. Closures и upvalues

### 5.1. `MAKE_CLOSURE`

Closure object v1 содержит:

```text
Closure(
 code,
 capture_cells[],
 arity_meta,
 placeholder_meta,
 flags
)
```

`code` immutable/shareable; mutability приходит только из `capture_cells[]`.

### 5.2. Capture semantics

Нормативная семантика closure-захватов — **by-reference**, а не по значению. Следовательно:

- пока родительский frame жив, upvalue может ссылаться на его slot как open-upvalue;
- при уходе кадра захваченный slot промотируется в heap-cell;
- разные closure, захватившие одну и ту же переменную, обязаны наблюдать одну и ту же cell.

Оптимизация по scalar replacement разрешена только если она наблюдаемо эквивалентна by-reference semantics.

### 5.3. Закрытие upvalues

Минимальный runtime primitive:

```text
CLOSE_UPVALUES from_slot
```

Он обязан переводить все open-upvalues, ссылающиеся на локальные слоты `>= from_slot`, в heap-cells перед выходом из соответствующей области жизни.

### 5.4. Closure и граница strand

Closure может пересекать strand boundary только если:

- `code` shareable (для `BcCode` это всегда так);
- все `capture_cells[]` содержат shareable значения;
- среди captures нет strand-confined ссылок.

Иначе `task.spawn` или иной cross-strand API обязан выдать compile-time diagnostic, если это видно статически, либо бросить `IsolationError` на runtime.

## 6. Heap ABI, ownership и no-GIL дисциплина

### 6.1. Object header

Минимальный header heap-объекта в reference runtime:

```text
ObjHeader(
 class_ptr,
 flags, # frozen / shareable / sync / finalizer / etc.
 owner_token,
 shape_ptr?,
 gc_meta
)
```

`owner_token` имеет три нормативных режима:

- `shareable` — объект можно читать и передавать между strand'ами свободно;
- `confined(strand_id)` — объект принадлежит одному strand;
- `sync` — объект сам является синхронизационным примитивом (`Channel`, `Mutex`, `Atomic`, `TaskHandle`).

### 6.2. Что рождается shareable, а что confined

По умолчанию:

- `BcModule`, `BcMethod`, `BcCode`, symbols, frozen literals, class metadata — shareable;
- обычные `Array`, `Map`, пользовательские экземпляры, mutable строки, capture-cells — confined текущему strand;
- `Channel`, `Mutex`, `Atomic`, `TaskHandle` — `sync`.

### 6.3. Нормативная runtime-проверка ownership

Reference VM обязана ловить illegal cross-strand access не позже первой наблюдаемой операции над объектом. Проверки обязаны существовать как минимум на:

- `SEND/CALL`, если receiver/callee является heap-object с ownership constraints;
- `LOAD_IVAR/STORE_IVAR`;
- индексировании и коллекционных builtin fast-path;
- `task.spawn` capture verification;
- `Channel.send` payload boundary.

Если доступ идёт из чужого strand к `confined`-объекту, результат — `IsolationError`.

### 6.4. Transfer semantics в v1 не вводится

Amber v1 **не** вводит скрытый move-transfer, copy-on-send или автоматический deep clone mutable-графов. Единственный допустимый cross-strand путь для обычных значений — через shareable/frozen graph либо через sync-объекты.

### 6.5. GC и ownership

Независимо от выбранного collector family, для ownership остаются две уже фиксированные нормы:

- GC не имеет права изменять ownership mode объекта так, чтобы strand-confined объект стал чужим без явного языкового механизма;
- временная stop-the-world пауза GC не считается GIL, если ordinary execution в остальное время не сериализуется глобальным lock'ом.

## 7. Shapes, method tables и inline caches

### 7.1. Class / method ABI

Каждый class object и mixin object в reference profile должен хранить как минимум:

```text
DispatchOwnerRuntime(
 method_table,
 method_version,
 direct_includes[],
 owner_flags,
 ivar_schema?,
 superclass?
)
```

Где:

- для class object заполнены `ivar_schema` и optional `superclass`;
- для mixin object `ivar_schema` может отсутствовать, а `superclass` всегда `null`.

`method_version` монотонно меняется при любой операции, влияющей на dispatch:

- open class/mixin mutation;
- `define_method`;
- `include`, меняющий direct include-set;
- удаление/замена метода;
- изменение `method_missing` policy.

### 7.2. Instance layout и shapes

Для компилируемого профиля v1 фиксируется shape-oriented layout:

```text
Shape(
 shape_id,
 ivar_slots: Map<Name, SlotIndex>,
 parent_shape?,
 shape_version
)
```

Instance хранит `shape_ptr` и storage для slot-based ivars. Реализация вправе иметь slow-object fallback, но наблюдаемые правила такие:

- `@field` всегда читается по имени языка;
- inline cache может резолвить имя в slot index;
- при shape mismatch VM обязана уйти в slow-path и обновить cache.

### 7.3. Call-site caches

Минимальный monomorphic call cache:

```text
CallIC(
 receiver_class,
 method_version,
 target_method,
 cache_flags
)
```

Cache валиден тогда и только тогда, когда:

- класс receiver совпадает;
- `cached.method_version == receiver_class.method_version`.

Иначе происходит slow-path lookup и перезапись cache.

### 7.4. Ivar-site caches

Минимальный ivar cache:

```text
IvarIC(
 shape_id,
 shape_version,
 slot_index,
 cache_flags
)
```

Это позволяет компилировать `@x` в быстрый slot load/store, не теряя корректность при shape transition.

### 7.5. Global invalidation и путь к AOT

Для динамического Amber reference VM хранит global `world_epoch`. Он не обязан участвовать в каждой dispatch-проверке, но обязан обновляться при world-level mutation, которая может интересовать tooling, JIT или AOT invalidation.

Это и есть bridge к будущему frozen/AOT profile: пока мир не frozen, send/ic остаются version-guarded.

## 8. Семантический bytecode ISA v1

Бинарная упаковка инструкций остаётся открытой. Нормативной является **семантика** инструкций и их обязательные семейства.

### 8.1. Data / frame instructions

| Инструкция | Семантика |
|---|---|
| `LOADK dst, k` | загрузить значение из constant pool |
| `LOADNULL dst` | положить `null` |
| `LOADBOOL dst, imm` | положить `true/false` |
| `MOVE dst, src` | копировать значение между регистрами |
| `LOADSELF dst` | загрузить `self` текущего frame |
| `GETLAST dst` | прочитать `frame.last_result` |
| `SETLAST src` | записать `frame.last_result` |
| `MAKE_LIST dst, r0, count` | собрать list из диапазона регистров |
| `MAKE_TUPLE dst, r0, count` | собрать tuple |
| `MAKE_MAP dst, pair_desc` | собрать map по compile-time descriptor |
| `FREEZE dst, src` | построить frozen/shareable представление либо вернуть исходный immutable объект |

### 8.2. Captures / variables / object state

| Инструкция | Семантика |
|---|---|
| `LOAD_UPVAL dst, u` | прочитать capture-cell |
| `STORE_UPVAL u, src` | записать capture-cell |
| `LOAD_IVAR dst, recv, name_id, site_id` | прочитать ivar c shape-cache fast-path |
| `STORE_IVAR recv, name_id, src, site_id` | записать ivar c shape-cache fast-path |
| `LOAD_CVAR dst, owner, name_id` | прочитать class var |
| `STORE_CVAR owner, name_id, src` | записать class var |
| `LOOKUP_CONST dst, path_id` | найти константу по lexical/constant rules |
| `MAKE_CLOSURE dst, code_id, capture_desc_id` | создать closure object |
| `MAKE_CALLABLE_REF dst, ref_desc_id` | создать callable reference object на static callable binding / module export |
| `MAKE_BOUND_SEND_REF dst, recv, selector_id` | создать bound send-reference для class-side method reference `&Class.method` |
| `MAKE_UNBOUND_SEND_REF dst, owner, selector_id` | создать unbound instance send-reference для `&Class#method` |
| `OBJ_DESTROY dst, obj` | выполнить terminal `destroy!`-semantics и вернуть bool |
| `OBJ_DEALLOC dst, obj` | немедленно разрушить и деаллоцировать payload с tombstone-model |
| `CLOSE_UPVALUES from_slot` | закрыть escaping captures перед выходом из области жизни |

Примечание: обычные локалы уже живут в `regs[]`, поэтому отдельные `LOAD_LOCAL/STORE_LOCAL` не требуются.

### 8.2a. Notebook watch opcodes

Эти инструкции обязательны только для `.amberbc`, собранного с Amber/Notebook Watch Profile. Production profile может запрещать emission этих opcodes verifier'ом.

| Инструкция | Семантика |
|---|---|
| `WATCH_BIND dst, binding_id, flags` | перевести local/upvalue/top-level binding cell в `WatchCell`, вернуть `WatchHandle` |
| `WATCH_IVAR dst, recv, name_id, flags` | перевести object/ivar target в watched representation, вернуть `WatchHandle` |
| `WATCH_CVAR dst, owner, name_id, flags` | перевести class storage target в watched representation, вернуть `WatchHandle` |
| `UNWATCH dst, handle` | снять watch-подписку, вернуть bool |
| `WATCH_REV dst, handle_or_target` | прочитать текущую revision target'а |
| `DEP_CAPTURE_BEGIN cell_id` | включить dependency-capture mode для текущего notebook cell |
| `DEP_CAPTURE_END dst` | выключить capture mode и вернуть dependency set |
| `WATCH_EVENT_FLUSH dst` | host-visible drain/poll watch events, если runtime использует queue model |

Нормативно:

- `WATCH_*` не bump'ят `world_epoch`;
- successful `STORE_IVAR` watched object'а bump'ит field/object revision после write barrier;
- failed write не публикует watch event;
- verifier обязан запрещать watch-opcodes вне profile flags, которые явно включают notebook instrumentation.

### 8.3. Calls, sends и protocol ops

| Инструкция | Семантика |
|---|---|
| `SEND dst, recv, selector_id, argv_desc, block_reg, site_id` | обычный method send с call-site cache |
| `SEND_DYN dst, recv, selector_reg, argv_desc, block_reg, site_id` | reflective send с selector'ом из регистра |
| `CALL dst, callee, argv_desc, block_reg, site_id` | вызов callable object; если `callee` является class object, выполняется constructor path через `:new` |
| `IN_OP dst, elem, container` | реализует language-level `in` по протоколу `contains?` или бросает `TypeError` |
| `TRIPLE_EQ dst, matcher, value` | реализует `===` с обязательной проверкой булевого результата |
| `TYPECHECK value, type_term_id` | runtime-hook для `as TypeTerm` в bind/prologue |

Нормативно:

- арифметические операторы, сравнения, индексирование и обычный `[]`/`[]=` могут lower'иться в `SEND` соответствующих селекторов;
- `SEND_DYN` обязан проверять, что selector_reg содержит `Symbol` или `Str`, иначе бросать `TypeError`;
- `CALL` обязан принимать closure objects, callable reference objects, unbound method reference objects и class objects; любые другие callee дают `TypeError`;
- если reflective `send(...)` имеет compile-time literal selector, emitter вправе использовать обычный `SEND` вместо `SEND_DYN`;
- safe-nav не имеет собственного bytecode-opcode и обязан быть уже разложен в ветвления до emission.

### 8.4. Control-flow, exceptions и safe-points

| Инструкция | Семантика |
|---|---|
| `JUMP label` | безусловный переход |
| `JUMP_IF_TRUE reg, label` | переход по truthy |
| `JUMP_IF_FALSE reg, label` | переход по falsy |
| `JUMP_IF_NULL reg, label` | переход, если значение `null` |
| `RETURN reg` | вернуть значение caller'у |
| `RAISE reg` | поднять исключение |
| `SAFEPOINT` | poll cancellation / scheduler / GC cooperation |

Для compiled bytecode обязательны такие правила:

- каждый back-edge цикла обязан проходить через `SAFEPOINT` либо сам считаться safepoint-инструкцией;
- `SEND`, `CALL`, blocking waits и `TASK_*`-операции являются scheduler-visible точками;
- unwinding использует `handler_table[]`, а не ad hoc поиск по source text.

### 8.5. Pattern opcodes

Pattern matching компилируется не в общие method send'ы, а в ограниченное семейство pattern-инструкций поверх того же register VM.

| Инструкция | Семантика |
|---|---|
| `P_PREP_SEQ dst, src, mode, fail` | проверить sequence-представимость (`native` или `deconstruct`), иначе jump в `fail` |
| `P_PREP_MAP dst, src, keyset_id, needs_full, fail` | проверить map-представимость (`native` или `deconstruct_keys`), иначе jump в `fail` |
| `P_CHECK_EQ src, const_id, fail` | сравнить по value-equality |
| `P_CHECK_PIN src, slot, fail` | сравнить со значением уже существующего биндинга |
| `P_CHECK_LEN_EQ seq, n, fail` | длина sequence обязана быть ровно `n` |
| `P_CHECK_LEN_GTE seq, n, fail` | длина sequence обязана быть не меньше `n` |
| `P_GET_INDEX dst, seq, idx` | достать элемент sequence |
| `P_HAS_KEY mp, key_id, fail` | убедиться, что ключ существует |
| `P_GET_KEY dst, mp, key_id` | достать значение по ключу |
| `P_TRIPLE_EQ matcher, value, fail` | выполнить matcher-style `===` |
| `P_BIND slot, src` | подготовить биндинг для последующего коммита |
| `P_COMMIT base_slot, count` | атомарно закоммитить накопленные биндинги |
| `P_FAIL mode` | завершить паттерн в соответствии с контекстом (`null`, `MatchError`, next-clause`) |

Нормативные следствия:

- одна и та же pattern-VM логика обслуживает `case`, `block params`, pattern assignment и clause-dispatch;
- partial bindings не наблюдаемы до `P_COMMIT`;
- OR-pattern компилируется в ветвления с независимыми временными зонами биндингов;
- dynamic pattern objects не требуют отдельного обязательного opcode: reference bytecode profile может lower'ить `matcher.match(value)` через обычный `SEND/CALL`, а затем продолжать matching returned `bindings` через существующие `P_*`-инструкции.

### 8.6. Concurrency / task opcodes

| Инструкция | Семантика |
|---|---|
| `SPAWN_SAME dst, closure` | создать child-task в том же strand |
| `SPAWN_NEW dst, closure` | создать child-task в новом strand |
| `TASK_WAIT dst, handle, timeout_reg?` | ждать завершения child-task либо бросить `TimeoutError` |
| `TASK_RESUME dst, handle` | enqueue/wake semantics для target-task, вернуть bool |
| `TASK_CANCEL dst, handle` | выставить cancellation flag, вернуть bool |
| `TASK_SLEEP secs` | перевести текущую task в sleeping-state |
| `TASK_YIELD` | добровольно уступить управление |

Surface-синтаксис остаётся прежним; эти инструкции — только lowering target для HIR/runtime intrinsics.

## 9. Lowering HIR -> bytecode

### 9.1. Базовое правило

Bytecode compiler обязан компилировать **из HIR**, а не из surface AST. Это означает:

- `block suffix` уже является `HClosure`;
- `many-def sugar` уже собран в единый `HMethod`;
- `safe-nav` уже развёрнут в `HIf/JUMP_IF_NULL`;
- `$_` уже явный `HLastGet/HLastSet`;
- callable references уже выражены как `HCallableRef/HUnboundMethodRef`;
- constructor-call sugar остаётся ordinary `HCall` по callee, если compiler не доказал безопасное понижение в `HSend(:new)`;
- notebook watch intrinsics уже выражены как `HWatchBinding/HWatchIvar/HWatchCvar`, если build profile включает Amber/Notebook.

### 9.2. `HSend`, `HSendDyn` и `HCall`

```text
HSend(receiver, selector, args..., block?)
 -> evaluate receiver/args/block into regs
 -> SEND dst, r_recv, selector_id, argv_desc, r_block, site_id

HSendDyn(receiver, selector_expr, args..., block?)
 -> evaluate receiver/selector/args/block into regs
 -> SEND_DYN dst, r_recv, r_sel, argv_desc, r_block, site_id

HCall(callable, args..., block?)
 -> evaluate callable/args/block into regs
 -> CALL dst, r_fn, argv_desc, r_block, site_id
 # если r_fn содержит class object, VM выполняет constructor path через:new
```


Callable reference nodes lower'ятся в dedicated reference construction opcodes или в эквивалентные runtime descriptors:

```text
HCallableRef(kind = static_callable, target)
 -> MAKE_CALLABLE_REF dst, ref_desc_id

HCallableRef(kind = bound_send, receiver, selector)
 -> evaluate receiver into r_recv
 -> MAKE_BOUND_SEND_REF dst, r_recv, selector_id

HUnboundMethodRef(owner_class, selector)
 -> evaluate owner_class into r_owner
 -> MAKE_UNBOUND_SEND_REF dst, r_owner, selector_id
```

### 9.3. `HMatchDispatch`

`HMatchDispatch` компилируется в линейную цепочку clause-block'ов:

1. вычислить scrutinee / clause-subject;
2. для каждой clause сверху вниз сгенерировать pattern-check region на `P_*`-инструкциях;
3. если pattern содержит dynamic pattern object, вычислить `matcher_expr`, вызвать `match(value)` и, при наличии `with...`, прогнать returned `bindings` через обычный map-pattern region;
4. guard компилируется только после успешного pattern region;
5. первая успешная clause прыгает в свой body;
6. иначе — `else_body` или fail mode.

Нормативно:

- `case` и `case!` обязаны использовать один и тот же lowering pipeline;
- различие между ними представляется только через `fail_mode`;
- matcher-produced bindings не могут обходить обычный `P_COMMIT` protocol.

### 9.4. `HPatternAssign`

Pattern assignment компилируется как:

- вычислить RHS в регистр;
- исполнить `P_*`-program;
- на успехе выполнить `P_COMMIT` и затем `SETLAST rhs`;
- на fail — `P_FAIL MatchError`.

### 9.5. `HSpawnSameStrand` / `HSpawnNewStrand`

```text
HSpawnSameStrand(block)
 -> MAKE_CLOSURE r_cl, code_id, captures
 -> SPAWN_SAME r_dst, r_cl

HSpawnNewStrand(block)
 -> MAKE_CLOSURE r_cl, code_id, captures
 -> verify shareable captures
 -> SPAWN_NEW r_dst, r_cl
```

### 9.6. Пример: safe-nav в байткоде

```amber
user.?.address.?.city
```

После HIR-lowering bytecode наблюдаемо эквивалентен такой схеме:

```text
r0 = <user>
JUMP_IF_NULL r0, L_null
SEND r1, r0, "address", [], null, site0
JUMP_IF_NULL r1, L_null
SEND r2, r1, "city", [], null, site1
RETURN r2
L_null:
LOADNULL r3
RETURN r3
```

То есть safe-nav окончательно закрывается как compile-time transformation, а не runtime magic opcode.

## 10. Verifier и invariants toolchain

Перед исполнением `BcModule` reference toolchain обязан пройти bytecode verifier. Минимум он проверяет:

- диапазоны регистров и ссылок на constant pool;
- корректность jump targets;
- согласованность `handler_table[]`;
- что `P_*`-инструкции не пишут за пределы разрешённых binding/temp slots;
- что `SAFEPOINT` присутствует на всех back-edge путях;
- что non-shareable объекты не встроены в shareable sections модуля.

Verifier может быть compile-time, load-time или обоими сразу; это implementation choice. Но выполнение неверифицированного байткода reference profile не предполагает.

## 11. Что уже закрыто этим профилем, а что ещё нет

Этой частью документа теперь считаются **закрытыми на уровне архитектуры**:

- выбор execution target: register/slot bytecode VM;
- frame/closure/object ABI;
- `$_` как frame-local slot;
- pattern matching как отдельная opcode family;
- inline cache envelope для method/ivar dispatch;
- ownership discipline для no-GIL runtime;
- deterministic lifetime model: `destroy!`, tombstones и `memory.dealloc`.

Сама по себе эта часть не замыкает весь runtime story. В текущей полной редакции следующие слои уже дополнительно закрываются ниже:

- reference collector/pinning/FFI profile;
- `.amberbc` serialization / loader / verifier profile.

После этого незакрытым на уровне архитектуры остаётся прежде всего frozen-world policy для AOT и более широкая distribution/tooling policy.

## 12. Практический следующий шаг после этой редакции

Теперь уже нет смысла снова спорить о surface syntax. Следующий инженерный шаг предельно конкретен:

1. реализовать `BcModule/BcMethod/BcCode` и verifier;
2. написать минимальный register VM loop;
3. эмитить bytecode из HIR для `SEND/CALL`, `P_*`, `GETLAST/SETLAST`, `MAKE_CLOSURE`, `TASK_*`, `OBJ_DESTROY`, `OBJ_DEALLOC`;
4. подключить class/method versions, shape transitions, tombstone checks и inline caches;
5. реализовать allocator/lifetime runtime без GIL;
6. затем добрать collector/pinning и `.amberbc` loader/profile уже по Частям VIII–IX, а после этого открывать FFI/AOT ветку более высокого уровня.


# Часть VII. Lifetime, explicit destruction, `memory.dealloc` и allocator profile v1

## 1. Статус и назначение

Эта часть закрывает reference lifetime profile для Amber v1. После неё язык фиксирует не только reachability-based GC для обычного managed heap, но и **детерминированное завершение жизни объекта** по явному запросу программы.

Нормативный вывод:

- Amber v1 сохраняет GC для автоматического сбора недостижимых объектов;
- Amber v1 дополнительно вводит explicit lifetime operations: `destroy!` и `memory.dealloc`;
- GC **не** обязан и не должен вызывать пользовательский деструктор автоматически;
- deterministic cleanup делается только явным пользовательским кодом или явным runtime intrinsic, а не неявным finalizer magic.

Это решение согласуется с no-GIL моделью: обычное исполнение остаётся lock-free на уровне интерпретатора/VM, а cleanup semantics становится предсказуемой и не привязанной к произвольному моменту прохода collector'а.

## 2. Surface lifetime API

### 2.1. `destroy!` как явный деструктор

`destroy!` — специальный lifecycle-selector. Класс вправе объявить его как обычный instance method по синтаксису, но по семантике это **терминальный деструктор**, а не просто произвольный mutating method.

```amber
class CachePage:
 def destroy!():
 @rows = null
 @index = null
```

Нормативно:

- `obj.destroy!()` переводит объект из состояния `live` в терминальную lifetime-последовательность;
- первая успешная операция `destroy!` возвращает `true`;
- повторный вызов на уже `destroyed` или `deallocated` объекте возвращает `false`;
- `destroy!` выполняется только в owner-strand объекта;
- `destroy!` не освобождает heap payload автоматически: для немедленного освобождения памяти используется `memory.dealloc(obj)`.

### 2.2. `memory.dealloc(obj)`

`memory` — встроенное runtime-namespace. `memory.dealloc(obj)` — нормативный primitive немедленного освобождения памяти объекта.

```amber
cache = CachePage.new(...)...
memory.dealloc(cache)
```

Семантика:

- если объект `live`, runtime обязан сначала выполнить `destroy!`-semantics, а затем перейти к deallocation path;
- если объект уже `destroyed`, runtime немедленно переходит к deallocation path;
- если объект уже `deallocated`, операция возвращает `false`;
- если значение не имеет heap payload (например `null`, immediates, symbols, small ints), операция возвращает `false`;
- при первом реальном освобождении payload операция возвращает `true`.

### 2.3. Introspection-интринсики lifetime

Для безопасной проверки статуса объекта reference runtime обязан предоставлять минимум:

```amber
memory.alive?(obj)
memory.destroyed?(obj)
memory.deallocated?(obj)
```

Эти функции обязаны быть безопасны даже для tombstone-объекта.

## 3. Lifetime state machine

Нормативные состояния heap-объекта:

```text
live -> destroying -> destroyed -> deallocated
```

Смысл состояний:

- `live` — обычный работающий объект;
- `destroying` — идёт деструкторная цепочка; внешний код не должен наблюдать объект как обычный live-instance;
- `destroyed` — деструктор уже выполнен, но payload ещё может существовать до GC или `memory.dealloc`;
- `deallocated` — payload уже освобождён, в heap остаётся только минимальный tombstone-header до тех пор, пока на него ещё есть ссылки.

Операции по состояниям:

- на `live` разрешены все обычные операции;
- на `destroyed` любые обычные method-send, ivar access, indexing и builtin fast-path обязаны бросать `DestroyedAccessError`;
- на `deallocated` любые обычные method-send, ivar access, indexing и builtin fast-path обязаны бросать `UseAfterFreeError`;
- только `memory.alive?`, `memory.destroyed?`, `memory.deallocated?` и диагностическая runtime-introspection обязаны быть корректны на dead/tombstone-объекте.

## 4. Нормативная семантика деструктора

### 4.1. Кто имеет право вызывать `destroy!`

Для ordinary managed объектов:

- `confined(strand_id)` объект может быть разрушен только из своего owner-strand;
- попытка вызвать `destroy!` из чужого strand даёт `IsolationError`;
- `shareable` объекты в v1 не поддерживают явный `destroy!`/`dealloc` как пользовательскую операцию и должны давать `LifetimeError`, если реализация не вводит специально оговорённый builtin-type exception;
- `sync`-объекты (`Channel`, `Mutex`, `Atomic`, `TaskHandle`) не подлежат обычному `memory.dealloc` из пользовательского кода; для них используются собственные lifecycle APIs типа `close`, если такие есть.

### 4.2. Порядок вызова по иерархии классов

Если деструктор определён в нескольких классах цепочки наследования, runtime обязан вызывать class-local реализации в порядке:

1. самый производный класс;
2. затем по цепочке вверх к superclass;
3. каждая class-local реализация не более одного раза.

Это значит, что `destroy!` имеет специальную цепочную семантику и не требует ручного `super` для базового teardown.

### 4.3. Исключения внутри деструктора

Если одна из стадий `destroy!` бросает исключение:

- runtime обязан сохранить **первое** необработанное исключение;
- оставшиеся базовые стадии destructor-chain всё равно должны быть выполнены;
- после завершения цепочки объект считается как минимум `destroyed`;
- затем сохранённое первое исключение повторно бросается вызывающему коду.

Иными словами, `destroy!` — terminal operation: rollback обратно в `live` невозможен.

### 4.4. Деструктор не может приостанавливаться

Чтобы не допускать полуживой объект между scheduler steps, на `destroy!` накладывается жёсткое правило:

- деструкторный код **не имеет права** выполнять suspending operations;
- `task.wait`, `task.sleep`, `task.yield`, blocking channel wait, scheduler hand-off и любые эквивалентные операции в `destroy!` запрещены;
- если это видно статически, компилятор обязан дать diagnostic;
- если это выясняется только на runtime, результат — `DestroySuspendError`.

Во время `destroy!` cancellation считается masked: запрос на отмену может быть помечен как pending, но наблюдается только после завершения terminal cleanup.

## 5. Нормативная семантика `memory.dealloc`

### 5.1. Preconditions

Перед немедленным deallocation runtime обязан проверить:

- ownership: объект должен быть разрушим из текущего strand;
- lifetime-kind: объект не должен быть `shareable` или обычным `sync`-primitive;
- pinning/borrowing: объект не должен быть закреплён внешним native/FFI кодом;
- runtime activity: hidden VM access after dealloc не допускается.

Если объект pinned или имеет активный foreign borrow, `memory.dealloc` обязана бросить `PinnedObjectError` и не менять состояние объекта.

### 5.2. Dealloc path

Если preconditions соблюдены, `memory.dealloc(obj)` обязана выполнить такую наблюдаемую последовательность:

1. если объект `live` — выполнить `destroy!`-semantics;
2. удалить или занулить все исходящие ссылки из payload;
3. освободить storage объекта, включая out-of-line slot storage, array/map backing store и другие managed buffers, принадлежащие payload;
4. заменить runtime-shape объекта на канонический `DeadShape`;
5. оставить минимальный tombstone-header до тех пор, пока сам reference на объект ещё достижим;
6. пометить lifetime-state как `deallocated`;
7. вернуть `true`, если это была первая реальная deallocation.

Если при шаге 1 деструктор бросил исключение, runtime всё равно обязана завершить шаги 2–6, а затем повторно бросить сохранённое исключение. Это ключевое требование для сценария «очистить память прямо сейчас».

### 5.3. Tombstone-модель вместо висячих ссылок

Amber v1 намеренно **не** допускает unmanaged dangling references внутри managed runtime. Поэтому immediate deallocation не означает, что все существующие ссылки превращаются в raw-invalid pointer. Вместо этого:

- heap-header объекта превращается в tombstone;
- пользовательский payload освобождается немедленно;
- любые последующие попытки доступа проходят через dead-check и получают `UseAfterFreeError`.

Таким образом пользователь действительно освобождает память объекта «прямо сейчас», но runtime остаётся memory-safe на уровне управляемого кода.

### 5.4. Что именно считается освобождённой памятью

Под `memory.dealloc` нормативно понимается освобождение:

- slot storage экземпляра;
- backing buffers массивов, словарей, строк и других builtin containers;
- out-of-line payload пользовательских runtime-типов;
- дополнительных managed-buffers, принадлежащих payload.

Если объект владеет **внешней** памятью (native handle, mmap, GPU buffer, foreign pointer), runtime освобождает такой ресурс через пользовательский `destroy!` или builtin-type teardown hook. Сам `memory.dealloc` отвечает за managed payload объекта, а не за произвольный внешний ресурс без деструктора.

## 6. Heap header, `DeadShape` и object layout

Эта часть уточняет ранее введённый `ObjHeader` из reference VM. Минимально наблюдаемая модель теперь такая:

```text
ObjHeader(
 class_ptr,
 flags, # frozen / shareable / sync / pinned / has_destructor / dead / etc.
 owner_token,
 shape_ptr,
 gc_meta,
 lifetime_state,
 payload_ptr?
)
```

`shape_ptr` для обычного живого объекта указывает на normal shape, а для deallocated-объекта обязан указывать на канонический `DeadShape`.

```text
DeadShape(
 shape_id = 0,
 shape_version = 0,
 dead = true
)
```

Следствия:

- fast-path `LOAD_IVAR/STORE_IVAR` обязаны проверять dead-state **до** использования обычного shape-cache;
- dead-object никогда не может корректно удовлетворить обычный ivar cache hit;
- tombstone-header может хранить class/debug cookie для качественных ошибок и tooling, но не обязан хранить прежний payload.

## 7. Shape transitions, storage growth и shrinking без GIL

### 7.1. Рост layout при появлении нового ivar

При slow-path записи в ранее отсутствующий ivar reference runtime выполняет:

1. lookup/создание нового shape;
2. выделение нового slot storage нужного размера;
3. копирование старых слотов;
4. запись нового значения в рассчитанный slot;
5. swap `shape_ptr`;
6. освобождение старого storage.

Так как ordinary mutable objects strand-confined, всё это может делаться без object-level mutex и без GIL.

### 7.2. Сжатие layout

Amber v1 **не требует** обязательного runtime shrinking layout при удалении ivar или после `destroy!`. Реализация вправе оставить shape как есть до `memory.dealloc`, где payload уже освобождается полностью.

### 7.3. Dead object и caches

После `memory.dealloc`:

- все send/ivar inline caches обязаны уходить в dead-check error path;
- `shape_version` живого shape больше не релевантен;
- `DeadShape` не подлежит обычной user-visible mutation.

## 8. Allocator profile для no-GIL runtime

Reference runtime должен допускать реализацию без глобального interpreter lock. Минимально совместимый allocator profile таков:

- small/medium объекты аллоцируются из per-worker size-class arenas;
- large objects идут в отдельный large-object-space;
- fast-path allocation не должен требовать global lock;
- освобождение памяти может использовать remote-free queues, если strand мигрировал между worker'ами и возвращает память не в тот arena-owner, где объект был создан;
- ownership объекта определяется strand'ом, а не worker'ом, поэтому миграция strand между worker'ами не должна ломать lifetime semantics.

Эта часть не фиксирует конкретный allocator algorithm, но фиксирует наблюдаемое свойство: immediate deallocation и ordinary allocation должны быть совместимы с реальным параллелизмом без GIL.

## 9. Взаимодействие с GC

### 9.1. Что GC делает, а чего не делает

После этой редакции роль GC фиксируется так:

- GC собирает **недостижимые live/destroyed managed-объекты**;
- GC не вызывает пользовательский `destroy!`;
- GC может позже освободить сам tombstone-header после того, как на него больше нет ссылок;
- GC не имеет права «оживлять» destroyed/deallocated объект и не имеет права отменять terminal lifetime transition.

### 9.2. Tombstone как leaf-object

После `memory.dealloc` объект обязан стать leaf с точки зрения tracer'а:

- outgoing references severed;
- remembered-set/card-table запись для него либо удалена, либо считается чистой;
- root scanning не должен видеть внутри tombstone прежний payload-граф.

### 9.3. Safepoints и stop-the-world

`memory.dealloc` и `destroy!` считаются GC-visible операциями. Stop-the-world collector по-прежнему допустим как implementation choice, но:

- ordinary execution между safepoints не сериализуется глобальным GIL;
- dealloc/destroy semantics остаются детерминированными относительно program order текущего strand;
- cancellation во время terminal cleanup откладывается до завершения cleanup-sequence.

## 10. Bytecode и lowering hooks для lifetime операций

### 10.1. Новые инструкции ISA

Reference bytecode ISA v1 расширяется двумя обязательными lifecycle-инструкциями:

| Инструкция | Семантика |
|---|---|
| `OBJ_DESTROY dst, obj` | выполнить terminal `destroy!`-semantics, вернуть `true/false`, при ошибках бросить исключение |
| `OBJ_DEALLOC dst, obj` | выполнить немедленный deallocation path с tombstoning и вернуть `true/false` |

`OBJ_DEALLOC` обязан быть safepoint-visible и exception-visible. `OBJ_DESTROY` обязан уважать masked-cancellation rule деструктора.

### 10.2. Lowering rules

Нормативный lowering:

- exact surface call `obj.destroy!()` должен lower'иться в lifecycle-intrinsic `HDestroy(obj)` и далее в `OBJ_DESTROY`, если компилятор знает, что речь идёт именно о special lifecycle selector;
- `memory.dealloc(obj)` должен lower'иться в `HDealloc(obj)` и далее в `OBJ_DEALLOC`;
- fallback через обычный `SEND` допустим только если реализация доказывает полную семантическую эквивалентность, включая idempotence, destructor-chain и tombstone behavior.

## 11. Что эта часть закрывает, а что ещё оставляет открытым

Эта часть **закрывает** на уровне reference-спеки:

- explicit object lifetime state machine;
- `destroy!` как терминальный деструктор;
- `memory.dealloc` как немедленный deallocation primitive;
- tombstone-model без висячих ссылок в managed runtime;
- `DeadShape` и dead-check path для caches/ivar access;
- allocator envelope, совместимый с no-GIL execution.

Эта часть **оставляет открытым** как локальный lifetime-layer:

- декларативные field-level lifetime annotations вроде `owned`/`weak`;
- более широкий embedding/tooling policy поверх runtime memory model.

Collector/pinning profile и binary module format в текущей полной редакции уже закрываются следующими частями документа.

## 12. Практический следующий шаг после этой редакции

После этой версии следующий инженерный слой уже очень конкретен:

1. добавить в runtime header lifetime bits, `DeadShape` и tombstone checks;
2. реализовать `OBJ_DESTROY` и `OBJ_DEALLOC`;
3. реализовать per-worker allocator с remote-free queues и large-object path;
4. прогнать conformance tests на use-after-free, double-destroy, double-dealloc и owner-strand violations;
5. затем фиксировать collector/pinning и compiled-module profile — именно это и делается в следующих частях текущей редакции.


# Часть VIII. Reference collector, pinning, borrowing boundary и FFI profile v1

## 1. Статус и выбор reference collector

Эта часть закрывает reference collector/pinning/FFI profile для Amber v1. После неё вопрос "какой именно GC и как он сочетается с `destroy!` / `memory.dealloc` / no-GIL?" больше не считается открытым на уровне эталонной архитектуры.

Для Amber v1 фиксируется такой выбор:

- collector **non-moving** для всех user-visible heap-объектов;
- collector **generational**, но не за счёт обязательного перемещения объектов;
- базовая стратегия — **mark/sweep over arenas/regions** с remembered sets и поколениями;
- ordinary execution остаётся без GIL;
- пользовательский `destroy!` не вызывается collector'ом автоматически;
- `memory.dealloc` остаётся детерминированной terminal-операцией и не подменяется GC.

Причины выбора именно non-moving профиля:

- он лучше согласуется с tombstone-model и immediate dealloc;
- он проще сочетается с pinning и native interop;
- stable object identity полезна для no-GIL runtime, inline caches и отладки;
- он не закрывает путь к будущему moving/JIT-профилю, но даёт надёжный reference runtime уже сейчас.

Нормативный вывод: reference runtime Amber v1 обязан быть наблюдаемо эквивалентен **non-moving generational mark/sweep runtime**. Реализация может использовать более агрессивные оптимизации, только если они не ломают этот контракт.

## 2. Пространства памяти и region model

### 2.1. Классы heap-space

Reference runtime различает как минимум такие пространства:

1. **Immortal / metadata space**
 - `BcModule`, `BcCode`, `BcMethod`, interned symbols, class metadata, frozen descriptor objects;
 - не собирается обычным object collector'ом.

2. **Confined young space**
 - новые ordinary mutable объекты;
 - оптимизирован под быстрые аллокации;
 - логически относится к strand-confined данным.

3. **Confined mature space**
 - долгоживущие strand-confined объекты;
 - собирается тем же non-moving collector'ом, но реже.

4. **Shared / sync space**
 - shareable и sync-объекты;
 - собирается отдельными shared-cycle правилами.

5. **Large object space**
 - большие объекты выше implementation threshold;
 - non-moving по определению;
 - `memory.dealloc` для них обязана немедленно возвращать крупный payload allocator'у или LOS free-list.

### 2.2. Per-worker fast path и реальный owner

Ранее в документе уже зафиксированы per-worker arenas. Эта часть уточняет их статус:

- fast allocation path действительно может быть **per-worker**;
- но семантическое владение объектом определяется **не worker'ом, а owner_token / strand ownership**;
- миграция strand между worker'ами не меняет ownership существующих объектов;
- remote-free queues и allocator handoff разрешены именно поэтому.

Иначе говоря, worker-local allocator — это performance detail, а не источник языковой семантики.

### 2.3. Размещение по умолчанию

Нормативно:

- обычный mutable экземпляр класса создаётся в confined young space текущего strand;
- shareable/frozen объект создаётся либо в shared space, либо в immortal/metadata space;
- `Channel`, `Mutex`, `Atomic`, `TaskHandle` и прочие `sync`-объекты создаются в shared/sync space;
- explicit tombstone после `memory.dealloc` остаётся в том же region family, пока на него ещё есть ссылки.

## 3. Поколения и GC-циклы

### 3.1. Young / mature policy

Amber v1 остаётся generational runtime, но поколение задаётся не обязательным перемещением, а **age/meta policy**:

- новый confined object стартует как young;
- после достаточного количества пережитых локальных GC-cycles объект может быть помечен mature;
- promotion не обязана менять адрес объекта.

Это важно: "generational" в Amber v1 означает раздельные политики и remembered sets, а не обязательно copying nursery.

### 3.2. Локальный confined-cycle

Для confined-объектов reference runtime обязан уметь запускать **local GC cycle**, не останавливая весь мир:

- cycle таргетирует confined young/mature regions конкретного owner-strand;
- root set состоит из stack/frame/upvalue roots этого strand, его task queue, suspend-state, pending wake records и strand-local runtime structures;
- другие strand'ы не обязаны останавливаться, потому что легальных сильных ссылок на confined graph у них быть не должно.

Если illegal sharing всё же произошло из-за бага runtime/native кода, это считается нарушением embedding contract, а не нормальной частью модели.

### 3.3. Shared-cycle

Для shared/sync graph reference runtime обязан иметь **shared GC cycle**:

- он может быть concurrent mark/sweep;
- он может начинаться и заканчиваться коротким safepoint-handshake всех worker'ов;
- но ordinary execution между handshake-фазами не должно превращаться в GIL-like serial execution.

Shared-cycle обязан видеть как корни:

- все task stacks всех strand'ов;
- module/class/global roots;
- sync-object internal queues;
- pin tables;
- native registered handles;
- runtime-owned pending exception/cancellation records.

### 3.4. Почему moving collector не является reference v1

Moving collector теоретически совместим с языком, но **не** является reference profile этой редакции. Следовательно:

- reference verifier/runtime tests не должны предполагать object relocation;
- pinning ABI строится вокруг already-stable addresses, а не вокруг "pin only to disable moving" semantics;
- future moving/JIT runtime допустим только как дополнительный профиль, а не как изменение наблюдаемой v1-базы.

## 4. Барьеры записи, remembered sets и root rules

### 4.1. Write barrier

Любая операция, эквивалентная записи heap-reference в heap-object (`STORE_IVAR`, element store в builtin collections, runtime field mutation), обязана проходить через write barrier.

Минимальный эффект barrier:

- если old/mature object начинает ссылаться на young object — remembered set/card marking обновляется;
- если shared object начинает ссылаться на confined object — runtime обязан либо запретить запись, либо бросить `IsolationError`;
- если target-объект `destroyed`/`deallocated`, запись запрещена и должна завершаться lifetime error до того, как barrier сочтёт её успешной.

### 4.2. Root categories

Нормативный минимум корней для tracer'а:

- все `Frame`-ы и их `regs[]`, `self`, `block`, `last_result`;
- open upvalues / capture-cells;
- текущие scheduler queues и sleep/timer structures;
- `Channel` buffers и sync-object queues;
- module/class/global tables;
- active `PinToken` registry;
- runtime-owned temporary handles, которыми loader/native interop удерживает объекты.

### 4.3. Tombstone как не-источник новых ссылок

Tombstone-object не имеет права быть источником новых графовых ребер:

- после `memory.dealloc` его payload уже severed;
- tracer может считать tombstone leaf-object;
- remembered set для него либо очищается, либо помечается как пустой.

## 5. Safepoints, GC-handshake и cancellation

### 5.1. Обязательные safepoints

К уже зафиксированным safepoint-инструкциям добавляется нормативное runtime-правило: slow allocation path, GC entry/exit и pin/unpin slow path тоже считаются safepoint-visible участками.

Минимум safepoints обязаны существовать на:

- back-edge цикла;
- `SEND` / `CALL`;
- blocking wait / sleep / channel operations;
- allocation slow path;
- `OBJ_DESTROY` / `OBJ_DEALLOC`;
- вход/выход shared GC handshake.

### 5.2. Отношение к cancellation

GC не может асинхронно разорвать критическую lifetime-последовательность. Следовательно:

- если task находится внутри terminal cleanup (`destroy!` / `memory.dealloc`), cancellation остаётся pending;
- если task находится внутри pin/unpin runtime critical path, cancellation тоже откладывается до консистентной точки;
- после выхода из этой точки pending cancellation проверяется обычным способом.

## 6. Pinning profile v1

### 6.1. Surface API

Amber v1 вводит минимальный pinning API:

```amber
pin = memory.pin(obj)...
memory.unpin(pin)
```

и block-scoped helper:

```amber
memory.pinned(obj) |pin|:
 ffi.use(pin)
```

Нормативно:

- `memory.pin(obj)` возвращает `PinToken`;
- `memory.unpin(pin)` возвращает `true`, если реально снял активный pin, иначе `false`;
- block-form обязан автоматически выполнять `unpin` через `ensure`-эквивалент.

### 6.2. `PinToken`

Минимальная abstract-форма:

```text
PinToken(
 object_ref,
 pin_epoch,
 view_kind,
 permissions,
 owner_info
)
```

`PinToken` нужен не только чтобы держать объект живым, но и чтобы:

- не допустить stale-unpin чужого/устаревшего pin;
- различать opaque handle vs pinned buffer/span view;
- проверять правильность использования на native boundary.

### 6.3. Что делает pin

Pin **не** меняет ownership mode и **не** делает объект shareable. Его эффект другой:

- объект гарантированно считается live для GC, пока активен pin;
- `destroy!` и `memory.dealloc` над pinned-объектом запрещены и дают `PinnedObjectError`;
- runtime обязан сохранить валидность того view, который выдан pin-токеном.

Так как reference collector non-moving, pin не нужен для "запрета relocation". Он нужен для запрета reclamation/deallocation и для фиксации native view contract.

### 6.4. Opaque pin vs buffer pin

Amber v1 различает два наблюдаемых режима pinning:

1. **opaque pin**
 - для обычного объекта;
 - native код получает только opaque handle;
 - layout ivar/storage runtime-private и не обещается внешнему миру.

2. **buffer/span pin**
 - для builtin contiguous types (`Bytes`, `ByteBuffer`, future typed buffer family);
 - native код может получить pointer + length / span-view;
 - resize/realloc такой структуры во время активного pin запрещён.

Это важная граница: pinning **не** означает, что пользовательский объект можно безопасно читать как C-struct по offset'ам.

### 6.5. Ограничения pinning

Нормативно:

- pin разрешён только для `live`-объекта;
- pin над `destroyed`/`deallocated` объектом обязан бросать `UseAfterFreeError` или `LifetimeError`;
- `sync`-объекты могут иметь специальный builtin pin-policy, но ordinary `memory.pin` не обязана его поддерживать;
- double-unpin не ломает runtime и просто возвращает `false`.

## 7. Native interop / FFI boundary v1

### 7.1. Что может пересекать native boundary

Reference FFI profile разрешает три класса значений:

1. **immediate values**
 - `null`, bool, small numeric immediates, symbols и другие non-heap immediates.

2. **opaque managed handles**
 - обычные heap-объекты без обещания layout.

3. **pinned spans / buffers**
 - только для явно поддержанных contiguous builtin types.

Передача произвольного user-object как raw address его ivar-storage в v1 **запрещена**.

### 7.2. Правило владения на native boundary

Native код обязан уважать Amber ownership discipline:

- confined object нельзя трогать из чужого OS-thread или чужого strand-context;
- если native callback приходит из внешнего потока, он обязан re-enter runtime через strand-aware entrypoint;
- "просто сохранить указатель и потом дернуть объект с другого потока" — нарушение ABI.

### 7.3. Lifetime и native код

Native код не имеет права:

- хранить raw pointer/span после `unpin`;
- вызывать `memory.dealloc` косвенно из-под активного pin;
- обходить tombstone checks или ownership checks через cached address.

Если host/native слой нарушает эти правила, это уже outside Amber managed safety contract.

### 7.4. Cancellation и foreign calls

Blocking foreign call не считается автоматически прерываемой точкой. Нормативно:

- pending cancellation становится видимой при возврате из foreign call;
- либо native код обязан явно вызывать runtime poll-hook, если хочет быть cooperatively cancellable;
- асинхронное прерывание чужого C-frame Amber v1 не гарантирует.

### 7.5. GC и foreign roots

Если native код временно удерживает managed object, это должно быть оформлено одним из двух способов:

- через active `PinToken`;
- через registered runtime handle, который входит в GC root set.

Неформальный "сырой указатель где-то в стороне" не считается валидным способом удерживать объект живым.

## 8. Что эта часть закрывает, а что оставляет открытым

Эта часть закрывает на reference-уровне:

- конкретный collector family для Amber v1;
- правила generations без moving requirement;
- safepoint/handshake contract;
- pinning API;
- opaque-handle vs pinned-span границу для FFI;
- запрет implicit GC-finalizer semantics для пользовательского `destroy!`.

Эта часть оставляет открытым:

- weak refs / ephemerons;
- богатую typed-buffer ecosystem;
- surface borrow annotations;
- production embedding API высокого уровня.

## 9. Практический следующий шаг после этой части

Следующий инженерный шаг после фиксации collector/pinning profile предельно конкретен:

1. реализовать region/arena metadata и remembered sets;
2. реализовать local confined-cycle и shared-cycle;
3. добавить `PinToken` registry и `memory.pinned(...)` block helper;
4. собрать минимальный opaque-handle FFI bridge;
5. прогнать stress-tests на `destroy!`, `memory.dealloc`, pin/unpin и concurrent GC-handshakes.


# Часть IX. Формат `.amberbc`, loader/linker и verifier profile v1

## 1. Статус и граница решения

Эта часть закрывает вопрос compiled-module distribution profile для Amber v1. Source-level syntax `package` / `import` / `from... import...` / `export` уже зафиксирован выше, но loader/verifier по-прежнему работают не с поверхностным spelling, а с нормализованными logical module ids и export tables. Здесь фиксируется, как выглядит уже скомпилированный модуль и как он загружается runtime'ом.

Нормативный вывод:

- stable compiled artifact Amber v1 называется `.amberbc`;
- `.amberbc` описывает `BcModule` в сериализованном виде;
- loader/verifier обязаны работать с `.amberbc` независимо от того, был dependency записан как `import x.y` или `from x.y import Z`;
- debug info, dependency manifest и init-state machine входят в reference profile;
- reference toolchain использует **одну canonical physical encoding** `.amberbc`: writer/reader/verifier/disasm не имеют права выбирать между несколькими взаимозаменяемыми binary spellings одного и того же module graph.

## 2. Binary container v1

### 2.1. Header

Каждый `.amberbc`-файл обязан начинаться с platform-independent header:

```text
AmberBcHeader(
 magic = "ABM1",
 format_major,
 format_minor,
 language_major,
 language_minor,
 profile_flags,
 section_count,
 file_flags,
 abi_hash,
 header_crc?
)
```

Нормативно:

- byte order — little-endian;
- offsets — 64-bit;
- section sizes / counts — фиксированные little-endian `u32`;
- строки — UTF-8;
- raw host pointers в файле запрещены.

### 2.2. Section table

После header идёт section directory:

```text
SectionEntry(
 kind,
 offset,
 size,
 align,
 flags
)
```

Порядок секций в файле implementation-defined, но loader обязан опираться на directory, а не на физический порядок.

## 3. Обязательные и опциональные секции

### 3.1. Обязательные секции

Reference `.amberbc` обязан иметь как минимум:

- `STRS` — string pool;
- `SYMS` — interned selectors / symbol names / constant-path atoms;
- `KONS` — serializable constant pool;
- `CODE` — serialized `BcCode`;
- `METH` — serialized `BcMethod` descriptors;
- `CLAS` — class/runtime descriptors;
- `DEPS` — dependency manifest;
- `EXPT` — export table;
- `INIT` — module init entry metadata.

### 3.2. Опциональные секции

Допустимы такие optional sections:

- `PATS` — precompiled pattern programs / descriptors;
- `SPAN` — source spans;
- `LINE` — line table для stack traces;
- `LOCS` — local name/debug metadata;
- `ATTR` — compiler attributes / build metadata;
- `HASH` — extra section digests / signing hooks.

Отсутствие debug sections не должно ломать исполнение, но ухудшает tooling.

## 4. Constant / symbol / index model

### 4.1. Только индексы, не указатели

Все межсекционные ссылки в `.amberbc` обязаны выражаться через индексы/offset references, а не через сырые адреса процесса.

Следовательно:

- `BcMethod` ссылается на `BcCode` по `code_id`;
- селекторы и имена идут через `SYMS`;
- source/debug info привязаны к code/site ids;
- loader после mmap/read создаёт runtime pointers уже у себя, а не читает их из файла.

### 4.2. Ограничения constant pool

Секция `KONS` может содержать только serializable shareable constants:

- `null`, bool, numeric literals, symbols;
- frozen strings/bytes;
- tuples/records из serializable constants;
- symbol paths, key sets, descriptor records;
- ссылки на `CODE` и другие immutable descriptors.

Обычные mutable heap-объекты и strand-confined значения не могут сериализоваться в `KONS`.

### 4.3. Deterministic interning policy

Для reference writer'а фиксируется такая policy:

- `STRS`, `SYMS` и `KONS` интернируются в first-use order при детерминированном обходе `HModule`/`BcModule`;
- emitter/writer не имеют права зависеть от hash-iteration order, адресов процесса или nondeterministic map traversal;
- одинаковый HIR и одинаковый dependency graph обязаны давать одинаковые pools, одинаковые индексы и byte-identical `.amberbc`, если build metadata/profile flags совпадают.

## 5. Сериализация `BcCode`, `BcMethod`, `BcClass`

### 5.1. `CODE`

Каждая запись `CODE` обязана включать как минимум:

```text
CodeRecord(
 kind,
 reg_count,
 local_layout_id,
 capture_layout_id,
 instr_stream,
 handler_table_id,
 call_site_table_id,
 ivar_site_table_id,
 safepoint_table_id,
 flags
)
```

`instr_stream` обязан кодировать тот же semantic ISA, который зафиксирован в части про reference bytecode VM. Physical encoding для reference format фиксируется так:

- 1-byte opcode;
- все immediate operands кодируются только через `ULEB128` / `SLEB128`;
- альтернативный fixed-width encoding в v1 не вводится;
- decoder/verifier обязан трактовать любое отклонение от этой схемы как format error.

### 5.2. `METH`

Каждый method-record обязан включать:

```text
MethodRecord(
 selector_sym,
 owner_dispatch_ref,
 signature_blob_id,
 default_thunk_ids[],
 type_hook_ids[],
 clause_table_id?,
 auto_assign_desc_id?,
 entry_code_id,
 flags
)
```

### 5.3. `CLAS`

Каждый class-record обязан включать:

```text
ClassRecord(
 class_name_sym,
 superclass_ref?,
 ivar_schema_id,
 method_range,
 class_flags,
 class_init_code_id?
)
```

## 6. Loader state machine

### 6.1. Состояния модуля

Runtime loader обязан поддерживать такой минимум состояний:

```text
unloaded -> mapped -> verified -> linked -> initializing -> ready
 \-> failed
```

### 6.2. Семантика состояний

- `mapped` — файл прочитан или memory-mapped, header/sections доступны;
- `verified` — structural verifier успешно прошёл;
- `linked` — зависимости резолвнуты, runtime descriptors собраны;
- `initializing` — выполняется module init code;
- `ready` — экспортируемые сущности доступны;
- `failed` — модуль считается неуспешно загруженным и не переходит в `ready` без полной новой загрузки.

### 6.3. Module init

Каждый `.amberbc` может иметь module-init entrypoint. Нормативно:

- он исполняется **ровно один раз** на успешную загрузку модуля;
- выполняется в strand-aware loader context;
- если во время init происходит рекурсивный импорт того же модуля, наблюдается состояние `initializing`, а не повторный запуск;
- доступ к export, который ещё не был инициализирован до конца init-phase, обязан давать `ModuleInitError`, а не тихий `null`.

## 7. Dependencies, linking и export model

### 7.1. `DEPS`

Dependency manifest обязан содержать как минимум:

```text
DepEntry(
 module_name,
 required_format_major,
 min_language_version,
 max_language_version?,
 abi_requirement?,
 dep_flags
)
```

В source-level syntax зависимости уже пишутся как absolute module ids; в `.amberbc` они в любом случае нормализуются до logical module ids.

### 7.2. `EXPT`

Export table обязана задавать:

- экспортируемые class/function/module symbols;
- их symbol ids;
- runtime visibility flags;
- optional re-export metadata.

### 7.3. Linking и `world_epoch`

Loader/linker обязан взаимодействовать с уже описанным runtime invalidation model:

- если загрузка модуля меняет method table существующего класса, соответствующий `method_version` обязан обновиться;
- world-level mutation обязана bump'ать `world_epoch`;
- чистое подключение нового неизменяющего существующий мир модуля может ограничиться регистрацией нового export-root без open-class invalidation.

Это создаёт мост к будущему frozen/AOT profile.

## 8. Verifier contract v1

### 8.1. Что verifier обязан проверить

Минимальный verifier обязан проверять:

- корректность header и section boundaries;
- совместимость `format_major/minor` и `language_version`;
- что все индексные ссылки валидны;
- что `instr_stream` декодируется в допустимые инструкции;
- корректность jump targets и handler ranges;
- наличие safepoint на всех back-edge путях;
- корректность binding/temp slot ranges для `P_*`-программ;
- что `KONS` содержит только shareable/serializable values;
- что dependency manifest не нарушает format/version constraints.

### 8.2. Что verifier не обязан решать

Verifier **не** обязан статически доказывать:

- отсутствие всех runtime `IsolationError`;
- типовую корректность программы;
- отсутствие всех `MatchError`/`UseAfterFreeError`;
- безопасную логику пользовательских `destroy!`.

Эти вещи остаются за runtime semantics и higher-level tooling.

## 9. Debug info и stack trace contract

### 9.1. Минимум для production-runtime

Даже release `.amberbc` должен иметь достаточно информации, чтобы stack trace был диагностируемым. Минимум:

- module id;
- code id;
- line/span mapping хотя бы для call sites и exception ranges.

Canonical `disasm` для `.amberbc` обязан быть deterministic: sections и records печатаются в stable order, registers именуются `r0..`, locals — `l0..`, captures — `u0..`, а source spans при наличии debug sections рендерятся только в canonical comment form.

### 9.2. Богатый debug-профиль

Если присутствуют `SPAN`, `LINE`, `LOCS`, runtime/tooling обязаны уметь:

- строить source-level stack traces;
- подсвечивать текущий span;
- показывать selector/site ids;
- при debug-build — отображать имена локалов и аргументов.

## 10. Cross-version и compatibility rules

### 10.1. Формат

- несовместимый `format_major` -> loader reject;
- больший `format_minor` может быть принят только если loader заявляет forward-compat support;
- `profile_flags`, которых runtime не знает, обязаны приводить к reject, а не к тихому игнору.

### 10.2. Язык

- модуль, скомпилированный под более новую языковую семантику, чем понимает runtime, должен быть отвергнут;
- debug sections и `ATTR` могут расширяться без ломки базового исполнения, если directory и major-version это допускают.

## 11. Что эта часть закрывает, а что оставляет открытым

Эта часть закрывает:

- stable compiled artifact `.amberbc`;
- section model;
- loader state machine;
- dependency/export metadata;
- минимальный verifier contract;
- debug-info envelope для stack traces и tooling.

Эта часть оставляет открытым:

- source-level package syntax;
- package registry/distribution policy;
- signing/trust chain;
- hot reload и incremental compilation semantics.

## 12. Практический следующий шаг после этой части

Следующий инженерный шаг после фиксации `.amberbc` profile такой:

1. реализовать serializer/deserializer для `BcModule`;
2. реализовать load-time verifier;
3. реализовать dependency resolver и module init state machine;
4. добавить debug sections и line mapping;
5. прогнать corpus-тесты на round-trip `HIR -> bytecode ->.amberbc -> load -> run`.


# Часть X. Minimal MOP, reflective dispatch и frozen-world profile v1

## 1. Статус и граница решения

Эта часть закрывает для Amber три старых узла:

1. **минимальный MOP / reflective dispatch profile**;
2. **mixin/`include` profile**;
3. **границу между dynamic Amber и Amber/Frozen для AOT/JIT**.

Решение намеренно консервативное. Оно **не** пытается копировать полный Ruby-MOP. Вместо этого фиксируется минимальный профиль, достаточный для:

- reopenable named classes и named mixins;
- позднего добавления/замены методов;
- declarative `include` с deterministic linearization;
- reflective send;
- корректного `method_missing` fallback;
- чёткой frozen-boundary для компилируемых модулей.

Все более широкие темы — class-side mixins/`extend`, hot reload, расширенная рефлексия, method alias/removal и package distribution policy — сознательно остаются за пределами этой части.

## 2. Объём минимального MOP 

Включено:

- named class create/reopen;
- atomic class-body commit;
- reflective `define_method`;
- builtin `send(receiver, selector,...)`;
- `method_missing` fallback;
- world-mutation model;
- frozen-world transition;
- named mixins;
- declarative `include` с linearized lookup order.

Не включено:

- class-side mixins / `extend`;
- reflective remove/alias/visibility hooks;
- общий introspection API (`methods`, `ancestors`, source locations, owner lookup и т.п.);
- hot reload;
- позднее добавление clause к уже существующему методу через reopen/`define_method`.

## 3. Named class forms и open classes

Нормативная семантика именованной `class`-формы такая:

```amber
class User:
 def full_name(): "#{@first} #{@last}"

class User:
 def admin?(): false
```

### 3.1. Create vs reopen

Когда исполняется `class Name...: body`:

- если в текущем lexical owner имя `Name` ещё не связано, создаётся новый class object;
- если имя уже связано с class object, форма означает **reopen** этого класса;
- если имя связано, но не с class object, это ошибка (`TypeError` либо более точная диагностируемая ошибка конкретной реализации).

### 3.2. Superclass rule

Superclass clause `class Name < Base:`:

- допустим на первоначальном объявлении;
- при reopen либо опускается, либо обязан резолвиться в тот же superclass;
- несовместимость даёт `SuperclassMismatchError`;
- реализация вправе диагностировать такой случай ещё на compile/link phase, если он статически прозрачен.

### 3.3. Class-body scope

В минимальном class-body нормализуется как **declarative body**. Для этого профиля нормативно существенны:

- `def`;
- `class_method def`;
- nested `class`;
- nested `mixin`;
- `include`;
- `pass`.

Эта часть **не** вводит произвольный исполняемый MOP-код в class-body как отдельный источник новой семантики.

### 3.4. Atomic commit

Один syntactic class-body образует одну world-mutation transaction:

- методы, определённые внутри body, не обязаны становиться видимыми по одному;
- при успешном завершении body изменения коммитятся атомарно;
- при ошибке выполнения/линковки partial commit запрещён.

### 3.5. Relation to multi-clause `def`

Поздний reopen не добавляет clause к уже существующему методу.

Нормативно:

- same-selector `def`-группы внутри одного syntactic body продолжают собираться по обычным правилам clause-style `def`;
- отдельный reopen или отдельный `define_method` **заменяет** целый method entry для selector'а;
- dynamic clause accretion across reopen boundaries в отсутствует.

## 4. Named mixins и `include`

Amber добавляет минимальный mixin-profile без возврата к runtime module/import semantics.

```amber
mixin Timestamped:
 def touch!():
 @updated_at = clock.now()

class User:
 include Timestamped
```

### 4.1. Create vs reopen

Когда исполняется `mixin Name: body`:

- если в текущем lexical owner имя `Name` ещё не связано, создаётся новый mixin object;
- если имя уже связано с mixin object, форма означает **reopen** этого mixin'а;
- если имя связано, но не с mixin object, это ошибка (`TypeError` либо более точная диагностируемая ошибка конкретной реализации).

### 4.2. Mixin-body scope

В минимальном mixin-body нормализуется как declarative body. Нормативно существенны:

- `def`;
- nested `class`;
- nested `mixin`;
- `include`;
- `pass`.

`class_method def` внутри mixin body не входит в v1 и должен диагностироваться как compile-time error.

### 4.3. Include contract

`include` разрешён только внутри declarative body класса или mixin'а.

Контракт:

- каждый operand обязан резолвиться в mixin object;
- `include` влияет только на instance-side lookup;
- `include` не импортирует lexical names и не подменяет `package` / `import`;
- repeated include идемпотентен;
- cycles запрещены и дают `IncludeCycleError`.

### 4.4. Linearization rule

Lookup использует такой порядок:

1. local method table owner'а;
2. direct includes справа налево по source order;
3. для каждого mixin — сначала сам mixin, затем его includes в том же nearest-first depth-first порядке;
4. после этого superclass chain.

Это даёт предсказуемое правило: **later include wins; local class method wins over any mixin; superclass идёт после current class + includes**.

### 4.5. Dispatch relevance

Создание/reopen mixin'а и изменение direct include-set считаются world mutation и обязаны:

- публиковаться атомарно;
- инвалидировать lookup caches/version guards;
- уважать frozen-boundary.

## 5. Reflective `define_method`

Reflective late method definition задаётся builtin-функцией:

```amber
define_method(User,:greet) |name|:
 "Hello, #{name}"
```

или

```amber
define_method(User,:greet, fn_obj)
```

### 5.1. Resolution rule

Специальная семантика действует только если имя `define_method` резолвится в builtin prelude binding. Если имя затенено локальной переменной, import-alias или параметром, форма становится обычным вызовом функции.

### 5.2. Contract

Минимальный контракт:

- первый аргумент обязан быть class object или mixin object;
- второй аргумент обязан быть `Symbol` или `Str`;
- реализация метода задаётся либо block suffix, либо явным callable-аргументом, но не обоими одновременно;
- в `define_method` воздействует только на **instance-side** target class/mixin.

### 5.3. Implementation source

Если используется block-form:

- сигнатура создаваемого метода берётся из параметров блока;
- call context предоставляет обычный `self`;
- keyword/default-rich reflective signatures специально не расширяются сверх того, что уже выражает сама callable-форма.

Если используется explicit callable object:

- runtime обязан оборачивать его в synthetic method entry с эквивалентным callable-contract;
- конкретное внутреннее представление (`BcMethod`-оболочка, closure-wrapper и т.п.) остаётся реализационной деталью, пока соблюдается surface-semantics.

### 5.4. Replacement semantics

Успешный `define_method`:

- создаёт новый method entry, если selector отсутствовал;
- заменяет существующий method entry, если selector уже был;
- не добавляет late clause к существующему multi-clause методу.

## 6. Reflective `send`

Reflective send задаётся builtin-функцией:

```amber
send(user,:full_name)
send(user, selector, a, b)
```

### 6.1. Resolution and arguments

Специальная семантика действует только если имя `send` резолвится в builtin prelude binding.

Контракт:

- первый аргумент — receiver;
- второй аргумент — selector (`Symbol` или `Str`);
- оставшиеся positional/keyword-аргументы и block suffix передаются как у обычного метода.

### 6.2. Semantic equivalence

После успешной резолюции selector'а вызов обязан следовать той же dispatch-semantics, что и ordinary `receiver.method(...)`:

- тот же lookup path;
- те же guard/exception rules;
- тот же `method_missing` fallback;
- тот же block forwarding.

### 6.3. Compilation rule

Если selector известен как compile-time literal `Symbol`/`Str`, lowering вправе понизить форму сразу в `HSend` / `SEND`.

Если selector не известен статически, lowering обязан использовать reflective форму `HSendDyn` / `SEND_DYN`.

Язык не требует, чтобы AOT/JIT де-виртуализовал такой участок. Generic reflective path — допустимая и нормативно достаточная реализация.

## 7. `method_missing`

### 7.1. Miss protocol

Для обычного method call и для reflective `send(...)` действует следующий miss-protocol:

1. выполнить обычный lookup;
2. если target найден — вызвать его;
3. если target не найден — попробовать найти selector `method_missing`;
4. если `method_missing` найден — вызвать его с первым positional-аргументом = missing selector (`Symbol`), затем со всеми исходными аргументами и тем же block context;
5. если `method_missing` тоже не найден — бросить `NoMethodError`.

### 7.2. Non-recursive rule

`method_missing` сам не получает ещё один fallback к `method_missing`, если lookup этого selector'а тоже не удался. Это разрывает потенциальную бесконечную рекурсию на уровне language contract.

### 7.3. Dispatch relevance

Изменение поведения `method_missing` считается dispatch-relevant world mutation и обязано инвалидировать соответствующие caches/version guards.

## 8. World mutation model

Amber различает обычную мутацию данных и мутацию самого dispatch-world.

### 8.1. Что считается world mutation

World mutation — это любая операция, меняющая граф dispatch/lookup:

- создание нового named class object;
- создание нового named mixin object;
- reopen существующего класса или mixin'а;
- `define_method`;
- `include`, меняющий direct include-set;
- изменение fallback-policy (`method_missing`);
- загрузка Amber-модуля в уже frozen dispatch-world.

### 8.2. Что не считается world mutation

Не считаются world mutation:

- присваивания в `@field` / `@@field`;
- обычные изменения контейнеров и других данных;
- allocation / GC / `destroy!` / `memory.dealloc`;
- обычный reflective `send(...)`, если он сам не порождает world mutation;
- выполнение уже существующего метода.

### 8.3. Invalidation contract

При каждой успешной world mutation reference runtime обязан:

- обновить relevant `method_version` или эквивалентный per-dispatch version guard;
- bump'нуть `world_epoch` один раз на успешную transaction;
- не публиковать partial state при неуспехе операции.

Для source-level `class`/`mixin` reopen transaction весь body считается одной publish-point transaction. Для `define_method` transaction — это один вызов.

## 9. Frozen-world profile

### 9.1. World states

Dispatch-world имеет как минимум два состояния:

```text
open -> frozen
```

### 9.2. Dynamic Amber

Обычный dynamic Amber может оставаться в состоянии `open` сколько угодно долго. Это законный и полноценный режим языка.

### 9.3. Amber/Frozen

Amber/Frozen — это build/runtime profile:

1. source modules компилируются обычным фронтендом;
2. loader/linker/module-init исполняются при состоянии `open`;
3. после успешной инициализации выбранного набора модулей host/toolchain выполняет freeze transition;
4. после этого dispatch-world считается стабильным для optimizer/JIT/native AOT.

### 9.4. Post-freeze behavior

После freeze:

- world mutations запрещены;
- попытка reopen класса или mixin'а, `define_method`, `include`, меняющего ancestor graph, или поздней Amber module load в тот же world обязана завершаться `WorldFrozenError` либо быть отклонена ещё до выполнения;
- `send(...)` и `method_missing` остаются законными, но не считаются loophole для world mutation;
- ordinary data mutation остаётся разрешённой, если её не запрещают другие части языка/библиотеки.

### 9.5. Relation to optimizer

Frozen-boundary гарантирует только стабильность dispatch-world:

- method tables;
- class graph / superclass relations;
- fallback policy, влияющая на method lookup;
- отсутствие новых Amber-module mutations после freeze.

Она **не** означает автоматически immutability пользовательских данных. Следовательно:

- optimizer/AOT может полагаться на стабильность lookup;
- но не может из факта freeze делать вывод о замороженности `@field` или контейнеров данных без дополнительных оснований.

### 9.6. Deopt is optional

Язык не требует обязательного deopt-механизма.

Допустимы обе стратегии:

- реализация всегда держит reflective места (`SEND_DYN`, потенциальный miss -> `method_missing`) на generic path;
- реализация строит поверх этого JIT/deopt, не меняя language contract.

## 10. Compiler и runtime hooks

### 10.1. HIR

Для reflective send минимально требуется:

```text
HSendDyn(receiver, selector_expr, pos_args[], kw_args[], block?)
```

`selector_expr` обязан вычисляться ровно один раз и до самого dispatch.

### 10.2. Bytecode

Для bytecode VM минимально требуется:

```text
SEND_DYN dst, recv, selector_reg, argv_desc, block_reg, site_id
```

Нормативно:

- selector обязан быть `Symbol` или `Str`, иначе `TypeError`;
- ordinary literal case вправе использовать `SEND`.

### 10.3. No dedicated opcode for world mutation

`class`/`mixin`-reopen, `include` и `define_method` **не обязаны** иметь специальные ISA-opcodes.

Reference implementation вправе реализовывать их как:

- module-init/runtime intrinsics;
- privileged builtin calls;
- либо любой другой путь, который соблюдает:
 - atomic commit,
 - version invalidation,
 - correct include linearization,
 - `WorldFrozenError` guard после freeze.

## 11. Что эта часть закрывает, а что оставляет открытым

Эта часть закрывает:

- Q3 (глубина нормализации метапрограммирования);
- mixin/`include` profile v1;
- Q11 (граница dynamic vs frozen/AOT profile).

Эта часть оставляет открытыми:

- class-side mixins / `extend`;
- distribution/package-manager/hot-reload story;
- deeper reflection/introspection API;
- типовую систему и optimizer/backend;
- политику frozen-image packaging и deployment.

## 12. Практический следующий шаг после этой части

После фиксации minimal MOP/frozen-boundary следующий инженерный шаг уже не концептуальный, а реализационный:

1. добавить builtin-resolution для `send` и `define_method` в binder/HIR lowering;
2. добавить `mixin`/`include` lowering и ancestor-linearization tests;
3. ввести `HSendDyn` и `SEND_DYN`;
4. реализовать world-state (`open`/`frozen`), `world_epoch` и version invalidation;
5. закрыть open-class/open-mixin transaction path, `include` invalidation и `method_missing` fallback в VM/loader tests;


# Часть XI. Reference implementation blueprint P0/P1

## 1. Статус и назначение

Этот раздел **не переоткрывает** языковые решения и не вводит новых surface-feature'ов. Его цель — зафиксировать минимальный engineering baseline, при котором reference implementation можно начинать сразу и без повторного архитектурного дрейфа.

Нормативный смысл этой части:

- для **reference implementation** обязательны канонические dump-форматы AST / HIR / diagnostics;
- для **conformance suite** обязательны стабильные имена диагностик, layout golden-файлов и deterministic serialization policy;
- для **порядка реализации** фиксируется bootstrap order P0/P1, чтобы frontend, VM, loader и stdlib сходились к одному и тому же набору артефактов.

Эта часть намеренно не делает обязательными:

- точный внутренний язык реализации;
- точную файловую организацию репозитория;
- конкретный packaging/build system;
- выбор между monorepo и multi-crate/multi-package layout.

Но она делает обязательным то, что должно быть наблюдаемо снаружи: AST/HIR dumps, diagnostic codes, bytecode compile/run path и corpus runner.

## 2. Обязательные внешние артефакты reference implementation

Reference implementation v1 обязана уметь наблюдаемо выполнять следующие переходы:

```text
source(.am)
 -> parse
 -> AST dump
 -> HIR dump
 -> compile
 ->.amberbc
 -> load
 -> run
```

Минимальный внешний набор инструментов reference implementation:

1. `amberc` — frontend/compiler tool;
2. `ambervm` — standalone VM runner для `.amberbc`;
3. `ambertest` — runner conformance corpus;
4. `amberdis` — текстовый disassembler для `.amberbc` (может быть subcommand `amberc disasm`).

Допустима и иная упаковка, но reference distribution обязана обеспечивать следующие наблюдаемые возможности:

- получить syntax-faithful AST из source file;
- получить HIR после нормализации;
- получить machine-readable diagnostics;
- собрать `.amberbc`;
- запустить `.amberbc` вне compiler-process;
- прогнать corpus и сравнить результат с golden expectations.

## 3. Рекомендуемая репозиторная раскладка

Это **рекомендуемая**, но не нормативная layout-модель для reference repo:

```text
amber/
 compiler/
 lexer/
 parser/
 ast/
 binder/
 patterns/
 hir/
 bytecode/
 diagnostics/
 cli/
 runtime/
 vm/
 objects/
 scheduler/
 gc/
 ffi/
 loader/
 stdlib/
 core/
 collections/
 concurrency/
 tests/
 parser/
 binder/
 runtime/
 loader/
 scheduler/
 golden/
 tools/
 amberc
 ambervm
 ambertest
 amberdis
 docs/
```

Инварианты:

- frontend (`lexer/parser/ast/binder/hir`) обязан собираться и тестироваться независимо от VM;
- `.amberbc` reader/writer/verifier обязан быть тестируем отдельно от scheduler и GC;
- `stdlib` не должна содержать скрытых правил языка: её контракт обязан воспроизводить уже зафиксированную surface semantics, а не подменять её.

## 4. Канонические dump-форматы

### 4.1. AST dump contract

AST dump для golden-тестов должен быть deterministic JSON-документом следующего общего вида:

```json
{
 "format": "amber.ast.v1",
 "module": "... or null...",
 "items": [... ],
 "source_hash": "sha256:..."
}
```

Каждый AST node обязан содержать как минимум:

- `"kind"` — стабильное имя node-kind;
- `"span"` — объект `{ "file": "...", "start": { "line": N, "col": N }, "end": { "line": N, "col": N } }`;
- все semantic fields данного узла;
- все дочерние узлы в source-order.

AST dump **не имеет права** содержать:

- pointer-адреса;
- runtime-only cache ids;
- hash-iteration-dependent порядок ключей/элементов;
- неустойчивые autogenerated symbol names, если они не соответствуют source syntax.

Нормативно:

- одинаковый source при одинаковой версии frontend обязан давать побайтно одинаковый AST dump;
- trivia вроде комментариев и пробелов может не сериализоваться, если они не участвуют в семантике;
- `CHAIN_DOT` как лексическая техника не обязан жить отдельным AST-kind, но boundary one-liner chain должен быть восстановим из `AstPostfixChain` + `AstTailBlockSuffix`.

### 4.2. HIR dump contract

HIR dump для golden-тестов должен быть deterministic JSON-документом следующего общего вида:

```json
{
 "format": "amber.hir.v1",
 "module": "... or null...",
 "procedures": [... ],
 "constants": [... ],
 "source_hash": "sha256:..."
}
```

Каждая `procedure` обязана содержать:

- `"name"`;
- `"signature"`;
- `"locals"`;
- `"captures"`;
- `"blocks"` либо `"body"` в стабильной структурной форме;
- `"spans"` для debug/diagnostic round-trip.

Нормативно:

- HIR обязан уже сделать явными `HLastGet/HLastSet`, safe-nav lowering boundary, clause-dispatch structure, pattern-dispatch nodes, builtin-lowering для `send`/task intrinsics и declarative object-model items (`HInclude`, `HMixin`, `HClass`);
- одинаковый AST при одинаковых lowering-rules обязан давать побайтно одинаковый HIR dump;
- synthetic local names и temporary ids обязаны нумероваться стабильно в source-order, а не по адресам объектов памяти.

### 4.3. Diagnostic dump contract

Machine-readable diagnostics для compiler/test runner должны сериализоваться в deterministic JSON следующего общего вида:

```json
{
 "format": "amber.diag.v1",
 "diagnostics": [
 {
 "code": "E1001",
 "severity": "error",
 "phase": "binder",
 "message": "...",
 "primary_span": {... },
 "related": [
 { "label": "...", "span": {... } }
 ],
 "notes": [ "..." ]
 }
 ]
}
```

Нормативно:

- diagnostics обязаны сортироваться по `primary_span.start`, затем по `code`;
- `code` обязателен для всех compile errors и обязательных warnings;
- textual `message` может эволюционировать, но `code`, `severity`, `phase` и span structure должны оставаться стабильными в пределах major-version corpus;
- при наличии нескольких связанных мест компилятор обязан выдать как минимум один `primary_span` и ноль или более `related` spans.

## 5. Канонический каталог диагностик v1

Ниже фиксируются **обязательные** коды диагностик reference compiler v1. Текст сообщений может различаться, но код и семантика обязаны совпадать.

### 5.1. Pattern / binder / signature diagnostics

| Code | Severity | Phase | Условие |
|---|---|---|---|
| `E1001` | error | pattern | duplicate binding names in one pattern |
| `E1002` | error | pattern | different binding sets across OR-pattern alternatives |
| `E1003` | error | pattern | `*rest` / `**rest` вне tail-position |
| `E1004` | error | binder | ambiguous clause subject |
| `E1005` | error | parser | mixing `_1/_2/...` with explicit block params |
| `E1006` | error | parser | sparse placeholder numbering |
| `E1007` | error | binder | default expression refers to a parameter to the right |
| `E1008` | error | pattern | bare matcher expression вне `case` / `case!` |
| `E1009` | error | pattern | dynamic pattern object в block params или pattern assignment |
| `E1010` | error | pattern | `pattern(expr)` exports bindings without `with MAP_PATTERN` |

### 5.2. Module / import / export diagnostics

| Code | Severity | Phase | Условие |
|---|---|---|---|
| `E2001` | error | parser | duplicate `package` declaration |
| `E2002` | error | parser | import outside contiguous import-zone |
| `E2003` | error | parser | `from... import *` forbidden |
| `E2004` | error | parser | relative imports forbidden in v1 |
| `E2005` | error | binder | export of unknown local name |
| `E2006` | error | binder | duplicate public export |
| `E2007` | error | binder | assignment to imported alias |

### 5.3. Object model / mixin / reopen diagnostics

| Code | Severity | Phase | Условие |
|---|---|---|---|
| `E3001` | error | parser | `include` outside declarative class/mixin body |
| `E3002` | error | binder | statically-provable non-mixin include target |
| `E3003` | error | binder | statically-provable include cycle |
| `E3004` | error | parser | `class_method def` inside mixin body |
| `E3005` | error | binder | reopen with incompatible superclass |
| `E3006` | error | binder | reopen mixin/class name collides with binding of different kind |
| `E3007` | error | parser | `extend` outside declarative class body |
| `E3008` | error | binder | statically-provable non-mixin extend target |

### 5.4. Обязательные warnings

| Code | Severity | Phase | Условие |
|---|---|---|---|
| `W1001` | warning | binder | default-expression reads `@field`, while the signature also contains delayed auto-assign to the same field |

### 5.5. Рекомендуемые lint-codes

Lint-слой не является частью language acceptance, но для reference toolchain рекомендуются стабильные коды:

| Code | Severity | Phase | Условие |
|---|---|---|---|
| `L1001` | lint | style | underscore-lookalike identifier near `_`, `$_`, `_1`... |
| `L1002` | lint | style | unused import |
| `L1003` | lint | style | excessively fragmented reopen of the same class across many files |

## 6. Канонический layout golden- и corpus-файлов

Для каждого тестового кейса reference corpus рекомендует следующий layout:

```text
case_name/
 main.am
 expect.ast.json
 expect.hir.json
 expect.diag.json
 expect.out.txt
 expect.err.txt
 expect.dis.txt
 meta.json
```

Правила:

- `main.am` обязателен всегда;
- `expect.ast.json` обязателен для parser/golden кейсов;
- `expect.hir.json` обязателен для lowering/golden кейсов;
- `expect.diag.json` обязателен для negative compiler cases;
- `expect.out.txt` используется для успешного run-result;
- `expect.err.txt` используется для unhandled runtime error / stack trace;
- `expect.dis.txt` рекомендуется для `.amberbc`/disasm round-trip тестов;
- `meta.json` хранит phase и режим сравнения.

Минимальный `meta.json`:

```json
{
 "phase": "parse|lower|check|compile|run|load",
 "expect": "ok|diag|runtime_error",
 "entry": "main.am",
 "module_name": null
}
```

Нормативно:

- один кейс не обязан иметь все expectation-файлы;
- сравнение JSON-артефактов должно выполняться по канонически сериализованному виду;
- сравнение текстовых `.out/.err/.dis` должно быть line-stable и не зависеть от абсолютных путей, если это специально не часть теста.

### 6.1. Loader / bytecode fixtures

Для loader/verifier-тестов допускается специальный layout:

```text
loader_case/
 modules/
 main.amberbc
 dep1.amberbc
 dep2.amberbc
 expect.out.txt
 expect.err.txt
 expect.diag.json
 meta.json
```

Это позволяет независимо тестировать:

- dependency linking;
- export/import resolution;
- verifier failures;
- init state machine;
- frozen-loader barriers.

## 7. Минимальный CLI contract reference toolchain

Для reference implementation рекомендуется и для corpus runner считается каноническим следующий CLI-profile.

### 7.1. `amberc`

```text
amberc parse path/to/file.am --json
amberc lower path/to/file.am --json
amberc check path/to/file.am --json
amberc compile path/to/file.am -o path/to/file.amberbc
amberc disasm path/to/file.amberbc
```

Наблюдаемые правила:

- `parse --json` печатает AST dump `amber.ast.v1`;
- `lower --json` печатает HIR dump `amber.hir.v1`;
- `check --json` печатает diagnostics dump `amber.diag.v1` и не пишет `.amberbc`;
- `compile` либо создаёт `.amberbc`, либо печатает diagnostics;
- `disasm` обязан быть deterministic и пригоден для golden-сравнения.

### 7.2. `ambervm`

```text
ambervm run path/to/file.amberbc
```

Наблюдаемые правила:

- VM не требует присутствия compiler process;
- unhandled exception печатается как deterministic stack trace без raw pointer values;
- если модульная инициализация падает, это считается runtime failure loader/run phase, а не compiler failure.

### 7.3. `ambertest`

```text
ambertest run tests/
ambertest run tests/parser/block_suffix/case_001/
```

Runner обязан:

- читать `meta.json`;
- выбирать нужную фазу (`parse/lower/check/compile/run/load`);
- вызывать соответствующий tool path;
- сравнивать фактический артефакт с golden expectation;
- возвращать non-zero exit status при первом несовпадении либо в конце batch-run summary.

## 8. Bootstrap order reference implementation

### 8.1. Этап F0 — lexer и spans

Сделать:

- INDENT/DEDENT model;
- `case!` как отдельный token;
- `pattern` / `as` как contextual keywords;
- `CHAIN_DOT` rule внутри one-liner block body;
- stable span model line/column.

Критерий выхода:

- lexer corpus покрывает block structure, `.?.`, `CHAIN_DOT`, `$_`, `_1`;
- token stream deterministic.

### 8.2. Этап F1 — parser и AST dump

Сделать:

- Pratt parser expressions/postfix;
- module directives;
- class/mixin/include forms;
- signatures с `TypeTerm`;
- syntax-faithful AST serializer.

Критерий выхода:

- parser-позитивный corpus проходит;
- `expect.ast.json` стабилен на всём parser/golden наборе.

### 8.3. Этап F2 — binder, defaults и diagnostics

Сделать:

- signature validation;
- default-eval ordering checks;
- import/export placement rules;
- object-model placement checks;
- canonical diagnostic codes `E1001..E3006`, `W1001`.

Критерий выхода:

- negative corpus сходится по `expect.diag.json`;
- ordinary/multi-clause `def` binding semantics воспроизводимы.

### 8.4. Этап F3 — pattern compiler и HIR lowering

Сделать:

- compiled pattern IR;
- `case` / `case!` lowering;
- many-def normalization;
- builtin lowering для `send` и task intrinsics;
- `$_`, safe-nav, block suffix lowering;
- HIR serializer.

Критерий выхода:

- `expect.hir.json` стабилен;
- одна и та же семантика подтверждается AST->HIR round-trip corpus.

### 8.5. Этап V0 — bytecode container и disassembler

Сделать:

- `BcModule/BcMethod/BcCode`;
- const/symbol tables;
- serializer/deserializer `.amberbc`;
- deterministic disassembly.

Критерий выхода:

- `compile -> disasm` golden проходит;
- `.amberbc` round-trip не меняет code/debug sections.

### 8.6. Этап V1 — register VM core

Сделать:

- frame model;
- `LOADK/MOVE/JUMP/RETURN/SEND/CALL/GETLAST/SETLAST/MAKE_CLOSURE`;
- exception unwinding;
- call/ivar caches minimum.

Критерий выхода:

- runtime corpus выполняется через VM, а не AST-walk;
- `$_`, block suffix и method send ведут себя наблюдаемо корректно.

### 8.7. Этап V2 — object model, lifetime и collector boundary

Сделать:

- object headers / shapes;
- tombstone states;
- `OBJ_DESTROY`, `OBJ_DEALLOC`;
- non-moving collector boundary;
- root scanning / safepoints / remembered sets.

Критерий выхода:

- use-after-free и illegal access ловятся корректно;
- lifetime corpus проходит без GIL.

### 8.8. Этап V3 — scheduler и concurrency base

Сделать:

- worker pool + strands;
- task states / wait / cancel / wake;
- `Channel`, `Mutex`, `Atomic`;
- same-strand vs new-strand spawn semantics.

Критерий выхода:

- scheduler corpus воспроизводим;
- blocking/timeout/cancel semantics стабильны.

### 8.9. Этап V4 — loader/linker/verifier

Сделать:

- dependency linker;
- verifier checks;
- module init state machine;
- frozen-world loader barriers.

Критерий выхода:

- precompiled multi-module corpus проходит;
- verifier/runtime failure paths детерминированы и диагностируемы.

### 8.10. Этап V5 — stdlib stabilization и full conformance pass

Сделать:

- chainable collection contract;
- `Map`-specific operations;
- finalize error classes and stack trace formatting;
- прогон полного corpus на frontend + VM + loader.

Критерий выхода:

- reference implementation проходит единый corpus без special-case режимов;
- implementation phase P0/P1 считается practically complete.

## 9. Что остаётся вне этого blueprint

Этот blueprint по-прежнему **не** закрывает и не блокирует отдельно следующие треки:

- full static checker / inference;
- package manager / registry / signing;
- hot reload;
- extended reflection / introspection API;
- class-side mixins / `extend`;
- advanced concurrency extensions (`select`, richer supervisors, move-semantics);
- MIR / SSA / JIT / native AOT backend;
- frozen-image deployment tooling.

## 10. Практический следующий шаг после этой части

После фиксации этой части reference implementation уже можно открывать как проект с конкретными первым набором задач:

1. поднять lexer + parser + AST JSON;
2. ввести canonical diagnostic codes и `check --json`;
3. собрать HIR lowering и `lower --json`;
4. реализовать `.amberbc` writer/reader + `disasm`;
5. поднять register VM loop;
6. затем подключить lifetime runtime, scheduler, loader и stdlib base.

Никаких новых языковых решений для старта reference runtime после этого не требуется.


# Часть XII. Closure-профили второй волны ()

## 1. Назначение этой части

Эта часть доводит до закрытого состояния те вопросы, которые в ещё оставались не как блокеры старта reference runtime, а как **вторая волна дизайна и toolchain policy**. После принятия этой части у Amber больше не остаётся незакрытых spec-level вопросов: дальше остаются только implementation backlog, corpus/tests и конкретные runtime/toolchain работы.

Нормативный принцип:

- dynamic core Amber и bytecode-first runtime остаются базовой обязательной моделью;
- все решения этой части **не ломают** уже зафиксированную dynamic semantics;
- typed/native/distribution/reflection/concurrency second wave оформляются как совместимые профили поверх той же language core.

## 2. Optional static profile: Amber/Typed

### 2.1. Общий статус

`Amber/Typed` — это **optional build/tooling profile** поверх уже существующего source language. Он:

- не вводит отдельный диалект;
- не меняет runtime semantics обычного dynamic Amber;
- использует уже зафиксированные surface forms `x as T`, `@x as T`, `-> TypeTerm` и `expr as TypeTerm`;
- может быть включён на уровне package/build profile.

В typed profile программа либо проходит static check, либо остаётся обычной dynamic Amber program без изменения наблюдаемой семантики.

### 2.2. Граница обязательных аннотаций

В typed package обязательны явные type annotations для:

- exported `def`;
- exported `class_method def`;
- boundary methods/classes, публикуемых наружу через package export surface.

Разрешается local inference для:

- локальных переменных;
- block parameters;
- private/internal defs, не выходящих в package boundary;
- field types, если они однозначно следуют из annotated auto-assign или из доминирующих присваиваний в `init`.

Если тип поля не удаётся вывести однозначно и нет явной boundary-annotation, typed checker обязан требовать явное `as TypeTerm` на параметре, из которого поле вводится, либо явный checked cast на месте присваивания.

### 2.3. Типовая решётка и exactness policy

В дополнительно фиксируются следующие type-level решения:

- `Any` — верхний тип typed profile;
- `Never` — нижний тип;
- `Null` остаётся обычным singleton-like типом значения `null`;
- generics считаются **invariant**;
- record types остаются **open by default**;
- exact-record задаётся формой `**Never`.

Примеры:

```amber
{id: Int, name: Str}
{id: Int, name: Str, **Never}
Map[Str, Int]
Result[Ast, ParseError]
```

В этой редакции не вводятся отдельные source-keywords для variance, ownership или effect types.

### 2.4. Flow typing

Typed checker обязан поддерживать flow-sensitive narrowing как минимум в следующих случаях.

#### Truthiness

Поскольку в Amber falsy только `false` и `null`, то:

- на truthy-ветке `if x:` из типа `x` удаляются `False | Null`;
- на falsy-ветке остаётся только пересечение с `False | Null`.

#### `and` / `or`

Нормативно:

- `a and b` имеет тип `(FalsyPart[a]) | TypeOf(b under Truthy[a])`;
- `a or b` имеет тип `(TruthyPart[a]) | TypeOf(b under Falsy[a])`;
- `not a` всегда имеет тип `Bool`.

#### `$_`

В typed-view `$_` имеет тип последнего уже вычисленного выражения в текущем lexical scope. Если до текущей точки ни одного выражения ещё не было, typed-view `$_` считается `Null`.

#### `case` / `case!`

Каждый `when PATTERN` обязан narrow'ить subject на соответствующей ветке согласно accepted pattern shape.

Дополнительно:

- `case` без `else` имеет тип `Union(branch_types..., Null)`;
- `case!` без `else` в typed profile допустим только при доказуемой exhaustiveness, иначе это typed compile error.

### 2.5. Типы и метапрограммирование

Чтобы закрыть конфликт между static typing и open-world MOP, фиксирует следующую границу:

- literal-selector ordinary sends типизируются обычным способом;
- `send(...)` с dynamic selector, `method_missing`, runtime `define_method`, late reopen/`include`/`extend` через внешний open-world path и иные reflective mutations дают **reflective boundary**;
- reflective boundary в open-world typed build типизируется как `Any -> Any`;
- frozen typed build вправе ужесточать это и принимать только те reflective sites, которые остаются допустимыми после freeze analysis.

То есть typed profile закрывается без требования «полностью статического Ruby-подобного мира».

## 3. Package/distribution/signing/hot-reload policy

### 3.1. Manifest и source layout

Package-level tooling стандартизуется через manifest `amber.toml`.

Минимально обязательные поля manifest:

```toml
package = "net.http"
version = "1.2.0"
amber = "1.0"
sources = ["src"]
profile = "dynamic" # or "typed"
```

Нормативно:

- manifest `package` задаёт artifact/package id;
- каждый source file внутри пакета обязан иметь `package`, равный этому id либо начинающийся с него как с dotted-prefix;
- package version использует semver;
- package build profile (`dynamic`, `typed`, `frozen`, `typed+frozen`) выбирается tooling, а не source grammar.

### 3.2. Publish unit и registry model

Registry/publish unit — signed package bundle `.amberpkg`.

`.amberpkg` обязан содержать:

- normalized manifest;
- compiled `.amberbc` modules;
- export/import tables;
- content digests;
- optional source/debug payload;
- signature envelope.

Registry coordinates строятся по `(package, version, amber_abi, build_profile, digest)`.

`amber.lock` обязан фиксировать как минимум:

- точную версию зависимости;
- digest артефакта;
- identity signer'а или доверенный fingerprint.

### 3.3. Trust, signatures и reproducibility

Для publishable artifacts обязательны:

- reproducible build outputs на уровне `.amberpkg` payload;
- SHA-256 content digests;
- embedded Ed25519 signatures.

Unsigned path/git/local dependencies допустимы только в dev workflow и не считаются publish-grade artifacts.

### 3.4. Hot reload

Hot reload стандартизуется как **open-world dev profile** и не допускается в frozen profile.

Нормативно:

- reload unit — целый package artifact;
- reload выполняется как atomic package-swap transaction;
- reload, меняющий public export surface, manifest identity, ABI/profile contract или incompatible selector/arity boundary, обязан завершаться `ReloadIncompatibleError`;
- compatible reload допускает замену internal bodies, пока dispatch-world остаётся `open`.

Это закрывает dev-server/hot-reload story без компромисса с frozen deployment.

## 4. Extended read-only reflection / introspection API

Расширенная рефлексия стандартизуется как stdlib/runtime package `amber.reflect`.

### 4.1. Mirror objects

Обязательные mirror kinds:

- `ClassMirror`
- `MixinMirror`
- `MethodMirror`
- `PackageMirror`
- `WorldMirror`

Mirrors являются:

- immutable snapshot-objects;
- read-only views;
- пригодными для debug/tooling/inspection;
- непригодными для прямой world mutation.

### 4.2. Обязательный API

Минимально обязательный introspection contract должен покрывать:

- `name`, `kind`, `owner_package`;
- `superclass`, `ancestors`, `includes`, `extends`;
- `methods(side::instance |:class, local: Bool = false, inherited: Bool = true)`;
- `method(selector, side:...)`;
- `source_location`;
- parameter metadata / arity / block-acceptance;
- optional typed signature metadata, если package собран в typed profile;
- `world_epoch` и frozen/open state для `WorldMirror`.

### 4.3. Граница мутаций

Reflection API **не** добавляет новый mutation path. World mutation по-прежнему выполняется только через уже принятые механизмы:

- `class` / `mixin` reopen;
- `include`;
- `extend`;
- `define_method`.

Тем самым Amber получает законченную introspection story без перехода к неограниченному full-MOP.

## 5. Class-side composition: `extend`

### 5.1. Surface form

В вводится declarative `extend` для class-side composition.

```amber
mixin FactoryDsl:
 def from_json(text):
 self.new(parse_json(text))

class User:
 extend FactoryDsl

 class_method def table() -> Str:
 "users"
```

Нормативно:

- `extend` разрешён только непосредственно внутри declarative body `class` и её reopen-форм;
- каждый operand обязан резолвиться в mixin object;
- методы mixin'а при `extend` становятся методами **class object** receiver'а;
- `class_method def` текущего класса доминируют над методами, пришедшими через `extend`;
- при нескольких direct `extend` действует то же правило, что и для `include`: later direct extend wins.

### 5.2. Ограничения

В по-прежнему не вводятся:

- `extend` внутри `mixin` body;
- reflective alias/remove/visibility API для class-side;
- новый мета-диалект наподобие `class << self`.

`extend` является world mutation и подчиняется тем же freeze/invalidation правилам, что и `include`.

## 6. Advanced concurrency profile

### 6.1. Ownership transfer через `move(expr)`

В вводится explicit ownership-transfer marker `move(expr)`.

Нормативно:

- `move(expr)` допустим только на ownership boundaries: `task.spawn`, `Channel.send`, `select` send-arm и аналогичных runtime APIs передачи между strand'ами;
- `move(expr)` запрещён для shareable/sync values, где transfer не нужен;
- после успешного transfer исходный binding считается moved-from;
- дальнейшее чтение moved-from binding должно завершаться compile-time diagnostic, если это видно статически, либо runtime `MovedValueError`.

Это закрывает move-semantics без введения глобальной affine/linear type system.

### 6.2. `select`

Вводится expression-form `select:`.

Минимальная surface shape:

```amber
select:
 when msg = inbox.recv():
 handle(msg)
 when outbox.send(move(packet))::sent
 timeout 1000::timeout
 else::idle
```

Нормативно:

- `when` arms могут ждать `Channel.recv`, `Channel.send`, `TaskHandle.await` и standard awaitable/readiness tokens;
- `else` выполняется немедленно, если ни один arm не готов и blocking wait не требуется;
- `timeout expr:` создаёт bounded wait;
- если готовы несколько arms, runtime обязан выбирать их fair-ish образом без фиксированного left-bias contract.

### 6.3. Supervisor policies

`async` и `task.spawn` получают optional keyword `policy:`.

Обязательные policy values:

- `:cancel_scope` — текущее поведение по умолчанию;
- `:one_for_one`;
- `:one_for_all`;
- `:rest_for_one`.

Нормативно эти policy управляют только child-failure/cancellation propagation и не меняют ownership/isolation model.

### 6.4. Async I/O

Async I/O встраивается через пакет `amber.io` и readiness/awaitable objects, совместимые с `select`.

Language core не получает отдельного `await`-диалекта: уже существующий async/task model + `select` считаются достаточной surface-моделью.

### 6.5. Что остаётся вне core spec

Distributed/multi-process runtime, cluster membership, remote actor transport и similar features **не входят** в core language spec. Они остаются library/host-level story поверх пакетов, каналов и ownership rules.

## 7. Memory/lifetime second wave без field modifiers

### 7.1. Окончательный отказ от field modifiers

Amber окончательно **не вводит** source-level field modifiers `owned`, `weak`, `borrowed`.

Ownership/borrowing/weakness выражаются через runtime/library objects и API, а не через annotations на полях пользовательских классов.

### 7.2. Weak / ephemeron / buffer story

В пакете `amber.memory` стандартизуются:

- `WeakRef[T]`;
- `Ephemeron[K, V]`;
- `Bytes`;
- `Buffer[T]`;
- `Slice[T]`.

Эти объекты принадлежат runtime/memory profile и вправе иметь специальные GC/pinning rules, но не меняют ordinary class syntax.

### 7.3. Borrow helpers

Для FFI/zero-copy взаимодействия стандартизуется block-scoped borrow helper.

```amber
memory.borrow(buf) |view|:
 native.fill(view)
```

Нормативно:

- borrow-view не должен переживать enclosing block;
- статически очевидный escape borrow-view — compile-time diagnostic typed/lint-layer;
- неочевидный escape — runtime `BorrowEscapeError`.

### 7.4. Host embedding profile

Sandboxing, process-level memory quotas, allocator telemetry/tuning, host lifecycle hooks и related production concerns относятся к host embedding profile, а не к core language syntax.

## 8. Canonical MIR / native / JIT / frozen-image profile

### 8.1. Backend IR

Поверх зафиксированного HIR стандартизуется canonical backend layer `MIR`.

`MIR` должен быть:

- SSA-based;
- CFG-oriented;
- explicit в отношении guards, safepoints, exception edges и reflective stubs.

Bytecode compiler и native/JIT backend обязаны разделять один frontend contract (`AST -> HIR -> MIR? -> codegen`).

### 8.2. Native/JIT policy

Нормативно:

- bytecode VM остаётся reference execution engine;
- JIT и native AOT являются дополнительными профилями;
- native compilation допускается только для frozen-world artifacts/images;
- reflective sites (`SEND_DYN`, `method_missing`, late open-world mutation paths) остаются runtime helpers/stubs и не требуют обязательного deopt механизма.

Если реализация хочет делать speculative optimization + deopt, это допустимо, но не входит в language contract.

### 8.3. Frozen image

Deployable frozen image стандартизуется как `.amberimg`.

`.amberimg` обязан bundlить:

- frozen manifest;
- package table;
- code payload (bytecode and/or native sections);
- debug/source map metadata;
- digests и signatures.

Frozen image несовместим с hot reload и требует already-frozen dispatch-world.

## 9. Итоговый статус после 

После принятия этой части:

- у Amber больше не остаётся незакрытых spec-level вопросов;
- dynamic core, typed profile, distribution policy, reflection, class-side composition, advanced concurrency, memory second wave и backend profile имеют зафиксированную границу;
- дальнейшая работа — это уже не дизайн-спор, а реализация parser/runtime/checker/registry/native backend и расширение corpus/tests.


# Часть XIII. Детализированная матрица имплементации ()

## 1. Назначение

Эта часть не переоткрывает дизайн языка и не меняет нормативную семантику. Её задача — превратить уже закрытые уровни `G1..G15` и bootstrap-этапы `F0..F3`, `V0..V5` в **исполняемую инженерную матрицу**, пригодную для:

- планирования репозитория;
- постановки epics/issues;
- параллельной работы frontend/runtime/tooling lane'ов;
- фиксации критериев готовности без архитектурного дрейфа.

В этой части вводятся:

- work-package model `W0..W10`;
- milestone map `M0..M9`;
- dependency lanes для параллельной реализации;
- единая definition-of-done для work packages;
- стартовый backlog reference repo.

Нормативно:

- эта часть обязательна только для **reference implementation planning**;
- она не предписывает конкретный язык реализации, build system, CI-провайдера или monorepo layout;
- но она предписывает наблюдаемые артефакты, порядок схождения и минимальные acceptance-критерии.

## 2. Модель пакетов работ

### 2.1. Уровни декомпозиции

В используются три уровня инженерной декомпозиции.

1. **Goal-tracks `G...`** — крупные проектные треки из части IV.
2. **Work packages `W...`** — исполнимые инженерные блоки на 1-3 недели концентрированной работы.
3. **Milestones `M...`** — точки интеграции, в которых несколько `W` должны сойтись в один наблюдаемый результат.

### 2.2. Правило соответствия

Каждый `W` обязан:

- ссылаться минимум на один `G`;
- выдавать конкретный внешний артефакт или тестируемое поведение;
- иметь явные входы, зависимости и критерий готовности;
- обновлять corpus/golden tests, если меняется наблюдаемый вывод toolchain.

### 2.3. Единая definition of done

Любой work package считается завершённым только если одновременно выполнены все условия:

1. код реализован без блокирующих `TODO/FIXME` на critical path;
2. добавлены unit/integration tests;
3. обновлены relevant golden/corpus fixtures;
4. CLI/JSON/disasm output детерминирован;
5. добавлены краткие engineering notes или doc-comments для нестандартных решений;
6. пакет проходит смежные regression-наборы, а не только свой локальный тест;
7. если пакет меняет формат артефакта, обновлена версия/совместимость этого формата или явно зафиксировано отсутствие format bump.

## 3. Детализированная матрица work packages

### 3.1. W0 — репозиторный и tooling baseline

| ID | Scope | Связь с G | Produces | Depends on | Acceptance |
|---|---|---|---|---|---|
| `W0.1` | repo skeleton, build targets, test harness skeleton | G8 | рабочие команды `build/test/fmt`, общий workspace layout | нет | пустой repo уже прогоняет smoke CI |
| `W0.2` | canonical JSON serialization helpers | G2, G5, G8 | stable serializer для AST/HIR/diag dumps | `W0.1` | одинаковый вход даёт побайтно одинаковый JSON |
| `W0.3` | corpus runner skeleton | G8 | базовый `ambertest run...` с `meta.json` parsing | `W0.1` | runner способен запустить хотя бы parse-case |
| `W0.4` | fixture normalizer / golden update scripts | G8 | dev-tooling для обновления golden-файлов | `W0.2`, `W0.3` | обновление corpus не требует ручной правки JSON |

### 3.2. W1 — lexer, parser, AST

| ID | Scope | Связь с G | Produces | Depends on | Acceptance |
|---|---|---|---|---|---|
| `W1.1` | lexer core: tokens, INDENT/DEDENT, `case!`, placeholders | G1 | deterministic token stream + spans | `W0.1` | lexer corpus покрывает block structure, `CHAIN_DOT`, `$_`, `_1` |
| `W1.2` | expression parser / Pratt core | G1 | postfix/send/call/safe-nav parsing | `W1.1` | expression examples из спеки парсятся без расхождений |
| `W1.3` | stmt/module parser | G1 | `package/import/export`, `def`, `class`, `mixin`, `include`, `extend`, `case` | `W1.2` | module grammar покрыта позитивным corpus |
| `W1.4` | AST schema + source-faithful dump | G2 | `amber.ast.v1` | `W1.3`, `W0.2` | parser/golden набор даёт стабильный `expect.ast.json` |

### 3.3. W2 — binder, signatures, diagnostics

| ID | Scope | Связь с G | Produces | Depends on | Acceptance |
|---|---|---|---|---|---|
| `W2.1` | scope graph и symbol binding | G3 | локальные/field/module bindings | `W1.4` | ordinary `def`, blocks и class bodies bind'ятся стабильно |
| `W2.2` | signatures, defaults, delayed auto-assign commit | G3 | `bind_call`, default ordering, auto-assign semantics | `W2.1` | examples с defaults/auto-assign совпадают со спецой |
| `W2.3` | diagnostic engine + canonical codes | G3, G8 | `amber.diag.v1`, `E1001..E3008`, `W1001` | `W2.1`, `W0.2` | negative corpus сходится по code/severity/span |
| `W2.4` | import/export/object-model placement checks | G3, G11 | parser+binder acceptance rules | `W2.1`, `W2.3` | import-zone / reopen / include / extend ошибки стабильны |

### 3.4. W3 — patterns и HIR lowering

| ID | Scope | Связь с G | Produces | Depends on | Acceptance |
|---|---|---|---|---|---|
| `W3.1` | pattern matcher runtime contract | G4 | `===`, `deconstruct`, `deconstruct_keys`, commit semantics | `W2.2` | pattern corpus воспроизводим во всех contexts |
| `W3.2` | pattern compiler | G4, G5 | internal decision-tree / match program | `W3.1`, `W2.3` | `case` и pattern assignment не требуют ad-hoc runtime branching |
| `W3.3` | HIR node set + lowering rules | G5 | `amber.hir.v1` | `W2.4`, `W3.2`, `W0.2` | AST -> HIR стабилен на lowering corpus |
| `W3.4` | lowering for `$_`, safe-nav, block suffix, many-def, task intrinsics | G5 | canonical lowered forms | `W3.3` | HIR одинаков для эквивалентных surface forms |

### 3.5. W4 — compiler artifacts и bytecode container

| ID | Scope | Связь с G | Produces | Depends on | Acceptance |
|---|---|---|---|---|---|
| `W4.1` | bytecode container schema | G6e, G13 | `BcModule/BcMethod/BcCode` layout | `W3.3` | `.amberbc` schema документирован и round-trip проходит |
| `W4.2` | serializer/deserializer + verifier skeleton | G6e | writer/reader/verifier | `W4.1` | corrupt/invalid fixtures дают детерминированные verifier failures |
| `W4.3` | deterministic disassembler | G6e, G8 | `amberdis` / `amberc disasm` | `W4.2` | `expect.dis.txt` стабилен |
| `W4.4` | HIR -> bytecode emitter core | G13 | method prologues, branches, calls, closures, debug spans | `W3.4`, `W4.1` | compile/disasm corpus проходит для non-trivial programs |

### 3.6. W5 — VM core и dispatch

| ID | Scope | Связь с G | Produces | Depends on | Acceptance |
|---|---|---|---|---|---|
| `W5.1` | frame model, registers, call/return | G6 | VM execution loop baseline | `W4.4` | функции и block calls выполняются через VM, не через AST-walk |
| `W5.2` | send/ivar access/caches | G6 | `SEND`, `SEND_DYN`, ivar lookup, inline caches | `W5.1` | method send semantics совпадает со спецификацией |
| `W5.3` | closures, `GETLAST/SETLAST`, exceptions | G6 | lexical capture, last-value model, unwind | `W5.1` | `$_`, blocks и exception corpus зелёные |
| `W5.4` | object headers, shapes, method tables | G6c | runtime object model | `W5.2` | shape transitions наблюдаемы и стабильны |

### 3.7. W6 — memory, lifetime, collector, pinning

| ID | Scope | Связь с G | Produces | Depends on | Acceptance |
|---|---|---|---|---|---|
| `W6.1` | allocator + per-worker arenas + remote-free queues | G6b, G6c | allocation path для objects/arrays/closures | `W5.4` | runtime выдерживает allocation-heavy corpus |
| `W6.2` | tombstones, `destroy!`, `memory.dealloc`, dead-object guards | G6b | lifecycle runtime | `W6.1`, `W5.3` | `UseAfterFreeError` и lifecycle corpus зелёные |
| `W6.3` | non-moving collector + barriers + remembered sets | G6b | GC boundary | `W6.1` | многопоточный GC smoke проходит без UB |
| `W6.4` | pinning / opaque handles / native-safe views | G6d | `PinToken`, pinned scopes | `W6.3` | pin/unpin не ломает collector и no-GIL semantics |

### 3.8. W7 — scheduler и concurrency base

| ID | Scope | Связь с G | Produces | Depends on | Acceptance |
|---|---|---|---|---|---|
| `W7.1` | worker pool, strands, wake/timer queues | G7 | scheduler core | `W5.3`, `W6.3` | несколько strand'ов реально исполняются параллельно |
| `W7.2` | task lifecycle, cancellation, joins, wait states | G7 | task runtime | `W7.1` | timeout/cancel/join semantics воспроизводимы |
| `W7.3` | `Channel`, `Mutex`, `Atomic` | G7, G9 | concurrency base stdlib/runtime | `W7.2` | concurrency corpus проходит без глобальной блокировки |

### 3.9. W8 — loader, stdlib, full corpus

| ID | Scope | Связь с G | Produces | Depends on | Acceptance |
|---|---|---|---|---|---|
| `W8.1` | dependency linker + module init state machine | G6e | multi-module load/run path | `W4.2`, `W5.3` | precompiled modules грузятся корректно |
| `W8.2` | export/import tables + debug sections + stack traces | G6e, G8 | human/machine-readable loader diagnostics | `W8.1` | loader failures детерминированы |
| `W8.3` | collections contract v1 | G9 | `each/map/select/reduce/...`, `Map` API | `W5.4` | stdlib corpus совпадает со spec examples |
| `W8.4` | full conformance runner pass | G8 | единый green corpus на parse/lower/check/compile/run/load | `W0.4`, `W8.1`, `W8.3`, `W7.3` | reference implementation проходит весь corpus без special-cases |

### 3.10. W9 — typed, open-world, packages

| ID | Scope | Связь с G | Produces | Depends on | Acceptance |
|---|---|---|---|---|---|
| `W9.1` | Amber/Typed checker core | G10 | `TypeTerm`, flow engine, invariance, exhaustiveness | `W3.4`, `W8.4` | typed corpus зелёный и не меняет dynamic behavior |
| `W9.2` | open-class/open-mixin/`extend` runtime path | G11 | transactions, invalidation, world-open semantics | `W5.4`, `W8.2` | open-world mutation работает без ambiguous dispatch |
| `W9.3` | reflection mirrors | G11 | `amber.reflect` read-only mirrors | `W9.2` | mirrors детерминированы и не открывают mutation backdoor |
| `W9.4` | package/registry/signing/lockfile tooling | G11 | `amber.toml`, `.amberpkg`, `amber.lock`, signatures | `W8.2`, `W8.4` | reproducible package artifacts и install/publish smoke работают |
| `W9.5` | hot reload as package-swap | G11 | compatible reload / incompatibility guards | `W9.4`, `W9.2` | reload success/failure paths наблюдаемо стабильны |

### 3.11. W10 — advanced concurrency и native profiles

| ID | Scope | Связь с G | Produces | Depends on | Acceptance |
|---|---|---|---|---|---|
| `W10.1` | `move(expr)`, `select`, supervisor policies | G12 | advanced concurrency runtime | `W7.3`, `W8.4` | extended concurrency corpus зелёный |
| `W10.2` | async-I/O awaitables | G12 | `amber.io` awaitable bridge | `W10.1`, `W6.4` | async I/O не ломает scheduler invariants |
| `W10.3` | MIR/SSA pipeline | G15 | canonical optimizer IR | `W4.4`, `W5.4` | MIR dump стабилен и пригоден для backend tests |
| `W10.4` | native/JIT backend | G14, G15 | codegen + runtime stubs | `W10.3`, `W9.2` | frozen/native smoke programs исполняются корректно |
| `W10.5` | frozen image builder | G14, G15 | `.amberimg`, freeze analysis, world barriers | `W10.4`, `W9.4` | frozen artifacts воспроизводимо собираются и грузятся |

## 4. Milestone map

### M0 — bootstrap repo

Сходятся:

- `W0.1`
- `W0.2`
- `W0.3`

Результат:

- существует живой repo с CI, базовым `ambertest` и канонической сериализацией.

### M1 — frontend green

Сходятся:

- `W1.1..W1.4`
- `W2.1..W2.4`
- `W3.1..W3.4`

Результат:

- `amberc parse/lower/check --json` детерминированно работает;
- parser/lower/negative corpus зелёный.

### M2 — compile/disasm green

Сходятся:

- `W4.1..W4.4`
- tool-path части `W0.4`

Результат:

- есть полный путь `source -> HIR ->.amberbc -> disasm`.

### M3 — single-worker runtime green

Сходятся:

- `W5.1..W5.4`
- `W6.1..W6.2`

Результат:

- язык исполняется через VM в single-worker режиме;
- lifecycle guards и exception semantics работают.

### M4 — no-GIL runtime green

Сходятся:

- `W6.3`
- `W6.4`
- `W7.1..W7.3`

Результат:

- scheduler, collector и concurrency base сходятся в реальном parallel runtime.

### M5 — modules/loader/stdlib green

Сходятся:

- `W8.1`
- `W8.2`
- `W8.3`
- `W8.4`

Результат:

- reference implementation проходит единый corpus на parse/lower/check/compile/run/load.

### M6 — typed + open-world + packages green

Сходятся:

- `W9.1..W9.5`

Результат:

- optional profiles Amber/Typed, package distribution и hot reload реально работают.

### M7 — advanced concurrency green

Сходятся:

- `W10.1`
- `W10.2`

Результат:

- вторая волна concurrency реализована поверх стабильного scheduler core.

### M8 — native/frozen green

Сходятся:

- `W10.3`
- `W10.4`
- `W10.5`

Результат:

- существует путь к MIR/native/JIT/frozen artifacts без слома dynamic core.

### M9 — release-grade reference implementation

Сходятся:

- все `W0..W10`
- полный corpus
- reproducibility/smoke/perf sanity checks

Результат:

- reference implementation пригодна как baseline для альтернативных реализаций и для language conformance.

## 5. Parallel lanes и critical path

### 5.1. Lane A — frontend

Состав:

- `W1`
- `W2`
- `W3`

Эта lane критична первой: без неё невозможны ни typed checker, ни bytecode pipeline, ни corpus.

### 5.2. Lane B — artifacts/tooling/corpus

Состав:

- `W0`
- части `W4`
- части `W8`

Эту lane выгодно начинать почти одновременно с frontend, чтобы не накапливать format drift и ручные golden-обновления.

### 5.3. Lane C — VM/object/memory

Состав:

- `W4.4`
- `W5`
- `W6`

Это главный runtime critical path. Его нельзя безопасно ускорять ценой пропуска tombstone/collector/pinning boundary, потому что потом это приведёт к архитектурному откату.

### 5.4. Lane D — scheduler/loader/stdlib

Состав:

- `W7`
- `W8`

Эта lane начинается после появления стабильного VM core и minimally safe collector boundary.

### 5.5. Lane E — typed/open-world/distribution

Состав:

- `W9`

Эта lane принципиально не должна блокировать `M5`; она стартует только после того, как dynamic reference runtime уже green на полном corpus.

### 5.6. Lane F — native/frozen

Состав:

- `W10.3..W10.5`

Эта lane последняя и не должна влиять на решения frontend/dynamic runtime, кроме уже заранее зафиксированных форматов и world-freeze boundary.

### 5.7. Критический путь 

Минимальный критический путь к рабочему reference runtime P0/P1:

`W0 -> W1 -> W2 -> W3 -> W4 -> W5 -> W6 -> W7 -> W8`

Typed/distribution/native workstreams не входят в этот путь.

## 6. Стартовый backlog reference repo

Ниже фиксируется рекомендуемый **первый набор epics/issues**, который можно почти без изменений переносить в репозиторий.

### 6.1. Epics

1. `EP-frontend-parser`
2. `EP-frontend-binder`
3. `EP-frontend-patterns-hir`
4. `EP-tooling-corpus`
5. `EP-bytecode-container`
6. `EP-vm-core`
7. `EP-memory-lifecycle`
8. `EP-scheduler-concurrency`
9. `EP-loader-stdlib`
10. `EP-typed-openworld-packages`
11. `EP-native-frozen`

### 6.2. First issue set (первые 24 задачи)

1. `ISS-001` repo skeleton + CI smoke (`W0.1`)
2. `ISS-002` canonical JSON writer (`W0.2`)
3. `ISS-003` `ambertest` meta.json parser (`W0.3`)
4. `ISS-004` lexer tokens/spans (`W1.1`)
5. `ISS-005` Pratt core for postfix/calls (`W1.2`)
6. `ISS-006` parser for module/class/mixin forms (`W1.3`)
7. `ISS-007` AST serializer `amber.ast.v1` (`W1.4`)
8. `ISS-008` scope graph + locals (`W2.1`)
9. `ISS-009` signature/default pipeline (`W2.2`)
10. `ISS-010` diagnostics engine + `E1001..E3008` (`W2.3`)
11. `ISS-011` import/export/include/extend placement checks (`W2.4`)
12. `ISS-012` pattern runtime protocol (`W3.1`)
13. `ISS-013` pattern compiler (`W3.2`)
14. `ISS-014` HIR node set (`W3.3`)
15. `ISS-015` lowering for `$_` / safe-nav / block suffix (`W3.4`)
16. `ISS-016` `.amberbc` schema (`W4.1`)
17. `ISS-017` writer/reader/verifier (`W4.2`)
18. `ISS-018` disassembler (`W4.3`)
19. `ISS-019` bytecode emitter baseline (`W4.4`)
20. `ISS-020` VM frames/registers/return (`W5.1`)
21. `ISS-021` send/call caches (`W5.2`)
22. `ISS-022` closures + `GETLAST/SETLAST` + unwind (`W5.3`)
23. `ISS-023` object headers/shapes/method tables (`W5.4`)
24. `ISS-024` allocator + tombstones + `memory.dealloc` baseline (`W6.1`, `W6.2`)

После закрытия `ISS-001..ISS-024` reference repo должен дойти как минимум до `M3` либо стоять вплотную к нему.

## 7. Реестр рисков и анти-дрейф правила

### 7.1. Главные архитектурные риски

| Risk | Где проявляется | Последствие | Анти-дрейф правило |
|---|---|---|---|
| AST drift | `W1.4` | golden corpus быстро устаревает | сначала стабилизировать schema names, потом расширять узлы |
| HIR leakage of optimizer concerns | `W3.3`, `W10.3` | frontend начинает зависеть от backend | HIR остаётся execution-oriented, MIR вводится отдельно |
| VM before lifecycle | `W5` без `W6` | use-after-free и невалидные инварианты | не считать VM-ready без tombstone/lifecycle guards |
| scheduler before GC barriers | `W7` без `W6.3` | гонки и повреждение heap | no-GIL runtime открывается только после barrier-safe collector boundary |
| loader before verifier | `W8.1` без `W4.2` | недетерминированные падения на битом bytecode | все load-paths проходят через verifier |
| typed checker before dynamic corpus green | `W9.1` раньше `W8.4` | checker начинает диктовать runtime semantics | typed lane стартует только после `M5` |
| native backend before freeze world | `W10.4` раньше `W10.5` | reflective invalidation ломает native assumptions | native code допустим только на freeze-aware boundary |

### 7.2. Правила анти-дрейфа

1. Никакой новый surface syntax не принимается через implementation workaround.
2. Любое наблюдаемое изменение AST/HIR/diag/disasm требует либо corpus update, либо explicit format-bump rationale.
3. Нельзя заменять спецификационную диагностику generic internal error'ом.
4. Нельзя открывать typed/native lanes как блокер для `M5`.
5. Любой runtime shortcut, который обходит `destroy!`/tombstone/pinning invariants, считается архитектурным дефектом, а не допустимым tech debt.

## 8. Практический порядок на ближайший цикл

Если начинать прямо сейчас, то самый рациональный короткий цикл такой:

### Цикл A

- `W0.1`
- `W0.2`
- `W0.3`
- `W1.1`
- `W1.2`
- `W1.3`
- `W1.4`

### Цикл B

- `W2.1`
- `W2.2`
- `W2.3`
- `W2.4`
- `W3.1`
- `W3.2`
- `W3.3`
- `W3.4`

### Цикл C

- `W4.1`
- `W4.2`
- `W4.3`
- `W4.4`
- `W5.1`
- `W5.2`
- `W5.3`

### Цикл D

- `W5.4`
- `W6.1`
- `W6.2`
- `W6.3`
- `W7.1`
- `W7.2`
- `W7.3`

### Цикл E

- `W8.1`
- `W8.2`
- `W8.3`
- `W8.4`

После циклов `A..E` reference implementation должна закрыть practical P0/P1.

## 9. Статус после 

После принятия этой части:

- верхнеуровневая матрица `G1..G15` остаётся в силе;
- `F0..F3`, `V0..V5` остаются bootstrap-порядком;
- `W0..W10` становятся рабочим инженерным слоем между спецификацией и репозиторием;
- старт reference repo больше не требует дополнительных spec-level решений.


# Часть XIV. Репозиторный backlog pack и milestone gating ()

## 1. Назначение

Если переводит закрытую спецификацию в инженерную матрицу `W0..W10`, то эта часть переводит матрицу в **операционный пакет для репозитория**.

Эта часть нужна для того, чтобы после открытия reference repo команда могла без дополнительных организационных RFC:

- завести labels и milestones;
- перенести `EP-*` и `ISS-*` в issue tracker;
- одинаково оформлять implementation issues, bugs и spec-sync tasks;
- проводить milestone gates без повторного обсуждения критериев готовности.

Нормативно:

- эта часть обязательна только для **reference implementation execution**;
- она не привязана к конкретной forge-платформе;
- но она фиксирует минимально достаточный operational contract, который должна воспроизводить любая площадка управления задачами.

## 2. Канонический layout reference repo

Рекомендуемый layout reference repo:

```text
/spec/
 changelog/
 registries/
 tokens.yaml
 diagnostics.yaml
 opcodes.yaml
 bytecode_sections.yaml
 runtime_errors.yaml
/corpus/
 parse/
 lower/
 check/
 compile/
 disasm/
 run/
 load/
 typed/
 concurrency/
 packages/
/tools/
 amberc/
 ambervm/
 ambertest/
 amberdis/
/frontend/
 lexer/
 parser/
 ast/
 binder/
 hir/
/runtime/
 vm/
 objects/
 memory/
 scheduler/
 loader/
/stdlib/
/docs/
 engineering/
/.github/ # либо эквивалентный каталог forge automation
```

Нормативно:

- `corpus/` обязан быть отделён от исходников toolchain;
- `spec/` обязан содержать зафиксированную редакцию спецификации, на которую ссылается текущий mainline;
- `spec/registries/` обязан содержать machine-readable registries для token kinds, diagnostics, opcodes, bytecode sections, runtime error taxonomy и optional profile opcodes/errors;
- любое изменение наблюдаемого формата или канонического runtime/error contract обязано обновлять соответствующий registry в том же changeset;
- `frontend/`, `runtime/`, `stdlib/` и `tools/` допускается объединять или переименовывать, если при этом не теряется трассируемость к `W0..W10`.

### 2.1. Соответствие каталогов пакетам работ

| Repo area | Основные `W` |
|---|---|
| `tools/ambertest`, `docs/engineering`, automation | `W0`, части `W8` |
| `frontend/lexer`, `frontend/parser`, `frontend/ast` | `W1` |
| `frontend/binder`, diagnostics | `W2` |
| `frontend/hir`, pattern lowering | `W3` |
| `tools/amberc`, `tools/amberdis`, bytecode schema | `W4` |
| `runtime/vm`, `runtime/objects` | `W5` |
| `runtime/memory` | `W6` |
| `runtime/scheduler` | `W7` |
| `runtime/loader`, `stdlib`, `corpus/load` | `W8` |
| typed/open-world/packages | `W9` |
| advanced concurrency/native/frozen | `W10` |

## 3. Canonical label taxonomy

### 3.1. Общие правила

Каждая issue в reference repo должна иметь как минимум:

- ровно один label `kind/...`;
- минимум один label `area/...`;
- ровно один label `priority/...`;
- ровно один целевой label `milestone/...`;
- при наличии блокеров — label `state/blocked`.

Epic issues могут не иметь целевого milestone label, если они существуют как umbrella-объекты над несколькими milestone'ами.

### 3.2. Обязательные label-префиксы

#### `kind/...`

- `kind/epic`
- `kind/implementation`
- `kind/bug`
- `kind/spec-sync`
- `kind/refactor`
- `kind/tooling`
- `kind/corpus`

#### `area/...`

- `area/repo`
- `area/frontend`
- `area/parser`
- `area/binder`
- `area/patterns-hir`
- `area/bytecode`
- `area/vm`
- `area/memory`
- `area/scheduler`
- `area/loader`
- `area/stdlib`
- `area/typed`
- `area/openworld`
- `area/packages`
- `area/native`
- `area/corpus`
- `area/docs`

#### `lane/...`

- `lane/A-frontend`
- `lane/B-tooling`
- `lane/C-runtime`
- `lane/D-loader-stdlib`
- `lane/E-typed-openworld`
- `lane/F-native`

#### `priority/...`

- `priority/P0`
- `priority/P1`
- `priority/P2`
- `priority/P3`

#### `milestone/...`

- `milestone/M0`
- `milestone/M1`
- `milestone/M2`
- `milestone/M3`
- `milestone/M4`
- `milestone/M5`
- `milestone/M6`
- `milestone/M7`
- `milestone/M8`
- `milestone/M9`

#### `state/...`

- `state/blocked`
- `state/ready`
- `state/in-progress`
- `state/in-review`
- `state/needs-corpus`
- `state/needs-spec-sync`

#### `risk/...`

- `risk/format`
- `risk/runtime-safety`
- `risk/concurrency`
- `risk/perf`
- `risk/reproducibility`

### 3.3. Нормативные ограничения

Нормативно:

1. одна issue не должна одновременно иметь больше одного label `priority/...`;
2. одна issue не должна одновременно иметь больше одного label `milestone/...`, кроме случаев явно документированного cross-milestone carry;
3. issue, меняющая наблюдаемый формат (`AST/HIR/diag/disasm/.amberbc/.amberimg`), обязана иметь `risk/format`;
4. issue, затрагивающая collector, pinning, scheduler или no-GIL boundary, обязана иметь `risk/runtime-safety` либо `risk/concurrency`;
5. issue, меняющая golden/corpus output, обязана иметь `state/needs-corpus` до тех пор, пока corpus не обновлён в том же changeset.

## 4. Epic board и трассировка к `W`/`M`

### 4.1. Canonical epic set

| Epic | Покрывает | Целевой выход |
|---|---|---|
| `EP-bootstrap-tooling` | `W0` | `M0` |
| `EP-frontend-parser` | `W1` | `M1` |
| `EP-frontend-binder` | `W2` | `M1` |
| `EP-patterns-hir` | `W3` | `M1` |
| `EP-bytecode-container` | `W4` | `M2` |
| `EP-vm-core` | `W5` | `M3` |
| `EP-memory-lifecycle` | `W6` | `M3`/`M4` |
| `EP-scheduler-concurrency` | `W7` | `M4` |
| `EP-loader-stdlib` | `W8` | `M5` |
| `EP-typed-openworld-packages` | `W9` | `M6` |
| `EP-native-frozen` | `W10` | `M7`/`M8` |

### 4.2. Epic acceptance contract

Каждый epic считается закрытым только если:

- закрыты все относящиеся к нему `P0/P1` issues текущего milestone;
- нет открытых blocker-багов той же области;
- corpus для соответствующего `W` стабильно проходит на mainline;
- engineering notes обновлены, если были приняты нестандартные решения.

## 5. Расширенный issue catalogue

### 5.1. Донабор к стартовому набору 

Ниже приводится рекомендуемое продолжение каталога после `ISS-001..ISS-024`.

| ID | Scope | `W` | Priority | Milestone | Depends on |
|---|---|---|---|---|---|
| `ISS-025` | fixture normalizer и golden update scripts | `W0.4` | `P0` | `M1` | `ISS-002`, `ISS-003` |
| `ISS-026` | default-expression ordering + delayed auto-assign commit edge-cases | `W2.2` | `P0` | `M1` | `ISS-009` |
| `ISS-027` | `W1001` warning и corpus для чтения старого `@field` в default | `W2.3` | `P1` | `M1` | `ISS-010`, `ISS-026` |
| `ISS-028` | binder corpus для import-zone, export tables и reopen placement | `W2.4` | `P0` | `M1` | `ISS-011` |
| `ISS-029` | `deconstruct_keys` / record-pattern runtime contract | `W3.1` | `P0` | `M1` | `ISS-012` |
| `ISS-030` | OR-pattern binding-set validator | `W3.2` | `P0` | `M1` | `ISS-013` |
| `ISS-031` | HIR nodes для `case`, `send`, assignment, returns | `W3.3` | `P0` | `M1` | `ISS-014` |
| `ISS-032` | lowering many-def clauses в canonical dispatch form | `W3.4` | `P1` | `M1` | `ISS-015`, `ISS-031` |
| `ISS-033` | lowering `task.spawn`, `async`, `await` intrinsics в HIR builtins | `W3.4` | `P1` | `M2` | `ISS-015`, `ISS-031` |
| `ISS-034` | verifier: structural invariants для `.amberbc` | `W4.2` | `P0` | `M2` | `ISS-017` |
| `ISS-035` | verifier: symbol/span/debug table invariants | `W4.2` | `P1` | `M2` | `ISS-017`, `ISS-034` |
| `ISS-036` | disasm canonical text layout + source span comments | `W4.3` | `P1` | `M2` | `ISS-018` |
| `ISS-037` | emitter for closures and lexical captures | `W4.4` | `P0` | `M2` | `ISS-019`, `ISS-022` |
| `ISS-038` | emitter for exception blocks and unwind metadata | `W4.4` | `P1` | `M2` | `ISS-019` |
| `ISS-039` | VM opcodes: moves, constants, jumps, locals | `W5.1` | `P0` | `M3` | `ISS-020` |
| `ISS-040` | VM calls, block invocation and returns | `W5.1` | `P0` | `M3` | `ISS-020`, `ISS-039` |
| `ISS-041` | inline cache invalidation hooks for method table changes | `W5.2` | `P1` | `M3` | `ISS-021`, `ISS-023` |
| `ISS-042` | exception frames, stack unwinding and human-readable traces | `W5.3` | `P0` | `M3` | `ISS-022`, `ISS-038` |
| `ISS-043` | object shapes, ivar slots and stable shape transitions | `W5.4` | `P0` | `M3` | `ISS-023` |
| `ISS-044` | allocator stress harness and fragmentation smoke | `W6.1` | `P1` | `M3` | `ISS-024`, `ISS-043` |
| `ISS-045` | tombstone guards, dead-object checks and lifecycle corpus | `W6.2` | `P0` | `M3` | `ISS-024`, `ISS-042` |
| `ISS-046` | collector barriers, remembered sets, root scanning | `W6.3` | `P0` | `M4` | `ISS-044`, `ISS-045` |
| `ISS-047` | `PinToken`, opaque handles and native-safe views | `W6.4` | `P1` | `M4` | `ISS-046` |
| `ISS-048` | worker pool, wake queues, timers | `W7.1` | `P0` | `M4` | `ISS-046`, `ISS-042` |
| `ISS-049` | task lifecycle: join, cancel, timeout, wait states | `W7.2` | `P0` | `M4` | `ISS-048` |
| `ISS-050` | `Channel` semantics, shareability gate, rendezvous vs buffered cases | `W7.3` | `P0` | `M4` | `ISS-049` |
| `ISS-051` | `Mutex` / `Atomic` runtime and corpus | `W7.3` | `P1` | `M4` | `ISS-049` |
| `ISS-052` | loader graph, dependency linker and module init state machine | `W8.1` | `P0` | `M5` | `ISS-017`, `ISS-042` |
| `ISS-053` | export/import tables, debug sections and loader diagnostics | `W8.2` | `P0` | `M5` | `ISS-052`, `ISS-035` |
| `ISS-054` | stdlib collections contract for sequences | `W8.3` | `P0` | `M5` | `ISS-043` |
| `ISS-055` | stdlib `Map` contract and transform APIs | `W8.3` | `P1` | `M5` | `ISS-054` |
| `ISS-056` | full conformance runner gate for parse/lower/check/compile/run/load | `W8.4` | `P0` | `M5` | `ISS-025`, `ISS-053`, `ISS-055`, `ISS-051` |
| `ISS-057` | typed checker: `TypeTerm`, parameter/return boundaries | `W9.1` | `P1` | `M6` | `ISS-056` |
| `ISS-058` | typed flow engine, `case!` exhaustiveness, `and/or` rules | `W9.1` | `P1` | `M6` | `ISS-057` |
| `ISS-059` | open-class/open-mixin/`extend` invalidation transactions | `W9.2` | `P1` | `M6` | `ISS-041`, `ISS-053` |
| `ISS-060` | reflection mirrors and deterministic ordering | `W9.3` | `P2` | `M6` | `ISS-059` |
| `ISS-061` | package manifest, lockfile and `.amberpkg` bundle | `W9.4` | `P1` | `M6` | `ISS-053`, `ISS-056` |
| `ISS-062` | signing and reproducible package artifacts | `W9.4` | `P2` | `M6` | `ISS-061` |
| `ISS-063` | hot reload as atomic package swap | `W9.5` | `P2` | `M6` | `ISS-059`, `ISS-061` |
| `ISS-064` | `move(expr)` ownership transfer semantics | `W10.1` | `P2` | `M7` | `ISS-050` |
| `ISS-065` | `select` runtime and fairness corpus | `W10.1` | `P2` | `M7` | `ISS-064`, `ISS-051` |
| `ISS-066` | async-I/O awaitables bridge | `W10.2` | `P2` | `M7` | `ISS-065`, `ISS-047` |
| `ISS-067` | MIR node set and SSA validator | `W10.3` | `P2` | `M8` | `ISS-019`, `ISS-043` |
| `ISS-068` | MIR dump format and optimization pass harness | `W10.3` | `P2` | `M8` | `ISS-067` |
| `ISS-069` | native codegen baseline for frozen world | `W10.4` | `P3` | `M8` | `ISS-068`, `ISS-063` |
| `ISS-070` | JIT runtime stubs and patchpoints | `W10.4` | `P3` | `M8` | `ISS-068` |
| `ISS-071` | freeze analysis and `.amberimg` writer | `W10.5` | `P3` | `M8` | `ISS-069`, `ISS-061` |
| `ISS-072` | frozen image loader and compatibility checks | `W10.5` | `P3` | `M8` | `ISS-071` |

### 5.2. Минимальный набор issue'ов для немедленного старта

Если у команды есть ресурс только на первый цикл, она обязана открыть как минимум следующий набор задач:

- `ISS-001..ISS-007`
- `ISS-008..ISS-015`
- `ISS-025`
- `ISS-026`
- `ISS-028`

Этого достаточно, чтобы без организационных провалов добраться до полноценного `M1`.

### 5.3. Правило дробления issue'ов

Нормативно:

- issue из каталога выше допускается дробить на подзадачи;
- но исходный `ISS-*` должен оставаться tracking issue с теми же acceptance-критериями;
- дробление не должно скрывать milestone risk: если tracking issue не закрыт, milestone не считается достигнутым.

## 6. Milestone gate checklists

### 6.1. `M0` gate

`M0` считается достигнутым только если:

- закрыты `ISS-001`, `ISS-002`, `ISS-003`;
- CI на mainline прогоняет smoke build/test;
- `ambertest` читает `meta.json` и умеет выполнить хотя бы parse-case;
- serializer выдаёт побайтно стабильный JSON на повторных прогонах.

### 6.2. `M1` gate

`M1` считается достигнутым только если:

- закрыты все `P0` issues, целящиеся в `M1`;
- команды `amberc parse --json`, `amberc lower --json`, `amberc check --json` работают на corpus;
- negative corpus сходится по `code/severity/span`;
- AST/HIR dumps стабильны и обновлены в mainline.

### 6.3. `M2` gate

`M2` считается достигнутым только если:

- закрыты все `P0` issues, целящиеся в `M2`;
- существует путь `source -> HIR ->.amberbc -> disasm`;
- verifier отсекает битые и несовместимые артефакты детерминированно;
- `expect.dis.txt` стабилен и регенерируется без ручной правки.

### 6.4. `M3` gate

`M3` считается достигнутым только если:

- закрыты все `P0` issues, целящиеся в `M3`;
- single-worker runtime исполняет corpus через VM, а не через AST-walk fallback;
- lifecycle corpus зелёный;
- stack traces и exception paths детерминированы.

### 6.5. `M4` gate

`M4` считается достигнутым только если:

- закрыты все `P0` issues, целящиеся в `M4`;
- no-GIL runtime действительно исполняет несколько strand'ов параллельно;
- collector barrier tests и scheduler tests зелёные;
- concurrency primitives не вводят скрытый global lock.

### 6.6. `M5` gate

`M5` считается достигнутым только если:

- закрыты `ISS-052..ISS-056` и все их `P0` зависимости;
- full conformance runner проходит parse/lower/check/compile/run/load;
- stdlib contract совпадает с примерами спецификации;
- mainline reproducibly rebuilds `.amberbc` fixtures и debug outputs.

### 6.7. Правило открытия последующих milestone'ов

Нормативно:

- `M6` не открывается как blocker для релизов `M0..M5`;
- `M7` не открывается до стабилизации `M6` или явного решения вести experimental lane отдельно;
- `M8` допускается только поверх freeze-aware boundary и не может откатывать инварианты dynamic core.

## 7. Issue templates

### 7.1. Template: implementation issue

```markdown
Title: <ISS-ID> <краткое действие>

## Summary
Коротко: что именно реализуется.

## Spec anchors
- Part/section:
- Related W:
- Related M:

## In scope
-...

## Out of scope
-...

## Dependencies
- blockers:
- follow-ups:

## Acceptance
- [ ] observable behavior / artifact
- [ ] tests
- [ ] corpus/golden updated
- [ ] docs/notes updated

## Format impact
- none / AST / HIR / diag / disasm / amberbc / amberimg

## Risk notes
- runtime-safety / concurrency / reproducibility / perf
```

### 7.2. Template: bug issue

```markdown
Title: BUG <area> <symptom>

## Observed
Что фактически произошло.

## Expected
Что должно происходить по спецификации.

## Reproducer
Минимальный пример или corpus path.

## Spec anchors
Ссылка на норму.

## Suspected area
`W` / subsystem / file family.

## Acceptance
- [ ] reproducer added to corpus
- [ ] fix validated on mainline
- [ ] no regression in adjacent suites
```

### 7.3. Template: spec-sync issue

```markdown
Title: SPEC-SYNC <artifact> <mismatch>

## Mismatch
Какой наблюдаемый вывод расходится со спецификацией.

## Candidate resolutions
- fix implementation
- update corpus
- format bump rationale

## Required decision
Что именно должно быть синхронизировано.

## Acceptance
- [ ] final decision recorded
- [ ] repo and spec converge
- [ ] stale fixtures removed
```

## 8. PR, merge и release rules

### 8.1. Обязательное содержимое PR

Каждый PR обязан содержать:

- ссылку минимум на один `ISS-*`;
- указание `W` и milestone;
- раздел `corpus impact`;
- раздел `format impact`;
- краткое описание invariant'ов, которые PR меняет или подтверждает.

### 8.2. Merge rules

Нормативно:

1. PR, меняющий наблюдаемый формат, не может быть влит без corpus/golden update в том же changeset.
2. PR, затрагивающий `destroy!`, tombstones, collector barriers, pinning или scheduler wake-up paths, не может быть влит без regression run смежных runtime suites.
3. PR, открывающий новый public CLI flag или новый machine-readable output, обязан обновить tool docs и smoke examples.
4. Нельзя сливать optimization-only PR, который меняет диагностику или observable ordering, без отдельного spec-sync объяснения.

### 8.3. Release train policy для `M0..M5`

Рекомендуемый release rhythm:

- `M0..M1` — frequent integration, допускаются ежедневные merges;
- `M2..M3` — интеграция батчами, freeze окна перед gate review;
- `M4..M5` — только changesets, проходящие расширенный runtime/corpus regression.

## 9. Рекомендации по распараллеливанию команды

### 9.1. Минимальная команда из 2 человек

- разработчик A: `lane/A` + части `lane/B`;
- разработчик B: `lane/C`, затем `lane/D`.

В этом режиме typed/native не открываются до `M5`.

### 9.2. Команда из 3-4 человек

- A: lexer/parser/AST;
- B: binder/HIR/diagnostics;
- C: VM/object/memory;
- D: tooling/corpus/loader/stdlib.

Это оптимальный режим для fastest path к `M5`.

### 9.3. Команда 5+ человек

Дополнительно можно открывать:

- experimental `lane/E` после стабилизации `M3`;
- experimental `lane/F` только после явного freeze boundary plan.

## 10. Статус после 

После принятия этой части:

- `W0..W10` и `M0..M9` получают прямое отображение на repo operations;
- стартовый backlog превращается в почти готовый issue tracker import set;
- milestone gates больше не требуют отдельных управленческих решений;
- переход от спецификации к фактическому запуску reference repo можно считать завершённым.

# Часть XV. Исполнимая декомпозиция tracking issues и внутренних контрактов ()

## 1. Назначение

Если переводит закрытую спецификацию в `W0..W10`, а — в operational backlog/labels/milestones, то добавляет ещё один инженерный слой: **короткие исполнимые slices под существующие `ISS-*`**.

Эта часть:

- не переоткрывает surface syntax и не меняет языковую семантику;
- не заменяет `ISS-*`, `W-*` и `M-*`, а дополняет их более коротким execution-level слоем;
- нужна, чтобы команда могла раскладывать tracking issues на PR-sized pieces, не теряя milestone risk и анти-дрейф инварианты.

Нормативно:

1. carrier milestone risk остаётся только `ISS-*`;
2. подзадачи уровня `T-*` не меняют milestone, priority и acceptance родительской issue без явного изменения tracking issue;
3. любое наблюдаемое изменение AST/HIR/diag/disasm/`.amberbc`/runtime behavior по-прежнему требует corpus update или format-bump rationale;
4. typed/native/post-`M5` lanes по-прежнему не имеют права блокировать путь `M1..M5`.

## 2. Issue-local subtask model `T-*`

### 2.1. Формат идентификаторов

Внутри tracking issue допускается локальный слой декомпозиции:

- `T-004.1`, `T-004.2`,... для `ISS-004`;
- `T-052.1`, `T-052.2`,... для `ISS-052`;
- при необходимости одна подзадача может иметь дополнительные checklist items без отдельного ID, но milestone-critical slices должны иметь явный `T-*`.

### 2.2. Где живут подзадачи

`T-*` могут жить в одном из трёх эквивалентных носителей:

1. checklist внутри tracking issue;
2. child issue/sub-issue;
3. отдельный PR slice, если в issue tracker нет иерархии.

Выбор forge-механизма implementation-defined. Наблюдаемо важно только то, что связь `T-* -> ISS-* -> W-* -> M-*` не теряется.

### 2.3. Размер подзадачи

Ожидаемый размер одной `T-*`:

- от половины дня до трёх дней концентрированной работы;
- один главный наблюдаемый результат;
- максимум один рискованный format change;
- обязательный тестовый след или corpus fixture.

Подзадача считается слишком крупной, если она одновременно:

- меняет больше одного внешнего артефакта;
- требует отдельных spec-sync решений;
- не помещается в один осмысленный review cycle.

### 2.4. Definition of done для `T-*`

Каждая `T-*` считается закрытой только если одновременно выполнены все условия:

1. есть код, тест или corpus change, ради которого подзадача была открыта;
2. есть трассировка к родительской `ISS-*`;
3. зафиксирован наблюдаемый output или invariant;
4. format-affecting change сопровождается golden update или явным пояснением, почему format не изменился;
5. adjacent regression suite не сломана.

### 2.5. Чего `T-*` не делают

Подзадачи **не** подменяют собой tracking acceptance. Даже если все `T-*` из body issue формально отмечены, `ISS-*` остаётся открытой, пока не достигнут её исходный acceptance-критерий и соответствующий milestone gate.

## 3. Обязательные внутренние контракты, которые нужно заморозить до широкого распараллеливания

До открытия нескольких параллельных implementation lane'ов reference repo обязан иметь короткие engineering notes в `/docs/engineering/` как минимум по пяти интерфейсам.

### 3.1. `amber.ast.v1`

Нормативно фиксируются:

- имена узлов и имена полей serializer'а;
- обязательность source spans;
- deterministic field ordering в dump;
- запрет lowering'а `safe-nav`, block suffix, `$_`, `package/import/export`, `mixin/include` на AST-уровне.

### 3.2. `amber.diag.v1`

Нормативно фиксируются:

- stable diagnostic code registry;
- machine-readable JSON schema;
- ordering diagnostics и related spans;
- жёсткое разведение `compile_error` / `warning` / `lint`.

### 3.3. `amber.hir.v1`

Нормативно фиксируются:

- execution-oriented, но ещё не optimizer-oriented node families;
- явные lowering points для `$_`, block suffix, safe-nav, clause dispatch и async intrinsics;
- запрет тащить MIR/native concerns в HIR.

### 3.4. `amber.bc.v1`

Нормативно фиксируются:

- header/section/index model;
- fixed physical encoding: little-endian `u64` offsets, little-endian `u32` sizes/counts, `1-byte opcode + ULEB128/SLEB128 operands`;
- deterministic `STRS` / `SYMS` / `KONS` interning order;
- verifier invariants;
- canonical disasm contract;
- deterministic emitter output для одинакового HIR.

### 3.5. `amber.runtime.v1`

Нормативно фиксируются:

- frame slot для `$_`;
- dead-object checks до fast-path `LOAD_IVAR/STORE_IVAR/SEND`;
- shareable vs strand-confined boundary;
- `Channel.close`, `ChannelClosedError` и FIFO channel semantics;
- non-reentrant `Mutex` и seq-cst `Atomic`;
- loader state machine;
- world-mutation invalidation hooks;
- optional notebook watch hooks: `WatchCell`, watched object revisions, dependency capture and `watch_epoch` без `world_epoch` bump.

Эти пять notes не обязаны быть длинными RFC, но обязаны появиться раньше, чем команда начнёт независимо менять frontend, bytecode и runtime.

## 4. Декомпозиция пути к `M1`

### 4.1. `W1` — lexer / parser / AST

#### `ISS-004` lexer tokens/spans

- `T-004.1` зафиксировать canonical registry token kinds, отдельно для `case!`, `.?.`, `CHAIN_DOT`, `$_`, `_1.._N` и contextual `pattern` / `as`.
- `T-004.2` определить trivia/spans policy: comments, whitespace, newline folding, preservation of exact source ranges.
- `T-004.3` реализовать indent stack и правила `INDENT/DEDENT` для block syntax.
- `T-004.4` реализовать special lexer mode для one-liner block body, где `CHAIN_DOT` различается с обычной `.` по правилу пробела слева.
- `T-004.5` собрать token-dump corpus и негативные fixtures на `.?.`, `CHAIN_DOT`, `case!`, placeholders и nested interpolation.

`ISS-004` закрывается только когда token dump побайтно стабилен на повторных прогонах и не смешивает внутреннюю точку блока с продолжением внешней цепочки.

#### `ISS-005` Pratt core for postfix/calls

- `T-005.1` собрать precedence table в одном месте, без размазывания приоритетов по parser functions.
- `T-005.2` реализовать prefix parselets для unary `+`, `-`, `not`.
- `T-005.3` реализовать postfix loop для member/index/call/safe-nav и block suffix.
- `T-005.4` реализовать bare-call legality gate строго по v1-правилам: bare args только для name/member send/safe member send.
- `T-005.5` реализовать inline-block parse mode, который завершает внутреннее выражение на `CHAIN_DOT` или `NEWLINE` при глубине скобок `0`.
- `T-005.6` добавить негативные тесты: `_1` вне implicit block, `map(_1 * 2)` invalid in v1, ambiguous postfix boundaries.

`ISS-005` закрывается только когда весь postfix grammar воспроизводит зафиксированную surface форму без ad hoc special cases вне Pratt core.

#### `ISS-006` parser for module/class/mixin forms

- `T-006.1` реализовать top-level `package` и contiguous import zone.
- `T-006.2` реализовать `import`, `from... import...`, `export` как отдельные top-level forms, не понижаемые в call-expression.
- `T-006.3` реализовать `class`, `mixin`, `class_method def`, parser-level `extend`, declarative `include` и placement-sensitive bodies; runtime invalidation для `extend` сознательно остаётся в `W9`.
- `T-006.4` реализовать `case` / `case!`, clause-style `def`, `when` / `else` blocks и one-liner variants.
- `T-006.5` собрать parser corpus на reopen forms, superclass clauses, `class_method def`, nested mixins/classes и invalid placements.

`ISS-006` закрывается только когда module/object-model forms парсятся как отдельные syntactic families и не теряют source-order body items.

#### `ISS-007` AST serializer `amber.ast.v1` + `ISS-025` fixture normalizer

- `T-007.1` зафиксировать AST node names, field names и обязательные span-поля.
- `T-007.2` реализовать deterministic JSON serializer с canonical field ordering и без скрытого lowering'а.
- `T-007.3` собрать AST golden corpus для parser-critical surface forms.
- `T-007.4` реализовать fixture normalizer и scripts для массового обновления golden files без ручного редактирования.
- `T-007.5` ввести rule, что любой AST format change сопровождается corpus update в том же changeset.

`ISS-007` и `ISS-025` закрываются только когда одинаковый source даёт побайтно одинаковый AST dump, а bulk-update fixtures не ломает детерминизм.

### 4.2. `W2` — binder / signatures / diagnostics

#### `ISS-008` scope graph + locals

- `T-008.1` реализовать lexical scopes для module/function/block/class/mixin bodies.
- `T-008.2` зафиксировать binding kinds: local, import alias, export source, ivar, cvar, constant, placeholder.
- `T-008.3` реализовать special handling для `_1.._N` как implicit-block placeholders с проверкой плотной нумерации.
- `T-008.4` реализовать `$_` как special read-only binding текущего frame scope на binder-уровне.
- `T-008.5` добавить shadowing/duplicate rules и negative corpus для wildcard `_`, placeholder misuse и duplicate local names.

`ISS-008` закрывается только когда binder строит детерминированный scope graph и одинаково обслуживает ordinary blocks, clause bodies и module-level bindings.

#### `ISS-009` signature/default pipeline + `ISS-026`

- `T-009.1` нормализовать сигнатуру в canonical param descriptors: `external_name`, `local_name`, `kind`, `auto_assign_kind`, `type_expr`, `default_expr`.
- `T-009.2` реализовать preflight checker для arity, keywords, unknown keys и duplicates.
- `T-009.3` реализовать explicit bind phase для positional/keyword params и common `MISSING` protocol.
- `T-009.4` реализовать left-to-right default evaluator с доступом к `self`, старым `@field` / `@@field` и локалам слева по сигнатуре.
- `T-009.5` реализовать delayed auto-assign buffer, который коммитится только после успешного dispatch.
- `T-009.6` подключить runtime type hooks для `as TypeTerm` на parameter boundary.
- `T-009.7` собрать edge corpus для default-ordering, self-reference, rightward reference и delayed auto-assign commit.

`ISS-009` и `ISS-026` закрываются только когда ordinary `def` и clause-style `def` используют один и тот же bind core, а commit auto-assign наблюдаемо никогда не происходит до выбора победившей ветки.

#### `ISS-010` diagnostics engine + `ISS-027`

- `T-010.1` зафиксировать `amber.diag.v1` JSON schema и code registry.
- `T-010.2` реализовать primary span + related spans + severity model.
- `T-010.3` зафиксировать deterministic diagnostic ordering и stable rendering в CLI/JSON.
- `T-010.4` развести hard compile errors, mandatory warnings и tooling-only lint.
- `T-010.5` добавить обязательный warning-кейс `W1001` для чтения старого `@field` в default при наличии позднего auto-assign в то же поле.

`ISS-010` и `ISS-027` закрываются только когда diagnostics machine-readable, deterministic и не подменяют спецификационную ошибку generic internal failure.

#### `ISS-011` import/export/include/extend placement checks + `ISS-028`

- `T-011.1` реализовать binder checks для contiguous import zone, duplicate `package`, duplicate public exports и export of unknown names.
- `T-011.2` реализовать read-only status импортированных alias'ов и запрет присваивания им.
- `T-011.3` реализовать placement checks для `include` и `extend`, делая `extend` frontend-stable ещё до полного runtime path этой feature.
- `T-011.4` реализовать reopen placement rules и superclass mismatch prechecks там, где они статически очевидны.
- `T-011.5` собрать binder corpus для import-zone, export tables, reopen placement, invalid `include`/`extend` contexts.

`ISS-011` и `ISS-028` закрываются только когда binder одинаково защищает namespace-level и object-model placement invariants и выдаёт коды диагностик из каталога v1.

### 4.3. `W3` — patterns / HIR / lowering

#### `ISS-012` pattern runtime protocol + `ISS-029`

- `T-012.1` реализовать runtime contracts для `===`, `deconstruct()`, `deconstruct_keys(keys)` и dynamic `match(value)`.
- `T-012.2` реализовать coercion helpers для tuple/list/map matching и protocol error paths.
- `T-012.3` реализовать `**rest`, `**_`, `**null` semantics и full-map path для `deconstruct_keys`.
- `T-012.4` реализовать dynamic pattern objects `pattern(expr)` / `pattern(expr) with MAP_PATTERN` с explicit-binding profile.
- `T-012.5` собрать negative corpus на protocol violations, non-empty bindings without `with` и invalid dynamic pattern contexts.

`ISS-012` и `ISS-029` закрываются только когда pattern runtime одинаково обслуживает `case`, `case!`, block params и clause-style `def`, а protocol violations наблюдаемо дают `TypeError`.

#### `ISS-013` pattern compiler + `ISS-030`

- `T-013.1` определить compiled pattern IR / decision program format.
- `T-013.2` реализовать compile-time binding-set collection и duplicate-binding validator.
- `T-013.3` реализовать OR-pattern binding-set equality validator.
- `T-013.4` реализовать lowering list/tuple/map/head/as/pin/dynamic patterns в decision program.
- `T-013.5` собрать dumps/goldens compiled patterns для deterministic regression.

`ISS-013` и `ISS-030` закрываются только когда pattern compiler детерминирован, а разные ветки OR-pattern не могут тайно вводить разные наборы bindings.

#### `ISS-014` HIR node set + `ISS-031`

- `T-014.1` зафиксировать canonical HIR families для module/import/export/class/mixin/include/method/closure.
- `T-014.2` зафиксировать control-flow, call/send, match, assignment и `last_result` nodes.
- `T-014.3` зафиксировать async/runtime intrinsic nodes и boundary к bytecode/native backends.
- `T-014.4` реализовать deterministic HIR dump serializer.
- `T-014.5` собрать HIR corpus, покрывающий `case`, `send`, assignment, returns и module init paths.

`ISS-014` и `ISS-031` закрываются только когда HIR остаётся execution-oriented и не протаскивает backend-specific optimizer concerns.

#### `ISS-015` lowering for `$_` / safe-nav / block suffix + `ISS-032`

- `T-015.1` понизить `$_` в `HLastGet/HLastSet` без потери source span information.
- `T-015.2` понизить block suffix и implicit placeholders в явный `HClosure` с рассчитанной arity.
- `T-015.3` понизить safe-nav в explicit null-guard form либо `HSafe*` shorthand, который на следующем слое раскладывается в null-guards.
- `T-015.4` нормализовать simple many-def sugar в canonical clause-style dispatch form.
- `T-015.5` собрать lowering corpus для chained one-liner blocks, safe-nav chains, `case!` и clause-style `def`.

`ISS-015` и `ISS-032` закрываются только когда surface sugar больше не протекает в emitter/VM paths и весь dispatch уже выражен через HIR.

## 5. Декомпозиция пути к `M2`

### 5.1. `W4` — `.amberbc`, verifier, disasm, emitter

#### `ISS-016` `.amberbc` schema

- `T-016.1` зафиксировать `AmberBcHeader`, version fields, feature flags, little-endian `u64/u32` layout и ABI hash policy.
- `T-016.2` зафиксировать section directory, section IDs, required vs optional sections.
- `T-016.3` зафиксировать index model между `CODE`, `METH`, `CLAS`, `SYMS`, `KONS`, `DEPS`, `EXPT` и deterministic interning order для `STRS`/`SYMS`/`KONS`.
- `T-016.4` зафиксировать ограничения на serializable constants и запрет raw host pointers.
- `T-016.5` собрать round-trip fixtures на минимальные и feature-rich modules.

`ISS-016` закрывается только когда format достаточно стабилен, чтобы writer/reader/verifier и disassembler работали поверх одного и того же header/section contract.

#### `ISS-017` writer/reader/verifier + `ISS-034` + `ISS-035`

- `T-017.1` реализовать serializer/deserializer section directory и базовых секций.
- `T-017.2` реализовать structural verifier: magic, versions, offsets, bounds, alignments, duplicate/missing sections и canonical physical encoding constraints.
- `T-017.3` реализовать semantic-lite verifier: `code_id`, symbol indexes, method/class ranges, dependency tables.
- `T-017.4` реализовать debug/span verifier для `SPAN`, `LINE`, `LOCS` и related source references.
- `T-017.5` собрать negative fixtures на broken offsets, broken indexes, malformed debug tables и incompatible format flags.

`ISS-017`, `ISS-034` и `ISS-035` закрываются только когда любой load-path сначала проходит verifier и broken bytecode отвергается детерминированной диагностикой.

#### `ISS-018` disassembler + `ISS-036`

- `T-018.1` зафиксировать canonical textual layout disasm output, section/record ordering и comment style для spans.
- `T-018.2` реализовать stable naming `rN` / `lN` / `uN` / `code#N` для registers/locals/captures/code IDs.
- `T-018.3` реализовать source span comments и optional debug sections rendering.
- `T-018.4` собрать golden corpus на canonical disasm, устойчивый к повторным прогонам.

`ISS-018` и `ISS-036` закрываются только когда одинаковый `.amberbc` даёт побайтно одинаковый disasm text и тот пригоден как regression artifact.

#### `ISS-019` bytecode emitter baseline + `ISS-037` + `ISS-038`

- `T-019.1` реализовать method/module prologues, locals layout и register allocation baseline.
- `T-019.2` реализовать control-flow emission: branches, loops, returns, `SETLAST/GETLAST`.
- `T-019.3` реализовать closure emission и lexical capture layout.
- `T-019.4` реализовать exception/unwind metadata emission и handler tables.
- `T-019.5` реализовать call-site / ivar-site descriptors, safepoints и source span tables.

`ISS-019`, `ISS-037` и `ISS-038` закрываются только когда весь путь `HIR ->.amberbc -> disasm` работает для ordinary methods, closures и exception-aware code.

#### `ISS-033` lowering `task.spawn`, `async`, await-like intrinsics

- `T-033.1` распознавать intrinsic selectors для same-strand и new-strand spawn на lowering-уровне.
- `T-033.2` зафиксировать deterministic HIR contract для `HSpawnSameStrand`, `HSpawnNewStrand`, `HWait`, `HResume`, `HSleep`, `HYield`, `HCancel`.
- `T-033.3` провести emitter bridge tests, чтобы async intrinsics уже могли пережить compile/disasm round-trip до появления полного scheduler runtime.

`ISS-033` закрывается только когда async surface syntax не остаётся special case в parser/binder и уже выражена через стабильные HIR/runtime intrinsics.

## 6. Декомпозиция пути к `M3`

### 6.1. `W5` / `W6.1` / `W6.2` — single-worker VM + lifecycle

#### `ISS-020` VM frames/registers/return + `ISS-039` + `ISS-040`

- `T-020.1` реализовать frame ABI: caller link, return pc, code ref, `self`, block, task, `last_result`, regs.
- `T-020.2` реализовать core opcodes для moves, constants, locals, branches и returns.
- `T-020.3` реализовать call/return protocol, method vs callable dispatch и block invocation baseline.
- `T-020.4` реализовать module-init / default-thunk / closure entry conventions.
- `T-020.5` собрать VM smoke corpus на calls, locals, branching, `$_` и block invocation.

`ISS-020`, `ISS-039` и `ISS-040` закрываются только когда VM исполняет ordinary control-flow и call semantics без AST-walk fallback.

#### `ISS-021` send/call caches + `ISS-041`

- `T-021.1` реализовать generic method lookup slow-path и call-site descriptors.
- `T-021.2` реализовать monomorphic inline cache baseline для `SEND` и ivar access.
- `T-021.3` зафиксировать cache key contract: selector, owner dispatch, shape/version, world epoch where relevant.
- `T-021.4` реализовать invalidation hooks на изменения method tables и ancestor composition.
- `T-021.5` собрать corpus на cache hit/miss/invalidation и reflective fallback sites.

`ISS-021` и `ISS-041` закрываются только когда cache invalidation корректна, а обычный call path не зависит от устаревшего method table state.

#### `ISS-022` closures + `GETLAST/SETLAST` + unwind + `ISS-042`

- `T-022.1` реализовать closure object layout и capture cells.
- `T-022.2` реализовать `GETLAST/SETLAST` в VM и frame-local semantics для `$_`.
- `T-022.3` реализовать exception frame stack, protected ranges и unwind protocol.
- `T-022.4` реализовать human-readable traces со span/line support.
- `T-022.5` собрать corpus на nested closures, implicit returns, rescues/ensures и failed child waits.

`ISS-022` и `ISS-042` закрываются только когда closures и exception paths наблюдаемо совместимы с frame ABI и не смешивают `$_` между кадрами.

#### `ISS-023` object headers/shapes/method tables + `ISS-043`

- `T-023.1` реализовать `ObjHeader`, shape IDs/versions и class/method table descriptors.
- `T-023.2` реализовать ivar slot allocation и stable shape transitions на slow-path.
- `T-023.3` реализовать method table storage для class и mixin owners.
- `T-023.4` реализовать `DeadShape` compatibility contract для runtime checks и cache invalidation.
- `T-023.5` собрать corpus на ivar growth, method replacement, reopen compatibility и shape transition stability.

`ISS-023` и `ISS-043` закрываются только когда object layout уже пригоден для поздних tombstone checks, loader descriptors и cache invalidation.

#### `ISS-024` allocator + tombstones + `memory.dealloc` baseline + `ISS-044` + `ISS-045`

- `T-024.1` реализовать per-worker allocation fast path, remote-free queue skeleton и large-object path.
- `T-024.2` реализовать lifecycle state machine: live, destroying, destroyed, deallocated, tombstoned.
- `T-024.3` реализовать `OBJ_DESTROY` / `OBJ_DEALLOC`, tombstone header rewrite и payload release.
- `T-024.4` реализовать dead-object guards для ivar access, ordinary sends и reflective sites.
- `T-024.5` собрать allocator stress harness, fragmentation smoke и lifecycle corpus на double-destroy, double-dealloc, use-after-free.

`ISS-024`, `ISS-044` и `ISS-045` закрываются только когда VM single-worker green не обходится без dead-object checks и lifecycle regression suite.

## 7. Декомпозиция пути к `M4` и `M5`

### 7.1. `W6.3` / `W6.4` — collector and pinning boundary

#### `ISS-046` collector barriers, remembered sets, root scanning

- `T-046.1` реализовать root scanning для frames, tasks, strands, loader/module state и shared descriptors.
- `T-046.2` реализовать remembered sets и write barriers для confined/shared generations.
- `T-046.3` реализовать safepoint handshake между VM, collector и scheduler.
- `T-046.4` собрать GC smoke corpus под parallel load, включая closures, channels, module init и exception paths.

`ISS-046` закрывается только когда scheduler может безопасно открываться поверх barrier-safe collector boundary.

#### `ISS-047` `PinToken`, opaque handles and native-safe views

- `T-047.1` реализовать `PinToken` и registry pinned objects.
- `T-047.2` реализовать stale-unpin guards, pin-scope nesting и pin-aware buffer views.
- `T-047.3` реализовать opaque handles для native interop без raw pointer leakage в language-visible layer.
- `T-047.4` собрать corpus на pin/unpin races, cancelled native waits и dealloc-after-pin violations.

`ISS-047` закрывается только когда native-safe boundary совместима с non-moving runtime и не требует GIL.

### 7.2. `W7` — scheduler / concurrency base

#### `ISS-048` worker pool, wake queues, timers

- `T-048.1` реализовать worker pool и runnable-strand global queues.
- `T-048.2` реализовать strand-local run queues и wake token coalescing.
- `T-048.3` реализовать timer wheel / timer index для `sleep` и timed waits.
- `T-048.4` собрать corpus на wake semantics, spurious resumes, empty queues и strand migration boundaries.

`ISS-048` закрывается только когда разные strands реально могут исполняться параллельно без global interpreter lock.

#### `ISS-049` task lifecycle: join, cancel, timeout, wait states

- `T-049.1` реализовать task state machine: new, runnable, running, sleeping, waiting, done, failed, cancelled.
- `T-049.2` реализовать structured child set, first-failure propagation и sibling cancellation.
- `T-049.3` реализовать `wait()`, `wait(timeout:)`, cooperative `cancel()` и timeout errors.
- `T-049.4` собрать corpus на join/rethrow, cancellation safepoints, timeout-without-auto-cancel и scope exit semantics.

`ISS-049` закрывается только когда structured concurrency работает как часть runtime contract, а не как library convention.

#### `ISS-050` `Channel` semantics + `ISS-051` `Mutex` / `Atomic`

- `T-050.1` реализовать rendezvous и buffered channel cases, explicit `close()`, `ChannelClosedError` и FIFO send/recv semantics.
- `T-050.2` реализовать shareability gate на cross-strand payloads и ошибки isolation boundary.
- `T-050.3` реализовать fairness corpus для send/recv/selectable wait paths.
- `T-051.1` реализовать non-reentrant `Mutex` и seq-cst `Atomic` как shareable sync objects без скрытого GIL semantics.
- `T-051.2` собрать corpus на lock/unlock, reentrant-lock failure, seq-cst compare-and-set visibility, wake ordering и contention smoke.

`ISS-050` и `ISS-051` закрываются только когда concurrency base пригодна для обязательного stdlib/runtime contract v1.

### 7.3. `W8` — loader / stdlib / full corpus

#### `ISS-052` loader graph, dependency linker and module init state machine

- `T-052.1` реализовать loader state machine `unloaded -> mapped -> verified -> linked -> initializing -> ready/failed`.
- `T-052.2` реализовать dependency graph linking и cycle-aware module init.
- `T-052.3` реализовать single-run guarantee для module-init и `ModuleInitError` для раннего чтения неинициализированного export.
- `T-052.4` собрать corpus на cyclic imports, verifier failure, init failure и repeated load attempts.

`ISS-052` закрывается только когда любой compiled-module load path идёт через verifier и loader state machine.

#### `ISS-053` export/import tables, debug sections and loader diagnostics

- `T-053.1` реализовать runtime export table materialization и live read-only aliases для import bindings.
- `T-053.2` реализовать loader diagnostics для missing exports, incompatible ABI/version и debug/source locations.
- `T-053.3` реализовать debug section plumbing для stack traces, disasm и package/tooling paths.
- `T-053.4` собрать corpus на missing export, version mismatch, bad debug sections и re-export chains.

`ISS-053` закрывается только когда compiled modules наблюдаемо воспроизводят source-level import/export contract.

#### `ISS-054` stdlib collections contract for sequences + `ISS-055` `Map`

- `T-054.1` реализовать обязательный eager API для `each`, `map`, `flat_map`, `select`, `reject`, `reduce`, `find`, `any?`, `all?`, `none?`, `first`, `count`, `group_by`, `to_a`, `lazy`.
- `T-054.2` реализовать `reduce(init)` и `reduce` без init с `EmptyCollectionError` на пустой последовательности.
- `T-054.3` реализовать `LazySeq` materialization path и corpus на lazy/eager chaining.
- `T-055.1` реализовать `Map#each`, `Map#map`, `Map#select`, `Map#reject`, `transform_values`, `keys`, `values`, `entries`.
- `T-055.2` собрать corpus на arity, return-shape и ordering expectations там, где ordering зафиксирован.

`ISS-054` и `ISS-055` закрываются только когда surface collection style языка больше не зависит от ad hoc helper functions вне stdlib contract.

#### `ISS-056` full conformance runner gate

- `T-056.1` реализовать единый runner для `parse`, `lower`, `check`, `compile`, `disasm`, `run`, `load` suites.
- `T-056.2` реализовать deterministic fixture discovery, failure rendering и corpus metadata handling.
- `T-056.3` реализовать milestone gate bundles для `M1..M5` и mandatory adjacent regression selection.
- `T-056.4` собрать one-command full corpus path для CI/mainline.

`ISS-056` закрывается только когда `M5` можно подтвердить одним reproducible corpus run, а не ручной последовательностью локальных команд.

## 8. Исполнимая декомпозиция путей после `M5`

### 8.1. `W9` — typed / open-world / packages

#### `ISS-057` typed checker: `TypeTerm`, parameter/return boundaries

- `T-057.1` реализовать typed-package mode и mandatory annotation boundaries для exported callables.
- `T-057.2` реализовать parameter/return/type-assert runtime and checker hooks поверх уже существующего `TypeTerm` AST/HIR storage.
- `T-057.3` собрать typed corpus на boundary mismatches, package rules и reflective `Any` boundaries.

#### `ISS-058` typed flow engine, `case!` exhaustiveness, `and/or` rules

- `T-058.1` реализовать truthiness-aware flow lattice и rules для `and` / `or`.
- `T-058.2` реализовать pattern-based narrowing и `case!` exhaustiveness для typed profile.
- `T-058.3` собрать diagnostics corpus на impossible branches, missing annotations и non-exhaustive strict matches.

#### `ISS-059` open-class/open-mixin/`extend` invalidation transactions

- `T-059.1` реализовать atomic publish path для reopen/`include`/`extend`/`define_method` mutations.
- `T-059.2` реализовать dispatch invalidation, `world_epoch` updates и class-side composition hooks.
- `T-059.3` собрать corpus на reopen conflicts, `WorldFrozenError`, `SuperclassMismatchError` и method table replacement.

#### `ISS-060` reflection mirrors and deterministic ordering

- `T-060.1` реализовать immutable mirror objects для class/mixin/method/package/world.
- `T-060.2` реализовать deterministic ordering экспонируемых lists/tables.
- `T-060.3` собрать corpus на read-only guarantees, source location visibility и mirror stability.

#### `ISS-061` package manifest, lockfile and `.amberpkg` bundle

- `T-061.1` зафиксировать `amber.toml`, package table, lockfile and bundle layout.
- `T-061.2` реализовать build/export/package pipeline для publishable artifacts.
- `T-061.3` собрать corpus на manifest/package-prefix invariants и bundle round-trips.

#### `ISS-062` signing and reproducible package artifacts

- `T-062.1` реализовать content digests и deterministic packaging.
- `T-062.2` реализовать signing/verification hooks и compatibility with lockfile digests.
- `T-062.3` собрать reproducibility corpus и negative tests на broken signatures.

#### `ISS-063` hot reload as atomic package swap

- `T-063.1` реализовать dev-profile package swap state machine.
- `T-063.2` реализовать compatibility checks на public export surface и ABI contracts.
- `T-063.3` собрать corpus на incompatible reload, frozen-profile rejection и rollback on failed swap.

### 8.2. `W10` — advanced concurrency / MIR / native / frozen

#### `ISS-064` `move(expr)` ownership transfer semantics

- `T-064.1` реализовать explicit move boundary для cross-strand transfer.
- `T-064.2` реализовать moved-from guards и diagnostics.
- `T-064.3` собрать corpus на channels, spawns и illegal post-move reads.

#### `ISS-065` `select` runtime and fairness corpus

- `T-065.1` реализовать multi-channel wait runtime path и arm selection semantics.
- `T-065.2` реализовать fairness/timeout/else behavior.
- `T-065.3` собрать corpus на starvation, wake races и cancellation interactions.

#### `ISS-066` async-I/O awaitables bridge

- `T-066.1` реализовать awaitable/readiness contract между scheduler и `amber.io`.
- `T-066.2` реализовать integration с pinning/native-safe handles.
- `T-066.3` собрать corpus на cancellation, timeout и readiness edge cases.

#### `ISS-067` MIR node set and SSA validator

- `T-067.1` зафиксировать MIR node families и SSA well-formedness rules.
- `T-067.2` реализовать HIR -> MIR lowering baseline.
- `T-067.3` реализовать SSA validator и MIR dump contract.

#### `ISS-068` MIR dump format and optimization pass harness

- `T-068.1` реализовать deterministic MIR dump.
- `T-068.2` реализовать pass harness, phase ordering и invalidation rules.
- `T-068.3` собрать optimization corpus, который не меняет observable semantics.

#### `ISS-069` native codegen baseline for frozen world

- `T-069.1` реализовать codegen для frozen-world methods, closures и call ABI.
- `T-069.2` реализовать runtime stubs для reflective slow paths, которые остаются легальными после freeze.
- `T-069.3` собрать corpus на equivalence bytecode vs native для frozen artifacts.

#### `ISS-070` JIT runtime stubs and patchpoints

- `T-070.1` реализовать patchpoint descriptors и runtime stub ABI.
- `T-070.2` реализовать integration с inline caches, safepoints и invalidation.
- `T-070.3` собрать perf/regression harness без изменения language contract.

#### `ISS-071` freeze analysis and `.amberimg` writer

- `T-071.1` реализовать world freeze analysis и closure of reachable packages/modules.
- `T-071.2` реализовать `.amberimg` layout, debug metadata и signature payloads.
- `T-071.3` собрать corpus на image writer reproducibility и incompatible frozen inputs.

#### `ISS-072` frozen image loader and compatibility checks

- `T-072.1` реализовать image loader state machine и startup ABI.
- `T-072.2` реализовать compatibility checks для image/runtime/profile versions.
- `T-072.3` собрать corpus на startup errors, version mismatch и debug/source map availability.

## 9. Первый обязательный import set подзадач для немедленного открытия в repo

Если команда стартует с нуля, то кроме tracking issues из ей выгодно сразу открыть как минимум следующий минимальный subtask-set:

- `T-004.1`, `T-004.4`, `T-004.5`
- `T-005.1`, `T-005.4`, `T-005.5`
- `T-006.1`, `T-006.3`
- `T-007.1`, `T-007.4`
- `T-008.1`, `T-008.3`
- `T-009.1`, `T-009.4`, `T-009.5`
- `T-010.1`, `T-010.5`
- `T-011.1`, `T-011.5`
- `T-012.1`, `T-012.3`
- `T-013.1`, `T-013.3`
- `T-014.1`, `T-014.4`
- `T-015.1`, `T-015.3`, `T-015.4`
- `docs/engineering/ast-v1.md`
- `docs/engineering/diag-v1.md`
- `docs/engineering/hir-v1.md`
- `docs/engineering/bc-v1.md`
- `docs/engineering/runtime-v1.md`
- `spec/registries/tokens.yaml`
- `spec/registries/diagnostics.yaml`
- `spec/registries/opcodes.yaml`
- `spec/registries/bytecode_sections.yaml`
- `spec/registries/runtime_errors.yaml`

Именно этот набор быстрее всего убирает архитектурную неоднозначность, фиксирует machine-readable source-of-truth для форматов и открывает независимую работу по frontend/corpus/tooling без дрейфа.

## 10. Дополнительные анти-дрейф правила для subtask-level исполнения

Нормативно:

1. нельзя закрывать `T-*`, меняющую AST/HIR/diag/disasm/bytecode output, без corpus update в том же changeset;
2. нельзя открывать parallel work по emitter/VM, пока не заморожены `amber.ast.v1` и `amber.hir.v1` notes;
3. нельзя считать `ISS-020..ISS-024` закрытыми, если fast-path'ы всё ещё обходят dead-object checks;
4. нельзя открывать real parallel scheduler path до закрытия `ISS-046`;
5. `ISS-052` и далее не имеют права обходить verifier path ни в тестах, ни в production loader;
6. нельзя менять token/diagnostic/opcode/bytecode/runtime-error contracts без обновления соответствующего registry в том же changeset.

## 11. Статус после 

После принятия этой части:

- `W0..W10`, `M0..M9` и `ISS-001..ISS-072` получают ещё и PR-sized execution layer;
- critical path `M1..M5` больше не требует новой организационной декомпозиции;
- у reference repo появляется стабильная точка заморозки для AST/HIR/diag/bytecode/runtime interfaces;
- typed/native/concurrency-second-wave workstreams остаются намеренно вторичными и не блокируют dynamic reference runtime.

---

## 12. compile-closure patch: недостающие контракты для полностью компилируемого Amber

### 12.1. Статус и цель добавления

Этот раздел добавлен как **compile-closure patch** к проектному слою. Он не заменяет уже описанные pipeline, ABI, `.amberbc`, VM, loader, no-GIL runtime и матрицы `W0..W10`. Его задача — закрыть те места, где язык уже семантически спроектирован, но независимая реализация всё ещё могла бы разойтись в деталях:

- как именно токенизируются комментарии, range expressions, строки, интерполяция и числовые литералы;
- как формируются module init, export-cells, prelude bindings и top-level slots;
- как устроены name resolution, `UNINIT` sentinel, captured cells и локальные слоты;
- как `CALL`, keyword arguments, block arguments, callable objects, operators and type hooks проходят через единый ABI;
- как runtime exceptions, diagnostics, source maps, root maps, verifier dataflow и bytecode encoding становятся machine-readable контрактами;
- как bootstrap, stdlib, incremental build и conformance suite делают Amber не только "запускаемым", но и воспроизводимо компилируемым.

**Ключевая формулировка:** полностью компилируемый Amber — это не только `source -> bytecode -> VM`. Это ещё и стабильный набор форматов, ошибок, bootstrap-артефактов, verifier-инвариантов и тестов, позволяющий собрать один и тот же проект на разных машинах с одинаковым наблюдаемым результатом.

### 12.2. Что теперь считается "полностью компилируемым Amber"

Amber считается полностью компилируемым на уровне reference profile, если выполняются все условия:

1. **Deterministic frontend:** один и тот же source tree даёт одинаковые token/AST/HIR/diagnostic dumps независимо от абсолютного пути, hash-map порядка и адресов памяти.
2. **Closed name binding:** каждое имя после binder phase классифицировано как local slot, upvalue cell, module cell, import alias, class/mixin binding, prelude binding или compile-time unresolved name.
3. **Executable module graph:** каждый source module компилируется в `.amberbc`, зависимости выражены через `DEPS`, exports через `EXPT`, а top-level executable code живёт в module-init entrypoint.
4. **Verified bytecode:** VM не исполняет `.amberbc`, пока verifier не проверил sections, registers, jumps, handler tables, initializedness, safepoint/root maps and profile flags.
5. **Stable runtime ABI:** calls, sends, closures, blocks, callable references, class-object calls, keyword args, default thunks, type hooks and exceptions проходят через единый call/frame ABI.
6. **Precise observable failures:** user-visible ошибки имеют канонические классы, stack traces и source spans; VM bugs, malformed bytecode and unsupported profiles не маскируются под language errors.
7. **Bootstrap closure:** prelude, core classes, stdlib collections, loader, diagnostic registry and bytecode registry имеют фиксированный порядок сборки.
8. **Conformance closure:** corpus покрывает compile-only, compile+disasm, compile+load, compile+run, scheduler, loader, diagnostics, stdlib and profile gates.
9. **Optimization-preserving semantics:** dynamic bytecode VM является source of truth; MIR/native/JIT/frozen image допускаются только как семантически эквивалентные lowerings с root maps, exception maps and reflective slow paths.

### 12.3. Матрица недостающей информации и закрытия

| Область | Риск без уточнения | Закрывающий контракт в этом разделе | Блокирует |
|---|---|---|---|
| Source/literals/comments | parser разных реализаций расходится на `#`, `1..10`, interpolation, underscores | §12.4 | F0/F1 |
| Name resolution/slots | разные semantics для locals/upvalues/imports/top-level | §12.5 | F2/F3/V1 |
| Prelude/builtins | intrinsic recognition зависит от ad hoc имён | §12.6 | F2/F3/V1/V4 |
| Call ABI | `fn(args)`, class calls, blocks and keywords не имеют единого runtime path | §12.7 | F3/V0/V1 |
| Operators | `+`, `==`, `in`, `and/or` могут понизиться несовместимо | §12.8 | F3/V0 |
| Module init/class body | exports, reopen and atomic publish расходятся между compiler and loader | §12.9 | V4/W9 |
| Value model | constant pool, identity, shareability and serialization могут зависеть от pointer layout | §12.10 | V0/V1/V6 |
| Runtime errors | часть фактических ошибок не попала в canonical registry | §12.11 | diagnostics/runtime corpus |
| Type hooks | `TypeTerm` хранится, но runtime check protocol не закрыт | §12.12 | F3/V1/W9 |
| Bytecode encoding | `.amberbc` sections описаны, но binary canonicalization не полная | §12.13 | V0/V4 |
| Verifier/dataflow | register initializedness, GC roots, safepoints require precise contract | §12.14 | V1/V6 |
| Pattern decision program | P_* opcode family есть, но transaction boundaries need canonical form | §12.15 | F3/V0 |
| Debug/source maps | stack trace and diagnostics risk non-determinism | §12.16 | tooling/conformance |
| Build graph/cache | multi-module builds and incremental compilation can drift | §12.17 | V4/W8/W11 |
| Stdlib/bootstrap | core types and prelude order can become circular | §12.18 | W8/release |
| Profile gating/security | unsupported Modern Pressure Profiles could silently load | §12.19 | loader/release |
| Native/AOT path | future backend may omit deopt/root/exception maps | §12.20 | W10+ |
| Implementation matrix | no tasks for the new closure items | §12.21-§12.23 | planning |

### 12.4. Source unit, comments, literals and lexical completion

#### 12.4.1. Source unit normalization

Reference frontend reads source as UTF-8 bytes and normalizes only line endings:

```text
CRLF -> LF
CR -> LF
```

No Unicode normalization is performed by the compiler. Identifiers are compared by exact normalized source bytes after UTF-8 validation. A UTF-8 decoding failure is a compile-time diagnostic, not runtime behavior.

A source file may start with a shebang line:

```amber
#!/usr/bin/env amber
```

If `#!` appears at byte offset `0`, the entire first line is treated as a comment. Elsewhere `#!` has no special meaning.

Canonical source extensions for tooling:

- `.amber` — preferred source extension;
- `.am` — accepted shorthand in reference tooling;
- extension does not define module id; `package` does.

A missing final newline is accepted. Diagnostics should render the last line as if it had a virtual line terminator for caret reporting, but source byte offsets must remain exact.

#### 12.4.2. Comment rule and `#` conflict

Amber already uses `#` inside unbound callable references:

```amber
m = &User#full_name
```

Therefore comments are lexical only under this rule:

```text
# starts a comment when it appears outside strings/interpolation and either:
 - it is the first non-space character of a line; or
 - the previous source character is whitespace.
```

Consequences:

```amber
x = 1 # comment # comment
# full-line comment # comment
&User#full_name # HASH separator inside callable reference
foo#bar # token HASH between names; invalid unless a future form claims it
```

This keeps inline comments usable and makes `&Class#method` unambiguous without context-sensitive lexer backtracking.

#### 12.4.3. Integer literals

Reference v1 integer literals are signed only through unary `+` / `-`; the literal token itself is non-negative.

Supported v1 forms:

```text
0
123
1_000_000
0xFF
0b1010_0101
0o755
```

Rules:

- `_` may appear between digits, never at the start/end and never doubled;
- parse-time integer has arbitrary precision;
- runtime `Int` is mathematically unbounded in observable semantics;
- VM may use tagged small-int and heap BigInt internally;
- constant pool stores canonical integer magnitude/sign, not target-machine word bytes.

Overflow in tagged fast-path must promote, not wrap.

#### 12.4.4. Float literals

Supported v1 forms:

```text
1.0
0.5
1e9
1.2e-3
```

Rules:

- `Float` is IEEE-754 binary64 in reference profile;
- `_` follows the same placement rule as integer literals inside digit runs;
- `NaN`, `Infinity` and `-Infinity` are not literal tokens; they are prelude constants if exposed by stdlib;
- constant pool stores canonical binary64 payload with deterministic NaN normalization.

If a future Decimal type is added, it must use a different literal marker or explicit constructor; v1 decimal-looking literals are `Float`.

#### 12.4.5. Strings and interpolation

String literals are immutable UTF-8 `Str` values.

Required escapes:

```text
\n \r \t \\ \" \# \u{HEX}
```

Interpolation syntax:

```amber
"hello #{name}"
"sum = #{a + b}"
```

Lowering:

```text
InterpString(parts[])
 -> StringBuilder.new()
 -> append literal parts in source order
 -> for each expression part:
 evaluate expression left-to-right
 call to_s protocol
 append result
 -> freeze immutable Str
```

Interpolation expressions use normal expression grammar and normal source spans. A failure inside interpolation appears in stack traces as the original string expression span, not as generated helper code.

#### 12.4.6. Symbols

Symbol literal syntax remains:

```amber:ok:full_name
```

Reference rules:

- symbol literals intern in the current dispatch world;
- `.amberbc` stores symbol table entries as strings, never as runtime addresses;
- loader interns symbols deterministically in section order;
- symbol identity is stable within a world, but serialized symbol numeric ids are file-local and cannot be observed by user code.

#### 12.4.7. Range expressions

The main specification already uses matcher examples such as `when 1..10`. This section makes the surface form compile-explicit.

Grammar insertion:

```ebnf
CompareExpr::= RangeExpr { CompareOp RangeExpr }
RangeExpr::= AddExpr [ ".." AddExpr ]
```

Rules:

- `a..b` is an inclusive `Range` expression;
- `...` exclusive range is not part of v1 unless a later spec bump adds it;
- operands evaluate left-to-right;
- `a..b` lowers to `Range.new(a, b, inclusive_end: true)` or an equivalent intrinsic constructor;
- as a `case` matcher expression, `range === value` is used by the ordinary matcher-expression rule;
- `in` against a range calls `Range#contains?(value)` as already required by the `in` contract.

This does not introduce a new semantic category; it formalizes syntax already implied by the accepted examples.

#### 12.4.8. Collection literals

Reference v1 treats collection literals as runtime constructor lowerings with stable evaluation order:

```amber
[expr1, expr2] # Array
(expr1, expr2) # Tuple expression only when comma is present
{expr1, expr2} # Set expression when entries are values, not key/value pairs
{key: value} # Map with symbol/string key according to parsed key form
```

Rules:

- list/set/map elements evaluate left-to-right;
- duplicate literal keys in a map literal are allowed only if runtime `Map` semantics replaces earlier value by later value; compiler may warn;
- duplicate literal values in a set literal evaluate normally and collapse to one member according to runtime `Set` equality semantics;
- empty `{}` is Map literal in expression context and map pattern in pattern context;
- non-empty `{expr}` is Set literal unless the top-level contents parse as map entries;
- tuple expression requires comma. Parenthesized expression without comma is grouping.

### 12.5. Name resolution, slots, upvalues and initialization

#### 12.5.1. Scope families

Binder constructs a scope graph with these scope kinds:

```text
module
class_body
mixin_body
method
function
block
pattern_transaction
type_term
```

Each resolved name is classified as exactly one of:

```text
local_slot
upvalue_cell
module_cell
export_cell
import_alias
class_binding
mixin_binding
prelude_binding
current_self
current_owner
unresolved
```

`unresolved` is a compile-time diagnostic unless the syntax is explicitly reflective, such as `send(receiver, selector_expr,...)`.

#### 12.5.2. Top-level pre-scan

For a module, binder performs a top-level pre-scan before expression resolution:

1. collect optional `package`;
2. collect imports and local import aliases;
3. collect declared top-level names from `def`, `class`, `mixin` and top-level assignments whose left side is a simple name;
4. collect export statements;
5. verify export names against the collected top-level/import alias set.

This allows mutually recursive top-level functions to compile while preserving source-order module init. The binding cell may exist before its value is initialized.

#### 12.5.3. `UNINIT` sentinel

Every local/module/export cell has an internal `UNINIT` state until assigned.

Reading `UNINIT` raises `NameError` at runtime if the compiler cannot prove it impossible. Statically obvious cases should be compile-time diagnostics.

Examples:

```amber
puts x
x = 1
# compile-time diagnostic if same-scope uninitialized read is obvious

def f(flag):
 if flag:
 x = 1
 x
# runtime NameError possible unless later definite-assignment analysis rejects it
```

`UNINIT` is not user-observable as a value and cannot be stored in arrays, maps or fields.

#### 12.5.4. Assignment and block capture

Reference v1 assignment rules:

- assignment to a simple name creates or updates the nearest lexical binding according to binder classification;
- method/function scope owns its local slots;
- block parameters and pattern bindings are block-local;
- a block may capture outer locals as upvalue cells;
- if a block assigns to an already existing outer local, it updates the captured cell;
- if a block assigns to a name not found in an enclosing non-type scope, it creates a block-local slot;
- `def` creates a fresh function/method scope; nested `def` captures only explicitly referenced lexical cells and is serialized as a closure-capable code object.

This keeps Ruby-like closure mutation for blocks while keeping `def` boundaries compile-explicit.

#### 12.5.5. Pattern transaction scopes

Pattern matching never writes directly into ordinary local slots until the whole pattern succeeds.

Lowering model:

```text
P_BEGIN_TXN
 candidate bindings -> transaction slots
 nested OR alternatives -> subtransactions
P_COMMIT_TXN -> ordinary locals/upvalues/module cells
P_ABORT_TXN -> discard all candidate bindings
```

This rule applies to:

- pattern assignment;
- block parameter destructuring;
- `case` / `case!`;
- multi-clause `def`.

#### 12.5.6. Imported aliases

Imported aliases are read-only module cells. Any assignment target resolving to `import_alias` is a compile-time error. A `from... import Name as Alias` creates a local import alias, not a copy of the exported value.

Runtime representation:

```text
ImportAliasCell(
 source_module_id,
 public_export_name,
 cached_export_cell_ref?
)
```

The alias observes live export cell updates according to loader semantics.

### 12.6. Prelude, builtin registry and intrinsic recognition

#### 12.6.1. Prelude injection order

Every module scope has an implicit prelude parent after explicit imports and local top-level bindings:

```text
local/top-level > imports > prelude
```

Therefore user code can shadow prelude names:

```amber
send = my_send
send(obj,:x) # ordinary call, not builtin reflective send
```

Intrinsic recognition is allowed only when binder resolves a name/path to the canonical prelude binding.

#### 12.6.2. Required prelude registry

Reference v1 requires a machine-readable registry:

```text
spec/registries/prelude.yaml
```

Minimum entries:

```text
Kernel
memory
send
define_method
Array
Tuple
Map
Set
Range
LazySeq
Str
Int
Float
Bool
Null
Symbol
Object
Class
Mixin
TaskHandle
Channel
Mutex
Atomic
MatchError
TypeError
NameError
ArgumentError
NoMethodError
ImportError
ModuleInitError
IsolationError
DestroyedAccessError
UseAfterFreeError
LifetimeError
IncludeCycleError
WorldFrozenError
SuperclassMismatchError
TimeoutError
CancelledError
ChannelClosedError
DeadlockError
EmptyCollectionError
BytecodeVerificationError
UnsupportedProfileError
```

Optional profile entries may appear behind feature flags, but a `.amberbc` file must record which profile flags were required when compiling it.

#### 12.6.3. Prelude version hash

`.amberbc` must include a prelude ABI fingerprint:

```text
prelude_abi_hash = hash(public builtin names + intrinsic ids + runtime error class ids + callable ABI version)
```

Loader rejects a module if its required prelude ABI is incompatible with the running VM.

#### 12.6.4. Intrinsic table

Minimum intrinsic ids:

```text
INTR_SEND_LITERAL
INTR_SEND_DYN
INTR_DEFINE_METHOD
INTR_MEMORY_DEALLOC
INTR_OBJECT_DESTROY
INTR_KERNEL_WATCH_BINDING
INTR_KERNEL_WATCH_IVAR
INTR_KERNEL_WATCH_CVAR
INTR_TASK_ASYNC
INTR_TASK_SPAWN
INTR_TASK_WAIT
INTR_TASK_CANCEL
INTR_TASK_RESUME
INTR_CHANNEL_SEND
INTR_CHANNEL_RECV
INTR_CHANNEL_CLOSE
```

Intrinsics are not syntax by themselves. They are selected by binder/lowering only when the resolved binding is the canonical prelude object/method and the argument shape matches the intrinsic contract.

### 12.7. Call ABI, keyword arguments, blocks and callable protocol

#### 12.7.1. Unified call packet

All calls and sends lower to a `CallPacket` before entering VM dispatch:

```text
CallPacket(
 kind, # call / send / send_dyn / super_reserved / native
 callee_or_receiver,
 selector_sym?, # for send
 selector_value?, # for send_dyn
 pos_regs[],
 kw_names_sym[],
 kw_value_regs[],
 block_reg?,
 callsite_id,
 source_span_id
)
```

Evaluation order remains source order:

1. receiver/callee;
2. positional args left-to-right;
3. keyword value expressions left-to-right in source order;
4. block closure creation if present;
5. call dispatch.

`kw_names_sym[]` may be canonicalized for cache keys after all keyword value expressions have been evaluated. Duplicate keyword names are detected before entering the target body and raise `ArgumentError` unless statically diagnosed earlier.

#### 12.7.2. Frame entry layout

Every callable frame has:

```text
Frame(
 code_object,
 caller_frame?,
 current_task,
 self_value?,
 current_owner?,
 local_slots[],
 temp_registers[],
 upvalue_cells[],
 block_slot?,
 last_result,
 handler_stack,
 return_ip,
 source_span_id
)
```

Rules:

- ordinary functions have `self_value = null` unless bound as a method;
- instance methods receive receiver as `self_value`;
- class methods receive class object as `self_value`;
- block closures inherit lexical `self_value` unless explicitly rebound by future syntax;
- `current_owner` is needed for `@@`, class-side dispatch and debug information.

#### 12.7.3. Callable object contract

`HCall` / `CALL` accepts:

1. closure objects;
2. native builtin callables;
3. callable reference objects created by `&target`;
4. class objects, which call constructor path through `:new`;
5. ordinary objects whose class lookup resolves selector `call`.

If none applies, runtime raises `TypeError`.

For ordinary objects with `call`, `obj(args...)` is observably equivalent to `obj.call(args...)`, including `method_missing`, keyword args and block forwarding. Class objects remain special: `Class(args...)` is constructor call and does not mean `Class.call(args...)`.

#### 12.7.4. Blocks as hidden final argument

A block suffix compiles to a closure object stored in `block_reg` of the `CallPacket`, not into the positional argument array.

A method that consumes a block receives it through the frame's hidden `block_slot`. Standard library methods such as `map`, `select`, `reduce` invoke the block through `CALL_BLOCK` or ordinary `CALL` on the block object.

Block parameter pattern matching happens at block frame entry. A mismatch raises `MatchError`.

#### 12.7.5. Default thunks and type hooks

Default parameter expressions compile to default-thunk code objects:

```text
DefaultThunk(
 param_index,
 code_object,
 captured_signature_prefix_slots,
 source_span_id
)
```

A default thunk runs in the callee binding context after explicit args left of it are bound, before auto-assign commit.

Type hooks compile to `TypeCheckProgram` ids and run after defaults, before clause dispatch/auto-assign.

#### 12.7.6. Callsite cache key

A callsite cache key must include at least:

```text
receiver_class_or_callable_shape
selector_sym or callable_kind
kw_shape_id
block_presence
world_epoch
method_table_version
```

For callable references to unbound instance methods, cache key also includes owner class and selector.

### 12.8. Operator lowering and primitive specialization

#### 12.8.1. Selector mapping

Operators lower to semantic operations with fallback selectors:

| Surface | Semantic opcode | Fallback selector/protocol |
|---|---|---|
| `a + b` | `BINARY_OP add` | `:+` |
| `a - b` | `BINARY_OP sub` | `:-` |
| `a * b` | `BINARY_OP mul` | `:*` |
| `a / b` | `BINARY_OP div` | `:/` |
| `a % b` | `BINARY_OP mod` | `:%` |
| `a == b` | `COMPARE_OP eq` | `:==` |
| `a != b` | `COMPARE_OP ne` | `:==` then boolean invert |
| `< <= > >=` | `COMPARE_OP` | corresponding symbolic selector |
| `x in y` | `MEMBER_OP in` | `y.contains?(x)` |
| `not x` | `BOOL_NOT` | truthiness primitive |
| `a and b` | control-flow | no selector |
| `a or b` | control-flow | no selector |

`BINARY_OP` may specialize for `Int`, `Float` and `Str`, but fallback must preserve normal method dispatch and errors.

#### 12.8.2. Truthiness lowering

Control-flow truthiness uses the language rule:

```text
false and null are falsy; everything else truthy
```

No user-defined `truthy?` hook exists in v1. This is important for predictable optimization and branch lowering.

#### 12.8.3. Equality vs identity

Reference runtime distinguishes:

```text
object_id / identity # VM identity
== # user equality protocol
value_equals for patterns # same observable equality as ==, with literal-specific fast paths allowed
```

Pattern literal matching may fast-path immediates but must be observably equivalent to language equality.

### 12.9. Module init, class/mixin bodies and atomic publish

#### 12.9.1. Module init code

Every `.amberbc` module may contain:

```text
module_init_code_id?
```

Top-level executable forms lower into `module_init_code`. `package`, `import` and `export` are not executable. `def`, `class` and `mixin` create or mutate module cells during init in source order, but their binding cells are allocated during link.

Module init runs once per successful loader instance:

```text
linked -> initializing -> ready
 \-> failed
```

If init fails, the module remains `failed` and its export cells are not considered ready.

#### 12.9.2. Export cells

Export table entries point to export cells:

```text
ExportCell(
 public_name,
 local_binding_ref,
 state, # uninit / initializing / ready / failed
 value
)
```

`from` imports read export cells. Reading a cell in `initializing` state raises `ModuleInitError` unless the loader can prove the value was already initialized before the cycle edge.

#### 12.9.3. Class and mixin body transaction

A class/mixin body compiles to a transaction object:

```text
WorldTransaction(
 target_kind, # class / mixin
 target_name,
 superclass_ref?,
 instance_methods[],
 class_methods[],
 direct_includes[],
 direct_extends[],
 nested_declarations[],
 source_span_id
)
```

Runtime executes the body in a staging area. If all body forms succeed, the transaction commits atomically:

- method table entries are replaced as whole entries;
- include/extend lists update in source order;
- class/mixin version increments;
- `world_epoch` increments if dispatch-relevant;
- inline caches become invalid according to epoch/version guards.

If body execution fails, no partial method/include publish is visible.

#### 12.9.4. Reopen compatibility

Reopen checks happen at transaction prepare time:

- existing binding must be class/mixin of the requested kind;
- superclass clause must match existing superclass if present;
- post-freeze transaction prepare raises `WorldFrozenError`;
- include cycle detection may happen before or during prepare, but must happen before commit.

### 12.10. Value model and serialized constants

#### 12.10.1. Runtime `Value` categories

Reference VM may choose any internal tagging scheme, but the semantic categories are fixed:

```text
NullValue
BoolValue
IntValue
FloatValue
SymbolValue
HeapObjectRef
NativeHandle
TombstoneRef
```

Only `false` and `null` are falsy. All other values, including `0`, `""`, empty collections and tombstones before access checks, are truthy at the truthiness primitive level. Accessing destroyed/deallocated objects fails before ordinary operations observe payload.

#### 12.10.2. Constant pool values

Constant pool may contain only immutable serialized constants:

```text
null
bool
int
float
string
symbol
tuple_of_constants
frozen_array_of_constants
frozen_map_of_constants
type_term_blob
signature_blob
pattern_blob
```

It must not contain:

- raw pointers;
- mutable heap objects;
- strand-confined objects;
- native handles;
- pre-initialized `Channel`, `Mutex`, `Atomic` or `TaskHandle`.

Mutable literals are constructed at runtime from immutable constant payloads.

#### 12.10.3. Identity and `object_id`

`object_id` is runtime-world-local. It is never serialized into `.amberbc`. Debug output must not include raw addresses; if an identity is needed in deterministic tests, use stable synthetic ids assigned by the test harness.

#### 12.10.4. Shareability metadata

Every heap object header exposes runtime flags:

```text
frozen
shareable
sync
pinned
has_destructor
dead
watched
```

Compiler may embed shareability expectations in bytecode metadata, but runtime remains authoritative at cross-strand boundaries.

### 12.11. Exception model and runtime error registry completion

#### 12.11.1. Exception object ABI

Every raised runtime error is an object with at least:

```text
ExceptionObject(
 error_class,
 message,
 payload?,
 backtrace_frames[],
 cause?,
 source_span_id?
)
```

Backtrace frames contain symbolic method/module/code ids and source spans, not memory addresses.

#### 12.11.2. Unwind model

VM unwind walks frames until it finds a matching handler table entry. During unwind:

1. mark current instruction as throwing;
2. run pending ensure/finalizer handlers represented in bytecode handler table;
3. release transient native pins whose scope is tied to the frame;
4. notify structured task failure machinery if the root task frame unwinds;
5. either enter handler or report unhandled exception.

Even if source-level `rescue` syntax is not enabled in P0, the bytecode ABI must support handler tables because runtime, stdlib, scheduler and native bridges need deterministic unwind/finalization.

#### 12.11.3. Required additions to runtime error registry

The main spec already names many canonical errors, but a fully compilable implementation also needs the following registry entries. These should be mirrored into `spec/registries/runtime_errors.yaml` and into the next main-spec editorial sync.

| Error | Raised when |
|---|---|
| `NameError` | unresolved dynamic name read or read of `UNINIT` local/module cell that was not rejected statically |
| `ArgumentError` | arity mismatch, duplicate keyword, missing required argument, unknown keyword, invalid block argument shape |
| `EmptyCollectionError` | `reduce` without init on empty collection and similar stdlib empty-required operations |
| `IndexError` | builtin indexed access is out of bounds |
| `KeyError` | builtin map/key access requires key presence and key is absent |
| `ZeroDivisionError` | builtin numeric division/modulo by zero |
| `EncodingError` | invalid runtime string/buffer encoding at a boundary that cannot be compile-time diagnosed |
| `BytecodeVerificationError` | verifier rejects malformed `.amberbc` before execution |
| `UnsupportedProfileError` | loader sees required feature/profile flag unsupported by the current runtime |
| `InternalCompilerError` | compiler invariant failure; not catchable as user runtime exception |
| `InternalVMError` | VM invariant failure; not catchable as ordinary user exception |

`InternalCompilerError` and `InternalVMError` are tooling/runtime fatal classes. They should be visible in logs but not part of normal language-level control flow.

#### 12.11.4. Compile diagnostics vs runtime exceptions

Rule of preference:

```text
if violation is statically obvious:
 emit compile diagnostic and do not produce executable bytecode
else:
 preserve runtime check and canonical runtime exception
```

A compiler must not remove runtime checks solely because a dynamic feature might be absent in a test corpus. In particular, reflective calls, module cycles, pattern protocol methods and FFI/native handles require runtime checks.

### 12.12. `TypeTerm` runtime check program

#### 12.12.1. Lowering

Every `TypeTerm` lowers to a `TypeCheckProgram`:

```text
TypeCheckProgram(
 ops[],
 source_span_id,
 mode # parameter / return / cast / internal
)
```

The bytecode emitter may inline simple checks or call a runtime type-check interpreter.

#### 12.12.2. Check semantics

Minimum operations:

```text
CHECK_CLASS const_ref # calls T === value and requires Bool
CHECK_NULL
CHECK_UNION program_ids[]
CHECK_TUPLE fixed_len, item_program_ids[]
CHECK_RECORD fields[], rest_program?
CHECK_GENERIC head_const_ref, arg_program_ids[]
CHECK_OPTIONAL inner # sugar for union(inner, Null)
```

Rules:

- `T` checks use `T === value`;
- `T?` is `T | Null`;
- tuple type requires exact tuple arity;
- record type requires named fields/keys and open-by-default extra keys unless exactness is represented by future syntax;
- builtin `Array[T]`, `Map[K,V]`, `Set[T]` may perform deep finite checks in reference stdlib;
- for user generics without a registered type-check hook, v1 preserves generic args in metadata and performs head check only.

This matches the existing "minimal type envelope" without pretending full static generics already exist.

#### 12.12.3. Return boundary

A function/method with `-> TypeTerm` checks the value at every explicit or implicit return path:

```text
result = frame.last_result or RETURN operand
CHECK_TYPE result, return_type_program
RETURN result
```

If a task root fails a return boundary, the failure is stored in `TaskHandle` and rethrown by `wait()` like any other exception.

### 12.13. Canonical `.amberbc` binary encoding

#### 12.13.1. Header

Reference binary layout:

```text
AmberBcHeader(
 magic = "AMBC",
 format_major: u16,
 format_minor: u16,
 language_major: u16,
 language_minor: u16,
 abi_major: u16,
 abi_minor: u16,
 endian = 1, # 1 = little endian
 pointer_size = 0, # 0 because file stores no raw pointers
 flags: u64,
 section_count: u32,
 header_size: u32,
 section_table_offset: u64,
 file_size: u64,
 content_digest_kind: u16,
 content_digest_offset: u64
)
```

All multi-byte integers are little-endian. Variable-length indexes may use unsigned LEB128 inside sections, but section table fields are fixed-width for mmap-friendly loading.

#### 12.13.2. Section table

Each section record:

```text
SectionRecord(
 id4: bytes[4],
 version_major: u16,
 version_minor: u16,
 flags: u32,
 offset: u64,
 length: u64,
 alignment: u32,
 uncompressed_length: u64,
 digest_offset: u64
)
```

Required section ids:

```text
SYMS symbol table
STRS string table
CONS constant pool
TYPE type-term/type-check blobs
SIGS signatures
PATT pattern programs
CODE bytecode code objects
METH method records
MODU module record and init entrypoint
DEPS dependency manifest
EXPT export table
LINE line/source map minimum
```

Optional section ids:

```text
DBUG rich debug info
DOCS doc/comments metadata
PROF profile requirements
CAPA capability manifest
MIR0 MIR/SSA future profile
NATV native code future profile
SIGN signature payload
```

Unknown required section flag -> `UnsupportedProfileError` at loader time. Unknown optional section may be ignored if digest and bounds are valid.

#### 12.13.3. Deterministic section ordering

Writer must emit sections in this order unless a future format bump changes it:

```text
SYMS, STRS, CONS, TYPE, SIGS, PATT, CODE, METH, MODU, DEPS, EXPT, LINE, DBUG, DOCS, PROF, CAPA, MIR0, NATV, SIGN
```

No timestamp, username, hostname, absolute build path or random UUID may appear in required deterministic sections.

### 12.14. Verifier dataflow, root maps and safepoints

#### 12.14.1. Control-flow graph verification

Verifier builds a CFG per `BcCode` and checks:

- every jump target lands on instruction boundary;
- no instruction falls through outside code length;
- handler ranges are non-empty, ordered and point to valid handler entries;
- all referenced constants, symbols, signatures, patterns and methods exist;
- opcodes are allowed under module feature/profile flags;
- register indexes are within declared register count;
- all registers read on a path are definitely initialized on that path.

The initializedness lattice:

```text
UNINIT < INIT_VALUE
```

Verifier is not a static type checker. It tracks "is initialized" and "may contain GC reference" categories, not exact Amber classes.

#### 12.14.2. Register categories for GC

Verifier computes or validates a root category per live register at safepoints:

```text
NON_REF
MAYBE_REF
PIN_TOKEN
NATIVE_HANDLE
CALLABLE_REF
```

A register may be conservative `MAYBE_REF`; precise GC can still scan it as a tagged value.

#### 12.14.3. Mandatory safepoints

Bytecode must have safepoints at:

- function/method call;
- backward branch edge;
- allocation;
- `SPAWN_*`, `WAIT`, `SLEEP`, `YIELD`;
- blocking `Channel` / `Mutex` ops;
- native/FFI call enter and exit;
- explicit `SAFEPOINT` instruction if none of the above appears on a long-running loop path.

At each safepoint, `RootMap` lists live locals, temps, upvalues and transient pins.

#### 12.14.4. RootMap format

```text
RootMapEntry(
 code_id,
 ip_offset,
 local_bitmap,
 temp_bitmap,
 upvalue_bitmap,
 pin_bitmap,
 handler_depth,
 flags
)
```

Root maps are required even for non-moving collector because shared cycles, FFI handles and future native/JIT backends need precise liveness.

#### 12.14.5. BytecodeVerificationError

Verifier failure creates a structured diagnostic for tooling and a loader-visible `BytecodeVerificationError`. The VM must not attempt partial execution of a rejected code object.

### 12.15. Pattern decision program canonical form

#### 12.15.1. Pattern program structure

Each compiled pattern is a small decision program:

```text
PatternProgram(
 temp_count,
 binding_count,
 ops[],
 success_label,
 fail_label,
 source_span_id
)
```

Required op families:

```text
P_LOAD_SUBJECT
P_TEST_LITERAL
P_TEST_PIN
P_TEST_CLASS
P_TEST_TRIPLE_EQ
P_COERCE_SEQ
P_COERCE_MAP
P_LEN_EQ
P_LEN_GE
P_HAS_KEY
P_GET_INDEX
P_GET_KEY
P_SLICE_REST
P_PROJECT_REST
P_BIND
P_BEGIN_ALT
P_COMMIT_ALT
P_ABORT_ALT
P_DYNAMIC_MATCH
P_CHECK_DYNAMIC_RESULT
P_JUMP
P_JUMP_IF_FAIL
P_SUCCESS
P_FAIL
```

#### 12.15.2. Binding transaction rule

`P_BIND` writes only to pattern transaction slots. The caller context commits transaction slots only after `P_SUCCESS`.

OR-pattern lowering:

```text
for each alternative:
 P_BEGIN_ALT
 run alternative program
 if success:
 P_COMMIT_ALT
 jump success
 else:
 P_ABORT_ALT
continue
fail
```

All alternatives must have identical binding sets by compile-time precheck.

#### 12.15.3. Guards

Pattern guards are not part of `PatternProgram`; they are normal HIR/bytecode expressions executed after pattern success and before binding commit is made externally visible to the clause body.

Guard environment sees candidate pattern bindings through temporary transaction slots.

### 12.16. Diagnostics, source maps and stack trace schemas

#### 12.16.1. Diagnostic JSON

`amber.diag.v1` entries must include:

```json
{
 "schema": "amber.diag.v1",
 "code": "E0000",
 "severity": "error|warning|lint",
 "phase": "lex|parse|bind|lower|verify|load|runtime",
 "message": "...",
 "primary_span": {
 "source_id": "...",
 "module_id": "...",
 "byte_start": 0,
 "byte_end": 0,
 "line_start": 1,
 "column_start": 1,
 "line_end": 1,
 "column_end": 1
 },
 "notes": [],
 "help": null,
 "related": []
}
```

Messages may be localized by tooling, but golden tests compare code, severity, phase and spans first.

#### 12.16.2. Source map minimum

`LINE` section must map bytecode instruction offsets to:

```text
SourceLoc(
 source_id,
 module_id,
 byte_start,
 byte_end,
 line_start,
 col_start,
 line_end,
 col_end,
 generated_kind? # direct / lowering / default_thunk / interpolation / block_suffix
)
```

Generated code still points back to the surface expression that caused it.

#### 12.16.3. Stack trace frame

Deterministic stack frame rendering uses:

```text
StackFrame(
 module_id,
 function_or_method_name,
 dispatch_owner?,
 code_id,
 source_loc,
 inline_context[]
)
```

No raw pointer values, thread ids or nondeterministic object ids appear in golden stack traces.

### 12.17. Build graph, incremental compilation and reproducibility

#### 12.17.1. Build graph node

A build graph node:

```text
BuildNode(
 module_id,
 source_path,
 source_digest,
 package_root_digest?,
 compiler_version,
 language_version,
 feature_flags,
 prelude_abi_hash,
 deps[]
)
```

`module_id` comes from `package` if present; otherwise from entrypoint build configuration.

#### 12.17.2. Dependency fingerprint

A dependency edge fingerprint includes:

```text
dep_module_id
dep_public_export_surface_hash
dep_abi_hash
dep_language_version
dep_feature_flags
```

Changing private implementation without public ABI changes may allow incremental reuse of downstream HIR if the compiler supports it, but reference conformance only requires safe invalidation, not maximal caching.

#### 12.17.3. Reproducible build rule

Two builds are reproducible if:

- same normalized source bytes;
- same compiler and prelude ABI;
- same feature/profile flags;
- same dependency ABI fingerprints;
- same target format version.

Then emitted `.amberbc` bytes must be identical except optional `SIGN` section if signing mode includes external timestamped signatures. Deterministic signing mode must also be byte-identical.

#### 12.17.4. Minimal build CLI

Add to `amberc` CLI contract:

```text
amberc build path/to/root.amber -o build/out/
amberc build --entry package.main -o build/out/
amberc metadata path/to/file.amberbc --json
amberc verify path/to/file.amberbc --json
```

`build` compiles the transitive source graph, writes `.amberbc` modules, and emits a build manifest:

```text
amber.build.json
```

### 12.18. Bootstrap and stdlib closure

#### 12.18.1. Bootstrap layers

Reference implementation uses four bootstrap layers:

```text
B0 runtime kernel
 - Value representation
 - Object/Class/Mixin metaobjects
 - allocator/GC roots
 - bytecode interpreter
 - loader/verifier

B1 native prelude
 - core classes
 - error classes
 - intrinsic registry
 - basic numeric/string/symbol operations

B2 Amber stdlib bytecode
 - collections
 - ranges
 - task/channel/mutex/atomic wrappers
 - pattern helper objects
 - diagnostics-facing helpers

B3 tools
 - amberc
 - ambervm
 - ambertest
 - package/build tooling
```

B2 must be buildable with the same `.amberbc` pipeline used for user code. B0/B1 may be native implementation code.

#### 12.18.2. Required stdlib modules

Minimum module ids:

```text
amber.core
amber.collection
amber.range
amber.string
amber.error
amber.task
amber.sync
amber.memory
amber.reflect
amber.pattern
amber.io # may be stubbed in P0 if no host I/O profile is enabled
```

`amber.core` and `amber.error` are preloaded before user module init. `amber.collection` must be ready before conformance runtime tests involving block suffix collection style.

#### 12.18.3. Stdlib ABI hash

Each stdlib module exports an ABI hash. User `.amberbc` compiled against stdlib must record the stdlib ABI range it requires. Loader rejects incompatible stdlib with `ImportError` or `UnsupportedProfileError` depending on whether the module is missing or present-but-incompatible.

### 12.19. Profile flags, capabilities and safe loading

#### 12.19.1. Feature/profile flag model

Every `.amberbc` records:

```text
required_features[]
optional_features[]
forbidden_features[]
```

Examples:

```text
core.v1
notebook.watch.v1
typed.v1
capabilities.v1
effects.v1
ffi.v1
native.mir.v1
```

Loader behavior:

- missing required feature -> `UnsupportedProfileError`;
- unsupported optional feature -> ignore optional sections and continue;
- feature explicitly forbidden by host policy -> `CapabilityError` or `UnsupportedProfileError` before init.

#### 12.19.2. Capability manifest

If capability profile is enabled, `CAPA` section declares host resources the module may request:

```text
CapabilityRequest(
 kind, # fs / net / env / process / clock / random / ffi / gpu / db / secrets
 mode, # read / write / execute / connect / allocate / observe
 target?,
 reason?
)
```

Compiler emits metadata; host grants capabilities at load/run time. Absence of capability must not be bypassed by native/FFI escape hatches.

#### 12.19.3. Bytecode safety boundary

`.amberbc` is data, not trusted code. Loader must bounds-check every section before decoding. Verifier must reject:

- unknown required opcodes;
- invalid section offsets/lengths;
- integer overflow in decoded sizes;
- code that references disabled profile instructions;
- malformed root maps/handler tables;
- constant pool values disallowed by shareability rules.

### 12.20. Native/AOT/JIT path closure requirements

Native/AOT remains optional, but the project layer now fixes what it must preserve.

A native backend must emit or preserve:

```text
NativeCodeObject(
 source_bc_code_id,
 machine_code_blob,
 relocation_table,
 call_stub_table,
 deopt_or_slowpath_table?,
 root_maps,
 exception_maps,
 safepoint_maps,
 world_epoch_assumptions,
 profile_flags
)
```

Rules:

- bytecode semantics remain reference truth;
- reflective `send`, `method_missing`, dynamic pattern objects and `TypeTerm` hooks must either compile to slow stubs or remain bytecode-interpreted;
- frozen-world native code records the `world_epoch` and method-table versions it assumed;
- if deopt is not implemented, invalidation must discard native code and re-enter bytecode at safe call boundaries;
- native code cannot omit GC/root maps;
- native code cannot turn runtime language errors into process crashes.

### 12.21. New implementation matrix additions: `W13`, `W14` and `W15`

The existing `W0..W12` / modern-profile matrix remains valid. This section adds three non-conflicting work packages that should be treated as blockers for "fully compilable release-grade Amber", even if a smaller prototype can run before they are complete.

| Work package | Priority | Scope | Exit criterion |
|---|---:|---|---|
| `W13` Compiler-contract closure | P0/P1 | source/literals, name slots, prelude registry, call ABI, operator lowering, error registry, verifier dataflow, root/source maps | independent compiler+VM components agree through machine-readable registries and golden tests |
| `W14` Build/bootstrap/conformance closure | P1/P2 | build graph, incremental cache, stdlib bootstrap, reproducible artifacts, profile flags, conformance bundles | multi-module Amber project builds reproducibly and passes compile/load/run corpus |
| `W15` Native-readiness metadata | P3 | MIR/native root maps, exception maps, slow stubs, frozen assumptions | native/JIT work can start without changing bytecode/VM semantics |

### 12.22. New issue catalogue `ISS-073..ISS-096`

#### `ISS-073` source/literal/comment completion

- finalize comment `#` rule and shebang handling;
- implement numeric literal validation and constant-pool canonicalization;
- implement string interpolation AST/HIR/source spans;
- implement inclusive range expression `a..b`;
- add parser and diagnostics corpus.

#### `ISS-074` binder slot model and `UNINIT`

- implement top-level pre-scan;
- classify names into local/upvalue/module/import/prelude classes;
- implement `UNINIT` sentinel and `NameError` path;
- implement block capture/update rules;
- add definite-assignment smoke diagnostics.

#### `ISS-075` prelude and intrinsic registry

- create `spec/registries/prelude.yaml`;
- assign stable intrinsic ids;
- implement prelude ABI hash in `.amberbc`;
- verify shadowing disables intrinsic lowering;
- add conformance tests for `send`, `define_method`, `Kernel.watch`, `memory.dealloc`.

#### `ISS-076` unified call ABI

- implement `CallPacket`;
- implement keyword shape canonicalization after source-order evaluation;
- implement hidden block slot;
- implement callable object protocol including ordinary object `call`;
- add cache key tests for selector/kw/block/world epoch.

#### `ISS-077` operator lowering

- implement semantic opcodes/fallback selectors for arithmetic/comparison;
- lower `and/or` as control-flow returning operands;
- lower `in` as `contains?`;
- add fast-path plus fallback corpus.

#### `ISS-078` module init and world transactions

- compile top-level executable forms to module init;
- allocate export cells at link;
- implement class/mixin body staging and atomic commit;
- add reopen/failure/no-partial-publish tests.

#### `ISS-079` value model and constant pool rules

- freeze constant pool allowed types;
- reject mutable/shareability-invalid constants;
- implement symbol/string deterministic interning;
- add serialization round-trip tests.

#### `ISS-080` runtime error registry completion

- add missing error classes to registry and prelude;
- define fatal internal compiler/VM error reporting;
- implement stable exception object ABI and stack frames;
- add runtime negative corpus for `NameError`, `ArgumentError`, `EmptyCollectionError`, `IndexError`, `KeyError`, `ZeroDivisionError`.

#### `ISS-081` `TypeCheckProgram`

- lower `TypeTerm` to check programs;
- implement parameter/return/cast check sites;
- implement builtin generic hooks for `Array`, `Map`, `Tuple`, `Set`;
- add typed-boundary corpus without requiring full static checker.

#### `ISS-082` canonical `.amberbc` binary encoding

- implement fixed header and section table;
- implement deterministic section ordering;
- add digest validation;
- add reader/writer round-trip corpus.

#### `ISS-083` verifier dataflow and root maps

- implement CFG verifier;
- implement initializedness analysis;
- validate handler ranges and root maps;
- reject missing safepoints on back-edge paths;
- add malformed bytecode fixtures.

#### `ISS-084` pattern decision program canonicalization

- implement `PatternProgram` transaction slots;
- implement OR subtransactions;
- implement dynamic matcher result verification;
- add pattern disasm golden tests.

#### `ISS-085` source maps and diagnostic schema

- implement `LINE` minimum section;
- implement generated-kind origin tags;
- stabilize `amber.diag.v1` fields;
- add stack trace golden tests without raw pointers.

#### `ISS-086` build graph and reproducible build

- implement `amberc build`;
- implement `amber.build.json`;
- hash source/prelude/dependency ABI inputs;
- prove byte-identical `.amberbc` for identical builds.

#### `ISS-087` stdlib bootstrap

- split B0/B1/B2/B3 bootstrap layers;
- compile B2 stdlib through ordinary bytecode pipeline;
- define stdlib ABI hashes;
- add loader tests for stdlib version mismatch.

#### `ISS-088` profile and capability metadata

- implement `PROF` section;
- implement `CAPA` section parser;
- reject unsupported required profiles;
- add host-policy negative tests.

#### `ISS-089` conformance compile-all bundle

- create corpus bundle that compiles every positive fixture to `.amberbc`;
- run `verify`, `disasm`, `load`, `run` where applicable;
- fail on missing golden expectations for changed public formats.

#### `ISS-090` native-readiness metadata

- specify native root maps, exception maps and safepoint maps;
- define runtime slow-stub ABI;
- add bytecode/native equivalence requirements for future W13.

#### `ISS-091` CLI metadata and verifier commands

- add `amberc metadata --json`;
- add `amberc verify --json`;
- normalize errors for corrupted bytecode files;
- add CLI golden tests.

#### `ISS-092` keyword/callsite cache corpus

- test duplicate keyword detection after value evaluation;
- test kw shape cache stability;
- test block presence in cache key;
- test `world_epoch` invalidation.

#### `ISS-093` export-cell cycle corpus

- test cyclic imports with initialized vs initializing export reads;
- test failed init remains failed;
- test repeated load attempts;
- test live alias updates.

#### `ISS-094` GC root-map conformance

- create stress fixtures for allocations at calls/back-edges/native boundaries;
- validate that live values survive local and shared cycles;
- test transient pin release during exception unwind.

#### `ISS-095` string/range/interpolation corpus

- test escape validation;
- test interpolation evaluation order;
- test `Range#===` and `in` behavior;
- test source spans inside interpolation.

#### `ISS-096` spec-sync registry issue

- track items that must be mirrored into the main language spec:
 - comment/shebang rules;
 - inclusive range expression grammar;
 - missing runtime error classes;
 - prelude/builtin registry;
 - callability of objects with `call`;
 - `.amber`/`.am` source extension policy.

### 12.23. New milestone `M11`: fully compilable reference gate

`M11` is reached only after `M0..M5` plus `W13/W14` are green.

Checklist:

- `amberc build` compiles a multi-module project into `.amberbc` artifacts;
- every artifact passes `amberc verify`;
- `ambervm run` executes the built entrypoint without compiler process present;
- stack traces are source-mapped and deterministic;
- rebuild with same inputs is byte-identical;
- all prelude/std/bytecode/error registries have versioned machine-readable files;
- positive corpus passes parse/lower/check/compile/verify/disasm/load/run phases;
- negative corpus confirms diagnostics/runtime errors with canonical codes/classes;
- unsupported profile/capability requests are rejected before module init;
- no test requires raw pointer values, absolute local paths or nondeterministic map ordering.

### 12.24. Updated immediate implementation order

For the next implementation cycle, the recommended order becomes:

1. freeze machine-readable registries: tokens, diagnostics, prelude, runtime errors, opcodes, bytecode sections;
2. implement source/literal/comment/range/interpolation parser coverage;
3. implement binder slot classification and `UNINIT`/`NameError`;
4. implement `CallPacket`, keyword shape and block slot ABI;
5. implement operator lowering and callable object `call` protocol;
6. implement `.amberbc` fixed header/section table and metadata command;
7. implement verifier CFG/dataflow/root-map validation before expanding VM fast paths;
8. implement module init/export-cell/class-body transaction path;
9. bootstrap B2 stdlib through ordinary bytecode pipeline;
10. add reproducible `amberc build` and full compile-all conformance bundle.

### 12.25. Anti-drift rules added by compile-closure patch

1. No new bytecode opcode without `spec/registries/opcodes.yaml`, verifier rule and disasm golden.
2. No new prelude intrinsic without `prelude.yaml`, shadowing test and ABI hash update.
3. No new runtime error class without `runtime_errors.yaml`, constructor ABI and at least one negative corpus case.
4. No lowering that creates generated code without a source-map `generated_kind`.
5. No call optimization that bypasses `CallPacket` observable semantics.
6. No native/JIT optimization without root map and exception map.
7. No loader acceptance of unknown required profile flags.
8. No `.amberbc` writer change without reproducible-build fixture update.
9. No pattern optimization that commits bindings before full success.
10. No stdlib bootstrap shortcut that tests user code against APIs unavailable from compiled `.amberbc`.

### 12.26. Final closure statement

After this section, the remaining work is implementation, not language architecture. Amber's reference path is now closed at these levels:

```text
source bytes
 -> normalized tokens/comments/literals
 -> syntax-faithful AST
 -> resolved slots/imports/prelude/intrinsics
 -> HIR with explicit calls/patterns/defaults/blocks
 -> pattern/type-check programs
 -> deterministic `.amberbc`
 -> verifier CFG/dataflow/root maps
 -> loader/linker/module init/export cells
 -> register/slot VM with stable call/value/error ABI
 -> stdlib/bootstrap/conformance
 -> optional MIR/native/frozen profile preserving bytecode semantics
```

This is the engineering definition of "Amber as a fully compilable language" for the project layer.