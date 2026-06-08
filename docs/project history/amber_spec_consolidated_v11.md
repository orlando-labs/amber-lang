> Редакция v11: в эту версию дополнительно включены нормативные решения по source-level module syntax: `package`, `import`, `from ... import ...`, `export`. Ранее зафиксированные reference bytecode VM, runtime ABI, explicit lifetime control (`destroy!` + `memory.dealloc`), reference collector/pinning/FFI profile, compiled-module/loader profile, dynamic pattern objects в explicit-binding profile и строгая match-форма `case!` сохраняются без регрессий.


# Amber

**Консолидированная спецификация (текущее зафиксированное состояние)**  
Редакторская консолидация по истории разработки  
6 апреля 2026


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

Amber в текущем виде — **консолидированный draft+**. Ядро синтаксиса и семантики уже достаточно устойчиво, чтобы писать не только прототип парсера и интерпретатора, но и первый байткодный runtime. В этой редакции дополнительно закрыты reference execution profile, reference lifetime profile, reference collector/pinning/FFI profile, compiled-module/loader profile, dynamic pattern objects в explicit-binding profile, строгая match-форма `case!` и source-level module syntax: register/slot bytecode VM, frame/closure/object ABI, pattern-matching opcode family, ownership model без GIL, explicit destruction/deallocation (`destroy!` + `memory.dealloc`), non-moving generational collector, pinning boundary для native interop, `.amberbc`-формат и verifier/loader state machine, явные dynamic matcher bindings без скрытой инъекции локалов, strict fail-mode для `case!` и статический пакетно-импортный профиль `package/import/export`. Полностью незавершёнными остаются прежде всего типовая система, глубина MOP/метапрограммирования, package-manager/distribution policy, field-level lifetime annotations (`weak`/`borrowed`) и frozen/AOT-профиль.

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
- `class` и `def` (с оговорённой ниже семантикой результата).

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
- `include` не является формой загрузки модулей и остаётся предметом будущего MOP/namespace design;
- runtime/dynamic import как отражательная операция не входит в source-level v1-core.

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

Строгая форма v11:

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
- неплотная нумерация placeholders.

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
- `in` применён к объекту, который не является контейнером и не поддерживает `contains?`.

## 12. Типовая система: текущий зафиксированный контур

Типовая система **не является завершённой нормативной частью**, но уже зафиксированы её рамки.

Принятые решения:

- язык остаётся gradual / optional typed;
- без аннотаций код ведёт себя как динамический;
- с аннотациями доступны статические проверки и инференс;
- вместо `::` предпочтён `as`, чтобы не конфликтовать с `A::B`;
- в binding-контексте ожидаются формы вида `x as Int`, `@x as User`, `def f(x as Int): ...`;
- `expr as Type` в expression-контексте трактуется как assertion / checked cast;
- `name as {id:, name:}` и `name as Int` в pattern-контексте остаются as-pattern, а не типизацией, потому что справа pattern-term, а не type-term.

На этом этапе спецификация фиксирует только surface envelope, но не полную type grammar.


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


## 14. Что входит в язык по намерению, но ещё не нормализовано до ядра

Следующие вещи явно входят в замысел Amber, но пока описаны скорее как design commitments, а не как законченная нормативная часть:

- открытые классы;
- `define_method`;
- `method_missing`;
- `send`;
- DSL-макросы в ruby-style;
- `.lazy` как явный ленивый режим для цепочек коллекций;
- package manager / artifact distribution / hot reload;
- MOP-level `include` / namespace composition / open-module semantics.



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
- ключевые слова `not`, `and`, `or`, `in`, `if`, `else`, `case`, `case!`, `when`, `def`, `class`, `while`, `until`, `do`, `loop`, `package`, `import`, `from`, `export`;
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
- rich multi-clause `def` обязаны иметь одну и ту же base signature в AST-эквивалентной форме.

