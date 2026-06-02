N = 1_000_000


def main() -> int:
    i = 0
    a = 1
    b = 2
    checksum = 0
    while i < N:
        a = a + b + 3
        if a > 2_147_483_647:
            a = a - 2_147_483_647
        b = b + a + i
        if b > 2_147_483_647:
            b = b - 2_147_483_647
        if a > b:
            checksum = checksum + a - b
        else:
            checksum = checksum + b - a
        i = i + 1
    return checksum + a + b


if __name__ == "__main__":
    print(main())
