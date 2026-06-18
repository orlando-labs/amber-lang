# UUID

`Uuid` is Amber's immutable 128-bit UUID value type. `UUID` is an equivalent
prelude alias for schema and annotation spelling.

```amber
id = Uuid.v4()
ordered_id = Uuid.v7()
same = Uuid.parse(id.to_str)
```

API:

- `Uuid.v4()` creates an RFC 9562 version 4 UUID from `SecureRandom`.
- `Uuid.v7()` creates an RFC 9562 version 7 UUID using Unix milliseconds and
  `SecureRandom`.
- `Uuid.parse(text)` accepts canonical `8-4-4-4-12` hexadecimal text,
  case-insensitively.
- `uuid.to_str` and `uuid.inspect` return lowercase canonical text.
- `uuid.to_json` returns that canonical text as a JSON string.
- `uuid.version` returns the encoded version nibble.
- UUID values compare by their 16 bytes and match `Uuid === value` /
  `UUID === value`.
- `SecureRandom.uuid` is an alias for `Uuid.v4()`.

Effects and failures:

- `Uuid.v4()` requires the `random` effect and the `random.secure` capability.
- `Uuid.v7()` requires `random` and `time`, plus `random.secure`; replay mode
  rejects unrecorded wall-clock access.
- Invalid text raises `UuidParseError`; an out-of-range v7 timestamp raises
  `UuidError`.
- The VM and direct native backend cover the complete API above.
