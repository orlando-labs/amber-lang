> Редакция v19: поверх v18 в эту версию дополнительно включена исполнимая декомпозиция tracking issues до уровня `T-*` / checklist-подзадач, зафиксированы внутренние interface contracts `amber.ast.v1`, `amber.diag.v1`, `amber.hir.v1`, `amber.bc.v1`, `amber.runtime.v1`, а также milestone-first breakdown для пути `M1..M5`. Языковые решения v16-v18 не меняются; документ доводит инженерный план до уровня PR-sized slices для reference implementation.


# Amber

**Консолидированная спецификация (текущее зафиксированное состояние)**  
Редакторская консолидация по истории разработки  
9 апреля 2026


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

Amber в текущем виде — **консолидированная спецификация с закрытым implementation gate, зафиксированным reference blueprint для runtime P0/P1 и закрытыми profile-level решениями второй волны**. Ядро синтаксиса и семантики уже достаточно устойчиво, чтобы писать не только прототип парсера и интерпретатора, но и первый байткодный runtime; инженерная граница старта поддержана каноническими AST/HIR/diagnostic dump-контрактами, обязательным каталогом diagnostic codes v1, layout-моделью corpus/golden-файлов и bootstrap order для reference toolchain. Ранее зафиксированные minimal type envelope для parser/HIR/runtime hooks, обязательный stdlib contract для chainable collections, формальная матрица диагностик, policy по underscore-спецформам, окончательная граница bare matcher expressions и v1-решение по field lifetime annotations, а также reference execution profile, reference lifetime profile, collector/pinning/FFI profile, compiled-module/loader profile, strict `case!`, source-level modules, minimal MOP, mixin/`include` profile и frozen-world boundary сохраняются без регрессий: register/slot bytecode VM, frame/closure/object ABI, pattern-matching opcode family, ownership model без GIL, explicit destruction/deallocation (`destroy!` + `memory.dealloc`), non-moving generational collector, pinning boundary для native interop, `.amberbc`-формат и verifier/loader state machine, static `package/import/export`, reopenable named classes, named mixins, declarative `include`, linearized ancestor composition, reflective `define_method`, builtin `send(...)`, `method_missing` fallback и двухфазная модель dispatch-world (`open` -> `frozen`). В v16 дополнительно закрываются optional Amber/Typed checker profile, package/distribution/signing/hot-reload policy, read-only reflection mirrors, class-side `extend`, advanced concurrency (`move`, `select`, supervisor policies, async I/O awaitables), weak/ephemeron/buffer/borrow story без field modifiers и canonical MIR/SSA + native/JIT/AOT + frozen-image profile. После этой редакции незакрытых spec-level вопросов больше не остаётся: дальше остаются только реализация, тестовые корпуса и toolchain.

# Часть I. Полноценная спека Amber в текущем зафиксированном виде

## 1. Дизайн-якоря языка

Amber — это язык с такими базовыми обязательствами:

- от Python берутся отступы как способ задавать блоки и отказ от `end`;
- от Ruby берутся объектная модель, сигилы полей `@` и `@@`, чейнинг методов, блоки как основной способ передавать замыкания, именование методов с суффиксами `?` и `!`, а также ориентация на метапрограммирование;
- управляющие конструкции являются выражениями;
- pattern matching — часть ядра, а не библиотечный сахар;
- методы не требуют явного первого параметра `self` или `cls`;
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

В postfix-хвосте поддерживаются:

- доступ к члену;
- вызов метода;
- вызов callable-объекта;
- индексация;
- safe-variants через `.?.`;
- block suffix после каждого применимого вызовного сегмента.

Базовые формы:

```amber
obj.field
obj.method(arg1, arg2)
obj.method arg1, arg2
obj[index]
fn(args)
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
  break :ok if ready?
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

### 8.4. Конструктор `init` и `new`

Конструктор называется `init`.

```amber
class Point:
  def init(@x, @y):
    pass
