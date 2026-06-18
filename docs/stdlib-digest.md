# Digest

`Digest` is Amber's dependency-free, one-shot hashing module. Every method
accepts explicit binary values (`Bytes`, `ByteSlice`, or `ByteBuffer`) and
returns raw `Bytes`; use `Hex.encode`, `Base64.encode`, or `bytes.hex()` for
text formatting.

```amber
data = Bytes.new("abc")

Digest.sha256(data).hex()
Digest.hmac_sha256(Bytes.new("key"), data).hex()
Base64.encode(Digest.streebog256(data))
```

## API

- `Digest.crc32(bytes) -> Bytes` — 4-byte CRC-32/ISO-HDLC value, big-endian.
- `Digest.md5(bytes) -> Bytes` — 16-byte MD5 digest.
- `Digest.sha1(bytes) -> Bytes` — 20-byte SHA-1 digest.
- `Digest.sha256(bytes) -> Bytes` — 32-byte SHA-256 digest.
- `Digest.hmac_sha256(key, bytes) -> Bytes` — 32-byte HMAC-SHA-256 tag.
- `Digest.streebog256(bytes) -> Bytes` — GOST R 34.11-2012, 256-bit.
- `Digest.streebog512(bytes) -> Bytes` — GOST R 34.11-2012, 512-bit.

Streebog aliases:

- Latin: `gost256`, `gost512`.
- Cyrillic: `гост256`, `гост512`, `стрибог256`, `стрибог512`.

`md5`, `sha1`, and `crc32` are compatibility/integrity algorithms, not suitable
for new cryptographic authentication. Prefer SHA-256, HMAC-SHA-256, or
Streebog where the relevant protocol requires it.
