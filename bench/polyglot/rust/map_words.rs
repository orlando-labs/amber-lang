use std::collections::HashMap;

fn main_workload() -> i64 {
    let mut m: HashMap<String, i64> = HashMap::new();
    let mut i: i64 = 0;
    while i < 30000 {
        let key = format!("w{}", (i * i + i / 3) % 2000);
        if m.contains_key(&key) {
            *m.get_mut(&key).expect("key present") += 1;
        } else {
            m.insert(key, 1);
        }
        i += 1;
    }
    let mut checksum: i64 = 0;
    for (k, v) in &m {
        checksum += v * k.len() as i64;
    }
    let mut hits: i64 = 0;
    let mut j: i64 = 0;
    while j < 10000 {
        let probe = format!("w{}", (j * 7) % 3000);
        if m.contains_key(&probe) {
            hits += m[&probe];
        }
        j += 1;
    }
    checksum + hits + m.len() as i64
}

fn main() {
    println!("{}", main_workload());
}
