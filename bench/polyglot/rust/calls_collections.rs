const LIMIT: i64 = 2147483647;
const ROUNDS: i64 = 20000;

fn wrap(mut value: i64, limit: i64) -> i64 {
    while value > limit {
        value -= limit;
    }
    value
}

fn lane_index(index: i64, size: i64) -> i64 {
    index - (index / size) * size
}

fn mix(value: i64, salt: i64) -> i64 {
    let mut mixed = value + salt + 17;
    mixed *= 13;
    wrap(mixed, LIMIT)
}

fn pick(weights: &[i64; 3], index: usize) -> i64 {
    weights[index]
}

fn score_row(row: &[i64; 3], weights: &[i64; 3], bias: i64) -> i64 {
    let x = row[0];
    let y = row[1];
    let z = row[2];
    let mut mixed = mix(x + pick(weights, 0), y + bias);
    if mixed > z {
        mixed -= z;
    } else {
        mixed = z - mixed;
    }
    mixed + pick(weights, 1) * y + pick(weights, 2)
}

fn fold_rows(rows: &[[i64; 3]], weights: &[i64; 3], rounds: i64) -> i64 {
    let mut total: i64 = 0;
    let mut i: i64 = 0;
    let size = rows.len() as i64;
    while i < rounds {
        let row = &rows[lane_index(i, size) as usize];
        total = wrap(total + score_row(row, weights, i), LIMIT);
        i += 1;
    }
    total
}

fn count_large(values: &[i64], threshold: i64) -> i64 {
    let mut total: i64 = 0;
    for &value in values {
        if value > threshold {
            total += 1;
        }
    }
    total
}

fn sum_values(values: &[i64]) -> i64 {
    let mut total: i64 = 0;
    for &value in values {
        total += value;
    }
    total
}

fn main_workload() -> i64 {
    let rows: Vec<[i64; 3]> = vec![
        [3, 5, 8],
        [13, 21, 34],
        [55, 89, 144],
        [233, 377, 610],
        [987, 1597, 2584],
        [4181, 6765, 10946],
        [17711, 28657, 46368],
        [75025, 121393, 196418],
    ];
    let weights: [i64; 3] = [11, 17, 23];
    let derived: Vec<i64> = (0..8)
        .map(|i| score_row(&rows[i], &weights, weights[0]))
        .collect();
    let selected_count = count_large(&derived, 1000);
    let folded = sum_values(&derived);
    fold_rows(&rows, &weights, ROUNDS) + folded + selected_count
}

fn main() {
    println!("{}", main_workload());
}
