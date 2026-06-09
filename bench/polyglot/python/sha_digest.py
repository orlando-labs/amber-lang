MASK32 = 0xFFFFFFFF
ROUNDS = 600

K = [
    0x428A2F98, 0x71374491, 0xB5C0FBCF, 0xE9B5DBA5,
    0x3956C25B, 0x59F111F1, 0x923F82A4, 0xAB1C5ED5,
    0xD807AA98, 0x12835B01, 0x243185BE, 0x550C7DC3,
    0x72BE5D74, 0x80DEB1FE, 0x9BDC06A7, 0xC19BF174,
    0xE49B69C1, 0xEFBE4786, 0x0FC19DC6, 0x240CA1CC,
    0x2DE92C6F, 0x4A7484AA, 0x5CB0A9DC, 0x76F988DA,
    0x983E5152, 0xA831C66D, 0xB00327C8, 0xBF597FC7,
    0xC6E00BF3, 0xD5A79147, 0x06CA6351, 0x14292967,
    0x27B70A85, 0x2E1B2138, 0x4D2C6DFC, 0x53380D13,
    0x650A7354, 0x766A0ABB, 0x81C2C92E, 0x92722C85,
    0xA2BFE8A1, 0xA81A664B, 0xC24B8B70, 0xC76C51A3,
    0xD192E819, 0xD6990624, 0xF40E3585, 0x106AA070,
    0x19A4C116, 0x1E376C08, 0x2748774C, 0x34B0BCB5,
    0x391C0CB3, 0x4ED8AA4A, 0x5B9CCA4F, 0x682E6FF3,
    0x748F82EE, 0x78A5636F, 0x84C87814, 0x8CC70208,
    0x90BEFFFA, 0xA4506CEB, 0xBEF9A3F7, 0xC67178F2,
]


def u32(value: int) -> int:
    return value & MASK32


def rotr(value: int, count: int) -> int:
    return u32((value >> count) | (value << (32 - count)))


def ch(x: int, y: int, z: int) -> int:
    return (x & y) ^ ((x ^ MASK32) & z)


def maj(x: int, y: int, z: int) -> int:
    return (x & y) ^ (x & z) ^ (y & z)


def big0(x: int) -> int:
    return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22)


def big1(x: int) -> int:
    return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25)


def small0(x: int) -> int:
    return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3)


def small1(x: int) -> int:
    return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10)


def compress(digest: list[int], block: list[int], constants: list[int]) -> None:
    w = [0] * 64
    i = 0
    while i < 16:
        w[i] = block[i]
        i += 1
    while i < 64:
        w[i] = u32(w[i - 16] + small0(w[i - 15]) + w[i - 7] + small1(w[i - 2]))
        i += 1

    a, b, c, d, e, f, g, h = digest
    i = 0
    while i < 64:
        t1 = u32(h + big1(e) + ch(e, f, g) + constants[i] + w[i])
        t2 = u32(big0(a) + maj(a, b, c))
        h = g
        g = f
        f = e
        e = u32(d + t1)
        d = c
        c = b
        b = a
        a = u32(t1 + t2)
        i += 1

    values = [a, b, c, d, e, f, g, h]
    i = 0
    while i < 8:
        digest[i] = u32(digest[i] + values[i])
        i += 1


def mix_block(block: list[int], round_index: int) -> None:
    carry = u32((round_index + 1) * 0x9E3779B1)
    i = 0
    while i < 16:
        left = block[(i + 1) % 16]
        right = block[(i + 9) % 16]
        block[i] = u32(block[i] + carry + (left ^ right) + ((i + 17) * (round_index + 3)))
        carry = rotr(carry ^ block[i], (i % 13) + 1)
        i += 1


def fold_digest(digest: list[int]) -> int:
    folded = 0
    i = 0
    while i < 8:
        folded = u32((folded ^ digest[i]) + ((i + 1) * 0x9E3779B1))
        i += 1
    return folded


def main() -> int:
    digest = [
        0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A,
        0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19,
    ]
    block = [
        0x416D6265, 0x72205348, 0x41206469, 0x67657374,
        0x20706F6C, 0x79676C6F, 0x74206265, 0x6E636820,
        0x76310000, 0, 0, 0, 0, 0, 0, 0x00000120,
    ]
    round_index = 0
    while round_index < ROUNDS:
        compress(digest, block, K)
        mix_block(block, round_index)
        round_index += 1
    return fold_digest(digest)


if __name__ == "__main__":
    print(main())
