const ITERATIONS: i64 = 1_000_000;
const LIMIT: i64 = 2147483647;

fn main_workload() -> i64 {
    let mut i: i64 = 0;
    let mut a: i64 = 1;
    let mut b: i64 = 2;
    let mut checksum: i64 = 0;
    while i < ITERATIONS {
        a = a + b + 3;
        if a > LIMIT {
            a -= LIMIT;
        }
        b = b + a + i;
        if b > LIMIT {
            b -= LIMIT;
        }
        if a > b {
            checksum = checksum + a - b;
        } else {
            checksum = checksum + b - a;
        }
        i += 1;
    }
    checksum + a + b
}

fn main() {
    println!("{}", main_workload());
}
