// Hand-rolled digest implementations mirroring runtime/digest.cpp so the Rust
// row measures the same algorithms without external crates (Rust std ships no
// hash primitives).

const SHA256_K: [u32; 64] = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
];

const MD5_K: [u32; 64] = [
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
    0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
    0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
    0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
    0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
    0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
];

const MD5_SHIFTS: [u32; 64] = [
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4,
    11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6,
    10, 15, 21,
];

fn padded_message(bytes: &[u8], little_length: bool) -> Vec<u8> {
    let mut padded = bytes.to_vec();
    padded.push(0x80);
    while padded.len() % 64 != 56 {
        padded.push(0);
    }
    let bit_length = (bytes.len() as u64) * 8;
    for i in 0..8u32 {
        let shift = if little_length { i * 8 } else { (7 - i) * 8 };
        padded.push((bit_length >> shift) as u8);
    }
    padded
}

fn load_be32(bytes: &[u8]) -> u32 {
    ((bytes[0] as u32) << 24)
        | ((bytes[1] as u32) << 16)
        | ((bytes[2] as u32) << 8)
        | (bytes[3] as u32)
}

fn crc32(bytes: &[u8]) -> Vec<u8> {
    let mut crc: u32 = 0xffffffff;
    for &byte in bytes {
        crc ^= byte as u32;
        for _ in 0..8 {
            let mask = (crc & 1).wrapping_neg();
            crc = (crc >> 1) ^ (0xedb88320 & mask);
        }
    }
    (crc ^ 0xffffffff).to_be_bytes().to_vec()
}

fn md5(bytes: &[u8]) -> Vec<u8> {
    let mut hash: [u32; 4] = [0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476];
    let padded = padded_message(bytes, true);
    for chunk in padded.chunks_exact(64) {
        let mut words = [0u32; 16];
        for (i, word) in chunk.chunks_exact(4).enumerate() {
            words[i] = u32::from_le_bytes([word[0], word[1], word[2], word[3]]);
        }
        let (mut a, mut b, mut c, mut d) = (hash[0], hash[1], hash[2], hash[3]);
        for i in 0..64usize {
            let (f, word_index) = if i < 16 {
                ((b & c) | (!b & d), i)
            } else if i < 32 {
                ((d & b) | (!d & c), (5 * i + 1) % 16)
            } else if i < 48 {
                (b ^ c ^ d, (3 * i + 5) % 16)
            } else {
                (c ^ (b | !d), (7 * i) % 16)
            };
            let next = b.wrapping_add(
                a.wrapping_add(f)
                    .wrapping_add(MD5_K[i])
                    .wrapping_add(words[word_index])
                    .rotate_left(MD5_SHIFTS[i]),
            );
            a = d;
            d = c;
            c = b;
            b = next;
        }
        hash[0] = hash[0].wrapping_add(a);
        hash[1] = hash[1].wrapping_add(b);
        hash[2] = hash[2].wrapping_add(c);
        hash[3] = hash[3].wrapping_add(d);
    }
    hash.iter().flat_map(|word| word.to_le_bytes()).collect()
}

fn sha1(bytes: &[u8]) -> Vec<u8> {
    let mut hash: [u32; 5] = [
        0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0,
    ];
    let padded = padded_message(bytes, false);
    for chunk in padded.chunks_exact(64) {
        let mut words = [0u32; 80];
        for i in 0..16 {
            words[i] = load_be32(&chunk[i * 4..]);
        }
        for i in 16..80 {
            words[i] = (words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16])
                .rotate_left(1);
        }
        let (mut a, mut b, mut c, mut d, mut e) =
            (hash[0], hash[1], hash[2], hash[3], hash[4]);
        for i in 0..80usize {
            let (f, constant) = if i < 20 {
                ((b & c) | (!b & d), 0x5a827999u32)
            } else if i < 40 {
                (b ^ c ^ d, 0x6ed9eba1)
            } else if i < 60 {
                ((b & c) | (b & d) | (c & d), 0x8f1bbcdc)
            } else {
                (b ^ c ^ d, 0xca62c1d6)
            };
            let temp = a
                .rotate_left(5)
                .wrapping_add(f)
                .wrapping_add(e)
                .wrapping_add(constant)
                .wrapping_add(words[i]);
            e = d;
            d = c;
            c = b.rotate_left(30);
            b = a;
            a = temp;
        }
        hash[0] = hash[0].wrapping_add(a);
        hash[1] = hash[1].wrapping_add(b);
        hash[2] = hash[2].wrapping_add(c);
        hash[3] = hash[3].wrapping_add(d);
        hash[4] = hash[4].wrapping_add(e);
    }
    hash.iter().flat_map(|word| word.to_be_bytes()).collect()
}

