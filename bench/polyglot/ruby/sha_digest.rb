MASK32 = 0xffffffff
ROUNDS = 600

K = [
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
  0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
  0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
  0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
  0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
  0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
  0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
  0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
  0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
]

def u32(value)
  value & MASK32
end

def rotr(value, count)
  u32((value >> count) | (value << (32 - count)))
end

def ch(x, y, z)
  (x & y) ^ ((x ^ MASK32) & z)
end

def maj(x, y, z)
  (x & y) ^ (x & z) ^ (y & z)
end

def big0(x)
  rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22)
end

def big1(x)
  rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25)
end

def small0(x)
  rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3)
end

def small1(x)
  rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10)
end

def compress(digest, block, constants)
  w = Array.new(64, 0)
  i = 0
  while i < 16
    w[i] = block[i]
    i += 1
  end
  while i < 64
    w[i] = u32(w[i - 16] + small0(w[i - 15]) + w[i - 7] + small1(w[i - 2]))
    i += 1
  end

  a = digest[0]
  b = digest[1]
  c = digest[2]
  d = digest[3]
  e = digest[4]
  f = digest[5]
  g = digest[6]
  h = digest[7]

  i = 0
  while i < 64
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
  end

  values = [a, b, c, d, e, f, g, h]
  i = 0
  while i < 8
    digest[i] = u32(digest[i] + values[i])
    i += 1
  end
end

def mix_block(block, round_index)
  carry = u32((round_index + 1) * 0x9e3779b1)
  i = 0
  while i < 16
    left = block[(i + 1) % 16]
    right = block[(i + 9) % 16]
    block[i] = u32(block[i] + carry + (left ^ right) + ((i + 17) * (round_index + 3)))
    carry = rotr(carry ^ block[i], (i % 13) + 1)
    i += 1
  end
end

def fold_digest(digest)
  folded = 0
  i = 0
  while i < 8
    folded = u32((folded ^ digest[i]) + ((i + 1) * 0x9e3779b1))
    i += 1
  end
  folded
end

def main
  digest = [
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
  ]
  block = [
    0x416d6265, 0x72205348, 0x41206469, 0x67657374,
    0x20706f6c, 0x79676c6f, 0x74206265, 0x6e636820,
    0x76310000, 0, 0, 0, 0, 0, 0, 0x00000120,
  ]
  round_index = 0
  while round_index < ROUNDS
    compress(digest, block, K)
    mix_block(block, round_index)
    round_index += 1
  end
  fold_digest(digest)
end

puts main