```

`new(...)` — классовый путь создания объекта, который выделяет объект и вызывает `init(...)`, если тот существует.

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
- отсутствие запрошенного export даёт `ImportError` либо эквивалентную диагностируемую ошибку загрузки;
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
- class-side mixins / `extend` в v1 не вводятся.

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
- class-side reflective `define_method`, remove/alias/visibility hooks и class-side mixins/`extend` в минимальный v1-профиль не входят.

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
- `class_method def` внутри mixin body.

### 11.2. Runtime errors

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
- `include` пытается включить значение, не являющееся mixin object.

#### `NoMethodError`

Выбрасывается, когда обычный method lookup не нашёл target и fallback через `method_missing` тоже не сработал.

#### `IncludeCycleError`

Выбрасывается, когда direct/indirect `include`-граф образует цикл. Компилятор или loader вправе диагностировать такой случай раньше, если цикл статически очевиден.

#### `WorldFrozenError`

Выбрасывается, когда код в frozen-profile пытается выполнить world mutation после freeze barrier: reopen класса или mixin'а, `define_method`, `include`, меняющий ancestor graph, либо позднюю загрузку Amber-модуля в уже frozen dispatch-world.

#### `SuperclassMismatchError`

Выбрасывается, когда reopen формы `class Name < Base:` пытается переоткрыть существующий класс с несовместимым superclass. Компилятор вправе диагностировать такой случай раньше, если он статически очевиден.

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
- использование `Mutex`/`Atomic` не вводит GIL: это локальная синхронизация конкретных объектов, а не глобальный lock VM.

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

Для v1 фиксируется простая модель видимости:

- внутри одного strand действует обычный program order;
- между strand'ами наблюдаемость гарантируется только через synchronization edges:
  - `wait()`
  - `Channel.send/recv`
  - `Mutex.unlock -> Mutex.lock`
  - successful `Atomic` operations.

На ordinary non-shareable объектах меж-strand race condition нормативно запрещены самой моделью изоляции.

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

После закрытия implementation gate следующие вещи остаются за пределами минимального reference-профиля и относятся ко второй волне дизайна, toolchain или экосистемы:

- DSL-макросы в ruby-style;
- package manager / registry / artifact distribution / signing / hot reload;
- class-side mixins / `extend` и richer composition DSL;
- расширенный reflection/introspection API поверх method tables, source locations и ancestor graph;
- full static checker, inference, variance, typing для `and/or`, typing для `$_` и точное type-narrowing через `case`;
- MIR/SSA, optimizer, native backend, JIT и frozen-image deployment.


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
- ключевые слова `not`, `and`, `or`, `in`, `if`, `else`, `case`, `case!`, `when`, `def`, `class`, `mixin`, `include`, `while`, `until`, `do`, `loop`, `package`, `import`, `from`, `export`;
- `NEWLINE` не разрывает выражение внутри `()`, `[]`, `{}` и внутри интерполяции строк.
- `case!` лексируется как отдельная keyword-form, а не как `case` + postfix `!`;
- `pattern` не является глобально зарезервированным словом: это contextual keyword только в pattern-position при синтаксисе `pattern(...)`;
- `as` остаётся contextual keyword в type-position и в alias-позициях `import` / `from` / `export`.

Дополнительный лексический механизм v1:

- внутри **one-liner block body** при глубине скобок `0` токен `.` с хотя бы одним пробелом слева лексируется как `CHAIN_DOT`;
- `CHAIN_DOT` существует только внутри разбора one-liner блока;
- обычная точка без пробела слева остаётся `.` и относится к внутреннему выражению блока;
- `.?.` никогда не разбивается на `CHAIN_DOT` + `?.`: safe-nav лексируется раньше.

Именно это реализует правило:

```amber
numbers.map: _1.email.downcase() .uniq()
#                    ^ внутренняя точка блока
#                                 ^ продолжение внешней цепочки
```

### 15.2. Приоритеты выражений v1

Нормативный порядок приоритетов для parser core:

1. postfix: member access, member send, call, safe-nav, indexing, safe-indexing, block suffix;
2. префиксные unary `+`, `-`, `not`;
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
Expr                ::= AssignExpr

AssignExpr          ::= Assignable "=" AssignExpr
                      | OrExpr

OrExpr              ::= AndExpr { "or" AndExpr }

AndExpr             ::= NotExpr { "and" NotExpr }

NotExpr             ::= "not" NotExpr
                      | CompareExpr

CompareExpr         ::= AddExpr { CompareOp AddExpr }

CompareOp           ::= "==" | "!=" | "<" | "<=" | ">" | ">=" | "in"

AddExpr             ::= MulExpr { ("+" | "-") MulExpr }

MulExpr             ::= PrefixExpr { ("*" | "/" | "%") PrefixExpr }

PrefixExpr          ::= ("+" | "-") PrefixExpr
                      | PostfixExpr

PostfixExpr         ::= PrimaryExpr { PostfixSuffix }

PrimaryExpr         ::= Literal
                      | Name
                      | "@" Name
                      | "@@" Name
                      | "(" Expr ")"
                      | ListLiteral
                      | MapLiteral
                      | IfExpr
                      | CaseExpr
                      | WhileExpr
                      | UntilExpr
                      | DoWhileExpr
                      | LoopExpr
                      | DefExpr
                      | ClassExpr

PostfixSuffix       ::= CallSuffix
                      | IndexSuffix
                      | MemberSuffix
                      | SafeCallSuffix
                      | SafeIndexSuffix
                      | SafeMemberSuffix

CallSuffix          ::= ParenArgs BlockSuffix?
                      | BareArgs  BlockSuffix?   (* parser note: only if callee can accept bare args *)

IndexSuffix         ::= "[" ExprList? "]"

MemberSuffix        ::= "." MethodName MemberTail?

MemberTail          ::= ParenArgs BlockSuffix?
                      | BareArgs  BlockSuffix?
                      | BlockSuffix
                      | ε

SafeCallSuffix      ::= ".?.(" ArgList? ")" BlockSuffix?

SafeIndexSuffix     ::= ".?.[" Expr "]"

SafeMemberSuffix    ::= ".?." MethodName SafeMemberTail?

SafeMemberTail      ::= ParenArgs BlockSuffix?
                      | BareArgs  BlockSuffix?
                      | BlockSuffix
                      | ε

ParenArgs           ::= "(" ArgList? ")"

BareArgs            ::= BareArg { "," BareArg } [ "," ]
BareArg             ::= Expr | KeywordArg

ArgList             ::= Arg { "," Arg } [ "," ]

Arg                 ::= Expr
                      | KeywordArg

KeywordArg          ::= Name ":" Expr

ExprList            ::= Expr { "," Expr } [ "," ]

BlockSuffix         ::= "|" PatternList? "|" ":" BlockBody
                      | ":" BlockBody

BlockBody           ::= INDENT Statement+ DEDENT
                      | OneLineBlockBody

PatternList         ::= Pattern { "," Pattern } [ "," ]
```

