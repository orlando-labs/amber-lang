# Amber — единая финальная спецификация языка

**Назначение документа:** единый Markdown-файл, описывающий язык Amber, его стандартную библиотеку, runtime-facing API и опциональные профили.

**Редакторская политика:** документ представлен как единая финальная редакция языка. Он содержит только языковую спецификацию, детали возможностей, их мотивации, известные ограничения и содержательные примеры. Исторические заметки о ходе реализации — патчи и их последовательность, backlog, milestone-гейты, матрицы разработки и редакторские следы закрытых обсуждений — из спецификации удалены. Инженерный слой reference implementation (bytecode VM, runtime ABI, модель памяти, `.amberbc`, loader/verifier, MOP, compiler pipeline) вынесен в отдельный документ (см. Часть V).

**Граница документа:** первые части описывают surface language и его семантику; далее следуют интегрированные языковые решения, стандартная библиотека / runtime-facing API и опциональные Modern Pressure Profiles. Все разделы следует читать как согласованное состояние Amber, а не как журнал изменений.

---

## Оглавление

- [Часть I. Языковая спецификация Amber](#часть-i-языковая-спецификация-amber)
- [Часть II. Интегрированные языковые решения](#часть-ii-интегрированные-языковые-решения)
- [Часть III. Standard library и runtime-facing API](#часть-iii-standard-library-и-runtime-facing-api)
- [Часть IV. Modern Pressure Profiles](#часть-iv-modern-pressure-profiles)
- [Часть V. Реализация и runtime-проектирование](#часть-v-реализация-и-runtime-проектирование)

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
absent?
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
- same-name identifier entry: `{name:}` означает `{:name: name}` and reads the
  ordinary lexical binding `name`;
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

### 3.4. Возведение в степень и bitwise XOR

Amber фиксирует отдельные surface-операторы для возведения в степень и bitwise XOR:

```amber
2 ** 10      # exponentiation => 1024
a ^ b        # bitwise xor
```

Нормативно:

- `a ** b` означает exponentiation / power operation;
- `a ^ b` означает bitwise XOR;
- `^` не используется как оператор возведения в степень;
- `**` как infix-оператор требует левый операнд и не является prefix-оператором;
- `^` как infix-оператор требует левый и правый операнды в expression-контексте.

Ассоциативность `**` — справа налево:

```amber
2 ** 3 ** 2
# читается как:
2 ** (3 ** 2)
# => 512
```

Unary `+`, `-` и `not` имеют более низкий приоритет, чем `**`. Поэтому:

```amber
-2 ** 2
# читается как:
-(2 ** 2)
# => -4

(-2) ** 2
# => 4
```

Приоритет `^` — ниже `&` и выше `|`, если bitwise AND/OR включены в используемый профиль реализации. В минимальном v1-profile, где `&` занят callable-reference prefix-form и bitwise AND/OR могут отсутствовать как surface-операторы, `^` всё равно резервируется как bitwise XOR level между shifts/additive arithmetic и comparisons. Если битовые операторы реализуются как ordinary method selectors, `^` обязан понижаться в тот же binary-operator dispatch pipeline, что и остальные арифметические операторы.

#### Контекстное разделение `**` и spread

`**` сохраняет уже зафиксированную роль contextual spread marker в call argument lists и map literals:

```amber
fn(**opts)
{**defaults, mode: :fast}
```

Это не конфликтует с exponentiation, потому что формы различаются синтаксической позицией:

```amber
fn(x ** y)       # infix exponent expression
fn(**opts)       # keyword spread argument

{power: x ** y}  # map value expression
{**opts}         # map spread entry
```

Parser обязан различать:

- `**expr` в позиции начала argument-entry или map-entry как spread marker;
- `lhs ** rhs` в обычном expression-контексте как exponentiation operator.

Вне call arguments и collection literals prefix-form `**expr` остаётся невалидной, потому что spread не является обычным prefix-оператором.

#### Контекстное разделение `^` и pin-pattern

`^name` уже используется как pin-pattern в pattern-контексте. Это не конфликтует с `a ^ b`, потому что pin является prefix-form внутри grammar паттернов, а XOR является infix-form внутри grammar выражений:

```amber
case value:
 when ^expected:
  :same

mask = flags ^ FLAG_A
```

В expression-контексте prefix-form `^expr` не вводится. В pattern-контексте infix-XOR не участвует в разборе паттерна; если нужен вычисленный matcher, используется уже существующий механизм matcher expression / guard, а не `^` как арифметический символ.

### 3.5. Оператор принадлежности `in`

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

### 3.6. Presence-операции поверх Ruby-like truthiness

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

Дополнительно вводится chain-preserving postfix callable-call segment `.()` (RFC bare-nullary + dot-call, принят 2026-06-12):

```amber
expr.()
expr.(arg1, arg2)
expr.?.()
expr.?.(arg1, arg2)
```

`expr.(args...)` наблюдаемо эквивалентен `(expr)(args...)`: сначала вычисляется `expr`, затем полученное значение вызывается через общий callable protocol (`HCall`). Это не method send: `obj.member()` остаётся explicit method send селектора `:member`, а `obj.member.()` означает member read / implicit nullary send с последующим вызовом полученного значения. Safe-вариант `expr.?.(args...)` при `expr == null` возвращает `null` без вызова. `.()` не может начинать выражение — это parser diagnostic `AMB_DOT_CALL_TARGET`. Вызов не-callable значения через `.()` даёт тот же `TypeError`, что и `fn(args...)` на не-callable.

`fn(args...)` и `expr.(args...)` оба понижаются в `HCall`; `expr.call()` остаётся обычным method send селектора `:call`.

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

Отдельный смысл `&` в **argument position**: trailing `&name` в списке аргументов вызова — это **block-pass**, направляющий callable из локала `name` в block channel вызываемого (см. §12.7.4.1), а не позиционное reference-значение. В v1 block-pass принимает только bare local name; `&NS.fn` / `&Class#m` как trailing block-pass зарезервированы (сначала свяжите reference в локал). Это единственное место, где `&` в argument position означает block channel, а не «создать callable reference».

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

`break` завершает **ближайший enclosing нативный цикл** (`while` / `until` / `loop` / `do…while`) и может нести значение, которое становится значением цикла-выражения:

```amber
result = loop:
  break :some_symbol
```

`break` **не разрешён внутри iterator-блока** (`each` / `map` / `select` / …): блок — это вызываемый объект, а не цикл, поэтому `break` там является compile-time ошибкой с corrective-подсказкой. (`next`, напротив, допустим в блоке: он завершает *текущий вызов блока*, давая своё значение — или `$_` при голой форме — как результат итерации, что удобно в `map` / `select`.)

Ранний выход из цепочки коллекций (`each` → `map` → `select` → `reduce` …) выражается двумя явными, greppable способами — **не** через `break`:

- **short-circuit комбинаторами** — `find` / `detect` (первое совпадение со значением), `any?` / `all?` / `none?`, `take(n)` / `take_while` / `drop(n)` / `drop_while`, `find_index`, а также ленивые последовательности (`.lazy. … .first`). Они покрывают подавляющее большинство случаев, не вводя нелокального управления и сохраняя композицию цепочки;
- для подлинно нелокального раннего выхода **со значением** из середины цепочки — существующей семантикой `throw` / `catch` (§10.5.16). `throw` / `catch` (как и `raise` / `rescue`, §10.5) **динамически проходит сквозь блоки**: `throw :tag, value`, выполненный внутри блока в `each` / `map` / `select`, ловится охватывающим `catch(:tag)`. Это и есть каноничный «break из цепочки»:

```amber
first_big = catch(:found):
  xs.each |x|:
    if x > threshold:
      throw :found, x
  null
```

### 7.5. `return`

`return` завершает ближайший enclosing callable: тело `def` / `class_method def`, lambda или block. Это **локальный** return: внутри lambda/block он завершает сам lambda/block, а не внешний метод.

Формы:

```amber
return expr
return
```

Правила:

- `return expr` — значением callable становится `expr`;
- голый `return` — значением callable становится текущее `$_` (то же правило, что и implicit return через последнее выражение, см. §6);
- `return` на верхнем уровне модуля завершает module-init code этого модуля;
- при выходе через `return` из защищённого body выполняются все охватывающие `ensure`-блоки в порядке от внутреннего к внешнему (см. §10.5);
- `return` внутри `rescue`-клаузы, `ensure`-клаузы и clause guard не поддерживается в v1 и является compile-time ошибкой;
- `return` — это statement-level форма; её значение как выражения не наблюдаемо (управление не возвращается).

```amber
def find_name(user):
 if user.missing?:
 return "unknown"
 user.name

def normalize(str):
 str.strip()
 return # завершает def текущим $_ — результатом strip()
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

## 10.5. Исключения: `raise`, `rescue`, `ensure`

### 10.5.1. Назначение

Amber вводит native exception handling syntax в Ruby-like стиле, но без обязательного `begin` для обычных function/method bodies.

Каноническая function-level форма:

```amber
def f(x as Int) -> Int:
 risky_calculation(x)
rescue TypeError |e|:
 recover_type_error(e)
ensure:
 release_critical_resource()
```

`rescue` перехватывает исключения, возникшие в защищённом body. `ensure` выполняется при любом выходе из защищённого body: normal completion, explicit `return`, implicit return через последнее выражение, `raise`, `throw`/`catch` unwind, cancellation/unwind и runtime unwinding.

Surface `rescue` / `ensure` являются языковой формой над уже обязательной VM handler-table/unwind моделью. Implementation не должна эмулировать их через обычные вызовы stdlib-функций.

### 10.5.2. Function-level `rescue` / `ensure`

`rescue` и `ensure` могут следовать после тела `def` на том же indentation level, что и тело функции или метода.

```amber
def parse_int(s as Str) -> Int:
 s.to_int()
rescue TypeError |e|:
 log "bad int: #{e.message}"
 0
ensure:
 metrics.increment(:parse_attempt)
```

Грамматически такие clauses относятся к ближайшему enclosing `def` / `class_method def` body, если они стоят на уровне body этого callable.

```amber
def f():
 risky()
rescue Error |e|:
 handle(e)
```

наблюдаемо эквивалентно protected callable body:

```amber
def f():
 try:
  risky()
 rescue Error |e|:
  handle(e)
```

но wrapper `try:` для whole-function case не требуется.

### 10.5.3. Explicit `try` expression

Для локальных protected regions вводится explicit `try:` expression.

```amber
value = try:
 read_config(path)
rescue FileError |e|:
 default_config()
ensure:
 close_temp_handles()
```

`try` является выражением.

Значение `try`:

- если protected body завершился нормально — значение последнего выражения protected body;
- если выбран `rescue` — значение последнего выражения выбранной rescue-ветки;
- если исключение не перехвачено — исключение продолжает unwind после выполнения `ensure`;
- `ensure` не заменяет значение выражения, если сам не бросает исключение или иной control completion.

### 10.5.4. `raise`

Минимальная surface-форма:

```amber
raise expr
```

`expr` должен вычисляться в `ExceptionObject` или exception-compatible значение, которое runtime умеет канонически поднять как `ExceptionObject`.

Рекомендуемая stdlib-конвенция:

```amber
raise TypeError("message")
raise TypeError.new("message")
```

Если `raise` получает значение, которое не является `ExceptionObject` и не является exception-compatible value, это `TypeError`.

`raise` без аргумента в v1 не вводится. Re-raise текущего исключения должен быть оформлен отдельным будущим решением, чтобы не создавать скрытую dynamic dependency на nearest rescue context.

### 10.5.5. Rescue clauses

Базовая форма:

```amber
rescue ErrorType |e|:
 body
```

Форма без binding:

```amber
rescue ErrorType:
 body
```

Форма catch-all:

```amber
rescue |e|:
 body

rescue:
 body
```

`rescue:` без matcher ловит все ordinary language-level exceptions, но не ловит `throw`/`catch` control flow и fatal tooling/runtime classes, которые не являются частью normal language-level exception flow: `InternalCompilerError`, `InternalVMError`, host fatal aborts и аналогичные runtime-fatal failures.

### 10.5.6. Несколько exception types

Для нескольких типов допускается только comma-separated форма:

```amber
rescue TypeError, ArgumentError |e|:
 recover(e)
```

Вертикальная черта `|` не является union-разделителем внутри rescue matcher list. Следующая форма не входит в язык и должна диагностироваться:

```amber
rescue TypeError | ArgumentError |e|:
 recover(e)
```

Причина: `|name|` уже является delimiter'ом binding exception object, а использование `|` как type-union внутри `rescue` создаёт нежелательную визуальную и грамматическую неоднозначность. Type unions сохраняются в `TypeTerm`, но rescue matcher list использует собственный comma-separated синтаксис.

### 10.5.7. Exception matching

`rescue T` матчится по правилу:

```amber
T === exception
```

Для exception classes стандартная семантика:

```text
ErrorClass === exception
```

истинна, если `exception.error_class` равен `ErrorClass` или является его subclass.

Rescue clauses проверяются сверху вниз. Первая matching clause побеждает. Последующие clauses не выполняются.

```amber
def load_user(id):
 db.users[id]
rescue KeyError |e|:
 null
rescue TimeoutError |e|:
 retry_later(id)
```

### 10.5.8. Binding exception object

Форма:

```amber
rescue TypeError |e|:
 log e.message
```

создаёт локальный binding `e`, видимый только внутри rescue-body.

Без binding:

```amber
rescue TypeError:
 0
```

exception object не связывается с локальным именем, но остаётся доступным runtime unwind, diagnostic и backtrace machinery.

Если локальная shadowing policy разрешает затенение, binding `e` может затенять внешний `e` только внутри rescue-body. Если реализация работает в профиле с запретом shadowing, нарушение диагностируется тем же механизмом, что и обычные локальные binding conflicts.

### 10.5.9. `ensure`

`ensure:` выполняется всегда после protected body или после выбранного rescue-body.

```amber
def f():
 acquire()
 risky()
rescue Error |e|:
 recover(e)
ensure:
 release()
```

Порядок выполнения:

1. выполнить protected body;
2. если body бросил exception — найти первый matching `rescue`;
3. если matching rescue найден — выполнить rescue-body;
4. выполнить `ensure`, если он есть;
5. вернуть pending normal value либо продолжить pending exception/control unwind.

`ensure` может существовать без `rescue`:

```amber
def with_lock(lock):
 lock.acquire()
 work()
ensure:
 lock.release()
```

### 10.5.10. Suppressed exception chain

Runtime обязан поддерживать suppressed exception chain как часть `ExceptionObject` ABI.

Если protected body или rescue-body уже имеет pending exception, а `ensure` бросает новое исключение, новое исключение становится наблюдаемым thrown exception, а предыдущее pending exception добавляется в `suppressed_exceptions` нового исключения.

```amber
try:
 raise TypeError("primary")
ensure:
 raise CleanupError("cleanup failed")
```

Наблюдаемое исключение: `CleanupError`.

`CleanupError.suppressed_exceptions` обязан содержать исходный `TypeError("primary")`.

Если при выполнении `ensure` возникает цепочка дополнительных failures, suppressed exceptions добавляются в порядке их подавления. Runtime, debugger и diagnostic printer обязаны сохранять suppressed-chain; реализация не вправе терять первичное исключение как mere debug metadata.

`cause` и `suppressed_exceptions` являются разными механизмами:

- `cause` описывает причинную связь при явном exception chaining или runtime wrapping;
- `suppressed_exceptions` описывает exception/control loss из-за failure в cleanup/ensure/finalization path.

### 10.5.11. Completion precedence

`ensure` не меняет normal result, если завершается нормально.

```amber
def f():
 10
ensure:
 log "done"
```

результат `f()` — `10`.

Если `ensure` бросает исключение, выполняет non-local control transfer или иначе создаёт abrupt completion, оно заменяет прежний pending normal result или pending exception. При замене pending exception новый exception обязан сохранить прежний exception в `suppressed_exceptions`.

### 10.5.12. Interaction with `$_` and implicit return

Amber использует `$_` как frame-local last-result slot и implicit return через последнее выражение. Поэтому `try` / `rescue` / `ensure` lowering обязан сохранять pending completion value отдельно от временных значений, возникающих внутри `ensure`.

```amber
def f():
 risky()
rescue TypeError:
 10
ensure:
 log "done"
```

Если `risky()` успешно вернул `5`, результат `f()` — `5`.

Если `risky()` бросил `TypeError`, результат `f()` — `10`.

Последнее выражение внутри `ensure` может обновлять локальный `$_` во время исполнения ensure-body, но не заменяет итоговое значение whole `try` / function body, если ensure-body завершился normally. Lowering обязан восстановить pending completion после successful ensure.

### 10.5.13. Grammar

```ebnf
TryExpr ::=
  "try" ":" Block
  RescueClause*
  EnsureClause?

ThrowExpr ::=
  "throw" Expr ("," Expr)?

CatchExpr ::=
  "catch" CatchTag ":" Block

CatchTag ::=
  Expr
| "(" Expr ")"

DefWithHandlers ::=
  DefHeader ":" Block
  RescueClause*
  EnsureClause?

RescueClause ::=
  "rescue" RescueMatcherList? ExceptionBinding? ":" Block

RescueMatcherList ::=
  RescueMatcher { "," RescueMatcher }

RescueMatcher ::=
  TypeTerm

ExceptionBinding ::=
  "|" Identifier "|"

EnsureClause ::=
  "ensure" ":" Block
```

Ограничения:

- `ensure` может быть только один;
- `ensure` должен идти после всех `rescue`;
- `rescue` после `ensure` — compile-time error;
- `rescue` без preceding `try` body или function/method body — compile-time error;
- `ensure` без preceding `try` body или function/method body — compile-time error;
- empty rescue/ensure body запрещён; для intentional no-op используются `pass` или `noop`;
- union spelling через `|` внутри rescue matcher list запрещён, даже если такой spelling допустим в обычном `TypeTerm`.
- `catch` tag может записываться как `catch(:tag):` или без скобок как `catch :tag:`.

### 10.5.14. Diagnostics

Compile-time diagnostics:

```text
E_RESCUE_WITHOUT_BODY
`rescue` must follow a `try` body or function/method body
```

```text
E_ENSURE_WITHOUT_BODY
`ensure` must follow a `try` body or function/method body
```

```text
E_RESCUE_AFTER_ENSURE
`rescue` clauses must appear before `ensure`
```

```text
E_DUPLICATE_ENSURE
only one `ensure` clause is allowed
```

```text
E_INVALID_RESCUE_BINDING
exception binding must use `|name|`
```

```text
E_INVALID_RESCUE_MATCHER
rescue matcher must be an exception class/type term
```

```text
E_RESCUE_PIPE_UNION_FORBIDDEN
use comma-separated rescue matcher list: `rescue TypeError, ArgumentError |e|:`
```

Runtime errors:

- invalid raised value: `TypeError`;
- unmatched `throw`: `UncaughtThrowError`;
- dynamic rescue matcher that is not an exception class/matcher: `TypeError`;
- fatal VM/tooling errors remain outside ordinary rescue control flow.

### 10.5.15. Lowering

Function-level form:

```amber
def f(x):
 body
rescue TypeError |e|:
 recover(e)
ensure:
 cleanup()
```

lowers to HIR equivalent:

```text
HDef f(x):
 HTry(
  body = HBlock(body),
  rescue_clauses = [
   HRescue(matchers = [TypeError], bind = e, body = recover(e))
  ],
  ensure_body = cleanup(),
  result_slot = fresh_completion_slot
 )
```

Multiple matchers lower as a matcher list, not as an OR-pattern:

```amber
rescue TypeError, ArgumentError |e|:
 recover(e)
```

```text
HRescue(
 matchers = [TypeError, ArgumentError],
 bind = e,
 body = recover(e)
)
```

Bytecode lowering must use handler table entries with protected ranges, rescue entries, catch entries and ensure/finalizer entries. VM unwind walks handler tables, executes pending ensure/finalizer handlers, preserves suppressed exception chains for exception unwinds and then either enters a matching rescue/catch handler or continues unhandled propagation.

### 10.5.16. Non-exception `throw` / `catch`

`throw` / `catch` provides tagged non-local control transfer. It is not exception handling and does not allocate or raise an `ExceptionObject`.

```amber
def deep_nested_code:
  throw :enough, 42

res = catch(:enough):
  deep_nested_code()

res # => 42
```

The catch tag may be parenthesized or written directly:

```amber
catch(:enough):
  work()

catch :enough:
  work()
```

Semantics:

- `catch tag: body` evaluates `tag` once before entering `body`;
- if `body` completes normally, the `catch` expression returns the normal body result;
- `throw tag, value` evaluates `tag`, then `value`, then starts tagged unwind;
- if the `value` operand is omitted, the thrown value is `null`;
- the nearest active `catch` whose evaluated tag is equal by Amber value equality receives the thrown value and becomes the result of that `catch` expression;
- catches with non-matching tags are skipped and unwind continues outward;
- `rescue`, including `rescue:`, never catches `throw`;
- `ensure` runs during `throw` unwind exactly as for exception unwind;
- if no matching `catch` exists, runtime raises `UncaughtThrowError`.

If an `ensure` body raises an exception or performs another non-local transfer while a `throw` is pending, the new abrupt completion replaces the pending `throw`. Because `throw` is not an exception, it is not added to `suppressed_exceptions`.

HIR lowering uses `HThrow(tag, value?)` and `HCatch(tag, body)`. Bytecode lowering uses a dedicated `THROW tag_reg value_reg` instruction and a catch handler-table entry carrying the evaluated catch-tag slot and result slot. VM exception unwind must ignore catch entries; VM throw unwind must ignore rescue entries.

### 10.5.17. Compatibility with Contracts Profile `ensure`

Contracts Profile already uses `ensure` as a postcondition statement:

```amber
def withdraw(account as Account, amount as Money) -> Account !{mut}:
 require amount > 0
 result = account.debit(amount)
 ensure result.balance == old(account.balance) - amount
 result
```

Это не конфликтует с exception-finalization clause, потому что формы различаются синтаксически:

```amber
ensure expr
```

— contract/postcondition statement внутри function body.

```amber
ensure:
 body
```

— exception-finalization clause после protected body.

Двоеточие является обязательным маркером exception-finalization clause.

## 10.6. `Result[T, E]` — value-based errors

`Result[T, E]` is a value-based, explicit complement to `raise`/`rescue`. A
`Result` is in exactly one of two states:

- `Ok(value)` — a success carrying a payload of type `T`;
- `Err(error)` — a failure carrying a payload of type `E`.

`Ok` and `Err` are prelude constructors (used like any `Type(args)` call); each
takes exactly one argument and accepts neither a block nor keyword arguments:

```amber
ok  = Ok(42)
err = Err("not found")
```

A `Result` displays as `Ok(<value>)` / `Err(<value>)`, with the wrapped payload
rendered in inspect form. Two `Result`s are equal when they share the same state
and their payloads are equal.

### 10.6.1. Methods

- `result.or(default)` — returns the `Ok` payload, or `default` if `Err`. One
  positional argument; no block.
- `result.or_raise` — returns the `Ok` payload, or **re-raises the `Err`
  payload through the exception path** (exactly as `raise <payload>` would), so
  an enclosing `rescue` catches it. Spelled bare (no parentheses).
- `result.ok?` / `result.err?` — state predicates (`error?` is an alias for
  `err?`).
- `result.value` — the `Ok` payload; raises `ValueError` if called on an `Err`.
- `result.error` — the `Err` payload; raises `ValueError` if called on an `Ok`.
- `result.map |v|: …` — transforms an `Ok` payload through the block, producing
  a new `Ok`; an `Err` is passed through unchanged.
- `result.or_else |e|: …` — returns the `Ok` payload, or the block's result
  applied to the `Err` payload. The block runs only in the `Err` state.

`or`, `or_raise`, `value`, `error`, `ok?`, and `err?` may be written without
parentheses; `or`, `map`, and `or_else` take their argument or block as usual.

### 10.6.2. Relationship to `raise` / `rescue`

`.or_raise` is the bridge from the value-based style to the exception style: the
contained payload is raised exactly as the `raise` keyword would raise it, and an
enclosing function-level or `try`/`rescue` handler (§10.5) catches it. When the
`Err` payload is a rescuable error instance, `rescue ErrorClass |e|:` matches it
by class; any payload is caught by a bare `rescue |e|:`.

```amber
def probe():
  try:
    Err("boom").or_raise
  rescue |e|:
    "caught"
```

Builtin runtime error classes (the `…Error` names in
`spec/registries/runtime_errors.yaml`) bind as ordinary prelude constants, so a
rescuable error instance can be constructed wherever a value is expected — not
only in `rescue` position. Both `ErrorClass.new(msg)` and the bare-call form
`ErrorClass(msg)` build an instance carrying `msg` as its `.message()` (with no
argument the message is empty); neither accepts a block or keyword arguments.
This is the idiomatic way to populate an `Err` whose payload should re-raise as a
typed error:

```amber
def probe():
  try:
    Err(KeyError.new("missing")).or_raise
  rescue KeyError |e|:
    "caught: " + e.message()
```

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
- использование `#` вне формы unbound callable reference `&Class#method`;
- prefix-form `**expr` вне call arguments и collection literals;
- prefix-form `^expr` вне pattern-контекста pin-pattern;
- использование `^` как попытки обозначить exponentiation не имеет специальной семантики: в expression grammar это всегда bitwise XOR.

Дополнительные diagnostics для exception syntax:

- `E_RESCUE_WITHOUT_BODY`: `rescue` не следует за `try` body или function/method body;
- `E_ENSURE_WITHOUT_BODY`: `ensure` не следует за `try` body или function/method body;
- `E_RESCUE_AFTER_ENSURE`: `rescue` указан после `ensure`;
- `E_DUPLICATE_ENSURE`: в одном protected region указано больше одного `ensure`;
- `E_INVALID_RESCUE_BINDING`: exception binding не использует форму `|name|`;
- `E_INVALID_RESCUE_MATCHER`: rescue matcher не является допустимым exception class/type term;
- `E_RESCUE_PIPE_UNION_FORBIDDEN`: внутри rescue matcher list использована запрещённая union-форма через `|`; нужно писать `rescue TypeError, ArgumentError |e|:`.

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
- `filter_map`
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
- `last`
- `count`
- `join`
- `sum`
- `product`
- `flattened`
- `compact`
- `zip`
- `partition`
- `tally`
- `each_with_index`
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
- `map`, `filter_map`, `flat_map`, `select`, `reject` и `group_by` по
 умолчанию eager;
- `filter_map` применяет block и возвращает `Array` truthy-результатов блока;
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
- `to_a` материализует `LazySeq`;
- `last` зеркалит `first`: без аргумента возвращает последний элемент (или
 `null`), `last(n)` — закрывающий срез из n элементов;
- `count(value)` считает элементы, равные `value`; `count |x|:` считает
 truthy-совпадения предиката; обе формы взаимно исключающие;
- `join([sep])` строит `Str` через display-стрингификацию элементов
 (разделитель по умолчанию — пустая строка);
- `sum([init])` и `product` — числовые свёртки: пустая коллекция даёт `0` и
 `1` соответственно, `Int`-аккумулятор проверяется на переполнение
 (`OverflowError`), первый `Float` переводит свёртку во float-режим,
 нечисловой элемент — `TypeError`;
- `flattened([depth])` рекурсивно разворачивает вложенные `Array` (глубина
 не ограничена без аргумента); самоссылающийся список — `ArgumentError`;
 мутирующая пара — `Array#flatten!([depth])`;
- `compact` отбрасывает `null`-элементы (для `Set` сохраняет `Set`);
 мутирующая пара — `Array#compact!` (зеркалит `Map#compact` /
 `Map#compact!`);
- `zip(seq, …)` строит массив рядов `[self[i], seq1[i], …]`, усечённый по
 кратчайшей последовательности (null-паддинга нет);
- `partition |x|:` возвращает `[matching, rest]`;
- `tally` возвращает `Map` element→count (ключи нормализуются как ключи
 `Map`);
- `each_with_index` с блоком yields `|value, index|` и возвращает receiver;
 без блока возвращает `Array` пар `[value, index]`.

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
- `filter_map |k, v|:`
- `select |k, v|:`
- `reject |k, v|:`
- `transform |k, v|:` возвращает key/value tuple или list
- `transform_values |v, k|:` где `k` — опциональный второй аргумент блока
- `merge(other)` / `merge(other) |k, old, new|:`
- `keys`
- `values`
- `entries`
- `dig(key, …)`
- `fetch(key[, default])`
- `contains?`
- `include?`

Нормативно:

- `Map#map` возвращает `Array`;
- `Map#dig(key, …)` выполняет вложенный lookup через `Map` / `Array` /
 `Tuple` (целочисленный индекс для последовательностей) и возвращает `null`
 при первом промахе;
- `Map#fetch` — **discouraged алиас**: каноничны `m[key]` (строгий,
 `KeyError`) и `m[?key]` (`null` при промахе); `fetch(key)` эквивалентен
 `m[key]`, `fetch(key, default)` возвращает `default` при отсутствии ключа;
- `Map#filter_map` возвращает `Array` truthy-результатов block;
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

С ordinary `Map` operations используют normalized key semantics. Для name keys canonical export — `Str`: `keys`, `entries`, `each`, `map`, `filter_map`, `select`, `reject`, `transform`, `transform_values` и `merge` обязаны передавать/возвращать string key для `Symbol(:name)` / `Str("name")` entry. `Map#[]`, `Map#[]?`, `contains?` и `include?` нормализуют lookup key тем же правилом, что и литерал; unsupported key values дают `TypeError`, отсутствующие valid keys дают `KeyError` только для обязательного `Map#[]`.

Изменяемые `Array` / `List` и `Set` дополнительно поддерживают
`filter_map! |x|:`. Метод вычисляет block по snapshot receiver'а, удаляет
`false` / `null` результаты, заменяет содержимое receiver truthy-результатами
block и возвращает receiver. `Set#filter_map!` нормализует truthy-результаты как
set elements и схлопывает дубликаты. `Tuple`, `Range` и `LazySeq` не имеют
`filter_map!`, потому что не мутируются. `Map#filter_map!` намеренно отсутствует:
`Map#filter_map` возвращает `Array`, а shape-preserving мутации `Map` остаются
за `select!`, `reject!`, `transform_keys!` / `transform_values!` и related
mutators.

`StrictMap` / `StrictHashMap` предоставляют тот же operation surface, но используют exact-key semantics и экспортируют фактический stored key value. `Map#merge` с ordinary maps схлопывает name-key duplicates, а strict merge схлопывает только exact-key duplicates.

### 13.12. Обязательный stdlib contract v1: строковые и числовые методы

Для `Str` обязательны следующие **чистые** методы (возвращают новую строку;
receiver — интернированная неизменяемая строка, поэтому мутирующие `!`-пары
отложены до появления изменяемого строкового представления):

- семейство регистров: `upcased`, `downcased`, `capitalized`, `titlecased`,
 `swapcased`, `humanized`, `underscored`, `camelized`, `dasherized`.
 `-ed`-формы каноничны; `upcase` / `downcase` / `reverse` / `replace` /
 `trim` / `strip` сохраняются как алиасы соответствующих чистых форм.
 Регистровые преобразования используют **Unicode simple case mapping** по
 curated-набору скриптов v1 (ASCII, Latin-1 Supplement, Latin Extended-A,
 греческий, кириллица); неохваченные кодпоинты проходят без изменений;
- `trimmed` (каноничная форма `trim` / `strip`), односторонние `ltrimmed` /
 `rtrimmed` (принимаются алиасы `lstripped` / `rstripped`) — обрезка
 ASCII-whitespace;
- `reversed`, `replaced(from, to)` — codepoint-reverse и подстановка всех
 неперекрывающихся вхождений;
- `*` (повтор): `"ab" * 3` → `"ababab"`; отрицательный счётчик —
 `ArgumentError`;
- `ljust(n[, pad])` / `rjust(n[, pad])` — паддинг до codepoint-ширины,
 pad-строка циклически повторяется; пустой pad — `ArgumentError`;
- `index(sub[, from])` / `rindex(sub)` — codepoint-смещение первого/последнего
 вхождения или `null`;
- `lines` — разбиение по `\n` с отбрасыванием терминатора (и хвостового
 `\r`); завершающая новая строка не добавляет пустого элемента;
- `count(sub)` — число неперекрывающихся вхождений подстроки;
- `[]` / `slice` — codepoint-индексация: `Int` (отрицательный — с конца)
 возвращает односимвольную `Str` либо `IndexError`, `IntRange` — подстроку
 со list-slice семантикой.

Для `Int` обязательны receiver-методы: `abs`, `even?`, `odd?`,
`divmod(n)`, `gcd(n)`, `lcm(n)`, `clamp(lo, hi)`, `upto(n)`, `downto(n)`;
для `Float` — `abs`, `divmod(n)`, `clamp(lo, hi)`.

Нормативно:

- `divmod` возвращает `[quotient, remainder]` c floor-семантикой,
 согласованной с `//` и `%`; деление на ноль — `ZeroDivisionError`;
 переполнение `Int` — `OverflowError`;
- `gcd` / `lcm` определены на полных магнитудах (включая `INT64_MIN`);
 результат вне диапазона `Int` — `OverflowError`; `lcm` с нулевым
 аргументом или receiver — `0`;
- `clamp(lo, hi)` возвращает receiver либо соответствующую границу
 (сохраняя её тип); `lo > hi` — `ArgumentError`; нечисловые границы —
 `TypeError`;
- `upto(n)` / `downto(n)` — инклюзивная итерация с шагом 1: с блоком yields
 каждое значение и возвращает receiver, без блока возвращает `Array`
 значений (пустой, если направление не совпадает);
- `abs` типосохраняющий; `Int#abs` на `INT64_MIN` — `OverflowError`.

## 14. Что входит в язык по намерению, но ещё не нормализовано до ядра

### 14.1. Amber/Notebook Watch Profile

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
absent?
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
 | Name ":"

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

The patch preserves the following existing design decisions (as amended by the accepted bare-nullary + dot-call RFC, 2026-06-12):

1. Ordinary callable values are invoked with `fn(args...)` or with the chain-preserving dot-call `expr.(args...)`.
2. Bare *identifiers* are ordinary binding reads and are never implicit call sites. Bare *member access* `receiver.name` resolves the member and may perform property get or an implicit zero-argument send when `name` is a syntactically nullary method (see the bare-nullary member access section).
3. `&target` creates an immutable callable reference object, not a raw machine address, and never invokes the target.
4. `Class(args...)` remains ordinary `HCall` / `CALL` over a callable class object and follows the constructor path.
5. Parser output remains syntax-faithful. A property declaration must not be erased into an ordinary method declaration at AST level.
6. HIR is the semantic-core representation and must lower property get/set operations explicitly, or into ordinary send/call semantics with preserved property markers.
7. Deterministic diagnostics, stack traces, disassembly and golden outputs must not expose raw memory addresses.
8. This section does not add hidden side effects to bare ordinary identifiers (lexical binding reads remain value reads).

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

For bare *identifiers* this remains intentionally different from implicit nullary function calls:

```amber
def f():
 42

f # binding read; not f()
f() # ordinary call
&f # callable reference
```

Bare ordinary identifiers remain value access, not hidden call sites. For *member access*, the accepted bare-nullary RFC additionally allows `receiver.name` to perform an implicit zero-argument send when `name` resolves to a syntactically nullary method, so `prop` and nullary `def` expose the same bare read surface:

```amber
collection.size # property get OR implicit nullary send
collection.size() # explicit method send (error if `size` is a property)
collection.size.() # call the value produced by `collection.size`
```

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

Normative distinction (bare identifiers vs member access):

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

For object members the read surface is uniform across member kinds:

```amber
obj.g # property get if `g` is a readable property;
 # implicit zero-argument send if `g` is a syntactically
 # nullary method
obj.g = x # property set if `g` is a writable property
obj.g() # explicit method send; AMB_PROP_CALLED_AS_METHOD if `g`
 # is a property
obj.g.() # call the value produced by `obj.g`
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
obj.size() # error: AMB_PROP_CALLED_AS_METHOD / TypeError when `size`
 # is a property; explicit call punctuation is method-send
 # syntax only
obj.size.() # call the value produced by `obj.size` (dot-call)
```

Implementations must not reinterpret `obj.size()` as getter invocation with call punctuation, and must not interpret it as call-of-property-result. Explicit call punctuation on a property member is a deterministic diagnostic with a fix-it pointing to `obj.size` or `obj.size.()`. Call-of-result is spelled `obj.size.()`.

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

Bare *identifiers* never become implicit call sites. Under the accepted bare-nullary RFC, bare *member access* `receiver.name` performs an implicit zero-argument send when `name` resolves to a syntactically nullary method; methods with any declared parameters (including defaults, rest, keyword or block parameters) are not bare-callable and diagnose `AMB_BARE_NON_NULLARY` / `ArgumentError`.

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

Property get and property set are explicit descriptor semantics, with syntax-faithful AST, deterministic binder validation, explicit HIR lowering and assignment behavior that returns the original RHS value. Together with the accepted bare-nullary RFC, the member read surface is uniform: `prop` and syntactically nullary `def` are read with the same bare spelling, while `prop` remains the only mechanism for assignability, validation on assignment, descriptor metadata/reflection and protocol participation (see the bare-nullary member access section).

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

## Bare-nullary member access and dot-call `expr.()`

This section integrates the accepted RFC "Bare-call для nullary-методов и chained callable-call `expr.()`" (2026-06-12, `docs/engineering/rfc-bare-nullary-and-dotcall-v1.md`) together with its gap closures (`DESIGN-bare-nullary-gap-closures-2026-06-12.md`).

### 1. Surface triad

```amber
obj.member # member read: property get OR implicit nullary method send
obj.member() # explicit method send only
obj.member.() # call the value produced by `obj.member`
obj.member.call() # ordinary `:call` method send to the produced value
```

Safe variants follow the existing safe-navigation model:

```amber
obj.?.member
obj.?.member()
obj.member.?.()
```

`&target` never invokes the target. Bare *identifiers* (`f`, locals, import-created bindings) remain ordinary binding reads and are never implicit call sites.

### 2. Member read resolution (lookup-then-kind)

Resolution for `receiver.name` is a single linearized lookup; the kind of the nearest member governs behavior. Kind never reorders lookup and arms never merge across owners:

1. Resolve `name` through the standard member linearization of the receiver's class — identical owner order to ordinary method send.
2. The nearest owner declaring external member `name` fixes the member; farther owners are never consulted. Dispatch on its kind:
 - readable property → property get (getter arm);
 - write-only property → `AMB_PROP_MISSING_GETTER` / `WriteOnlyPropertyError`;
 - syntactically nullary method → implicit zero-argument send;
 - any other method → `AMB_BARE_NON_NULLARY` (static) / `ArgumentError` (dynamic);
 - field accessor / readable binding → ordinary read.
3. If no owner declares `name`, dynamic receivers take the ordinary zero-argument missing-member path (`method_missing(:name)`); otherwise `NoMethodError`. Statically known receivers should diagnose before runtime.

A method is **syntactically nullary** when its declared signature is empty: no positional, default, rest, keyword or block parameters. Methods that merely *can* be called with zero arguments (e.g. all-defaults) are not bare-callable.

Property assignment `receiver.name = value` uses the same single lookup: the nearest member must be a writable property; a read-only property diagnoses `AMB_PROP_MISSING_SETTER` / `ReadOnlyPropertyError`; assignment to a missing member is `NoMethodError` (no `name=` selector family exists, and `method_missing` does not participate in property set in v1).

### 3. Explicit call on a property

`obj.name(args...)` is method-send syntax only. When `name` resolves to a property, the implementation must emit `AMB_PROP_CALLED_AS_METHOD` (static) / `TypeError` (dynamic) with a fix-it pointing at `obj.name` and `obj.name.()`:

```text
TypeError: property `name` is not a method; use `name` or `name.()` if the
property value is callable
```

Block suffix is call syntax and follows the same rule: a property member cannot take a block.

### 4. Dot-call segment

`expr.(args...)` ≡ `(expr)(args...)` as a chain-preserving postfix segment lowering to `HCall`. The AST preserves the distinction between method-call tails and dot-call tails (`AstTailDotCall`). `.()` cannot start an expression (`AMB_DOT_CALL_TARGET`). Calling a non-callable value raises the same `TypeError` as `fn(args...)` on a non-callable. `expr.call()` remains ordinary method send and is not special syntax.

### 5. Namespace and class-side access

Dotted access is member access regardless of receiver kind ("dot is a message; identifier is a value"):

- `Build.version` where `version` is a syntactically nullary class-side method performs an implicit call; `Build.version()` is the explicit spelling.
- Module-namespace access follows the same dispatch: a nullary module function invokes; a non-nullary one diagnoses `AMB_BARE_NON_NULLARY`; a value export is a plain read.
- Consequence: `fn = Math.answer` binds the *result* of `answer`; extraction is spelled `fn = &Math.answer`.
- Import-created local bindings (bare `c` after `import a.b.c`) are identifier reads and never invoke.

### 6. Compatibility contract

> The bare read form `x.name` is stable across `prop` ↔ nullary-`def` refactors. The explicit form `x.name()` is method-call syntax only and is stable only while `name` is method-shaped. Public APIs should document query members in bare form.

The formatter normatively rewrites zero-argument explicit calls of `?`-suffixed predicates (`x.empty?()` → `x.empty?`). Other explicit nullary calls are left untouched: explicit parentheses remain a legitimate style for effectful operations. Bare access to a `!`-suffixed method warns (`W_BARE_BANG_CALL`); strict profiles may promote the warning to an error.

### 7. Non-suspendable property arms

Property getter and setter arms must not suspend. A suspension attempt (task sleep/yield/sync, task-handle wait, blocking channel send/recv, or any other scheduler yield point) inside a property arm raises `EffectViolationError` with a deterministic message:

```text
EffectViolationError: property getter `name` attempted to suspend
(task.sleep); property arms are non-suspendable
```

Rules:

1. The restriction attaches to the **declaration kind** (property arms), not access syntax: a nullary `def` invoked bare may still suspend, since `x.f` and `x.f()` must not differ in suspendability.
2. The no-suspend region is a **dynamic extent**: it covers the arm body and everything it calls transitively, and ends when the arm's frames unwind. Exceptions propagating out of the arm are unaffected.
3. Both arms are covered; a suspending setter would place a scheduling point inside assignment evaluation.
4. Spawning a task from an arm is not suspension and is permitted; raising is permitted; runtime IO that suspends is forbidden by consequence.
5. `class_prop`, module-level props, mixin props and `attr`-generated descriptors follow the same rule.
6. There is no opt-out in v1; async properties are revisitable only alongside an explicit use-site suspension marker RFC.

Consequence (with §11.5 of keyword spread): protocol-driven implicit reads never suspend, so spread/kwargs-view construction lowers to straight-line code.

### 8. Protocol positions stay property-only

Protocol positions (keyword spread `kwargs`, and any future protocol reads) require a **readable property** and never accept implicit nullary method sends. The human call surface is uniform; protocol participation is a declared capability. See keyword-spread §11.5 for the rationale and the required teaching hint.

### 9. Diagnostics

| Code | Phase | Situation |
|---|---|---|
| `AMB_BARE_NON_NULLARY` | runtime (static where provable) | bare member access resolves to a non-nullary method; dynamic twin `ArgumentError` |
| `AMB_PROP_CALLED_AS_METHOD` | binder / runtime | call punctuation or block suffix applied to a property member; dynamic twin `TypeError` |
| `AMB_DOT_CALL_TARGET` | parser | `.()` without a preceding postfix expression |
| `AMB_PROP_SUSPEND` | runtime | suspension attempt inside a property arm; raises `EffectViolationError` |
| `AMB_BLOCK_PARAM_NOT_LAST` | parser | a `&name` block parameter is not the final parameter, or more than one is declared |
| `AMB_BLOCK_PARAM_PATTERN` | parser | `&` applied to a pattern rather than a single name |
| `AMB_BLOCK_PASS_TARGET` | parser / binder | a trailing `&name` block-pass is not last, or its target is not a bound callable |
| `W_BARE_BANG_CALL` | binder, warning | bare access invokes a `!`-suffixed method |

Runtime error classes `ReadOnlyPropertyError` and `WriteOnlyPropertyError` are registered and rescuable.

### 10. Conformance anchors

Conformance coverage lives in `corpus/run/bare_nullary_member`, `corpus/run/dot_call_member_result`, `corpus/run/prop_called_as_method`, `corpus/run/prop_non_suspendable` and `corpus/run/kwargs_spread_property_only`.

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

If the same bare identifier key is immediately followed by the entry boundary,
the value may be omitted:

```amber
name = "Ada"
{name:}
```

This is exactly equivalent to:

```amber
{name: name}
```

The right-hand side is an ordinary lexical binding read. The shorthand applies
only to bare identifier keys; explicit symbol keys (`{:name:}`), string keys and
expression keys still require an explicit value.

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
 | Identifier ":"
 | "*" Expr
 | "**" Expr
```

Ordering restrictions are semantic/parser validation rules, not precedence rules.
`Identifier ":"` is same-name keyword argument shorthand and is accepted only at
an argument boundary. It is exactly equivalent to `Identifier ":" Identifier`,
where the value side is an ordinary lexical binding read.

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
 | Identifier ":"
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

The `Identifier ":"` form is same-name map entry shorthand and is exactly
equivalent to `Identifier ":" Identifier`.

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

##### 11.5. Protocol positions are property-only

The protocol uses a property, not an implicit call to a method named `kwargs`. This is deliberate and survives the bare-nullary RFC: the human call surface reads `prop` and nullary `def` uniformly, but **protocol positions are capability declarations, not call sites** — participation must be declared with `prop`, never inferred from a method name. This prevents accidental protocol conformance (a class that happens to define a nullary method named `kwargs` does not silently become keyword-spreadable) and, combined with non-suspendable property arms, guarantees that protocol-driven implicit reads never suspend.

If a class declares:

```amber
def kwargs():
 {mode::fast}
```

then `fn(**obj)` does not call `obj.kwargs()`. The diagnostic appends a teaching hint when this shape is detected:

```text
TypeError: keyword argument spread operand must expose a readable `kwargs`
property; note: the class defines method `kwargs()`; keyword spread requires
a readable property - declare `prop kwargs`
```

To participate, the class should declare:

```amber
prop kwargs:
 {mode::fast}
```

or expose an equivalent property descriptor. Future protocols must use the same "readable property" requirement so this question does not reopen per-protocol.

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

## S1. Core collections stdlib

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

collection.filter_map |x|:...

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

map.filter_map |k, v|:...

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

## S2. Task / threading / async modules

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

## S3. Watch profile

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

## S4. IO foundation

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

## S5. Low-level networking

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

## S6. HTTP client

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

## S7. Advanced concurrency

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

# Часть IV. Modern Pressure Profiles

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

## 15. Ненормативные внешние ориентиры

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

---

# Часть V. Реализация и runtime-проектирование

Инженерный слой reference implementation Amber — compiler pipeline, bytecode ISA,
runtime ABI, объектная модель, lifetime/allocator/collector/pinning/FFI, формат
`.amberbc`, loader/verifier и minimal MOP — вынесен в отдельный документ:
[`amber_runtime_project_design.md`](amber_runtime_project_design.md).

Языковая спецификация выше не зависит от конкретных решений этого слоя: она описывает
только наблюдаемое поведение языка, стандартной библиотеки и профилей.
