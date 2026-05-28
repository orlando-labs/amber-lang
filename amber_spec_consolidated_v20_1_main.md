> Редакция v20.1: поверх v20.0 зафиксирован **Callable Reference & Constructor Call Revision**. В core syntax добавлены callable references `&NameSpace.some_fn`, class-side callable references `&Class.method`, unbound instance method references `&Class#method`, канонический вызов callable-значений `fn(args...)`, а также callable class objects: `Class(args...)` является preferred constructor-call form и наблюдаемо эквивалентен `Class.new(args...)`. `&` не означает raw machine address; это создание immutable callable reference object, совместимого с `HCall` / `CALL`, open-world dispatch и frozen-world invalidation rules.


# Amber

**Консолидированная спецификация (текущее зафиксированное состояние)**  
Редакторская консолидация по истории разработки  
29 апреля 2026

> **Редакторская реструктуризация v20.1-project.** Подробный проектный слой компилируемого Amber, включая bytecode/runtime blueprint, `.amberbc`/loader/verifier, репозиторную декомпозицию, backlog, milestone-gates и матрицу имплементации, вынесен в отдельный файл: [`amber_compilable_project_layer_v20_1_complete.md`](amber_compilable_project_layer_v20_1_complete.md). В этом основном файле сохраняется языковая спецификация и базовый reference на проектный слой; длинная инженерная декомпозиция больше не дублируется здесь.



# О документе

Этот документ сводит историю проектирования Amber в одну согласованную редакцию. Основание — три файла с историей обсуждения: `amber.lang.txt`, `amber.lang-2.txt`, `amber-lang-3.txt`.

Цель документа — не «додумать язык с нуля», а:
1. зафиксировать то, что уже принято;
2. развести нормативные части и незакрытые зоны;
3. собрать единый пакет для следующего шага: парсер, рантайм, типизация и инструментирование.

## Редакторские правила консолидации

История разработки содержит несколько противоречивых промежуточных черновиков. В этой редакции используется такой принцип:

- при конфликте между файлами приоритет имеет `amber-lang-3.txt`, затем `amber.lang-2.txt`, затем `amber.lang.txt`;
- внутри `amber-lang-3.txt` нормативными считаются прежде всего явно закрывающие формулировки вроде «фиксируем», «финальный синтаксис», «закрытая v1-спека»;
- если в `amber-lang-3.txt` встречаются более поздние, но явно объяснительные или регрессивные примеры, которые противоречат уже закрытому нормативному блоку, это помечается как редакторская коллизия и разбирается отдельно;
- всё, что не удалось честно довести до «зафиксировано», вынесено в раздел **Открытые вопросы**.

## Статус

Amber в текущем виде — **консолидированная спецификация с закрытым implementation gate, зафиксированным reference blueprint для runtime P0/P1 и закрытыми profile-level решениями второй волны**. Ядро синтаксиса и семантики уже достаточно устойчиво, чтобы писать не только прототип парсера и интерпретатора, но и первый байткодный runtime; инженерная граница старта поддержана каноническими AST/HIR/diagnostic dump-контрактами, обязательным каталогом diagnostic codes v1, layout-моделью corpus/golden-файлов и bootstrap order для reference toolchain. Ранее зафиксированные minimal type envelope для parser/HIR/runtime hooks, обязательный stdlib contract для chainable collections, формальная матрица диагностик, policy по underscore-спецформам, окончательная граница bare matcher expressions и v1-решение по field lifetime annotations, а также reference execution profile, reference lifetime profile, collector/pinning/FFI profile, compiled-module/loader profile, strict `case!`, source-level modules, minimal MOP, mixin/`include` profile и frozen-world boundary сохраняются без регрессий: register/slot bytecode VM, frame/closure/object ABI, pattern-matching opcode family, ownership model без GIL, explicit destruction/deallocation (`destroy!` + `memory.dealloc`), non-moving generational collector, pinning boundary для native interop, `.amberbc`-формат и verifier/loader state machine, static `package/import/export`, reopenable named classes, named mixins, declarative `include`, linearized ancestor composition, reflective `define_method`, builtin `send(...)`, `method_missing` fallback и двухфазная модель dispatch-world (`open` -> `frozen`). В v16 дополнительно закрываются optional Amber/Typed checker profile, package/distribution/signing/hot-reload policy, read-only reflection mirrors, class-side `extend`, advanced concurrency (`move`, `select`, supervisor policies, async I/O awaitables), weak/ephemeron/buffer/borrow story без field modifiers и canonical MIR/SSA + native/JIT/AOT + frozen-image profile. В v19.2 дополнительно закрыт optional Amber/Notebook Watch Profile: `Kernel.watch(target)` является compiler/kernel intrinsic для notebook-инвалидации, но не считается world mutation и не меняет production semantics. В v20 дополнительно закрывается слой Modern Pressure Profiles: capabilities/sandbox, effects, observability/replay, columnar BI, schemas/API contracts, Wasm components, accelerators, AI-agent tooling, contracts/property testing, privacy/lineage и durable workflows. В v20.1 поверх этого закрывается core-level callable reference / constructor-call revision: `&target`, `&Class#method`, canonical `fn(args...)` и callable class objects `Class(args...)`. Эти возможности не расширяют Modern Pressure Profiles и не являются host-only фичами: они входят в минимальный синтаксис и lowering core. После этой редакции незакрытых spec-level вопросов больше не остаётся: дальше остаются только реализация, тестовые корпуса, toolchain и выбор порядка включения профилей в конкретных hosts.

# Часть I. Полноценная спека Amber в текущем зафиксированном виде

## 1. Дизайн-якоря языка

Amber — это язык с такими базовыми обязательствами:

- от Python берутся отступы как способ задавать блоки и отказ от `end`;
- от Ruby берутся объектная модель, сигилы полей `@` и `@@`, чейнинг методов, блоки как основной способ передавать замыкания, именование методов с суффиксами `?` и `!`, а также ориентация на метапрограммирование;
- управляющие конструкции являются выражениями;
- pattern matching — часть ядра, а не библиотечный сахар;
- методы не требуют явного первого параметра `self` или `cls`;
- callable-значения являются first-class: `&target` создаёт callable reference, `fn(args...)` является каноническим вызовом callable, а class object может вызываться как constructor `Class(args...)`;
- базовая коллекционная модель — `map/select/reduce/...` как методы, а не питоновские внешние `map/filter`.

## 2. Лексика, идентификаторы и блоки

### 2.1. Отступы и блоки

Блок открывается двоеточием `:` и продолжается отступом, как в Python.

Это относится к:

