import base64
import binascii
import secrets
import uuid


ROUNDS = 2000


def pad_base64(text: str) -> str:
    return text + "=" * ((4 - len(text) % 4) % 4)


def main() -> int:
    checksum = 0
    for _ in range(ROUNDS):
        checksum += len(secrets.token_bytes(32))
        checksum += len(binascii.unhexlify(secrets.token_hex(16)))

        b64 = base64.b64encode(secrets.token_bytes(18)).decode()
        checksum += len(base64.b64decode(b64))

        compact = base64.b64encode(secrets.token_bytes(17)).decode().rstrip("=")
        checksum += len(base64.b64decode(pad_base64(compact)))

        url = base64.urlsafe_b64encode(secrets.token_bytes(18)).decode().rstrip("=")
        checksum += len(base64.urlsafe_b64decode(pad_base64(url)))

        token = str(uuid.uuid4())
        if len(token) == 36 and "-" in token:
            checksum += 37

        value = secrets.randbelow(900) + 100
        if 100 <= value <= 999:
            checksum += 10

    return checksum


if __name__ == "__main__":
    print(main())
