package bench.polyglot.calls_collections

export main

def wrap(value, limit):
  while value > limit:
    value = value - limit
  value

def lane_index(index, size):
  index - (index / size) * size

def mix(value, salt):
  mixed = value + salt + 17
  mixed = mixed * 13
  wrap(mixed, 2147483647)

def pick(weights, index):
  weights[index]

def score_row(row, weights, bias):
  x = row[0]
  y = row[1]
  z = row[2]
  mixed = mix(x + pick(weights, 0), y + bias)
  if mixed > z:
    mixed = mixed - z
  else:
    mixed = z - mixed
  mixed + pick(weights, 1) * y + pick(weights, 2)

def fold_rows(rows, weights, rounds):
  total = 0
  i = 0
  size = rows.count()
  while i < rounds:
    row = rows[lane_index(i, size)]
    total = wrap(total + score_row(row, weights, i), 2147483647)
    i = i + 1
  total

def count_large(values, threshold):
  total = 0
  i = 0
  size = values.count()
  while i < size:
    if values[i] > threshold:
      total = total + 1
    i = i + 1
  total

def sum_values(values):
  total = 0
  i = 0
  size = values.count()
  while i < size:
    total = total + values[i]
    i = i + 1
  total

def main():
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
    score_row(rows[0], weights, weights.first()),
    score_row(rows[1], weights, weights.first()),
    score_row(rows[2], weights, weights.first()),
    score_row(rows[3], weights, weights.first()),
    score_row(rows[4], weights, weights.first()),
    score_row(rows[5], weights, weights.first()),
    score_row(rows[6], weights, weights.first()),
    score_row(rows[7], weights, weights.first()),
  ]
  selected_count = count_large(derived, 1000)
  folded = sum_values(derived)
  fold_rows(rows, weights, 20000) + folded + selected_count

main()