- `if / else if / elif / elsif / else`;
- `case` / `case!`;
- `while / until / do ... while / loop`;
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
- `_1`, `_2`, ... — это нумерованные аргументы блока только в блоках без `|...|`;
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
foo?bar     # ? не внутри имени
bang!name   # ! не внутри имени
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
[expr1, expr2]       # Array
(expr1, expr2)       # Tuple, только если внутри скобок есть запятая
{expr1, expr2}       # Set, если элементы не являются key/value парами
{key: value}         # Map с symbol/string ключом по форме записи ключа
```

`{}` в expression-контексте остаётся пустым `Map`. Непустая форма `{expr}`
является одноэлементным `Set`, если содержимое верхнего уровня не разбирается
как map-entry. Элементы `Array`/`Set` и значения `Map` вычисляются слева
направо. Повторные ключи `Map` заменяют предыдущие значения, а повторные
элементы `Set` схлопываются по runtime-семантике равенства.

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
name  = input.strip().presence() or "anon"
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
2. без списка параметров, но с `_1`, `_2`, ... внутри тела.

```amber
numbers.map |x|: x * 2
numbers.map: _1 * 2
```

Блок **всегда относится к ближайшему вызову слева**, а не «перепрыгивает» на следующий сегмент цепочки.

```amber
numbers.map: _1 * 2 .select: _1 > 0
# читается как:
# (numbers.map { ... }).select { ... }
```

### 4.3. One-liner block boundary rule

Чтобы one-liner-блоки были однозначны в чейнинге, фиксируется лексическое правило.

- обычный доступ/вызов: **без пробела перед точкой**;
- продолжение цепочки после one-liner блока: **точка с пробелом слева**.

```amber
numbers.map: _1 * 2 .select: _1 > 0 .reduce 0: _1 + _2
```

Внутри one-liner блока чейнинг разрешён, но только без пробела перед точкой:

```amber
users.map: _1.email.downcase().strip() .uniq()
```

То есть:

- `_1.email.downcase()` — часть тела блока;
- ` .uniq()` — продолжение внешней цепочки.

### 4.4. Скобки в чейнинге

В этой редакции статус скобочной формы **закрыт**:

```amber
numbers.map(_1 * 2).select(_1 > 0)
```

трактуется **только как обычный вызов с аргументами в скобках**, а не как альтернативная компактная форма блока.

Следствия:

- `_1`, `_2`, ... существуют только внутри block suffix без `|...|`;
- запись `map(_1 * 2)` в v1 **невалидна**, потому что `_1` вне блока не существует;
- для компактного трансформационного стиля нужно писать либо `map: _1 * 2`, либо `map |x|: x * 2`.

То есть допустимы:

```amber
numbers.map: _1 * 2
numbers.map |x|: x * 2
```

А это не входит в v1:

```amber
numbers.map(_1 * 2)   # invalid in v1
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

Amber v20.1 фиксирует first-class callable references как часть core syntax.

Каноническая форма вызова callable-значения:

```amber
fn(args...)
```

Форма `fn.()` в язык не вводится и не является альтернативным spelling'ом. Точка остаётся только operator'ом member access / method send, а `fn(args...)` понижается в `HCall`.

Prefix `&` создаёт **callable reference object**, а не raw machine address. Пользователь не получает числовой адрес функции, FFI pointer или стабильный code pointer. Runtime вправе представлять callable reference как closure, descriptor object, send-reference, loader-backed entry или иной объект, если выполняется наблюдаемый callable contract.

Поддерживаемые v1-формы:

```amber
fn = &NameSpace.some_fn      # module/top-level callable binding или export
cm = &User.find              # class-side method reference: receiver = User, selector = :find
m  = &User#full_name         # unbound instance method reference
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
&obj.method      # instance-bound method reference не входит в v1 spelling
&foo()           # нельзя взять reference результата вызова через &
&(foo + bar)     # нельзя брать reference произвольного выражения
```

Если нужен bound instance callable, v1 использует обычный closure/block-level adapter, а не новый surface spelling. Более явные формы вроде `obj.&method` могут быть добавлены отдельным будущим RFC, но не являются частью v20.1.

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

Если список параметров не указан, внутри блока доступны placeholders `_1`, `_2`, ...

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
- `def name(...): ...` как выражение даёт `:name`
- `class Name: ...` как выражение даёт созданный объект класса
- `mixin Name: ...` как выражение даёт созданный mixin object
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
- `do ... while`
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
    break :ok
# result == :ok
```

### 7.4. `break`

`break` может нести значение:

```amber
break :some_symbol
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
  class_method def find(id):
    ...
```

### 8.4. Конструктор `init`, `new` и constructor-call sugar

Конструктор называется `init`.

```amber
class Point:
  def init(@x, @y):
    pass
```

`new(...)` остаётся явным классовым путём создания объекта: он выделяет объект и вызывает `init(...)`, если тот существует.

Amber v20.1 дополнительно фиксирует preferred constructor-call form:

```amber
p1 = Point.new(10, 20)  # explicit construction path
p2 = Point(10, 20)      # preferred constructor-call sugar
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

То есть `HCall` / `CALL` по class object выполняет constructor path через selector `:new` с теми же positional/keyword-аргументами и optional block. `.new(...)` не удаляется: он остаётся частью явного MOP/reflection story и может использоваться через ordinary send, `send(Point, :new, ...)` или callable reference `&Point.new`.

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

- `import` и `from ... import ...` разрешены только на top-level;
- import-секция идёт после optional `package` и до первого non-import top-level item;
- bare `import a.b.c` создаёт локальный read-only binding с именем последнего сегмента пути, то есть `c`;
- `import a.b.c as x` создаёт локальный binding `x`;
- `from a.b.c import Name as Alias` создаёт локальный read-only binding `Alias`, связанный с export `Name` из целевого модуля;
- все imports входят в статический dependency graph и сериализуются в `.amberbc` через `DEPS`;
- относительные импорты (`.foo`, `..bar`) в v1 не вводятся;
- `from ... import *` в v1 не вводится;
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

Amber v13 фиксирует **named mixin object profile**, отделённый и от package/import system, и от ordinary class inheritance.

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
- mixin object является именованным binding'ом и может импортироваться/экспортироваться обычным `import` / `from ... import ...` / `export`;
- mixin body допускает `def`, nested `class`, nested `mixin`, `include` и `pass`;
- `class_method def` внутри mixin body в v1 запрещён как compile-time error;
- методы mixin'а живут на **instance-side** и участвуют в lookup только после включения через `include`.

`mixin Name: ...` как выражение даёт соответствующий mixin object.

### 8.12. `include` и ancestor composition

`include` в v13 — это **declarative body form**, а не loader-форма и не namespace-import.

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
- class-side mixins / `extend` не входят в минимальный v13-профиль этой подчасти; финальная v16-норма позже добавляет отдельную declarative form `extend` на class-side, см. Q15 и последующие реализационные части.

### 8.13. Open classes


Amber v13 сохраняет **minimal open-class model** и сочленяет его с именованными mixin'ами.

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

- reopenable `class` не является новым синтаксисом: используется та же surface-form `class Name: ...`;
- superclass clause может присутствовать у первоначального объявления;
- при reopen superclass clause либо опускается, либо должен резолвиться в тот же superclass; несовместимость даёт `SuperclassMismatchError` (компилятор вправе диагностировать это раньше);
- один syntactic class-body коммитится **атомарно**: методы/классовые методы, определённые внутри, становятся видимы целиком после успешного завершения body;
- поздний reopen **заменяет** целый method entry по данному selector'у на соответствующей стороне dispatch, а не «добавляет ещё одну clause» к уже существующему multi-clause `def`;
- clause aggregation по правилам §10 работает только внутри одного syntactic def-group / class-body, а не через отдельные reopen-операции.

### 8.14. Reflective `define_method`

Минимальный reflective API v13 задаётся builtin-функцией:

```amber
define_method(User, :greet) |name|:
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
- class-side reflective `define_method`, remove/alias/visibility hooks и richer class-side composition сверх later-added declarative `extend` в минимальный v13-профиль не входят.

Успешный `define_method` считается world mutation и обязан обновлять dispatch invalidation metadata по правилам части X.

### 8.15. Reflective `send`

Reflective dispatch фиксируется как builtin-функция:

```amber
send(user, :full_name)
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

`method_missing` получает в v13 минимальную, но нормативную семантику.

Правило для обычного `obj.foo(...)` и reflective `send(obj, sel, ...)`:

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

Amber v13 разводит **data mutation** и **world mutation**.

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

### 8.18. Что не входит в minimal MOP v13

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

Каноническая форма, закреплённая поздними нормативными блоками `amber-lang-3`, такая:

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
4. если матч успешен — вычислить guard `if ...`, если он есть;
5. первая ветка, у которой match + guard, побеждает;
6. если нет совпадения:
   - если есть `else` — выполняется `else`;
   - иначе для обычного `case` результатом является `null`, и `$_` становится `null`;
   - иначе для `case!` поднимается `MatchError`.

Нормативно:

- `case!` является strict-form того же `case`, а не отдельным паттерн-языком;
- lowering для `case!` обязан использовать тот же `HMatchDispatch`, меняя только `fail_mode`;
- `case!` лексируется как отдельная keyword-form, а не как `case` + postfix `!`.

> Редакторское примечание: внутри истории есть и более строгая ветка, где `case` без `else` бросает `MatchError`. Более поздняя закрытая v1-формулировка в `amber-lang-3` переводит обычный `case` в safe-form со значением `null`. В этой редакции tension закрыт так: обычный `case` остаётся safe-form, а строгий вариант доступен как отдельная surface-form `case!`.

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
(x, x)   # compile-time error
```

#### Литералы

```amber
null
true
false
42
"str"
:ok
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
T(p1, p2, ...)
T(x:, y:, ...)
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
- при `success = true` и отсутствии `with ...` `bindings` также обязаны быть пустыми;
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

Только в `case` и `case!` разрешается fallback-форма, когда `when ...` содержит не структурный паттерн, а выражение-матчер.

```amber
case x:
  when 1..10:
    :small
  when String:
    :str
  when {id:, **null}:
    :obj
  else:
    :other
```

Если после `when` запись не разбирается как `Pattern`, она трактуется как `MatcherExpr`, и проверяется:

```amber
MatcherExpr === value
```

Отдельно от bare matcher expressions в v1 разрешены dynamic pattern objects явной формы:

```amber
when pattern(route("/users/:id")) with {id:, **null}:
  ...
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
- если `CLAUSE_PATTERN` — tuple pattern `( ... )`, матч идёт по `ArgsTuple` — кортежу позиционных аргументов;
- иначе форма разрешена только если в сигнатуре ровно один позиционный параметр; тогда матч идёт по нему.

Примеры:

```amber
def fmt(x, mode: :short):
  when {mode: :short}:
    "S: #{x}"
  when {mode: :long}:
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

Выбрасывается, когда source-level `import` / `from ... import ...` не может быть удовлетворён на loader/linker path: отсутствует требуемый модуль, отсутствует запрошенный export или нарушен обязательный dependency contract.

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

Выбрасывается, когда `wait(timeout: ...)` или иной нормативный deadline-aware runtime path не успевает завершиться до дедлайна.

#### `CancelledError`

Выбрасывается, когда task наблюдает ранее поставленный cancellation flag в safe-point и не завершилась естественным образом раньше этого места.

#### `ChannelClosedError`

Выбрасывается, когда код пытается `send()` в уже закрытый channel либо делает `recv()` из закрытого и уже пустого channel.

#### `DeadlockError`

Выбрасывается, когда non-reentrant `Mutex` наблюдает повторный `lock()` тем же владельцем без промежуточного `unlock()`.

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


## 12. Типовая система v14: minimal type envelope для implementation gate

Типовая система по-прежнему **не является завершённой нормативной частью full-checker'а**, но v14 закрывает минимальный контур, достаточный для parser/HIR/runtime hooks и старта reference implementation.

Принятые решения:

- язык остаётся gradual / optional typed;
- без аннотаций код ведёт себя как динамический;
- с аннотациями допускаются runtime type-hooks уже в первой реализации;
- `as` сохраняется как единая surface-form для binding annotation и checked cast;
- `name as {id:, name:}` и `name as Int` в pattern-контексте остаются as-pattern, а не типизацией, потому что справа pattern-term, а не type-term.

### 12.1. Return type syntax

Разрешается следующая форма:

```amber
def parse(src as Str) -> Ast:
  ...

class Parser:
  class_method def load(path as Str) -> Parser:
    ...
```

Нормативно:

- `-> TypeTerm` допускается после списка параметров `def` и `class_method def`;
- return type относится ко всему callable;
- return boundary использует тот же type-hook contract, что и параметрический `as TypeTerm`.

### 12.2. Минимальная grammar `TypeTerm`

```ebnf
TypeTerm        ::= TypeUnion
TypeUnion       ::= TypeSuffix { "|" TypeSuffix }
TypeSuffix      ::= TypePrimary [ "?" ]
TypePrimary     ::= ConstPath
                  | ConstPath "[" TypeTerm { "," TypeTerm } [ "," ] "]"
                  | "(" TypeTerm { "," TypeTerm } [ "," ] ")"
                  | "{" TypeField { "," TypeField } [ "," ] [ "," "**" TypeTerm ] "}"
TypeField       ::= Name ":" TypeTerm
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

a = Atomic.new(0)
a.get()
a.set(1)
a.compare_and_set(1, 2)
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
- `Atomic.get()`, `Atomic.set(...)` и `Atomic.compare_and_set(...)` в v1 обладают seq-cst semantics.

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

Чтобы surface syntax языка опирался на единый нормативный runtime API, в v14 фиксируется обязательный коллекционный профиль стандартной библиотеки.

Для `Array`, `Tuple`, `Range`, `Set` и `LazySeq` обязательны:

- `each`
- `map`
- `flat_map`
- `select`
- `reject`
- `reduce`
- `find`
- `any?`
- `all?`
- `none?`
- `first`
- `count`
- `group_by`
- `to_a`
- `lazy`

Нормативно:

- `each` возвращает receiver;
- `map`, `flat_map`, `select`, `reject` и `group_by` по умолчанию eager;
- `.lazy` переводит дальнейшую цепочку в lazy-profile;
- `to_a` материализует `LazySeq`.

`reduce` поддерживает две формы:

```amber
xs.reduce(init) |acc, x|: ...
xs.reduce |acc, x|: ...
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
- `transform_values |v|:`
- `keys`
- `values`
- `entries`

Нормативно:

- `Map#map` возвращает `Array`;
- `Map#select` и `Map#reject` возвращают `Map`;
- `Map#transform_values` возвращает `Map`.


## 14. Что входит в язык по намерению, но ещё не нормализовано до ядра


