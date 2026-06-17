# Base64, Base64Url и Hex в Amber

`Base64`, `Base64Url` и `Hex` - встроенные модули стандартной библиотеки для
кодирования байтов в текст и обратного декодирования текста в байты. Их не
нужно импортировать: имена доступны как prelude-константы в обычном Amber-коде.

Эти модули намеренно не называются `Encoding`: они работают с binary-to-text
кодеками, а будущий `Encoding` остаётся свободным для перекодирования текста.

Короткий пример:

```amber
payload = Bytes.new("hello")

standard = Base64.encode(payload)                  # "aGVsbG8="
compact = Base64.encode(payload, padding: false)   # "aGVsbG8"
url = Base64Url.encode(payload)                    # "aGVsbG8"
hex = Hex.encode(payload)                          # "68656c6c6f"

text = Base64.decode(standard).to_str()            # "hello"
same = Hex.decode(hex).to_str()                    # "hello"
```

Все `encode`-методы возвращают `Str`. Все `decode`-методы возвращают `Bytes`.
Вызывайте `.to_str()` только когда декодированные байты точно являются UTF-8
текстом. Для бинарных данных обычно лучше оставить `Bytes` как есть или
посмотреть на них через `.hex()`.

```amber
bytes = Base64Url.decode("-_8")
bytes.hex()                                        # "fbff"
```

## Быстрая Таблица

| Модуль | Encode | Decode | Формат по умолчанию |
| --- | --- | --- | --- |
| `Base64` | `Base64.encode(bytes, padding: true)` | `Base64.decode(text, mode: :strict)` | стандартный алфавит, с padding |
| `Base64Url` | `Base64Url.encode(bytes, padding: false)` | `Base64Url.decode(text, mode: :strict)` | URL-safe алфавит, без padding |
| `Hex` | `Hex.encode(bytes)` | `Hex.decode(text, mode: :strict)` | lowercase hex |

`encode` принимает только явные байтовые значения: `Bytes`, `ByteSlice` или
`ByteBuffer`. `Str` не преобразуется в байты неявно; если строку нужно
закодировать как UTF-8 байты, используйте `Bytes.new(text)`.

```amber
Base64.encode("hello")          # TypeError
Base64.encode(Bytes.new("hello"))
```

## Base64

`Base64` использует стандартный RFC 4648 алфавит:

```text
A-Z a-z 0-9 + /
```

API:

```amber
Base64.encode(bytes, padding: true) -> Str
Base64.decode(text, mode: :strict) -> Bytes
```

`padding:` должен быть `Bool`. По умолчанию `padding: true`, поэтому результат
канонически дополняется `=`.

```amber
bytes = Bytes.new("abcd")

padded = Base64.encode(bytes)                    # "YWJjZA=="
compact = Base64.encode(bytes, padding: false)   # "YWJjZA"

Base64.decode(padded).to_str()                   # "abcd"
Base64.decode(compact).to_str()                  # "abcd"
```

`Base64.decode` принимает padded и unpadded строки, если длина всё ещё возможна
для base64. В `:strict` режиме пробелы, переводы строк, неверный padding и
non-zero padding bits дают `CodecDecodeError`. В `:lenient` режиме декодер
сначала игнорирует ASCII whitespace.

```amber
folded = "Y WJj\nZA=="
Base64.decode(folded, mode: :lenient).to_str()   # "abcd"
```

## Base64Url

`Base64Url` использует URL-safe RFC 4648 алфавит:

```text
A-Z a-z 0-9 - _
```

API:

```amber
Base64Url.encode(bytes, padding: false) -> Str
Base64Url.decode(text, mode: :strict) -> Bytes
```

По умолчанию `Base64Url.encode` не добавляет `=`, потому что URL-токены, имена
файлов и компактные wire-форматы чаще используют unpadded форму.

```amber
token = Base64Url.encode(Hex.decode("fbff"))       # "-_8"
round = Base64Url.decode(token).hex()              # "fbff"

padded = Base64Url.encode(Bytes.new("ok"), padding: true)
Base64Url.decode(padded).to_str()                  # "ok"
```

`Base64Url.decode` не принимает стандартные `+` и `/`. Если вход пришёл в
обычном base64, декодируйте его через `Base64`, а не через `Base64Url`.

## Hex

`Hex` кодирует байты в lowercase hexadecimal text.

API:

```amber
Hex.encode(bytes) -> Str
Hex.decode(text, mode: :strict) -> Bytes
```

`Hex.decode` принимает цифры `0-9`, `a-f` и `A-F`. В `:strict` режиме вход
должен состоять только из hex-цифр, а количество цифр должно быть чётным.

```amber
bytes = Hex.decode("CAFE")
bytes.hex()                                        # "cafe"
```

В `:lenient` режиме декодер игнорирует ASCII whitespace, `:` и `-`, а также
принимает один ведущий `0x` или `0X` перед первой цифрой. После нормализации
количество hex-цифр всё равно должно быть чётным.

```amber
Hex.decode("0xCAFE")                               # CodecDecodeError
Hex.decode("0xCA-FE", mode: :lenient).hex()        # "cafe"
Hex.decode("68 65:6C-6c 6F", mode: :lenient).to_str()
                                                    # "hello"
```

## Ошибки

Некорректный decode завершается `CodecDecodeError`; имя входит в семейство
`CodecError` в runtime error registry. В текущей native stdlib реализации эти
ошибки валидации приходят как runtime fault, а не как recoverable
`try` / `rescue` exception.

Типичные ошибки:

| Ситуация | Ошибка |
| --- | --- |
| `encode` получил `Str` вместо `Bytes` / `ByteSlice` / `ByteBuffer` | `TypeError` |
| `padding:` не `Bool` | `TypeError` |
| `mode:` не `:strict`, `:lenient`, `"strict"` или `"lenient"` | `ArgumentError` |
| Base64 содержит неверные символы, плохой padding, невозможную длину или non-zero padding bits | `CodecDecodeError` |
| Strict Base64 содержит whitespace | `CodecDecodeError` |
| Base64Url содержит стандартные `+` или `/` | `CodecDecodeError` |
| Hex содержит неверные символы или нечётное число нормализованных цифр | `CodecDecodeError` |

## ByteBuffer И ByteSlice

Кодеки принимают `ByteBuffer` и `ByteSlice` напрямую. Для `ByteBuffer` кодируется
его активный диапазон байтов; для `ByteSlice` - только сам slice.

```amber
buf = io.ByteBuffer(3)
buf.put!(0)
buf.put!(127)
buf.put!(255)
buf.flip!()

Hex.encode(buf)                                    # "007fff"
Hex.encode(buf.slice(1, 2))                        # "7fff"
Base64Url.encode(buf)                              # "AH__"
```

## Что Выбрать

Используйте `Base64`, когда другая сторона ждёт обычный RFC 4648 base64:
алфавит `+` и `/`, чаще всего с padding.

Используйте `Base64Url` для URL path/query tokens, filename-safe идентификаторов,
JWT-like сегментов и compact wire values: алфавит `-` и `_`, обычно без padding.

Используйте `Hex` для логов, digest/checksum значений, fixture-файлов и отладки
бинарных данных, где читаемость важнее компактности.