`MethodName` в этой грамматике означает обычный идентификатор метода, включая суффиксы `?` и `!`. `PrimaryExpr` перечислен укрупнённо: точная grammar литералов и statement-like expressions задаётся соответствующими разделами Части I.

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
users.map: _1.email.downcase().strip() .uniq()
```

парсится как:

```amber
(users.map { _1.email.downcase().strip() }).uniq()
```

### 15.6. Assignable и pattern assignment

В expression grammar `Assignable` — это только lvalue:

```ebnf
Assignable          ::= Name
                      | "@" Name
                      | "@@" Name
                      | PostfixExpr "[" Expr "]"
                      | PostfixExpr "." Name
```

Деструктурирующее присваивание:

```amber
PATTERN = expr
```

является отдельной statement-form, а не частью общего `Assignable "=" ...`. То есть у языка есть два вида assignment:

1. обычное lvalue-присваивание;
2. pattern assignment.

Это снимает конфликт между parser core и pattern grammar.

### 15.7. Surface grammar параметров v1

До закрытия полной type grammar фиксируется такой surface grammar сигнатур:

```ebnf
ParamListDef        ::= [ Param { "," Param } [ "," ] ]

Param               ::= PosParam
                      | KwParam

PosParam            ::= LocalName
                      | LocalName "=" Expr
                      | AutoName
                      | AutoName "=" Expr
                      | LocalName "as" TypeTerm
                      | LocalName "as" TypeTerm "=" Expr
                      | AutoName "as" TypeTerm
                      | AutoName "as" TypeTerm "=" Expr

KwParam             ::= LocalName ":"
                      | LocalName ":" Expr
                      | AutoName ":"
                      | AutoName ":" Expr
                      | LocalName "as" TypeTerm ":"
                      | LocalName "as" TypeTerm ":" Expr
                      | AutoName "as" TypeTerm ":"
                      | AutoName "as" TypeTerm ":" Expr

AutoName            ::= "@" Name
                      | "@@" Name

LocalName           ::= Name
TypeTerm            ::= <type grammar TBD; atomically parsed here>
```

Нормативные следствия:

- у `@x` внешнее имя аргумента всё равно `x`;
- `def f(@x, x): ...` — compile-time error, потому что локальное имя параметра дублируется;
- rich multi-clause `def` обязаны иметь одну и ту же base signature в AST-эквивалентной форме;
- `include`-операнды не могут ссылаться на имена, вводимые тем же syntactic class/mixin body.

### 15.8. Surface grammar package/import/export/mixin/include v1

Эти формы не являются обычными выражениями. `package` / `import` / `export` живут на module/top-level и участвуют в dependency graph. `mixin` и `include` живут в declarative body-позициях object model.

```ebnf
ModulePath           ::= Name { "." Name }

PackageDecl          ::= "package" ModulePath

ImportDecl           ::= ImportModuleDecl
                       | FromImportDecl

ImportModuleDecl     ::= "import" ModulePath [ "as" Name ]

FromImportDecl       ::= "from" ModulePath "import" ImportName { "," ImportName } [ "," ]

ImportName           ::= Name [ "as" Name ]

ExportStmt           ::= "export" ExportName { "," ExportName } [ "," ]

ExportName           ::= Name [ "as" Name ]

MixinDef             ::= "mixin" Name ":" NEWLINE INDENT MixinBody DEDENT

MixinBody            ::= { MixinItem }

MixinItem            ::= DefStmt
                       | ClassDef
                       | MixinDef
                       | IncludeStmt
                       | PassStmt

IncludeStmt          ::= "include" IncludePath { "," IncludePath } [ "," ]

IncludePath          ::= Name { "." Name }
```

Нормативные parser/binder rules:

- `PackageDecl` допустим только как первая non-empty top-level форма;
- `ImportDecl` допустим только в contiguous import zone сразу после optional `PackageDecl`;
- `ExportStmt` допустим только на top-level;
- `ModulePath` v1 всегда absolute; relative spellings и `*` не являются частью grammar;
- `mixin` допускается на top-level и внутри declarative class/mixin body;
- `include` допустим только внутри declarative class/mixin body;
- `import` / `from` / `export` / `mixin` / `include` не могут lower'иться в обычные call-expression узлы.

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
  kw    = copy(passed_kw)

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

  subject_map   = build_args_map(defn.base_signature, locals)
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
- tuple pattern `( ... )` -> матчим против `ArgsTuple`;
- иной pattern -> разрешён только если у base signature ровно один positional parameter; тогда матчим значение этого единственного позиционного параметра;
- если positional-параметров больше одного и паттерн не `{...}` / `( ... )` -> compile-time error `ambiguous clause subject`.

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
pattern(expr) with {id:, meta: {role: :admin}, **null}
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
  when 1..10:
    :small
  when String:
    :str
```