### 14.1. Amber/Notebook Watch Profile [закрыто в v19.2]

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
#   binding:user@rev0
#   object:user.@name@rev0

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
Kernel.watch(x)   # ordinary send, не intrinsic
```

`Kernel.watch(...)` не является ordinary method call в полном смысле: runtime должен получить не только значение `x`, но и binding/ivar target. Поэтому frontend обязан распознавать эту форму после name resolution и понижать её в dedicated HIR/bytecode hook либо в эквивалентный VM intrinsic.

#### Допустимые watch-targets v1

В v19.2 notebook profile допустимы только syntactic watch-targets:

```amber
Kernel.watch(x)       # local / top-level binding
Kernel.watch(@x)      # instance variable текущего self
Kernel.watch(@@x)     # class variable текущего owner
```

В v1 запрещены:

```amber
Kernel.watch(foo())        # нет binding target
Kernel.watch(user.name)    # неоднозначно: field read или method call
Kernel.watch(xs[0])        # indexing является protocol call
Kernel.watch(1 + 2)        # expression value, не storage target
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
user.object_id == old_id   # true
user.class == User         # true
User === user              # true
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
world_epoch  не меняется
watch_epoch  может меняться
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

- файл: [`amber_compilable_project_layer_v20_1_complete.md`](amber_compilable_project_layer_v20_1_complete.md);
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

Ниже собраны остаточные незакрытые зоны, а также несколько редакторских следов решений, закрытых уже к редакции v11, чтобы не потерять контекст дальнейших обсуждений.

## Q1. Политика стиля для имён с подчёркиванием [закрыто в v14]

В v14 политика закрывается так:

- `_`, `$_`, `_1`, `_2`, ... — специальные формы языка;
- все остальные идентификаторы синтаксически допустимы;
- имена, слишком похожие на спец-формы, не становятся compile-time error и могут подпадать только под lint/tooling policy.

То есть вопрос закрывается не дополнительными запретами grammar-level, а разведением language core и style-level lint.

## Q2. Финальный модульный / импортный синтаксис [закрыто в v11]

В v11 source-level module syntax зафиксирован в static-profile:

- `package module.path` задаёт logical module id импортируемого файла;
- `import module.path [as Alias]` и `from module.path import Name [as Alias]` — единственные специальные формы загрузки/связывания;
- `export Name [as Public]` формирует export table исходного модуля;
- relative imports и star-import в v1 не вводятся;
- `require` не является loader-формой;
- `include` зафиксирован отдельно как mixin-composition form и не участвует в loader semantics.

В v16 ecosystem/toolchain-level хвосты тоже закрываются: manifest/registry/signing/hot-reload policy стандартизованы отдельно, без изменения source-level module syntax.

## Q3. Глубина нормализации метапрограммирования [закрыто в v13]

В v13 зафиксирован **minimal MOP profile**:

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

## Q4. Типовая система: grammar, semantics, inference [закрыто в v16]

В v16 типовая система закрывается как optional **Amber/Typed** profile поверх того же source language.

Принятые решения:

- typed profile включается на уровне package/build profile, а не новым source-keyword;
- exported `def`, `class_method def` и публичные constructor/boundary API в typed-package обязаны иметь явные parameter и return annotations;
- локалы, block params, внутренние/private callables и field-types допускают local inference;
- generics в v16 считаются **invariant**;
- record-types остаются open by default, а exact-record пишется через `**Never`;
- `and` / `or` получают truthiness-aware flow typing (`false | null` — falsy-set Amber);
- `$_` имеет flow-type последнего выражения текущего scope; в начале scope его typed-view считается `Null`;
- `case` / `case!` делают pattern-based narrowing по subject; `case!` без `else` в typed profile требует exhaustiveness;
- reflective boundaries (`send(...)` с dynamic selector, `method_missing`, runtime `define_method`, reopen/`include`/`extend` через внешний open-world path) считаются `Any`-boundary, если сборка не находится в frozen typed profile.

Этим grammar/semantics/inference-вопросы закрываются на уровне спецификации. Дальше остаётся только реализация checker'а, flow engine и tooling.

## Q5. Concurrency после фиксации v1-core [закрыто в v16]

Базовая no-GIL модель v1 сохраняется, а вторая волна закрывается следующими решениями:

- ownership transfer вводится через explicit `move(expr)` на cross-strand boundaries (`task.spawn`, `Channel.send`, `select` send-arm и другие ownership APIs);
- moved-from binding после успешного transfer больше не может читаться: статически очевидные случаи — compile error typed/lint-layer, иначе runtime `MovedValueError`;
- multi-channel wait вводится как expression `select:` с `when`, optional `timeout` и optional `else` arms;
- supervisor policy стандартизуется как keyword `policy:` для `async` и `task.spawn`; обязательные значения: `:cancel_scope` (default), `:one_for_one`, `:one_for_all`, `:rest_for_one`;
- async I/O интегрируется через standard awaitables/readiness tokens из `amber.io`, совместимые с `select`;
- distributed / multi-process runtime **не входит** в core language spec и остаётся library/host-level story поверх тех же message/ownership правил.

То есть concurrency-вопросы больше не открыты на языке: дальше остаётся только реализация scheduler/runtime и библиотек.

## Q6. Динамические pattern-objects [закрыто в v10]

В v10 dynamic pattern objects включены в v1, но только в **explicit-binding profile**.

Принятое решение:

- разрешить `pattern(expr)` и `pattern(expr) with MAP_PATTERN`;
- запретить скрытую инъекцию локалов;
- разрешить feature только в `case`, `case!` и clause-style `def`;
- оставить block params и pattern assignment вне v1 для этой формы.

Richer matcher protocols, typed bindings-map и library-level combinators остаются допустимой будущей библиотечной эволюцией, но больше не считаются незакрытым spec-level вопросом.

## Q7. Формальный статус `matcher expressions` вне `case` / `case!` [закрыто в v14]

В v14 это закрывается так:

- bare matcher expressions разрешены только в `case` / `case!`;
- в `def`-клаузаx, block params и pattern assignment они не вводятся;
- любые дальнейшие расширения этой формы возможны только отдельным RFC второй волны.

## Q8. Финальный раздел по стандартной библиотеке коллекций [закрыто в v14]

В v14 обязательный коллекционный профиль нормализован как часть спецификации:

- для `Array`, `Tuple`, `Range`, `Set` и `LazySeq` зафиксирован минимальный `Enumerable`-подобный contract;
- для `Map` зафиксированы `each/map/select/reject/transform_values/keys/values/entries`;
- для `reduce` зафиксированы формы с `init` и без `init`, включая `EmptyCollectionError` на пустой коллекции без `init`.

Следующая волна может расширять stdlib, но старт reference implementation больше не зависит от незакрытого коллекционного API.

## Q9. Нужна ли строгая match-форма поверх безопасного `case` [закрыто в v10]

Да. В v10 принят `case!` как строгая surface-form поверх того же `case`-engine.

Принятое решение:

- `case` без `else` остаётся safe-form и возвращает `null`;
- `case!` без `else` бросает `MatchError`;
- grammar `when PATTERN if GUARD:` и lowering остаются общими;
- отдельный `match!` в v1 не вводится.

## Q10. Формальная матрица диагностик компилятора [закрыто в v14]

В v14 вводится обязательное трёхчастное разведение диагностик:

- `compile_error` — hard fail языка;
- `warning` — обязательное предупреждение компилятора, не останавливающее сборку;
- `lint` — tooling-level правила, не входящие в language acceptance.

