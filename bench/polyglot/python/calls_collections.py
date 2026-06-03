LIMIT = 2_147_483_647
ROUNDS = 20_000


def wrap(value: int, limit: int) -> int:
    while value > limit:
        value = value - limit
    return value


def lane_index(index: int, size: int) -> int:
    return index - (index // size) * size


def mix(value: int, salt: int) -> int:
    mixed = value + salt + 17
    mixed = mixed * 13
    return wrap(mixed, LIMIT)


def pick(weights: list[int], index: int) -> int:
    return weights[index]


def score_row(row: list[int], weights: list[int], bias: int) -> int:
    x = row[0]
    y = row[1]
    z = row[2]
    mixed = mix(x + pick(weights, 0), y + bias)
    if mixed > z:
        mixed = mixed - z
    else:
        mixed = z - mixed
    return mixed + pick(weights, 1) * y + pick(weights, 2)


def fold_rows(rows: list[list[int]], weights: list[int], rounds: int) -> int:
    total = 0
    i = 0
    size = len(rows)
    while i < rounds:
        row = rows[lane_index(i, size)]
        total = wrap(total + score_row(row, weights, i), LIMIT)
        i = i + 1
    return total


def count_large(values: list[int], threshold: int) -> int:
    total = 0
    i = 0
    size = len(values)
    while i < size:
        if values[i] > threshold:
            total = total + 1
        i = i + 1
    return total


def sum_values(values: list[int]) -> int:
    total = 0
    i = 0
    size = len(values)
    while i < size:
        total = total + values[i]
        i = i + 1
    return total


def main() -> int:
    rows = [
        [3, 5, 8],
        [13, 21, 34],
        [55, 89, 144],
        [233, 377, 610],
        [987, 1597, 2584],
        [4181, 6765, 10946],
        [17711, 28657, 46368],
        [75025, 121393, 196418],
    ]
    weights = [11, 17, 23]
    derived = [
        score_row(rows[0], weights, weights[0]),
        score_row(rows[1], weights, weights[0]),
        score_row(rows[2], weights, weights[0]),
        score_row(rows[3], weights, weights[0]),
        score_row(rows[4], weights, weights[0]),
        score_row(rows[5], weights, weights[0]),
        score_row(rows[6], weights, weights[0]),
        score_row(rows[7], weights, weights[0]),
    ]
    selected_count = count_large(derived, 1000)
    folded = sum_values(derived)
    return fold_rows(rows, weights, ROUNDS) + folded + selected_count


if __name__ == "__main__":
    print(main())
