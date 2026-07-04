fn main_workload() -> i64 {
    let mut checksum: i64 = 0;
    for i in 0..4000i64 {
        let token = format!("user-{}-record", i);
        let upper = token.to_ascii_uppercase();
        let lower = upper.to_ascii_lowercase();
        let line = format!("{}|{}|segment-{}", lower, token, i % 97);
        let parts: Vec<&str> = line.split('|').collect();
        let rebuilt = format!("{};{};{}", parts[0], parts[1], parts[2]);
        let replaced = rebuilt.replace("user-", "member-");
        let padded = format!("  {}  ", replaced);
        let trimmed = padded.trim();
        checksum += trimmed.len() as i64 + parts.len() as i64;
        if trimmed.contains("member-") {
            checksum += 3;
        }
        if trimmed.starts_with("member-") {
            checksum += 7;
        }
        if trimmed.ends_with('7') {
            checksum += 11;
        }
        if i % 5 == 0 {
            checksum += line.replace('e', "E").len() as i64;
        }
    }
    checksum
}

fn main() {
    println!("{}", main_workload());
}