Если `when ...` не разбирается как `Pattern`, запись трактуется как `MatcherExpr`, и runtime делает `MatcherExpr === value`.

Эта форма отличается от dynamic pattern objects из §17.8 тем, что:

- не экспортирует bindings;
- не использует `with`;
- остаётся pure fallback, если `when ...` не разбирается как `Pattern`.

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
- ordinary def: positional + keyword + defaults + auto-assign;
- static module syntax: `package`, `import`, `from ... import ...`, `export`, explicit re-export;
- multi-clause `def`: map subject / tuple subject / single-arg subject;
- `case` / `case!` со структурными паттернами и bare matcher expressions;
- dynamic pattern objects: `pattern(expr)` и `pattern(expr) with {...}`;
- pin / as-pattern / OR-pattern / `**null` / `**rest`;
- named `mixin` и `include` с linearized lookup order;
- open class и open mixin через повторные `class Name: ...` / `mixin Name: ...`-формы;
- `define_method(Target, :name) |...|: ...`, где `Target` может быть классом или mixin'ом;
- reflective `send(receiver, selector, ...)` с literal и dynamic selector;
- `method_missing` fallback;
- freeze transition и запрет world mutation после неё;
- `$_` в обычной функции, блоке и fiber frame.

### 18.3. Обязательные негативные тесты

Минимум:

- `map(_1 * 2)` как invalid v1;
- `_1` вне implicit-block;
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
AstClauseDef(name, base_signature, clauses[], else_body?)
AstIncludeStmt(paths[])
AstExprStmt(expr)
```

#### Сигнатуры

```text
AstSignature(params[])
AstParam(
  kind,                 # positional / keyword / rest-pos / rest-kw
  external_name?,
  local_name,
  default_expr?,
  type_expr?,
  auto_assign_kind?     # none / @ / @@
)
AstClause(pattern, guard_expr?, body)
```

#### Выражения и control-flow

```text
AstLiteral(value)
AstName(name)
AstIvar(name)
AstCvar(name)
AstConst(path)
AstInterpString(parts[])
AstUnary(op, expr)
AstBinary(op, left, right)
AstAssign(target, value)
AstPatternAssign(pattern, value)
AstIf(cond, then_body, else_body?)
AstWhile(cond, body)
AstUntil(cond, body)
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
HMethod(
  name,
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

### 18.9. Нормативная форма HIR для `def`

HIR видит метод уже не как «синтаксический def», а как единый callable object с тремя стадиями.

#### Стадия 1. bind

- preflight shape-check;
- defaults left-to-right;
- type hooks;
- формирование locals;
- **без** auto-assign commit.

#### Стадия 2. dispatch

- строится clause-subject по правилам latest `amber-lang-3`:
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

#### 0a. `mixin` / `include`

`AstMixinDef` и `AstIncludeStmt` никогда не lower'ятся в обычные expressions.

Нормативно:

- `AstMixinDef` нормализуется в `HMixin(name, body[])`;
- `AstIncludeStmt` допустим только внутри `AstClassDef` / `AstMixinDef` и lower'ится в `HInclude(paths[])`;
- binder обязан до expression-lowering проверить placement `include`, запрет `class_method def` внутри mixin body, явные type-mismatch'и include-target'ов и циклы, которые можно доказать статически;
- object-body lowering обязан сохранять source order direct includes, потому что later include доминирует в ancestor linearization.

#### 1. `$_`

```text
AST:  $_
HIR:  HLastGet
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
  [AstTailDotMember("map"), AstTailBlockSuffix(AstBlock(implicit_placeholders=1, ...))]
)
```

HIR:

```text
HSend(
  receiver = HLoadLocal(numbers),
  selector = "map",
  pos_args = [],
  kw_args = [],
  block   = HClosure(params=[p1], body=...)
)
```

`map(_1 * 2)` не lower'ится, потому что в v1 это невалидная surface-form.

#### 2a. Reflective `send`

Когда expression `send(recv, selector, ...)` резолвится именно к builtin prelude-binding `send`, lowering обязан идти так:

- если `selector` — compile-time literal `Symbol`/`Str`, форма может быть понижена в обычный `HSend`;
- иначе форма обязана понижаться в `HSendDyn(receiver, selector_expr, ...)`.

Shadowing имени `send` отключает этот lowering и возвращает обычный `HCall`.

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

- `task.async { ... }` -> `HSpawnSameStrand(...)`
- `task.spawn { ... }` -> `HSpawnNewStrand(...)`
- `handle.resume()` -> `HResume(handle)`
- `handle.wait(...)` -> `HWait(...)`

Это не меняет surface syntax: на AST это всё ещё обычные postfix/send-конструкции.

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
| G9. Stdlib collections & concurrency base | Закрыто на уровне спецификации v14 | Реализовать зафиксированный chainable API коллекций и concurrency primitives (`Channel`, `Mutex`, `Atomic`) | G5, G7 | Runtime API стабилен для пользовательского кода |

### P2 — вторая волна реализации и профилей

| Трек | Состояние | Ближайший шаг | Зависимости | Критерий выхода |
|---|---|---|---|---|
| G10. Типовая система v1 | Спецификация закрыта в v16; реализация не начата | Реализовать Amber/Typed checker: flow engine, invariance rules, exhaustiveness, reflective `Any`-boundary и typed-package tooling | G1–G9 | Typed profile воспроизводим и не меняет dynamic semantics языка |
| G11. Modules & metaprogramming | Спецификация закрыта в v16; реализация частично не начата | Реализовать open-class/open-mixin transactions, `define_method`, builtin `send`, `method_missing`, `include`/`extend` linearization, world freeze state, manifest/registry/client path и hot-reload guards | G1, G5, G6e, G9 | Dispatch-world, distribution и class-side composition реализуются без двусмысленности |
| G12. Advanced concurrency extensions | Спецификация закрыта в v16; реализация не начата | Реализовать `move(expr)` boundaries, `select`, supervisor policies и async-I/O awaitables поверх уже зафиксированного scheduler core | G7, G9 | Beyond-v1 concurrency работает без слома core no-GIL model |

### P3 — компилируемость и оптимизация

| Трек | Состояние | Ближайший шаг | Зависимости | Критерий выхода |
|---|---|---|---|---|
| G13. Bytecode compiler pipeline | Закрыто на уровне архитектуры, не реализовано | Эмитить bytecode из HIR: method prologues, pattern dispatch, safepoints, debug spans, task intrinsics и lifecycle intrinsics (`OBJ_DESTROY`, `OBJ_DEALLOC`) | G5–G7 | Один и тот же фронтенд обслуживает интерпретатор и компилятор |
| G14. Dynamicity boundary for AOT | Концептуально закрыто: frozen-world profile зафиксирован | Реализовать `world_epoch`/freeze transition, `SEND_DYN`, loader freeze mode и invalidation tests | G11, G13 | Понятно, какие модули можно компилировать нативно и какие места обязаны оставаться reflective |
| G15. Native backend / JIT | Архитектурно закрыто в v16; реализация не начата | Реализовать MIR/SSA pipeline, native/JIT codegen, frozen-image builder и runtime stubs для reflective sites | G13, G14 | Появляется путь к нативному профилю без слома динамического языка |

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
5. реализовать open-class/open-mixin transaction path, `define_method`, `include` linearization, `method_missing` fallback и `WorldFrozenError` guards поверх уже фиксированного ISA;
6. реализовать non-moving collector, remembered sets, ownership checks, tombstone checks, per-worker allocator и pin tables;
7. реализовать loader/linker/init state machine, dependency tests и frozen-mode loader barrier для precompiled modules.

Именно этот путь даёт самый короткий маршрут к работающему Amber runtime без возврата назад по архитектуре.


## 5. Implementation gate v14 [закрыт]

Implementation phase считается открытой для **reference implementation P0/P1**.

В качестве блокеров старта больше не остаются открытыми:

1. scope freeze первой реализации как **dynamic core + bytecode VM**;
2. minimal type envelope (`-> TypeTerm`, grammar `TypeTerm`, runtime hooks);
3. обязательный stdlib contract для chainable collections и concurrency base;
4. формальная матрица диагностик (`compile_error` / `warning` / `lint`);
5. minor closures: underscore policy, boundary bare matcher expressions и отказ от field lifetime annotations в v1.

После v16 на уровне спецификации больше не остаётся незакрытых вопросов второй волны.

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
  kind,                 # module / method / block / ensure / rescue / default-thunk
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
2. `SEND`/`CALL` резолвит target method/callable;
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
  flags,           # frozen / shareable / sync / finalizer / etc.
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
| `OBJ_DESTROY dst, obj` | выполнить terminal `destroy!`-semantics и вернуть bool |
| `OBJ_DEALLOC dst, obj` | немедленно разрушить и деаллоцировать payload с tombstone-model |
| `CLOSE_UPVALUES from_slot` | закрыть escaping captures перед выходом из области жизни |

Примечание: обычные локалы уже живут в `regs[]`, поэтому отдельные `LOAD_LOCAL/STORE_LOCAL` не требуются.

### 8.3. Calls, sends и protocol ops

| Инструкция | Семантика |
|---|---|
| `SEND dst, recv, selector_id, argv_desc, block_reg, site_id` | обычный method send с call-site cache |
| `SEND_DYN dst, recv, selector_reg, argv_desc, block_reg, site_id` | reflective send с selector'ом из регистра |
| `CALL dst, callee, argv_desc, block_reg, site_id` | вызов callable object |
| `IN_OP dst, elem, container` | реализует language-level `in` по протоколу `contains?` или бросает `TypeError` |
| `TRIPLE_EQ dst, matcher, value` | реализует `===` с обязательной проверкой булевого результата |
| `TYPECHECK value, type_term_id` | runtime-hook для `as TypeTerm` в bind/prologue |

Нормативно:

- арифметические операторы, сравнения, индексирование и обычный `[]`/`[]=` могут lower'иться в `SEND` соответствующих селекторов;
- `SEND_DYN` обязан проверять, что selector_reg содержит `Symbol` или `Str`, иначе бросать `TypeError`;
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
- `$_` уже явный `HLastGet/HLastSet`.

### 9.2. `HSend`, `HSendDyn` и `HCall`

```text
HSend(receiver, selector, args..., block?)
  -> evaluate receiver/args/block into regs
  -> SEND dst, r_recv, selector_id, argv_desc, r_block, site_id

HSendDyn(receiver, selector_expr, args..., block?)
  -> evaluate receiver/selector/args/block into regs
  -> SEND_DYN dst, r_recv, r_sel, argv_desc, r_block, site_id

HCall(callable, args..., block?)
  -> CALL dst, r_fn, argv_desc, r_block, site_id
```

### 9.3. `HMatchDispatch`

`HMatchDispatch` компилируется в линейную цепочку clause-block'ов:

1. вычислить scrutinee / clause-subject;
2. для каждой clause сверху вниз сгенерировать pattern-check region на `P_*`-инструкциях;
3. если pattern содержит dynamic pattern object, вычислить `matcher_expr`, вызвать `match(value)` и, при наличии `with ...`, прогнать returned `bindings` через обычный map-pattern region;
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
cache = CachePage.new(...)
...
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
  flags,            # frozen / shareable / sync / pinned / has_destructor / dead / etc.
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
pin = memory.pin(obj)
...
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

Эта часть закрывает вопрос compiled-module distribution profile для Amber v1. Source-level syntax `package` / `import` / `from ... import ...` / `export` уже зафиксирован выше, но loader/verifier по-прежнему работают не с поверхностным spelling, а с нормализованными logical module ids и export tables. Здесь фиксируется, как выглядит уже скомпилированный модуль и как он загружается runtime'ом.

Нормативный вывод:

- stable compiled artifact Amber v1 называется `.amberbc`;
- `.amberbc` описывает `BcModule` в сериализованном виде;
- loader/verifier обязаны работать с `.amberbc` независимо от того, был dependency записан как `import x.y` или `from x.y import Z`;
- debug info, dependency manifest и init-state machine входят в reference profile.

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
- section sizes / counts — 32-bit или ULEB128, но в пределах формата должны быть однозначны;
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

`instr_stream` обязан кодировать тот же semantic ISA, который зафиксирован в части про reference bytecode VM. Конкретный physical encoding допустим такой:

- 1-byte opcode;
- дальше operands в ULEB128/SLEB128 или фиксированном encoded form;
- decoder/verifier обязан однозначно это понимать.

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
- доступ к export, который ещё не был инициализирован до конца init-phase, обязан давать `ModuleInitError` либо эквивалентную диагностируемую ошибку, а не тихий `null`.

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

В source-level syntax v11 зависимости уже пишутся как absolute module ids; в `.amberbc` они в любом случае нормализуются до logical module ids.

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
5. прогнать corpus-тесты на round-trip `HIR -> bytecode -> .amberbc -> load -> run`.




# Часть X. Minimal MOP, reflective dispatch и frozen-world profile v1

## 1. Статус и граница решения

Эта часть закрывает для Amber v13 три старых узла:

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

## 2. Объём минимального MOP v13

Включено:

- named class create/reopen;
- atomic class-body commit;
- reflective `define_method`;
- builtin `send(receiver, selector, ...)`;
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

Когда исполняется `class Name ...: body`:

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

В минимальном v13 class-body нормализуется как **declarative body**. Для этого профиля нормативно существенны:

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
- dynamic clause accretion across reopen boundaries в v13 отсутствует.

## 4. Named mixins и `include`

Amber v13 добавляет минимальный mixin-profile без возврата к runtime module/import semantics.

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

В минимальном v13 mixin-body нормализуется как declarative body. Нормативно существенны:

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
define_method(User, :greet) |name|:
  "Hello, #{name}"
```

или

```amber
define_method(User, :greet, fn_obj)
```

### 5.1. Resolution rule

Специальная семантика действует только если имя `define_method` резолвится в builtin prelude binding. Если имя затенено локальной переменной, import-alias или параметром, форма становится обычным вызовом функции.

### 5.2. Contract

Минимальный контракт v13:

- первый аргумент обязан быть class object или mixin object;
- второй аргумент обязан быть `Symbol` или `Str`;
- реализация метода задаётся либо block suffix, либо явным callable-аргументом, но не обоими одновременно;
- в v13 `define_method` воздействует только на **instance-side** target class/mixin.

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
send(user, :full_name)
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

Amber v13 различает обычную мутацию данных и мутацию самого dispatch-world.

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

Этот раздел **не переоткрывает** языковые решения v14 и не вводит новых surface-feature'ов. Его цель — зафиксировать минимальный engineering baseline, при котором reference implementation можно начинать сразу и без повторного архитектурного дрейфа.

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
  -> .amberbc
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
- `stdlib` не должна содержать скрытых правил языка: её контракт обязан воспроизводить уже зафиксированную v14 surface semantics, а не подменять её.

## 4. Канонические dump-форматы

### 4.1. AST dump contract

AST dump для golden-тестов должен быть deterministic JSON-документом следующего общего вида:

```json
{
  "format": "amber.ast.v1",
  "module": "... or null ...",
  "items": [ ... ],
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
  "module": "... or null ...",
  "procedures": [ ... ],
  "constants": [ ... ],
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
      "primary_span": { ... },
      "related": [
        { "label": "...", "span": { ... } }
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
| `E2003` | error | parser | `from ... import *` forbidden |
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
amberc parse   path/to/file.am   --json
amberc lower   path/to/file.am   --json
amberc check   path/to/file.am   --json
amberc compile path/to/file.am   -o path/to/file.amberbc
amberc disasm  path/to/file.amberbc
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


# Часть XII. Closure-профили второй волны (v16)

## 1. Назначение этой части

Эта часть доводит до закрытого состояния те вопросы, которые в v15 ещё оставались не как блокеры старта reference runtime, а как **вторая волна дизайна и toolchain policy**. После принятия этой части у Amber больше не остаётся незакрытых spec-level вопросов: дальше остаются только implementation backlog, corpus/tests и конкретные runtime/toolchain работы.

Нормативный принцип v16:

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

В v16 дополнительно фиксируются следующие type-level решения:

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

Чтобы закрыть конфликт между static typing и open-world MOP, v16 фиксирует следующую границу:

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
- `methods(side: :instance | :class, local: Bool = false, inherited: Bool = true)`;
- `method(selector, side: ...)`;
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

В v16 вводится declarative `extend` для class-side composition.

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

В v16 по-прежнему не вводятся:

- `extend` внутри `mixin` body;
- reflective alias/remove/visibility API для class-side;
- новый мета-диалект наподобие `class << self`.

`extend` является world mutation и подчиняется тем же freeze/invalidation правилам, что и `include`.

## 6. Advanced concurrency profile

### 6.1. Ownership transfer через `move(expr)`

В v16 вводится explicit ownership-transfer marker `move(expr)`.

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
  when outbox.send(move(packet)):
    :sent
  timeout 1000:
    :timeout
  else:
    :idle
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

Language core не получает отдельного `await`-диалекта: уже существующий async/task model + `select` считаются достаточной surface-моделью v16.

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

## 9. Итоговый статус после v16

После принятия этой части:

- у Amber больше не остаётся незакрытых spec-level вопросов;
- dynamic core, typed profile, distribution policy, reflection, class-side composition, advanced concurrency, memory second wave и backend profile имеют зафиксированную границу;
- дальнейшая работа — это уже не дизайн-спор, а реализация parser/runtime/checker/registry/native backend и расширение corpus/tests.


# Часть XIII. Детализированная матрица имплементации (v17)

## 1. Назначение

Эта часть не переоткрывает дизайн языка и не меняет нормативную семантику v16. Её задача — превратить уже закрытые уровни `G1..G15` и bootstrap-этапы `F0..F3`, `V0..V5` в **исполняемую инженерную матрицу**, пригодную для:

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

В v17 используются три уровня инженерной декомпозиции.

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
| `W0.3` | corpus runner skeleton | G8 | базовый `ambertest run ...` с `meta.json` parsing | `W0.1` | runner способен запустить хотя бы parse-case |
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

- есть полный путь `source -> HIR -> .amberbc -> disasm`.

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

### 5.7. Критический путь v17

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

## 9. Статус после v17

После принятия этой части:

- верхнеуровневая матрица `G1..G15` остаётся в силе;
- `F0..F3`, `V0..V5` остаются bootstrap-порядком;
- `W0..W10` становятся рабочим инженерным слоем между спецификацией и репозиторием;
- старт reference repo больше не требует дополнительных spec-level решений.



# Часть XIV. Репозиторный backlog pack и milestone gating (v18)

## 1. Назначение

Если v17 переводит закрытую спецификацию в инженерную матрицу `W0..W10`, то эта часть переводит матрицу в **операционный пакет для репозитория**.

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
  amber_spec_consolidated_v19.md
  changelog/
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
/.github/   # либо эквивалентный каталог forge automation
```

Нормативно:

- `corpus/` обязан быть отделён от исходников toolchain;
- `spec/` обязан содержать зафиксированную редакцию спецификации, на которую ссылается текущий mainline;
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

### 5.1. Донабор к стартовому набору v17

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
- существует путь `source -> HIR -> .amberbc -> disasm`;
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
- ...

## Out of scope
- ...

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

## 10. Статус после v18

После принятия этой части:

- `W0..W10` и `M0..M9` получают прямое отображение на repo operations;
- стартовый backlog превращается в почти готовый issue tracker import set;
- milestone gates больше не требуют отдельных управленческих решений;
- переход от спецификации к фактическому запуску reference repo можно считать завершённым.

# Часть XV. Исполнимая декомпозиция tracking issues и внутренних контрактов (v19)

## 1. Назначение

Если v17 переводит закрытую спецификацию в `W0..W10`, а v18 — в operational backlog/labels/milestones, то v19 добавляет ещё один инженерный слой: **короткие исполнимые slices под существующие `ISS-*`**.

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

- `T-004.1`, `T-004.2`, ... для `ISS-004`;
- `T-052.1`, `T-052.2`, ... для `ISS-052`;
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
- verifier invariants;
- canonical disasm contract;
- deterministic emitter output для одинакового HIR.

### 3.5. `amber.runtime.v1`

Нормативно фиксируются:

- frame slot для `$_`;
- dead-object checks до fast-path `LOAD_IVAR/STORE_IVAR/SEND`;
- shareable vs strand-confined boundary;
- loader state machine;
- world-mutation invalidation hooks.

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
- `T-006.2` реализовать `import`, `from ... import ...`, `export` как отдельные top-level forms, не понижаемые в call-expression.
- `T-006.3` реализовать `class`, `mixin`, `class_method def`, declarative `include` и placement-sensitive bodies.
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
- `T-011.3` реализовать placement checks для `include` и `extend`, не дожидаясь полного runtime path этих feature'ов.
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

- `T-016.1` зафиксировать `AmberBcHeader`, version fields, feature flags и ABI hash policy.
- `T-016.2` зафиксировать section directory, section IDs, required vs optional sections.
- `T-016.3` зафиксировать index model между `CODE`, `METH`, `CLAS`, `SYMS`, `KONS`, `DEPS`, `EXPT`.
- `T-016.4` зафиксировать ограничения на serializable constants и запрет raw host pointers.
- `T-016.5` собрать round-trip fixtures на минимальные и feature-rich modules.

`ISS-016` закрывается только когда format достаточно стабилен, чтобы writer/reader/verifier и disassembler работали поверх одного и того же header/section contract.

#### `ISS-017` writer/reader/verifier + `ISS-034` + `ISS-035`

- `T-017.1` реализовать serializer/deserializer section directory и базовых секций.
- `T-017.2` реализовать structural verifier: magic, versions, offsets, bounds, alignments, duplicate/missing sections.
- `T-017.3` реализовать semantic-lite verifier: `code_id`, symbol indexes, method/class ranges, dependency tables.
- `T-017.4` реализовать debug/span verifier для `SPAN`, `LINE`, `LOCS` и related source references.
- `T-017.5` собрать negative fixtures на broken offsets, broken indexes, malformed debug tables и incompatible format flags.

`ISS-017`, `ISS-034` и `ISS-035` закрываются только когда любой load-path сначала проходит verifier и broken bytecode отвергается детерминированной диагностикой.

#### `ISS-018` disassembler + `ISS-036`

- `T-018.1` зафиксировать canonical textual layout disasm output.
- `T-018.2` реализовать stable naming для registers/locals/captures/code IDs.
- `T-018.3` реализовать source span comments и optional debug sections rendering.
- `T-018.4` собрать golden corpus на canonical disasm, устойчивый к повторным прогонам.

`ISS-018` и `ISS-036` закрываются только когда одинаковый `.amberbc` даёт побайтно одинаковый disasm text и тот пригоден как regression artifact.

#### `ISS-019` bytecode emitter baseline + `ISS-037` + `ISS-038`

- `T-019.1` реализовать method/module prologues, locals layout и register allocation baseline.
- `T-019.2` реализовать control-flow emission: branches, loops, returns, `SETLAST/GETLAST`.
- `T-019.3` реализовать closure emission и lexical capture layout.
- `T-019.4` реализовать exception/unwind metadata emission и handler tables.
- `T-019.5` реализовать call-site / ivar-site descriptors, safepoints и source span tables.

`ISS-019`, `ISS-037` и `ISS-038` закрываются только когда весь путь `HIR -> .amberbc -> disasm` работает для ordinary methods, closures и exception-aware code.

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

- `T-050.1` реализовать rendezvous и buffered channel cases, включая close/error semantics если они входят в выбранный runtime profile.
- `T-050.2` реализовать shareability gate на cross-strand payloads и ошибки isolation boundary.
- `T-050.3` реализовать fairness corpus для send/recv/selectable wait paths.
- `T-051.1` реализовать `Mutex` и `Atomic` как shareable sync objects без скрытого GIL semantics.
- `T-051.2` собрать corpus на lock/unlock, compare-and-set, wake ordering и contention smoke.

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

Если команда стартует с нуля, то кроме tracking issues из v18 ей выгодно сразу открыть как минимум следующий минимальный subtask-set:

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

Именно этот набор быстрее всего убирает архитектурную неоднозначность и открывает независимую работу по frontend/corpus/tooling без дрейфа.

## 10. Дополнительные анти-дрейф правила для subtask-level исполнения

Нормативно:

1. нельзя закрывать `T-*`, меняющую AST/HIR/diag/disasm/bytecode output, без corpus update в том же changeset;
2. нельзя открывать parallel work по emitter/VM, пока не заморожены `amber.ast.v1` и `amber.hir.v1` notes;
3. нельзя считать `ISS-020..ISS-024` закрытыми, если fast-path'ы всё ещё обходят dead-object checks;
4. нельзя открывать real parallel scheduler path до закрытия `ISS-046`;
5. `ISS-052` и далее не имеют права обходить verifier path ни в тестах, ни в production loader.

## 11. Статус после v19

После принятия этой части:

- `W0..W10`, `M0..M9` и `ISS-001..ISS-072` получают ещё и PR-sized execution layer;
- critical path `M1..M5` больше не требует новой организационной декомпозиции;
- у reference repo появляется стабильная точка заморозки для AST/HIR/diag/bytecode/runtime interfaces;
- typed/native/concurrency-second-wave workstreams остаются намеренно вторичными и не блокируют dynamic reference runtime.

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
