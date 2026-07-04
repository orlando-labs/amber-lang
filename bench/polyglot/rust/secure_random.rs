// OS entropy comes from a single /dev/urandom handle (Rust std exposes no
// direct getrandom); base64/hex codecs are hand-rolled like the C++ row.

use std::fs::File;
use std::io::Read;

const ROUNDS: i32 = 2000;
const BASE64: &[u8; 64] =
    b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
const BASE64_URL: &[u8; 64] =
    b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
const HEX: &[u8; 16] = b"0123456789abcdef";

struct Entropy {
    source: File,
}

impl Entropy {
    fn new() -> Entropy {
        Entropy {
            source: File::open("/dev/urandom").expect("failed to open /dev/urandom"),
        }
    }

    fn bytes(&mut self, count: usize) -> Vec<u8> {
        let mut out = vec![0u8; count];
        self.source
            .read_exact(&mut out)
            .expect("failed to read entropy");
        out
    }

    fn random_u64(&mut self) -> u64 {
        let bytes = self.bytes(8);
        let mut value: u64 = 0;
        for (i, &byte) in bytes.iter().enumerate() {
            value |= (byte as u64) << (i * 8);
        }
        value
    }

    fn random_int(&mut self, min: i64, max: i64) -> i64 {
        let count = (max - min) as u64 + 1;
        let threshold = (u64::MAX - count + 1) % count;
        loop {
            let sample = self.random_u64();
            if sample >= threshold {
                return min + (sample % count) as i64;
            }
        }
    }
}

fn hex_encode(bytes: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(bytes.len() * 2);
    for &byte in bytes {
        out.push(HEX[((byte >> 4) & 0x0f) as usize]);
        out.push(HEX[(byte & 0x0f) as usize]);
    }
    out
}

fn hex_value(c: u8) -> i32 {
    match c {
        b'0'..=b'9' => (c - b'0') as i32,
        b'a'..=b'f' => 10 + (c - b'a') as i32,
        b'A'..=b'F' => 10 + (c - b'A') as i32,
        _ => -1,
    }
}

fn hex_decode(text: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(text.len() / 2);
    let mut i = 0;
    while i + 1 < text.len() {
        out.push(((hex_value(text[i]) << 4) | hex_value(text[i + 1])) as u8);
        i += 2;
    }
    out
}

fn base64_encode(bytes: &[u8], url: bool, padding: bool) -> Vec<u8> {
    let alphabet = if url { BASE64_URL } else { BASE64 };
    let mut out = Vec::with_capacity(((bytes.len() + 2) / 3) * 4);
    let mut i = 0;
    while i < bytes.len() {
        let b0 = bytes[i] as u32;
        let have_b1 = i + 1 < bytes.len();
        let have_b2 = i + 2 < bytes.len();
        let b1 = if have_b1 { bytes[i + 1] as u32 } else { 0 };
        let b2 = if have_b2 { bytes[i + 2] as u32 } else { 0 };
        out.push(alphabet[((b0 >> 2) & 0x3f) as usize]);
        out.push(alphabet[(((b0 & 0x03) << 4) | ((b1 >> 4) & 0x0f)) as usize]);
        if have_b1 {
            out.push(alphabet[(((b1 & 0x0f) << 2) | ((b2 >> 6) & 0x03)) as usize]);
        } else if padding {
            out.push(b'=');
        }
        if have_b2 {
            out.push(alphabet[(b2 & 0x3f) as usize]);
        } else if padding {
            out.push(b'=');
        }
        i += 3;
    }
    out
}

fn base64_value(c: u8, url: bool) -> i32 {
    match c {
        b'A'..=b'Z' => (c - b'A') as i32,
        b'a'..=b'z' => 26 + (c - b'a') as i32,
        b'0'..=b'9' => 52 + (c - b'0') as i32,
        _ => {
            if url {
                match c {
                    b'-' => 62,
                    b'_' => 63,
                    _ => -1,
                }
            } else {
                match c {
                    b'+' => 62,
                    b'/' => 63,
                    _ => -1,
                }
            }
        }
    }
}

fn base64_decode(text: &[u8], url: bool) -> Vec<u8> {
    let mut normalized = text.to_vec();
    let residue = normalized.len() % 4;
    if residue != 0 {
        normalized.resize(normalized.len() + (4 - residue), b'=');
    }
    let mut out = Vec::with_capacity((normalized.len() / 4) * 3);
    let mut i = 0;
    while i < normalized.len() {
        let v0 = base64_value(normalized[i], url);
        let v1 = base64_value(normalized[i + 1], url);
        let c2 = normalized[i + 2];
        let c3 = normalized[i + 3];
        let v2 = if c2 == b'=' { 0 } else { base64_value(c2, url) };
        let v3 = if c3 == b'=' { 0 } else { base64_value(c3, url) };
        out.push(((v0 << 2) | (v1 >> 4)) as u8);
        if c2 != b'=' {
            out.push((((v1 & 0x0f) << 4) | (v2 >> 2)) as u8);
        }
        if c3 != b'=' {
            out.push((((v2 & 0x03) << 6) | v3) as u8);
        }
        i += 4;
    }
    out
}

fn uuid_v4(entropy: &mut Entropy) -> String {
    let mut bytes = entropy.bytes(16);
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    let hex = hex_encode(&bytes);
    let text = String::from_utf8(hex).expect("hex is ASCII");
    format!(
        "{}-{}-{}-{}-{}",
        &text[0..8],
        &text[8..12],
        &text[12..16],
        &text[16..20],
        &text[20..32]
    )
}

fn main() {
    let mut entropy = Entropy::new();
    let mut checksum: i64 = 0;
    for _ in 0..ROUNDS {
        checksum += entropy.bytes(32).len() as i64;
        checksum += hex_decode(&hex_encode(&entropy.bytes(16))).len() as i64;
        checksum += base64_decode(
            &base64_encode(&entropy.bytes(18), false, true),
            false,
        )
        .len() as i64;
        checksum += base64_decode(
            &base64_encode(&entropy.bytes(17), false, false),
            false,
        )
        .len() as i64;
        checksum +=
            base64_decode(&base64_encode(&entropy.bytes(18), true, false), true)
                .len() as i64;

        let id = uuid_v4(&mut entropy);
        if id.len() == 36 && id.contains('-') {
            checksum += 37;
        }

        let value = entropy.random_int(100, 999);
        if (100..=999).contains(&value) {
            checksum += 10;
        }
    }
    println!("{}", checksum);
}
