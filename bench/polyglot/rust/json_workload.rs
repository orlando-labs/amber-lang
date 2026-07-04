use std::fs::File;
use std::io::{BufRead, BufReader};

const JSON_PATH: &str = "bench/polyglot/build/json/events.jsonl";

fn parse_int_at(text: &[u8], pos: &mut usize) -> i64 {
    while *pos < text.len() && text[*pos] != b'-' && !text[*pos].is_ascii_digit() {
        *pos += 1;
    }
    if *pos >= text.len() {
        panic!("expected integer");
    }
    let negative = if text[*pos] == b'-' {
        *pos += 1;
        true
    } else {
        false
    };
    let mut value: i64 = 0;
    while *pos < text.len() && text[*pos].is_ascii_digit() {
        value = value * 10 + (text[*pos] - b'0') as i64;
        *pos += 1;
    }
    if negative {
        -value
    } else {
        value
    }
}

fn find(text: &[u8], needle: &[u8], from: usize) -> Option<usize> {
    text[from..]
        .windows(needle.len())
        .position(|window| window == needle)
        .map(|offset| from + offset)
}

fn read_after(text: &[u8], needle: &[u8]) -> i64 {
    let mut pos = find(text, needle, 0).expect("missing JSON field") + needle.len();
    parse_int_at(text, &mut pos)
}

fn read_item(text: &[u8], index: usize) -> i64 {
    let mut pos = find(text, b"\"items\":[", 0).expect("missing JSON items") + 9;
    let mut value = 0;
    for i in 0..=index {
        value = parse_int_at(text, &mut pos);
        if i < index {
            pos = find(text, b",", pos).expect("missing JSON item separator") + 1;
        }
    }
    value
}

fn make_document(i: i64) -> String {
    format!(
        "{{\"id\":{},\"value\":{},\"group\":{},\"items\":[{},{},{}],\"name\":\"event\"}}",
        i,
        i * 2,
        i % 7,
        i,
        i + 1,
        i + 2
    )
}

fn main() {
    let mut checksum: i64 = 0;
    for i in 0..1000 {
        let compact = make_document(i);
        let bytes = compact.as_bytes();
        checksum += read_after(bytes, b"\"id\":");
        checksum += read_after(bytes, b"\"value\":");
        checksum += read_after(bytes, b"\"group\":");
        checksum += read_item(bytes, 1);
        if i < 20 {
            checksum += read_item(bytes, 2);
        }
    }

    let input = File::open(JSON_PATH).expect("failed to open events.jsonl");
    let reader = BufReader::new(input);
    let mut count: i64 = 0;
    for line in reader.lines() {
        let line = line.expect("failed to read line");
        let bytes = line.as_bytes();
        checksum += read_after(bytes, b"\"id\":") * 3;
        checksum += read_after(bytes, b"\"value\":");
        checksum -= read_after(bytes, b"\"group\":");
        count += 1;
    }

    println!("{}", checksum + count);
}