Минимальный обязательный каталог `compile_error` охватывает pattern/binder, module/import/export и class/mixin/MOP placement rules; обязательным `warning` v1 считается чтение `@field` из default-expression при наличии позднего auto-assign в то же поле.


## Q11. Где проходит граница между fully dynamic Amber и native/AOT profile [закрыто в v12]

Граница теперь зафиксирована так:

- обычный dynamic Amber может жить в dispatch-world состоянии `open` неограниченно долго;
- Amber/Frozen — build/runtime profile, в котором после loader/linker/module-init выполняется freeze transition;
- после freeze любая world mutation (`class`/`mixin` reopen, `define_method`, `include`, меняющий ancestor graph, поздняя Amber module load в тот же world) запрещена и даёт `WorldFrozenError`;
- `send(...)` и `method_missing` после freeze остаются легальными, но рассматриваются как reflective slow-path, а не как источник новых world mutations;
- язык не требует обязательного deopt-механизма: реализации вправе либо держать такие места на generic path, либо строить JIT/deopt поверх того же language contract.

В v16 и backend/toolchain boundary тоже закрывается: canonical MIR/SSA, native/JIT/AOT profile и `.amberimg` фиксируются как отдельный post-v1 profile.



## Q12. Остаточные вопросы после фиксации collector/pinning/FFI profile [закрыто в v16]

Reference profile по-прежнему закрывает: non-moving generational collector, pin tokens, pinned scopes, opaque-handle FFI boundary, safe-point handshake и запрет implicit GC-finalizer semantics для пользовательского `destroy!`.

В v16 вторая волна памяти фиксируется так:

- weak refs и ephemerons стандартизуются как runtime/library types `WeakRef[T]` и `Ephemeron[K, V]` в пакете `amber.memory`;
- surface borrow annotations в source grammar **не добавляются**; borrowing остаётся API-level через block-scoped helpers вроде `memory.borrow(obj) |view|: ...`;
- zero-copy typed buffers/slices стандартизуются как runtime classes (`Bytes`, `Buffer[T]`, `Slice[T]`) с pin-aware semantics;
- collector telemetry/tuning и host embedding API относятся к host/runtime profile, а не к core language syntax.

Этим memory/FFI second wave закрывается на уровне спецификации: дальше остаются только runtime и embedding implementation details.

## Q13. Остаточные вопросы после фиксации module format / loader / verifier profile [закрыто в v16]

Reference profile по-прежнему закрывает: `.amberbc`-артефакт, section model, loader state machine, dependency manifest, export/import symbol tables и минимальный verifier contract.

В v16 distribution/toolchain policy фиксируется так:

- package manifest стандартизуется как `amber.toml`;
- registry/publish unit — signed package bundle `.amberpkg`, который содержит manifest, compiled modules, export tables, digests и optional source/debug payload;
- source files внутри пакета обязаны иметь `package`, равный manifest package либо находящийся под тем же dotted-prefix;
- reproducible builds и content digests обязательны для publishable artifacts; trust chain строится на embedded Ed25519 signatures + lockfile digests;
- hot reload разрешён только в open-world dev profile и только как atomic package-swap; в frozen profile он запрещён;
- incompatible reload, меняющий public export surface или нарушающий manifest/ABI contract, обязан завершаться `ReloadIncompatibleError`.

Этим distribution ecosystem закрывается на уровне спецификации; дальше остаётся только реализация registry/client/publisher/tooling.

## Q14. Нужны ли полевые lifetime-аннотации (`owned`, `weak`, `borrowed`) [окончательно закрыто в v16]

В v16 это решение доводится до окончательного вида:

- source-level field modifiers `owned`, `weak`, `borrowed` **не будут добавляться** в Amber source grammar;
- lifecycle языка остаётся построенным вокруг ordinary object model, `destroy!`, `memory.dealloc`, weak/ephemeron wrappers и block-scoped borrow helpers;
- ownership/borrowing/weakness выражаются не модификаторами полей, а runtime/library objects и host-interop API.

То есть Amber окончательно закрывает memory story без field-level lifetime annotations.

## Q15. Class-side mixins / `extend` [закрыто в v16]

В v16 class-side composition фиксируется как отдельный declarative profile:

- `extend` разрешён только непосредственно внутри body `class` и её reopen-форм;
- каждый operand `extend` обязан резолвиться в mixin object;
- instance-methods mixin'а при `extend` становятся методами class object receiver'а;
- локальные `class_method def` доминируют над методами, пришедшими через `extend`;
- при нескольких `extend` действует то же правило, что и для `include`: later direct extend wins;
- `extend` является world mutation и подчиняется тем же freeze/invalidation правилам, что и `include`.

`extend` в `mixin` body и произвольный reflective class-side alias/remove API по-прежнему не входят в язык.

## Q16. Расширенный reflection / introspection API [закрыто в v16]

В v16 расширенная рефлексия стандартизуется как read-only stdlib/runtime package `amber.reflect`.

Принятые решения:

- reflection выдаёт immutable mirror objects (`ClassMirror`, `MixinMirror`, `MethodMirror`, `PackageMirror`, `WorldMirror`);
- mirror API покрывает name/kind/superclass/ancestors/includes-or-extends, method tables, selector ownership, source locations, parameter metadata и optional typed signature metadata;
- mirrors являются snapshot-views и не дают прав на world mutation;
- mutation path по-прежнему ограничен ранее зафиксированными механизмами (`class`/`mixin` reopen, `include`, `extend`, `define_method`).

То есть вопрос introspection закрывается без возврата к "полной Ruby-MOP".

## Q17. Native backend / JIT / frozen-image profile [закрыто в v16]

В v16 backend boundary фиксируется так:

- canonical optimizer/backend IR — `MIR` в SSA-форме поверх уже зафиксированного HIR;
- bytecode VM остаётся reference execution engine, а native/JIT backend — дополнительным профилем;
- native compilation допускается только для frozen-world artifacts/images;
- reflective sites (`SEND_DYN`, `method_missing`, open-world mutation paths) остаются runtime stubs/guards и не требуют обязательного deopt-механизма;
- deployable frozen image стандартизуется как `.amberimg`, bundling manifest, package table, code payload (bytecode and/or native), debug map и signatures.

После этого AOT/JIT вопрос тоже закрыт на уровне языка и artifact model: дальше остаётся только реализация MIR/backend/image-builder.



## Q18. Capability and sandbox profile [закрыто в v20]

В v20 фиксируется optional **Amber/Capabilities & Sandbox** profile.

Принятые решения:

- package, plugin, notebook cell, workflow step и Wasm component не получают host resources по умолчанию;
- filesystem, network, env, process, clock, random, FFI, GPU/device, database и secret-store доступы выдаются через manifest-declared capability grants;
- capability grant является host-issued token, а не ordinary user object, который можно подделать в Amber коде;
- `amber.toml` получает секцию `[capabilities]`, а `.amberbc`/`.amberpkg` могут хранить declared capability requirements в optional metadata section;
- отсутствие required capability является compile/load-time diagnostic, если доказуемо, и `CapabilityError`, если обнаружено только runtime;
- sandbox profile не меняет core language semantics и не считается world mutation.

Этим закрывается вопрос безопасного исполнения untrusted notebooks, BI snippets, plugins, serverless functions и AI-agent generated code.

