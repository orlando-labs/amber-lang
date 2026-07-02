# Json в Amber

`Json` - встроенная стандартная библиотека для чтения, записи и потоковой
обработки JSON. Её не нужно импортировать: `Json` доступен как runtime native
тип в обычном Amber-коде.

Короткий пример:

```amber
payload = {id: 42, name: "Ada", tags: ["compiler", "vm"]}

compact = payload.to_json
same = Json.generate(payload)
pretty = Json.pretty_generate(payload, indent: 2)

parsed = Json.parse(compact)
parsed[:id] + parsed[:tags][0].count()
```

## Модель Значений

`Json.parse` переводит JSON в обычные Amber-значения:

| JSON | Amber |
| --- | --- |
| object | `Map` по умолчанию |
| array | `List` |
| string | `Str` |
| integer number | `Int` |
| fractional/exponent number | `Float` |
| `true` / `false` | `Bool` |
| `null` | `null` |

Обычный `Map` в Amber name-indifferent: ключи JSON-объекта можно читать строкой
или символом.

```amber
user = Json.parse("{\"id\": 7, \"name\": \"Iris\"}")

user["id"]   # 7
user[:id]    # 7
user[:name]  # "Iris"
```

Если важно сохранить точное строковое имя ключа и не смешивать строковые ключи с
символами, используйте `StrictMap`:

```amber
user = Json.parse("{\"id\": 7}", map: StrictMap)

user["id"]           # 7
user.has_key?(:id)   # false
```

`strict: true` ещё распознаётся runtime для совместимости, но в новом коде лучше
писать `map: StrictMap`: это явнее и совпадает с типом результата.

## Parse

```amber
value = Json.parse("{\"ok\": true, \"count\": 3}")
strict = Json.parse("{\"ok\": true}", map: StrictMap)
```

`Json.parse(text)` принимает `Str` и возвращает одно JSON-значение. На входе
должен быть полный JSON-документ; лишний не-whitespace текст после значения
считается ошибкой.

Ошибки парсинга приходят как `JsonParseError` с позицией вида `line:col` и
offset. Слишком глубокие документы также считаются ошибкой парсинга.

## Generate

```amber
Json.generate({ok: true, count: 3})
# "{\"ok\":true,\"count\":3}"

Json.pretty_generate({ok: true, items: [1, 2]}, indent: 4)
# {
#     "ok": true,
#     "items": [
#         1,
#         2
#     ]
# }

{ok: true, count: 3}.to_json
# "{\"ok\":true,\"count\":3}"

{id: 1, name: "Ada", password: "secret"}.to_json(only: [:id, :name])
# "{\"id\":1,\"name\":\"Ada\"}"

{id: 1, name: "Ada", password: "secret"}.to_json(except: :password)
# "{\"id\":1,\"name\":\"Ada\"}"
```

`value.to_json` - это value-method форма для того же генератора, что и
`Json.generate(value)`. Для `Map` и `StrictMap` он также принимает keyword-опции
`only:` и `except:`. Значение может быть одним ключом или списком ключей; если
переданы обе опции, сначала применяется `only:`, затем `except:`.

Генератор принимает JSON-представимые значения: `null`, `Bool`, `Int`, `Float`,
`Str`, `Symbol`, `List`, `Map` и `StrictMap`. Ключи объектов должны быть `Str`
или `Symbol`. `NaN`, `Infinity`, функции, задачи, каналы и другие runtime-объекты
не являются JSON и дают `JsonGenerateError`.

`Json.pretty_generate` принимает только `indent:`. Значение `indent` должно быть
целым числом от `0` до `16`; по умолчанию используется `2`.

## Файлы

```amber
config = Json.load_from_file("config.json")

Json.save_to_file("config.json", {enabled: true, retry: 3})
Json.save_to_file("config.pretty.json", config, pretty: true, indent: 2)
```

`Json.load_from_file(path)` читает файл целиком и парсит один JSON-документ.
`Json.save_to_file(path, value)` пишет компактный JSON и добавляет завершающий
перевод строки. С `pretty: true` используется форматирование
`Json.pretty_generate`.

В capability-aware запуске и в собранном `amberc build` executable для файлового
I/O нужны grants:

```sh
build/amberc build app.am -o build/app \
  --grant fs.read=config.json \
  --grant fs.write=out.json
```

