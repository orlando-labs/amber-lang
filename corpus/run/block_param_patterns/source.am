def probe():
  pairs = [(1, 2), (3, 4)].map |(a, b)|: a * b
  ok_tuple = pairs == [2, 12]
  lists = [[1, 2]].map |[a, b]|: a + b
  ok_list = lists == [3]
  nested = [[1, [2, 3]]].map |(x, (a, b))|: x + a + b
  ok_nested = nested == [6]
  f = |(a, b)|: b
  ok_lambda = f.((7, 8)) == 8
  kv = {k: [1, 2]}.map |k, (a, b)|: b
  ok_kv = kv == [2]
  rescued = false
  try:
    [5].map |x, y|: x
  rescue ArgumentError:
    rescued = true
  extra_ok = {a: 1}.each |k|: k
  if ok_tuple and ok_list and ok_nested and ok_lambda and ok_kv and rescued and extra_ok == {a: 1}:
    42
  else:
    0