### 15.8. Surface grammar package/import/export v1

Эти формы не являются выражениями. Они живут на module/top-level и участвуют в построении dependency graph.

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
```

Нормативные parser/binder rules:

- `PackageDecl` допустим только как первая non-empty top-level форма;
- `ImportDecl` допустим только в contiguous import zone сразу после optional `PackageDecl`;
- `ExportStmt` допустим только на top-level;
- `ModulePath` v1 всегда absolute; relative spellings и `*` не являются частью grammar;
- `import` / `from` / `export` не могут lower'иться в обычные call-expression узлы.

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
- ссылка из `pattern(expr)` на имя, вводимое тем же enclosing pattern.

### 18.4. Golden representation policy

Для стабильной реализации рекомендуются три слоя golden-представлений:

1. **AST golden** — проверяет чисто синтаксический разбор;
2. **HIR golden** — проверяет нормализацию:
   - simple many-def sugar -> clause-style `def`;
   - safe-nav -> explicit null-guard nodes;
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
AstClassDef(name, body[])
AstDefStmt(name, signature, body)
AstClauseDef(name, base_signature, clauses[], else_body?)
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
- понижать `package` / `import` / `export` в обычные call-узлы;
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
HClass(name, body[])
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

## Q1. Политика стиля для имён с подчёркиванием

В текущей редакции уже зафиксировано следующее:

- `_` — wildcard в pattern-контекстах;
- `$_` — last-result register;
- `_1`, `_2`, ... — placeholders в блоках без `|...|`;
- `_tmp` и обычные underscore-start имена допускаются как нормальные идентификаторы.

Открытым остаётся уже не синтаксис, а policy-уровень: хотим ли мы линтерно ограничивать имена, слишком похожие на спец-формы.

## Q2. Финальный модульный / импортный синтаксис [закрыто в v11]

В v11 source-level module syntax зафиксирован в static-profile:

- `package module.path` задаёт logical module id импортируемого файла;
- `import module.path [as Alias]` и `from module.path import Name [as Alias]` — единственные специальные формы загрузки/связывания;
- `export Name [as Public]` формирует export table исходного модуля;
- relative imports и star-import в v1 не вводятся;
- `require` и `include` не являются loader-формами.

Открытыми остаются уже не syntax-level, а ecosystem/MOP-level вопросы: package manager, trust/signing, hot reload и будущая роль `include` / module objects в метаобъектной модели.

## Q3. Глубина нормализации метапрограммирования

Есть явное целевое намерение взять ruby-style метапрограммирование, но не зафиксированы:

- границы открытых классов;
- позднее добавление клауз в multi-clause def;
- поведение `method_missing`;
- протокол `define_method`;
- отражение и introspection API.

## Q4. Типовая система: grammar, semantics, inference

Зафиксирован только контур, но не завершены:

- type grammar;
- return type syntax;
- record/map types с ключами;
- вариантность generics;
- typing для `and/or`;
- typing для `$_`;
- narrowing через `case`;
- взаимодействие типов и метапрограммирования.

## Q5. Concurrency после фиксации v1-core

Базовая модель concurrency больше не является незакрытой: v1 фиксирует no-GIL runtime через **worker pool + strand isolation + cooperative tasks**.

Открытыми остаются уже только **расширения второго этапа**:

- ownership transfer / move-semantics для больших mutable-объектов;
- `select` / multi-channel wait;
- priority scheduling;
- async I/O completion API;
- supervisor policies богаче, чем current structured-cancellation;
- distributed / multi-process runtime.

## Q6. Динамические pattern-objects [закрыто в v10]

В v10 dynamic pattern objects включены в v1, но только в **explicit-binding profile**.

Принятое решение:

- разрешить `pattern(expr)` и `pattern(expr) with MAP_PATTERN`;
- запретить скрытую инъекцию локалов;
- разрешить feature только в `case`, `case!` и clause-style `def`;
- оставить block params и pattern assignment вне v1 для этой формы.

Остаются открытыми только расширения второго этапа: richer matcher protocols, typed bindings-map и возможные library-level combinators.

## Q7. Формальный статус `matcher expressions` вне `case` / `case!`

Сейчас bare matcher expressions честно разрешены только в `case` / `case!`. Нужно отдельно решить, стоит ли когда-либо допускать их в других контекстах. Пока разумнее не допускать.

## Q8. Финальный раздел по стандартной библиотеке коллекций

Синтаксис языка опирается на наличие цепочечных методов (`map/select/reduce/each`, `group_by`, `transform_values`, `.lazy`). Но каталог обязательного коллекционного API пока не оформлен как часть спецификации стандартной библиотеки.

## Q9. Нужна ли строгая match-форма поверх безопасного `case` [закрыто в v10]

Да. В v10 принят `case!` как строгая surface-form поверх того же `case`-engine.

Принятое решение:

- `case` без `else` остаётся safe-form и возвращает `null`;
- `case!` без `else` бросает `MatchError`;
- grammar `when PATTERN if GUARD:` и lowering остаются общими;
- отдельный `match!` в v1 не вводится.

## Q10. Формальная матрица диагностик компилятора

Часть диагностик уже ясна, но ещё не собран отдельный нормативный каталог:

- какие ошибки compile-time;
- какие warnings;
- какие lint-правила;
- какие из них обязательны для компилятора, а какие относятся к tooling.


## Q11. Где проходит граница между fully dynamic Amber и native/AOT profile

После закрытия parser core стало ясно, что Amber **компилируем** как минимум в байткод/VM. Но для native/AOT пока не закрыты границы динамичности:

- можно ли открывать классы после загрузки пакета;
- как взаимодействуют `define_method`, `send`, `method_missing` и AOT;
- нужна ли отдельная "frozen world" модель для компилируемых модулей;
- допускается ли deopt / fallback в интерпретатор для слишком динамичных мест.

Этот вопрос больше не блокирует v1-спеку языка, но блокирует дизайн оптимизирующего компилятора.



## Q12. Остаточные вопросы после фиксации collector/pinning/FFI profile

Reference profile теперь уже **закрывает**: non-moving generational collector, pin tokens, pinned scopes, opaque-handle FFI boundary, safe-point handshake и запрет implicit GC-finalizer semantics для пользовательского `destroy!`.

Открытыми остаются уже более узкие вопросы второго этапа:

- weak refs / ephemerons / soft refs;
- surface-синтаксис для borrowing, если он вообще понадобится языку, а не только embedding API;
- zero-copy typed buffers / slices / SIMD-friendly memory views;
- telemetry и tuning policy collector'а (thresholds, pacing, quotas);
- production embedding API уровня host-process lifecycle, sandboxing и memory limits.

Это больше не блокирует reference runtime Amber, но блокирует production-hosting story и часть системного инструментария.

## Q13. Остаточные вопросы после фиксации module format / loader / verifier profile

Reference profile теперь уже **закрывает**: `.amberbc`-артефакт, section model, loader state machine, dependency manifest, export/import symbol tables и минимальный verifier contract.

Открытыми остаются вопросы toolchain/distribution-уровня:

- package manager policy, registry/source layout и mapping `package` -> artifact;
- artifact signing, reproducible builds и trust chain;
- политика совместимости между версиями VM и формата beyond basic major/minor gating;
- hot reload / incremental recompilation / dev-server semantics.

Это уже не блокирует реализацию VM и precompiled modules, но блокирует полноценную distribution ecosystem.

## Q14. Нужны ли полевые lifetime-аннотации (`owned`, `weak`, `borrowed`)

После добавления explicit dealloc остаётся отдельный дизайнерский вопрос второго этапа:

- нужен ли декларативный способ описывать, какие ссылки объект обязан разрушать сам;
- хотим ли мы `owned`-слоты для deterministic deep teardown;
- нужен ли `weak`/`borrowed` профиль для FFI и больших графов.

В текущей v1-редакции этого нет: разрушение объектных графов описывается пользовательским `destroy!`, а не полевой аннотацией языка.


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
11. только после этого — типы, stdlib, MOP и frozen/AOT profile.

## 2. Матрица

### P0 — frontend и semantic core

| Трек | Состояние | Ближайший шаг | Зависимости | Критерий выхода |
|---|---|---|---|---|
| G1. Parser core / expressions | Закрыто на уровне спецификации | Реализовать lexer + Pratt parser с `CHAIN_DOT`, `.?.`, block suffix и bare-call rules | Нет | Все примеры из grammar-раздела дают стабильный syntax-faithful AST |
| G2. AST schema | Закрыто на уровне архитектуры | Собрать node set, source spans и AST golden corpus | G1 | Parser выдаёт один и тот же AST на одинаковом surface syntax |
| G3. Binder / signatures / defaults | Закрыто на уровне спецификации | Реализовать `bind_call`, preflight, default-eval, typecheck hooks и delayed auto-assign commit | G1, G2 | Ordinary def и multi-clause def воспроизводимы по golden tests |
| G4. Pattern runtime v1 | Закрыто на уровне спецификации | Реализовать `match()` + протокол `===` / `deconstruct*` + context-specific commit semantics | G1, G2, G3 | Все примеры pattern matching из спеки исполняются без расхождений |
| G5. HIR и lowering | Закрыто на уровне архитектуры | Зафиксировать и реализовать AST->HIR lowering для safe-nav, implicit block, many-def sugar, `$_`, async intrinsics | G1–G4 | Есть стабильный HIR для интерпретатора и байткодного компилятора |

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
| G9. Stdlib collections & concurrency base | Дизайн-намерение | Оформить обязательный chainable API коллекций и concurrency primitives (`Channel`, `Mutex`, `Atomic`) | G5, G7 | Runtime API стабилен для пользовательского кода |

### P2 — вторая волна дизайна

| Трек | Состояние | Ближайший шаг | Зависимости | Критерий выхода |
|---|---|---|---|---|
| G10. Типовая система v1 | Контур без формализации | Зафиксировать type grammar, return types, map/record types, typing `$_` и `and/or` | G1–G9 | Checker стабилен на примерах и не конфликтует с pattern grammar |
| G11. Modules & metaprogramming | Частично закрыто: source-level module syntax зафиксирован, MOP нет | Нормализовать minimal MOP и distribution policy: open classes, `define_method`, `send`, `method_missing`, `include`/namespace objects и package layout | G1, G5, G6e, G9 | Можно описать позднее расширение классов и упаковку модулей без двусмысленности |
| G12. Advanced concurrency extensions | Не начато | Решить move-semantics, channel select, richer supervisors, async I/O integration | G7, G9 | Beyond-v1 concurrency становится расширяемой без слома core model |

### P3 — компилируемость и оптимизация

| Трек | Состояние | Ближайший шаг | Зависимости | Критерий выхода |
|---|---|---|---|---|
| G13. Bytecode compiler pipeline | Закрыто на уровне архитектуры, не реализовано | Эмитить bytecode из HIR: method prologues, pattern dispatch, safepoints, debug spans, task intrinsics и lifecycle intrinsics (`OBJ_DESTROY`, `OBJ_DEALLOC`) | G5–G7 | Один и тот же фронтенд обслуживает интерпретатор и компилятор |
| G14. Dynamicity boundary for AOT | Не закрыто концептуально | Определить frozen-world profile, invalidation rules и reflective slow-path | G10, G11, G13 | Понятно, какие модули можно компилировать нативно |
| G15. Native backend / JIT | Не начато | Выбрать MIR/SSA слой и backend strategy | G13, G14 | Появляется путь к нативному профилю без слома динамического языка |

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

### Этап 3. Закрыть вторую волну дизайна
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
2. реализовать register VM loop с `SEND/CALL/JUMP/RETURN/GETLAST/SETLAST/MAKE_CLOSURE/OBJ_DESTROY/OBJ_DEALLOC`;
3. эмитить bytecode из HIR для `safe-nav`, clause dispatch, pattern assignment, lifecycle intrinsics и `task.async` / `task.spawn`;
4. подключить method tables, class/shape versions, `DeadShape` и inline caches для send/ivar access;
5. реализовать non-moving collector, remembered sets, ownership checks, tombstone checks, per-worker allocator и pin tables поверх уже фиксированного ISA;
6. реализовать loader/linker/init state machine и dependency tests для precompiled modules.

Именно этот путь даёт самый короткий маршрут к работающему Amber runtime без возврата назад по архитектуре.


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
- метапрограммирование и open classes ещё не нормализованы;
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

Прямой native backend сейчас упрётся не в синтаксис, а в динамические свойства языка:

- позднее открытие классов;
- `define_method`;
- `method_missing`;
- `send` с динамическим именем метода;
- gradual typing;
- matcher protocol через `===`;
- потенциальный reflective import/load;
- дальнейшие concurrency-расширения сверх зафиксированного no-GIL core.

Сделать native backend можно, но до стабилизации этих зон он будет постоянно ломаться о меняющийся runtime contract.

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

### 5.2. Что нужно зафиксировать, чтобы AOT был реалистичен

Для native/AOT в будущем придётся ввести или явно описать:

- frozen-world boundary: когда набор классов и методов считается стабильным;
- invalidation rules для `open class`, `define_method`, `send`, `method_missing`;
- policy для reflective slow-path;
- package/module loading phase;
- ABI объектов, closure-ов и frame-ов;
- policy для generics/type metadata;
- deopt или fallback-механизм, если код вышел за границы профиля.

### 5.3. Практичный компромисс

Самый реалистичный вариант — двухрежимная модель:

1. **обычный Amber**:
   - компиляция в байткод;
   - полная динамичность;
   - reflective features разрешены;

2. **Amber/Frozen** или **Amber/AOT profile**:
   - модуль загружается в "закрытом мире";
   - после фиксации world-state запрещаются или инвалидируют оптимизации:
     - late open class,
     - dynamic `define_method`,
     - неконтролируемый `send`,
     - runtime shape-changing MOP;
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

- open classes;
- `define_method`;
- `method_missing`;
- `send`;
- возможный runtime import/load;
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
- закрыты Q3–Q5 и Q11 из этого документа.

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
  nested_classes[],
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
  owner_class,
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

Каждый class object в reference profile должен хранить как минимум:

```text
ClassRuntime(
  method_table,
  method_version,
  ivar_schema,
  class_flags,
  superclass?
)
```

`method_version` монотонно меняется при любой операции, влияющей на dispatch:

- open class mutation;
- `define_method`;
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
| `CALL dst, callee, argv_desc, block_reg, site_id` | вызов callable object |
| `IN_OP dst, elem, container` | реализует language-level `in` по протоколу `contains?` или бросает `TypeError` |
| `TRIPLE_EQ dst, matcher, value` | реализует `===` с обязательной проверкой булевого результата |
| `TYPECHECK value, type_term_id` | runtime-hook для `as TypeTerm` в bind/prologue |

Нормативно:

- арифметические операторы, сравнения, индексирование и обычный `[]`/`[]=` могут lower'иться в `SEND` соответствующих селекторов;
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

### 9.2. `HSend` и `HCall`

```text
HSend(receiver, selector, args..., block?)
  -> evaluate receiver/args/block into regs
  -> SEND dst, r_recv, selector_id, argv_desc, r_block, site_id

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
  owner_class_ref,
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