Без нужного grant операция завершится `CapabilityError`. Ошибки самого I/O
приходят из host/runtime провайдера, например `FileNotFoundError` или `IOError`.

## JSONL

JSONL, или NDJSON, - это формат "одно JSON-значение на строку". Включается только
явным `jsonl: true`; расширение файла не переключает режим автоматически.

```amber
rows = [
  {id: 1, value: 10},
  {id: 2, value: 20}
]

Json.save_to_file("events.jsonl", rows, jsonl: true)

loaded = Json.load_from_file("events.jsonl", jsonl: true)
loaded[0][:value] + loaded[1][:value]
```

`Json.save_to_file(path, rows, jsonl: true)` ожидает `List`; каждый элемент
записывается компактным JSON на отдельной строке. `pretty:` в JSONL-режиме не
используется, потому что переносы внутри одного значения ломают формат "одна
строка - одно событие".

`Json.load_from_file(path, jsonl: true)` возвращает `List` всех непустых строк.
Для больших логов и event streams чаще лучше использовать потоковый API ниже:
он не материализует весь список строк.

## Path

`Json.path` и `Json.paths` выполняют маленький стабильный subset JSONPath-like
навигации по обычным Amber-значениям (`Map`, `StrictMap`, `List` и скаляры).
Это не отдельный язык выражений: фильтры, условия и агрегация пишутся обычным
Amber-блоком.

Поддерживаемый синтаксис:

| Синтаксис | Значение |
| --- | --- |
| `$` | корневое значение |
| `.key` | строковый ключ-идентификатор |
| `["key"]` / `['key']` | строковый ключ с любыми символами |
| `[0]` | индекс списка |
| `[*]` | все элементы списка или все значения map |

```amber
payload = Json.parse("{\"items\":[{\"id\":1},{\"id\":2}],\"user\":{\"name\":\"Ada\"}}")

payload.path("$.user.name")        # "Ada"
payload.path("$.items[*].id")      # 1
payload.paths("$.items[*].id")     # [1, 2]

Json.path(payload, "$.user.name")
Json.paths(payload, "$.items[*].id")
```

`path(query)` работает как `find_first`: возвращает первый match или `null`.
`paths(query)` работает как `find_all`: возвращает `List` всех matches.

`Map#path`, `Map#paths`, `List#path` и `List#paths` - shorthand-формы для тех
же функций. Обычный `Map` использует name-indifferent lookup, а `StrictMap`
ищет точный строковый ключ:

```amber
{name: 1}.path("$.name")                       # 1
StrictMap{name: 1, "name": 2}.path("$.name")   # 2
```

Для фильтрации и reduce используйте `paths(query, init)` с блоком
`|element, accumulator|`. Если блок возвращает non-`null`, это становится новым
accumulator; если возвращает `null`, сохраняется текущий accumulator, что удобно
для мутации коллекции.

```amber
ids = payload.paths("$.items[*]", []) |item, acc|:
  if item[:id] > 1:
    acc.push!(item[:id])
  null
```

В path-fold блоке `Json.stop` прекращает дальнейший обход и возвращает текущий
accumulator. `Json.stop(value)` прекращает обход и возвращает `value` как
финальный accumulator.

```amber
first_big = payload.paths("$.items[*]", null) |item, acc|:
  if item[:id] > 1:
    Json.stop(item)
  acc
```

## Streaming

`Json.stream_parse` и `Json.stream_parse_file` вызывают блок для уже разобранных
значений на выбранной глубине и возвращают количество переданных в блок значений.

```amber
sum = 0

count = Json.stream_parse("[1, 2, 3, 4]", depth: 1) |value|:
  sum = sum + value

sum + count
```

Глубина считается от корневого документа:

| Вход | `depth:` | Что получает блок |
| --- | --- | --- |
| `[1, 2, 3]` | `1` | `1`, потом `2`, потом `3` |
| `{"a": 1, "b": 2}` | `1` | значения полей `1` и `2` |
| `{"items": [{"id": 1}]}` | `2` | объект `{"id": 1}` внутри `items` |
| любой JSON | `0` | корневое значение целиком |

Для `Json.stream_parse(text)` default `depth` равен `1`.

```amber
seen = []

n = Json.stream_parse("[{\"id\":1}, {\"id\":2}]", depth: 1) |row|:
  seen.push!(row[:id])

n
```

