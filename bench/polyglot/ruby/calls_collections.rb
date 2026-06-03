LIMIT = 2_147_483_647
ROUNDS = 20_000

def wrap(value, limit)
  while value > limit
    value = value - limit
  end
  value
end

def lane_index(index, size)
  index - (index / size) * size
end

def mix(value, salt)
  mixed = value + salt + 17
  mixed = mixed * 13
  wrap(mixed, LIMIT)
end

def pick(weights, index)
  weights[index]
end

def score_row(row, weights, bias)
  x = row[0]
  y = row[1]
  z = row[2]
  mixed = mix(x + pick(weights, 0), y + bias)
  if mixed > z
    mixed = mixed - z
  else
    mixed = z - mixed
  end
  mixed + pick(weights, 1) * y + pick(weights, 2)
end

def fold_rows(rows, weights, rounds)
  total = 0
  i = 0
  size = rows.length
  while i < rounds
    row = rows[lane_index(i, size)]
    total = wrap(total + score_row(row, weights, i), LIMIT)
    i = i + 1
  end
  total
end

def count_large(values, threshold)
  total = 0
  i = 0
  size = values.length
  while i < size
    if values[i] > threshold
      total = total + 1
    end
    i = i + 1
  end
  total
end

def sum_values(values)
  total = 0
  i = 0
  size = values.length
  while i < size
    total = total + values[i]
    i = i + 1
  end
  total
end

def main
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
    score_row(rows[0], weights, weights.first),
    score_row(rows[1], weights, weights.first),
    score_row(rows[2], weights, weights.first),
    score_row(rows[3], weights, weights.first),
    score_row(rows[4], weights, weights.first),
    score_row(rows[5], weights, weights.first),
    score_row(rows[6], weights, weights.first),
    score_row(rows[7], weights, weights.first),
  ]
  selected_count = count_large(derived, 1000)
  folded = sum_values(derived)
  fold_rows(rows, weights, ROUNDS) + folded + selected_count
end

puts main
