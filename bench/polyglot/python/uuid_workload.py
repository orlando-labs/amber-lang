#!/usr/bin/env python3
from __future__ import annotations

import secrets
import time
import uuid


ROUNDS = 5_000
FIXED = "550e8400-e29b-41d4-a716-446655440000"


def uuid_v4() -> uuid.UUID:
    return uuid.UUID(bytes=secrets.token_bytes(16), version=4)


def uuid_v7() -> uuid.UUID:
    milliseconds = time.time_ns() // 1_000_000
    raw = bytearray(secrets.token_bytes(16))
    for index in range(6):
        raw[index] = (milliseconds >> ((5 - index) * 8)) & 0xFF
    raw[6] = (raw[6] & 0x0F) | 0x70
    raw[8] = (raw[8] & 0x3F) | 0x80
    return uuid.UUID(bytes=bytes(raw))


def main() -> int:
    checksum = 0
    for _ in range(ROUNDS):
        v4 = uuid_v4()
        v4_text = str(v4)
        parsed_v4 = uuid.UUID(v4_text)

        v7 = uuid_v7()
        v7_text = str(v7)
        parsed_v7 = uuid.UUID(v7_text)

        delegated = uuid_v4()
        delegated_json = f'"{delegated}"'

        fixed = uuid.UUID(FIXED.upper())
        fixed_text = str(fixed)

        checksum += v4.version
        checksum += v7.version
        checksum += delegated.version
        checksum += len(v4_text)
        checksum += len(v7_text)
        checksum += len(delegated_json)

        if v4 == parsed_v4:
            checksum += 11
        if v7 == parsed_v7:
            checksum += 13
        if isinstance(v4, uuid.UUID) and isinstance(v7, uuid.UUID):
            checksum += 17
        if fixed.version == 4 and fixed_text == FIXED:
            checksum += 19
        if "-" in v4_text and "-" in v7_text:
            checksum += 23

    return checksum


if __name__ == "__main__":
    print(main())