## Q19. Effect and purity profile [закрыто в v20]

В v20 фиксируется optional **Amber/Effects** profile поверх Amber/Typed.

Принятые решения:

- callable может объявлять effect row через suffix `!{...}` после return type или после parameter list, если return type отсутствует;
- минимальные effect labels: `pure`, `alloc`, `mut`, `world`, `watch`, `async`, `strand`, `fs`, `net`, `env`, `time`, `random`, `ffi`, `db`, `gpu`, `unsafe`, `reflect`, `workflow`;
- пустая строка effects `!{}` означает deterministic/pure-without-observable-effects boundary, кроме allocation, если host profile явно считает allocation unobservable;
- effect rows используются checker'ом, optimizer'ом, notebook invalidator, sandbox loader и replay runtime;
- dynamic/reflective boundaries, которые невозможно проверить статически, поднимаются до conservative effect row `!{unsafe, reflect}` либо требуют explicit annotation.

Этим закрывается вопрос формального различения pure computation, data mutation, world mutation и host I/O без добавления checked exceptions.

## Q20. Observability and replay profile [закрыто в v20]

В v20 фиксируется optional **Amber/Observability & Replay** profile.

Принятые решения:

- runtime events получают canonical names, attributes, source spans, task/strand ids, world/watch epochs и optional trace context;
- обязательные event families: task, strand, channel, mutex, atomic, gc, loader, world, watch, ffi, capability, effect, schema, workflow;
- `trace.span "name": ...` является library/intrinsic boundary, который может lower'иться в HIR event scope;
- deterministic execution scope виртуализирует clock, random, scheduler order и external input providers;
- replay trace `.ambertrace` хранит event log, dependency fingerprints, virtual sources, watch revisions и package/build digests;
- replay divergence является `ReplayDivergenceError`, а forbidden nondeterminism внутри deterministic scope — `DeterminismError`.

Этим закрывается вопрос воспроизводимых notebooks, BI refresh jobs, CI flakes и production debugging без изменения обычного scheduler contract.

## Q21. DataFrame / columnar BI profile [закрыто в v20]

В v20 фиксируется optional **Amber/DataFrame & Columnar BI** profile.

Принятые решения:

- stdlib получает normative table abstractions: `Table`, `Column[T]`, `Series[T]`, `Schema`, `LazyTable`, `QueryPlan`, `GroupedTable`;
- `Table` operations являются relational/vectorized operations, а не ordinary object iteration;
- lazy query plans обязаны быть inspectable, hashable по lineage fingerprint и совместимыми с notebook dependency capture;
- column-level revision keys расширяют v19.2 watch profile: cell may depend on `table_id.column(:amount).revision`;
- columnar memory ABI может быть Arrow-compatible, но core Amber не зависит от конкретного external memory format.

Этим закрывается BI/story для аналитических notebooks, reactive dashboards и high-volume ETL без превращения `Array#map` в скрытый relational engine.

## Q22. Schema, serialization and API contracts profile [закрыто в v20]

В v20 фиксируется optional **Amber/Schema & API Contracts** profile.

Принятые решения:

- `schema Name vN:` является declarative profile form, сериализуемой в AST/HIR metadata и optional `.amberbc` section;
- schema fields имеют required/optional/default/deprecated/renamed metadata;
- schema evolution поддерживает explicit migration hooks и compatibility checks;
- codecs (`Json.codec(T)`, `Binary.codec(T)`, host codecs) обязаны выполнять boundary validation;
- API contract generator может эмитить OpenAPI-like descriptions для HTTP/RPC boundaries;
- schema violations дают `SchemaViolationError`.

Этим закрывается вопрос stable wire contracts для backend, services, plugin APIs и data pipelines.

## Q23. Wasm component and host plugin profile [закрыто в v20]

В v20 фиксируется optional **Amber/Wasm Component** profile.

Принятые решения:

- `.amberwasm` является deployable component artifact поверх frozen-world subset;
- imports/exports мапятся на WIT-like component interface descriptions;
- Amber capabilities мапятся на host/WASI-style resource permissions;
- raw FFI внутри Wasm profile запрещён по умолчанию;
- reflective world mutation после component instantiation запрещена;
- host plugin execution должен сочетать schema contracts, capabilities и effect rows.

Этим закрывается portable sandbox story для BI plugins, edge/serverless snippets и embeddable extensions.

## Q24. Accelerator / GPU / SIMD profile [закрыто в v20]

В v20 фиксируется optional **Amber/Accelerator** profile.

Принятые решения:

- `Tensor`, `DeviceBuffer[T]`, `Device`, `Kernel` и `DeviceStream` являются runtime/library types;
- accelerator kernels используют restricted closure subset: primitive numeric types, tensors, buffers, slices, constants и pure helper calls;
- arbitrary object access, dynamic dispatch, allocation, reflection, exceptions через host stack и hidden I/O внутри kernel запрещены;
- host/device transfer semantics explicit: copy, borrow/pin, map, unmap, synchronize;
- accelerator operations несут effect `gpu` или более конкретный device effect;
- нарушения kernel subset или device lifetime дают `AcceleratorError`.

Этим закрывается путь к GPU/SIMD/accelerator workloads без переноса полной dynamic object model на устройство.

## Q25. AI-agent tooling and provenance profile [закрыто в v20]

В v20 фиксируется optional **Amber/AI-Agent Tooling & Provenance** profile.

Принятые решения:

- compiler/toolchain обязан уметь эмитить machine-readable symbol graph, semantic spans, type/effect summaries и refactoring-safe anchors;
- agent patches должны применяться через structured patch protocol, а не только raw text diff, если host включил этот profile;
- patch transaction фиксирует author, tool id, prompt/request digest, changed symbols, tests run, diagnostics before/after и capability grants;
- provenance log может сериализоваться в `.amberprov` и/или optional package metadata;
- compiler обязан иметь `amber patch check` mode, который проверяет semantic patch до применения.

Этим закрывается вопрос безопасного AI-assisted maintenance без превращения source tree в неотслеживаемый набор textual edits.

## Q26. Contracts and property testing profile [закрыто в v20]

В v20 фиксируется optional **Amber/Contracts & Property Testing** profile.

Принятые решения:

- `require`, `ensure`, `invariant` и `old(expr)` являются contract-profile forms;
- contracts могут исполняться runtime, использоваться typed checker'ом и документироваться tooling'ом;
- property tests используют генераторы, shrinkers, seeds и replayable counterexamples;
- failed contract даёт `ContractViolationError`;
- contracts не являются optimizer assumptions в unsafe mode, если build не включил explicit `assume_contracts`.

Этим закрывается вопрос бизнес-инвариантов и AI-generated code validation без включения dependent types в core.

## Q27. Privacy, taint and data lineage profile [закрыто в v20]

В v20 фиксируется optional **Amber/Privacy, Taint & Lineage** profile.

Принятые решения:

- значения, поля schema, columns и tables могут иметь `DataLabel` / taint tags (`pii`, `secret`, `regulated`, custom labels);
- taint propagation работает через ordinary operations, table query plans, codecs and API exports;
- export boundary обязан проверять policy context;
- redaction protocol должен быть explicit и observable;
- lineage graph связывает source datasets, transformations, notebook cells, watch revisions и exported artifacts;
- policy violations дают `PolicyViolationError`.

