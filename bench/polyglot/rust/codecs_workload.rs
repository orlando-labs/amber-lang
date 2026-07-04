const BASE64: &[u8; 64] =
    b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
const BASE64_URL: &[u8; 64] =
    b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
const HEX: &[u8; 16] = b"0123456789abcdef";

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

fn b64_encode(bytes: &[u8], url: bool, padding: bool) -> Vec<u8> {
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

fn b64_value(c: u8, url: bool) -> i32 {
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

fn b64_decode(text: &[u8], url: bool, lenient: bool) -> Vec<u8> {
    let mut normalized = Vec::with_capacity(text.len() + 4);
    for &c in text {
        if lenient && (c == b' ' || c == b'\n' || c == b'\r' || c == b'\t') {
            continue;
        }
        normalized.push(c);
    }
    let residue = normalized.len() % 4;
    if residue != 0 {
        normalized.resize(normalized.len() + (4 - residue), b'=');
    }
    let mut out = Vec::with_capacity((normalized.len() / 4) * 3);
    let mut i = 0;
    while i < normalized.len() {
        let v0 = b64_value(normalized[i], url);
        let v1 = b64_value(normalized[i + 1], url);
        let c2 = normalized[i + 2];
        let c3 = normalized[i + 3];
        let v2 = if c2 == b'=' { 0 } else { b64_value(c2, url) };
        let v3 = if c3 == b'=' { 0 } else { b64_value(c3, url) };
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

fn main() {
    let mut checksum: i64 = 0;
    for i in 0..5000i64 {
        let raw_text = format!("event-{}:{}", i, (i * 17) % 100000);
        let raw = raw_text.as_bytes();

        let b64 = b64_encode(raw, false, true);
        let roundtrip = b64_decode(&b64, false, false);
        checksum += roundtrip.len() as i64;
        checksum += roundtrip[0] as i64 + roundtrip[roundtrip.len() - 1] as i64;

        let hx = hex_encode(raw);
        let url = b64_encode(&hex_decode(&hx), true, false);
        let back = b64_decode(&url, true, false);
        checksum += back.len() as i64;
        checksum += back[1] as i64;

        let decoded = hex_decode(&hex_encode(&back));
        checksum += decoded[2] as i64;

        if i % 16 == 0 {
            let mut folded_input = b64.clone();
            folded_input.push(b'\n');
            checksum += b64_decode(&folded_input, false, true).len() as i64;
        }
        if i % 31 == 0 {
            checksum +=
                b64_decode(&b64_encode(raw, true, true), true, false).len() as i64;
        }
        if i % 17 == 0 {
            checksum += hex_decode(&hx)[0] as i64;
        }
    }
    println!("{}", checksum);
}
