def probe():
  ok_flattened = [[1, [2]], 3].flattened() == [1, 2, 3] and [[1, [2]], 3].flattened(1) == [1, [2], 3]
  a = [1, [2, [3]]]
  a.flatten!()
  ok_flatten_bang = a == [1, 2, 3]
  ok_compact = [1, null, 2].compact() == [1, 2]
  b = [1, null, 2]
  b.compact!()
  ok_compact_bang = b == [1, 2]
  xs = [1, 2, 3]
  ok_last = xs.last == 3 and xs.last() == 3 and xs.last(2) == [2, 3] and [].last == null and xs.last(0) == [] and xs.last(9) == [1, 2, 3]
  ok_lazy_last = [4, 5].lazy().last == 5
  if ok_flattened and ok_flatten_bang and ok_compact and ok_compact_bang and ok_last and ok_lazy_last:
    42
  else:
    0