fn sha256(bytes: &[u8]) -> Vec<u8> {
    let mut hash: [u32; 8] = [
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c,
        0x1f83d9ab, 0x5be0cd19,
    ];
    let padded = padded_message(bytes, false);
    for chunk in padded.chunks_exact(64) {
        let mut words = [0u32; 64];
        for i in 0..16 {
            words[i] = load_be32(&chunk[i * 4..]);
        }
        for i in 16..64 {
            let x = words[i - 15];
            let y = words[i - 2];
            let small0 = x.rotate_right(7) ^ x.rotate_right(18) ^ (x >> 3);
            let small1 = y.rotate_right(17) ^ y.rotate_right(19) ^ (y >> 10);
            words[i] = words[i - 16]
                .wrapping_add(small0)
                .wrapping_add(words[i - 7])
                .wrapping_add(small1);
        }
        let (mut a, mut b, mut c, mut d, mut e, mut f, mut g, mut h) = (
            hash[0], hash[1], hash[2], hash[3], hash[4], hash[5], hash[6],
            hash[7],
        );
        for i in 0..64usize {
            let big1 = e.rotate_right(6) ^ e.rotate_right(11) ^ e.rotate_right(25);
            let choice = (e & f) ^ (!e & g);
            let temp1 = h
                .wrapping_add(big1)
                .wrapping_add(choice)
                .wrapping_add(SHA256_K[i])
                .wrapping_add(words[i]);
            let big0 = a.rotate_right(2) ^ a.rotate_right(13) ^ a.rotate_right(22);
            let majority = (a & b) ^ (a & c) ^ (b & c);
            let temp2 = big0.wrapping_add(majority);
            h = g;
            g = f;
            f = e;
            e = d.wrapping_add(temp1);
            d = c;
            c = b;
            b = a;
            a = temp1.wrapping_add(temp2);
        }
        hash[0] = hash[0].wrapping_add(a);
        hash[1] = hash[1].wrapping_add(b);
        hash[2] = hash[2].wrapping_add(c);
        hash[3] = hash[3].wrapping_add(d);
        hash[4] = hash[4].wrapping_add(e);
        hash[5] = hash[5].wrapping_add(f);
        hash[6] = hash[6].wrapping_add(g);
        hash[7] = hash[7].wrapping_add(h);
    }
    hash.iter().flat_map(|word| word.to_be_bytes()).collect()
}

fn hmac_sha256(key: &[u8], bytes: &[u8]) -> Vec<u8> {
    let mut normalized_key = if key.len() > 64 {
        sha256(key)
    } else {
        key.to_vec()
    };
    normalized_key.resize(64, 0);
    let mut inner: Vec<u8> = normalized_key.iter().map(|b| b ^ 0x36).collect();
    inner.extend_from_slice(bytes);
    let inner_hash = sha256(&inner);
    let mut outer: Vec<u8> = normalized_key.iter().map(|b| b ^ 0x5c).collect();
    outer.extend_from_slice(&inner_hash);
    sha256(&outer)
}

fn fold_digest(checksum: u64, digest: &[u8]) -> u64 {
    checksum
        + digest.len() as u64
        + digest[0] as u64
        + digest[digest.len() - 1] as u64
}

fn main_workload() -> u64 {
    let payloads: [&[u8]; 4] = [
        b"Amber digest polyglot benchmark payload zero",
        b"Amber digest polyglot benchmark payload one 1234567890",
        b"The quick brown fox jumps over the lazy dog",
        b"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
    ];
    let key: &[u8] = b"amber-digest-benchmark-key";
    let mut checksum: u64 = 0;
    for i in 0..4000usize {
        let data = payloads[i % payloads.len()];
        checksum = fold_digest(checksum, &crc32(data));
        checksum = fold_digest(checksum, &md5(data));
        checksum = fold_digest(checksum, &sha1(data));
        checksum = fold_digest(checksum, &sha256(data));
        checksum = fold_digest(checksum, &hmac_sha256(key, data));
    }
    checksum
}

fn main() {
    println!("{}", main_workload());
}