Для `Json.stream_parse_file(path, jsonl: true)` default `depth` равен `0`, чтобы
блок получал каждую JSONL-строку как отдельное значение:

```amber
total = 0

count = Json.stream_parse_file("events.jsonl", jsonl: true) |row|:
  total = total + row[:value]

{count: count, total: total}.to_json
```

Для большого top-level массива используйте обычный JSON-режим и `depth: 1`:

```amber
checksum = 0

count = Json.stream_parse_file("events.json", depth: 1) |event|:
  checksum = checksum + event[:id]

checksum + count
```

Такой вариант держит в памяти только текущий элемент и runtime-структуры парсера,
а не весь массив целиком.

## Ранняя Остановка

Внутри streaming-блока можно вызвать `Json.stop`. Это не значение-сентинел, а
управляющий выход: парсер прекращает чтение, а `stream_parse` возвращает число
уже переданных значений.

```amber
sum = 0
limit = 100
seen_so_far = 0

seen = Json.stream_parse_file("events.jsonl", jsonl: true) |row|:
  seen_so_far = seen_so_far + 1
  sum = sum + row[:value]
  if seen_so_far == limit:
    Json.stop

seen
```

`Json.stop(value)` предназначен для path-fold и в streaming-блоке является
`ArgumentError`: streaming API всегда возвращает count. `Json.stop` без
аргументов работает в `Json.stream_parse`, `Json.stream_parse_file` и path-fold.
Вызов вне такого блока является runtime fault. Любая другая ошибка из блока не
проглатывается и выходит наружу.

## Производительность

Для небольших документов `Json.parse` и `Json.generate` обычно проще и достаточно
быстры. Для больших файлов:

- используйте `Json.stream_parse_file(..., jsonl: true)` для JSONL;
- используйте `Json.stream_parse_file(..., depth: 1)` для top-level массива;
- избегайте `Json.load_from_file(..., jsonl: true)`, если не нужен весь список в
  памяти;
- в build-mode горячие common flows `parse` / `generate` / `to_json` /
  JSONL-streaming имеют native coverage для benchmark-подобных путей.

Практический benchmark лежит в `bench/polyglot`:

```sh
python3 bench/polyglot/run_benchmark.py --workload json --repeats 5
```

## Краткая Таблица API

| API | Возвращает | Назначение |
| --- | --- | --- |
| `Json.parse(text)` | value | разобрать один JSON-документ |
| `Json.parse(text, map: StrictMap)` | `StrictMap` внутри объектов | разобрать без name-indifferent ключей |
| `Json.path(value, query)` | value / `null` | первый match по path |
| `Json.paths(value, query)` | `List` | все matches по path |
| `Json.paths(value, query, init) ...` | value | fold по matches |
| `Json.generate(value)` | `Str` | компактный JSON |
| `Json.pretty_generate(value, indent: 2)` | `Str` | форматированный JSON |
| `value.to_json` | `Str` | value-method форма `Json.generate`; для Map доступно `only:` / `except:` |
| `Json.load_from_file(path)` | value | прочитать и разобрать JSON-файл целиком |
| `Json.load_from_file(path, jsonl: true)` | `List` | прочитать JSONL целиком |
| `Json.save_to_file(path, value)` | `null` | записать JSON-файл |
| `Json.save_to_file(path, rows, jsonl: true)` | `null` | записать JSONL из `List` |
| `Json.stream_parse(text, depth: 1) ...` | `Int` | стримить JSON из строки |
| `Json.stream_parse_file(path, depth: 1) ...` | `Int` | стримить JSON из файла |
| `Json.stream_parse_file(path, jsonl: true) ...` | `Int` | стримить JSONL по строкам |
| `Json.stop` | не возвращает значение | остановить streaming из блока |

## Частые Ошибки

- В Amber-строке JSON нужно экранировать кавычки: `"{\"id\": 1}"`.
- Для JSONL всегда пишите `jsonl: true`; runtime не угадывает режим по
  расширению `.jsonl`.
- `load_from_file` читает файл целиком. Для логов и больших массивов берите
  `stream_parse_file`.
- `map: StrictMap` есть у `Json.parse`; file-load API в v1 создаёт обычные
  `Map`.
- В build executable не забывайте `--grant fs.read=...` и `--grant fs.write=...`
  для файлов, с которыми работает программа.
