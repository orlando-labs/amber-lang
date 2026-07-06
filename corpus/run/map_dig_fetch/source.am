def probe():
  m = {a: {b: [10, {c: 20}]}}
  ok_dig = m.dig(:a, :b, 0) == 10 and m.dig(:a, :b, 1, :c) == 20 and m.dig(:a, :missing) == null and m.dig(:a, :b, 9) == null and m.dig(:a, :b, 0, :x) == null
  ok_fetch = m.fetch(:a) == {b: [10, {c: 20}]} and {x: 1}.fetch(:y, 99) == 99 and {x: 1}.fetch(:x, 99) == 1
  if ok_dig and ok_fetch:
    42
  else:
    0