Этим закрывается enterprise/BI вопрос: какие данные откуда пришли, какие ячейки/отчёты зависят от sensitive inputs и где запрещён export.

## Q28. Durable workflow profile [закрыто в v20]

В v20 фиксируется optional **Amber/Durable Workflow** profile.

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

[`amber_compilable_project_layer_v20_1_complete.md`](amber_compilable_project_layer_v20_1_complete.md)

В этом месте намеренно оставлен только базовый reference, чтобы основной документ оставался языковой спецификацией, а проектная декомпозиция жила отдельно и могла обновляться без шума в language-core тексте.

# Часть XVI. Modern Pressure Profiles v20

## 1. Назначение v20

v20 не меняет уже зафиксированное ядро Amber. Его назначение — закрыть современные pressure-points, которые стали критичными для языков, запускаемых в notebooks, BI-платформах, sandboxed plugins, CI/CD, serverless/edge, AI-agent workflows, production observability и accelerator-heavy data workloads.

Главное правило v20:

```text
Новая возможность сначала оформляется как optional profile.
Core syntax/runtime меняются только если без этого нельзя выразить профиль через уже существующие AST/HIR/bytecode/tooling контракты.
```

v20 вводит следующие normative profile families:

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

## 2. Общие правила профилей v20

### 2.1. Профильность

Каждый v20 profile является opt-in:

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

v20 profiles не меняют:

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
AST metadata      — для IDE/formatter/refactor/source tools
HIR metadata      — для checker/lowering/interpreter
bytecode metadata — для verifier/loader/runtime
package metadata  — для registry/sandbox/provenance
trace metadata    — для observability/replay/audit
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

### 3.4. Capability taxonomy v20

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
Fetcher   = Fn[UserId -> User !{net, async}]
```

### 4.3. Canonical effect labels

Минимальный v20 set:

```text
alloc       allocation observable to host/profile
mut         ordinary data mutation
world       dispatch-world mutation
watch       notebook watch/revision mutation
async       creates/awaits task or awaitable
strand      crosses strand boundary or uses synchronization
fs          filesystem I/O
net         network I/O
env         environment read/write
time        real or virtual clock
random      random source
ffi         native/foreign call
reflect     dynamic reflection / SEND_DYN / mirrors
unsafe      unchecked host/runtime escape hatch
db          database or external storage
gpu         accelerator/device operation
schema      schema encode/decode/migration boundary
trace       telemetry emission
workflow    durable workflow persistence/replay
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

run(p)       # ok
run(clocky)  # error if clocky has !{time}
```

### 4.5. Dynamic boundaries

These constructs force conservative effects unless statically resolved:

- `send(receiver, selector_expr, ...)` with non-literal selector;
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

Amber runtime has tasks, strands, channels, GC, loader, MOP, frozen-world, notebook watches, FFI, schemas and optional native/JIT. v20 requires observability to be semantic, not purely logging.

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
orders
  .where: _1.status == :paid
  .select(:country, :amount, :user_id)
  .group_by(:country)
  .agg(
    revenue: sum(:amount),
    users: count_distinct(:user_id)
  )
  .sort_by(:revenue, desc: true)
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

Columnar profile extends v19.2 watch keys:

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
  if row.amount > 1000: :large else: :small
```

A UDF with `!{net}` or `!{time}` can be accepted only if host policy allows nondeterministic query plans.

## 7. Amber/Schema & API Contracts Profile

### 7.1. Schema syntax

```amber
schema User v2:
  id as UUID
  name as Str
  email as Str?
  created_at as Time
  deprecated legacy_id as Int?

schema Order v1:
  id as UUID
  user_id as UUID
  amount as Decimal
  status as Symbol = :new
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
schema Customer v3:
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
migration User v1 -> v2 |old|:
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
xs = Tensor.f32([1.0, 2.0, 3.0], device: :gpu)
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
  deny label :pii
  deny label :secret
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
safe = users.redact(:email, with: :hash)
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
  step fetch !{net} retry: {max: 3, backoff: :exponential}:
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
!{time}   -> virtual clock or recorded clock values
!{random} -> seeded/recorded random source
!{net}    -> recorded response provider or denied in deterministic mode
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

## 15. HIR and bytecode additions v20

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

Optional sections:

```text
CAPS  capability requirements and grants metadata
EFCT  effect summaries per callable/site
OBSV  event schemas and trace site table
RPLY  replay/determinism metadata
SCMA  schema definitions and migration table
TABL  table/query-plan metadata
LINE  lineage metadata anchors
PRIV  taint labels and policy ids
CNTR  contracts/property-test metadata
WASM  component import/export mapping
ACCL  accelerator kernel descriptors
WFLW  durable workflow descriptors
PROV  provenance/agent patch metadata
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

## 16. CLI additions v20

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

## 17. Conformance lanes v20

v20 adds optional conformance lanes:

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

## 18. Development matrix updates v20

v20 extends the existing implementation matrix with P4/P5 tracks. These do not block P0/P1 dynamic runtime.

### P4 — platform safety, observability and data profiles

| Трек | Состояние | Ближайший шаг | Зависимости | Критерий выхода |
|---|---|---|---|---|
| G16. Capabilities & sandbox | Закрыто на уровне v20 profile | Реализовать manifest parser, capability resolver, runtime checks and `CapabilityError` | G6e, G11 | Package/plugin/notebook contexts deny host resources by default |
| G17. Effects checker | Закрыто на уровне v20 profile | Добавить effect rows в typed checker, HIR summaries and call-site validation | G10, G16 | Pure/effectful boundaries диагностируются воспроизводимо |
| G18. Observability & replay | Закрыто на уровне v20 profile | Реализовать event schema, trace spans, `.ambertrace`, deterministic scheduler mode | G7, G13, G16 | CI может записать run и воспроизвести его до первого divergence |
| G19. Schema/API contracts | Закрыто на уровне v20 profile | Реализовать `schema`, codecs, migrations and API description generator | G10, G17 | Encode/decode/API boundaries валидируются schema-first |
| G20. DataFrame/Columnar BI | Закрыто на уровне v20 profile | Реализовать `Table`, `Column`, `LazyTable`, query fingerprints and watch integration | G9, G18, G19 | Notebook invalidation работает на column/query-plan granularity |
| G21. Privacy/Taint/Lineage | Закрыто на уровне v20 profile | Реализовать labels, policy checks, lineage graph and export audit | G18, G19, G20 | Sensitive data export блокируется или требует explicit redaction |

### P5 — portability, accelerators, agent tooling and workflows

| Трек | Состояние | Ближайший шаг | Зависимости | Критерий выхода |
|---|---|---|---|---|
| G22. Wasm Component | Закрыто на уровне v20 profile | Реализовать frozen subset checker, component interface mapping and capability host imports | G14, G16, G19 | `.amberwasm` plugin исполняется sandboxed без world mutation |
| G23. Accelerator | Закрыто на уровне v20 profile | Реализовать kernel subset checker, tensor/device buffer runtime and CPU/SIMD fallback | G6d, G15, G17 | Kernel checker rejects dynamic Amber features and runs supported numeric kernels |
| G24. AI-agent tooling/provenance | Закрыто на уровне v20 profile | Реализовать symbol graph, explain JSON, structured patch protocol and `.amberprov` | G1-G5, G10, G17 | Agent patches проверяются semantic-first before apply |
| G25. Contracts/property testing | Закрыто на уровне v20 profile | Реализовать `require/ensure/invariant/property`, generators and shrinkers | G10, G18 | Failed contract/property yields replayable diagnostic |
| G26. Durable workflow | Закрыто на уровне v20 profile | Реализовать workflow history, step replay, idempotency and compensation | G16-G19, G25 | Workflow survives restart and replays committed steps correctly |

