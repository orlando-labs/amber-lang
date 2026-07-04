use std::fs::File;
use std::io::Read;
use std::time::{SystemTime, UNIX_EPOCH};

const ROUNDS: i32 = 5000;
const HEX: &[u8; 16] = b"0123456789abcdef";
const FIXED: &str = "550e8400-e29b-41d4-a716-446655440000";

#[derive(PartialEq, Clone, Copy)]
struct Uuid {
    bytes: [u8; 16],
}

struct Entropy {
    source: File,
}

impl Entropy {
    fn new() -> Entropy {
        Entropy {
            source: File::open("/dev/urandom").expect("failed to open /dev/urandom"),
        }
    }

    fn fill(&mut self, out: &mut [u8]) {
        self.source.read_exact(out).expect("failed to read entropy");
    }
}

fn hex_value(c: u8) -> i32 {
    match c {
        b'0'..=b'9' => (c - b'0') as i32,
        b'a'..=b'f' => 10 + (c - b'a') as i32,
        b'A'..=b'F' => 10 + (c - b'A') as i32,
        _ => -1,
    }
}

impl Uuid {
    fn v4(entropy: &mut Entropy) -> Uuid {
        let mut out = Uuid { bytes: [0; 16] };
        entropy.fill(&mut out.bytes);
        out.bytes[6] = (out.bytes[6] & 0x0f) | 0x40;
        out.bytes[8] = (out.bytes[8] & 0x3f) | 0x80;
        out
    }

    fn v7(entropy: &mut Entropy) -> Uuid {
        let mut out = Uuid { bytes: [0; 16] };
        entropy.fill(&mut out.bytes);
        let milliseconds = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("wall clock before epoch")
            .as_millis() as u64;
        for index in 0..6 {
            out.bytes[index] = (milliseconds >> ((5 - index) * 8)) as u8;
        }
        out.bytes[6] = (out.bytes[6] & 0x0f) | 0x70;
        out.bytes[8] = (out.bytes[8] & 0x3f) | 0x80;
        out
    }

    fn parse(text: &str) -> Uuid {
        let bytes = text.as_bytes();
        if bytes.len() != 36
            || bytes[8] != b'-'
            || bytes[13] != b'-'
            || bytes[18] != b'-'
            || bytes[23] != b'-'
        {
            panic!("invalid UUID");
        }
        let mut out = Uuid { bytes: [0; 16] };
        let mut byte_index = 0;
        let mut index = 0;
        while index < bytes.len() {
            if bytes[index] == b'-' {
                index += 1;
                continue;
            }
            let high = hex_value(bytes[index]);
            let low = hex_value(bytes[index + 1]);
            if high < 0 || low < 0 || byte_index >= out.bytes.len() {
                panic!("invalid UUID");
            }
            out.bytes[byte_index] = ((high << 4) | low) as u8;
            byte_index += 1;
            index += 2;
        }
        if byte_index != out.bytes.len() {
            panic!("invalid UUID");
        }
        out
    }

    fn to_text(&self) -> String {
        let mut out = String::with_capacity(36);
        for (index, &byte) in self.bytes.iter().enumerate() {
            if index == 4 || index == 6 || index == 8 || index == 10 {
                out.push('-');
            }
            out.push(HEX[((byte >> 4) & 0x0f) as usize] as char);
            out.push(HEX[(byte & 0x0f) as usize] as char);
        }
        out
    }

    fn inspect(&self) -> String {
        self.to_text()
    }

    fn to_json(&self) -> String {
        format!("\"{}\"", self.to_text())
    }

    fn version(&self) -> i64 {
        ((self.bytes[6] >> 4) & 0x0f) as i64
    }
}

fn main() {
    let mut entropy = Entropy::new();
    let mut checksum: i64 = 0;
    for _ in 0..ROUNDS {
        let v4 = Uuid::v4(&mut entropy);
        let v4_text = v4.to_text();
        let parsed_v4 = Uuid::parse(&v4_text);

        let v7 = Uuid::v7(&mut entropy);
        let v7_text = v7.inspect();
        let parsed_v7 = Uuid::parse(&v7_text);

        let delegated = Uuid::v4(&mut entropy);
        let delegated_json = delegated.to_json();

        let fixed = Uuid::parse("550E8400-E29B-41D4-A716-446655440000");
        let fixed_text = fixed.to_text();

        checksum += v4.version();
        checksum += v7.version();
        checksum += delegated.version();
        checksum += v4_text.len() as i64;
        checksum += v7_text.len() as i64;
        checksum += delegated_json.len() as i64;

        if v4 == parsed_v4 {
            checksum += 11;
        }
        if v7 == parsed_v7 {
            checksum += 13;
        }
        checksum += 17;
        if fixed.version() == 4 && fixed_text == FIXED {
            checksum += 19;
        }
        if v4_text.contains('-') && v7_text.contains('-') {
            checksum += 23;
        }
    }
    println!("{}", checksum);
}
