import hashlib
import hmac
import zlib


PAYLOADS = [
    b"Amber digest polyglot benchmark payload zero",
    b"Amber digest polyglot benchmark payload one 1234567890",
    b"The quick brown fox jumps over the lazy dog",
    b"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
]
KEY = b"amber-digest-benchmark-key"


def fold_digest(checksum: int, digest: bytes) -> int:
    return checksum + len(digest) + digest[0] + digest[-1]


def main() -> int:
    checksum = 0
    for i in range(4000):
        data = PAYLOADS[i % len(PAYLOADS)]
        checksum = fold_digest(checksum, zlib.crc32(data).to_bytes(4, "big"))
        checksum = fold_digest(checksum, hashlib.md5(data).digest())
        checksum = fold_digest(checksum, hashlib.sha1(data).digest())
        checksum = fold_digest(checksum, hashlib.sha256(data).digest())
        checksum = fold_digest(checksum, hmac.digest(KEY, data, "sha256"))
    return checksum


if __name__ == "__main__":
    print(main())