## 19. Ненормативные внешние ориентиры

Этот список не является dependency Amber. Он фиксирует external design references, на которые v20 consciously ориентируется концептуально:

- SLSA: supply-chain levels / provenance / artifact assurance — https://slsa.dev/
- OpenTelemetry: traces, metrics, logs and context propagation — https://opentelemetry.io/
- W3C Trace Context: standard HTTP headers for distributed trace context — https://www.w3.org/TR/trace-context/
- Apache Arrow: language-independent columnar memory format — https://arrow.apache.org/
- OpenAPI Specification: language-agnostic HTTP API description — https://swagger.io/specification/
- WebAssembly Component Model — https://component-model.bytecodealliance.org/
- WASI Preview 2 / WIT / component model direction — https://github.com/WebAssembly/WASI/blob/main/docs/Preview2.md
- NIST AI Risk Management Framework — https://www.nist.gov/itl/ai-risk-management-framework
- eBPF concept for low-level observability/security probes — https://ebpf.io/

Amber v20 does not copy these specifications. It uses them as pressure tests for Amber's own profile boundaries.

## 20. Итоговый статус v20

После v20 Amber имеет три слоя зрелости:

```text
Core language and reference runtime contracts: closed for implementation.
Second-wave compiler/runtime profiles v16-v19.2: closed for implementation.
Modern platform profiles v20: closed as optional profile specifications, implementation order remains product/host-driven.
```

v20 делает Amber не только dynamic/typed/no-GIL/compiled language, но и platform-oriented language: безопасно запускаемый, объяснимый, воспроизводимый, пригодный для BI/data, переносимый в sandboxed components, готовый к AI-agent tooling and auditable enterprise workflows.


# Приложение A. Редакторская нормализация конфликтов между черновиками

Ниже — важные конфликты, которые я сознательно свёл в одну редакцию.

## A1. `_`, `$_` и underscore-формы

В ранних черновиках last-result-переменная и wildcard менялись местами. Поздние закрывающие блоки `amber-lang-3` фиксируют такую развязку:

- `_` = wildcard в pattern-контекстах;
- `$_` = last result;
- `_1`, `_2`, ... = placeholders в блоках без `|...|`;
- `__` и другие double-underscore имена не имеют встроенной магии.

Именно этот поздний вариант принят в текущей редакции.

## A2. `case in when` vs `case when if`

В `amber-lang-3` действительно присутствуют обе линии:

- более ранняя ветка с `case ... in pattern when guard:`;
- поздняя нормативная ветка с `case ... when pattern if guard:`.

Так как в тех же поздних блоках `in` окончательно закрепляется как инфиксный оператор принадлежности, а `case when if` прямо помечается как унифицированный и финальный вариант, в этой редакции канонической формой принят:

```amber
case expr:
  when PATTERN if GUARD:
    ...
```

Изолированные более поздние объяснительные примеры, где снова всплывает `in`, трактуются как регрессия примеров, а не как повторное открытие решения.

## A3. `case` без `else`

Внутри истории есть две несовместимые ветки:

- строгая: `case` без совпадения и без `else` бросает `MatchError`;
- поздняя safe-ветка: `case` без совпадения и без `else` возвращает `null`.

Так как закрытая v1-спека pattern matching в `amber-lang-3` и её поздний уточняющий walkthrough уже используют второй вариант, текущая редакция сохраняет именно этот safe-default и одновременно включает строгую форму `case!`.

Итог:

- `if` без сработавшей ветки -> `null`
- `case` без совпадения и без `else` -> `null`
- `case!` без совпадения и без `else` -> `MatchError`
- destructuring assignment / block params / multi-clause `def` при no-match -> `MatchError`

## A4. Dynamic pattern objects

В истории есть богатая линия про runtime-pattern-objects с `match(value)` и биндингами из конфигурации. Поздняя критика справедливо указывает, что риск несут прежде всего **скрытые локалы**, а не сама идея runtime-configured matcher object.

Поэтому текущая редакция включает только explicit-binding profile:

- разрешены `pattern(expr)` и `pattern(expr) with MAP_PATTERN`;
- matcher object обязан возвращать `DynamicMatchResult(success: Bool, bindings: Map)`;
- без `with ...` matcher не имеет права экспортировать bindings;
- dynamic pattern objects разрешены только в `case`, `case!` и clause-style `def`;
- dynamic pattern objects с неявной инъекцией локалов — вне v1.

Bare matcher expressions через `===` по-прежнему остаются отдельной fallback-формой только для `case` / `case!`.

# Приложение B. Короткий индекс примеров

## One-liner chain with placeholders

```amber
numbers.map: _1 * 2 .select: _1 > 0 .reduce 0: _1 + _2
```

## `class_method def`

```amber
class User:
  class_method def find(id):
    ...
```

## Auto-assign и defaults

```amber
class Connection:
  def init(@host:, @port: 5432, timeout = @timeout):
    pass
```

## `case` с pattern matching

```amber
case shape:
  when Point(x, y):
    x * y
  when {w:, h:, **null}:
    w * h
  else:
    0
```

## Clause-style `def`

```amber
def fmt(x, mode: :short):
  when {mode: :short}:
    "S: #{x}"
  when {mode: :long}:
    "LONG: #{x}"
  else:
    "??"
```

## Simple many-def sugar

```amber
def fact(0): 1
def fact(n) if n > 0: n * fact(n - 1)
```

## Explicit destruction и immediate dealloc

```amber
class CachePage:
  def init(rows):
    @rows = rows
    @index = rows.group_by: _1.id

  def destroy!():
    @index = null
    @rows = null

page = CachePage.new(load_rows())
...
memory.dealloc(page)
```

## Dead-object guard

```amber
obj = CachePage.new(load_rows())
memory.dealloc(obj)
obj.rows()
# => UseAfterFreeError
```


## Capability manifest v20

```toml
[capabilities]
fs.read = ["./data"]
fs.write = ["./out"]
net.connect = []
time = true
random = true
ffi = false
```

## Effects v20

```amber
def normalize(row as Row) -> Row !{}:
  row.trimmed()

def fetch(url as Str) -> Response !{net, async}:
  http.get(url).await()
```

## Trace/replay v20

```amber
Kernel.deterministic(seed: 42):
  trace.span "report.refresh":
    Report.refresh()
```

## Schema v20

```amber
schema User v2:
  id as UUID
  email as Str? @pii
  created_at as Time
```

## Table/BI v20

```amber
orders.lazy
  .where: _1.status == :paid
  .group_by(:country)
  .agg(revenue: sum(:amount))
  .collect()
```

## Durable workflow v20

```amber
workflow ImportOrders:
  step fetch !{net}:
    http.get(source)

  step commit !{db} idempotency_key: fetch.result.digest():
    db.insert_many(fetch.result)
```
