#include <array>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

constexpr std::int64_t kLimit = 2147483647;
constexpr std::int64_t kRounds = 20000;

using Row = std::array<std::int64_t, 3>;
using Weights = std::array<std::int64_t, 3>;

std::int64_t wrap(std::int64_t value, std::int64_t limit) {
  while (value > limit) {
    value = value - limit;
  }
  return value;
}

std::int64_t lane_index(std::int64_t index, std::int64_t size) {
  return index - (index / size) * size;
}

std::int64_t mix(std::int64_t value, std::int64_t salt) {
  std::int64_t mixed = value + salt + 17;
  mixed = mixed * 13;
  return wrap(mixed, kLimit);
}

std::int64_t pick(const Weights &weights, std::int64_t index) {
  return weights[static_cast<std::size_t>(index)];
}

std::int64_t score_row(const Row &row, const Weights &weights,
                       std::int64_t bias) {
  const std::int64_t x = row[0];
  const std::int64_t y = row[1];
  const std::int64_t z = row[2];
  std::int64_t mixed = mix(x + pick(weights, 0), y + bias);
  if (mixed > z) {
    mixed = mixed - z;
  } else {
    mixed = z - mixed;
  }
  return mixed + pick(weights, 1) * y + pick(weights, 2);
}

std::int64_t fold_rows(const std::vector<Row> &rows, const Weights &weights,
                       std::int64_t rounds) {
  std::int64_t total = 0;
  std::int64_t i = 0;
  const std::int64_t size = static_cast<std::int64_t>(rows.size());
  while (i < rounds) {
    const Row &row = rows[static_cast<std::size_t>(lane_index(i, size))];
    total = wrap(total + score_row(row, weights, i), kLimit);
    i = i + 1;
  }
  return total;
}

std::int64_t count_large(const std::vector<std::int64_t> &values,
                         std::int64_t threshold) {
  std::int64_t total = 0;
  std::int64_t i = 0;
  const std::int64_t size = static_cast<std::int64_t>(values.size());
  while (i < size) {
    if (values[static_cast<std::size_t>(i)] > threshold) {
      total = total + 1;
    }
    i = i + 1;
  }
  return total;
}

std::int64_t sum_values(const std::vector<std::int64_t> &values) {
  std::int64_t total = 0;
  std::int64_t i = 0;
  const std::int64_t size = static_cast<std::int64_t>(values.size());
  while (i < size) {
    total = total + values[static_cast<std::size_t>(i)];
    i = i + 1;
  }
  return total;
}

std::int64_t main_workload() {
  const std::vector<Row> rows = {
      {3, 5, 8},          {13, 21, 34},       {55, 89, 144},
      {233, 377, 610},    {987, 1597, 2584},  {4181, 6765, 10946},
      {17711, 28657, 46368},                  {75025, 121393, 196418},
  };
  const Weights weights = {11, 17, 23};
  const std::vector<std::int64_t> derived = {
      score_row(rows[0], weights, weights[0]),
      score_row(rows[1], weights, weights[0]),
      score_row(rows[2], weights, weights[0]),
      score_row(rows[3], weights, weights[0]),
      score_row(rows[4], weights, weights[0]),
      score_row(rows[5], weights, weights[0]),
      score_row(rows[6], weights, weights[0]),
      score_row(rows[7], weights, weights[0]),
  };
  const std::int64_t selected_count = count_large(derived, 1000);
  const std::int64_t folded = sum_values(derived);
  return fold_rows(rows, weights, kRounds) + folded +
         selected_count;
}

} // namespace

int main() {
  std::cout << main_workload() << '\n';
  return 0;
}
