def probe():
  xs = [1, 2, 3]
  ok_join = xs.join(",") == "1,2,3" and ["a", "b"].join() == "ab"
  ok_sum = xs.sum() == 6 and xs.sum(10) == 16 and [].sum() == 0 and [1.5, 2].sum() == 3.5
  ok_product = [2, 3].product() == 6 and [].product() == 1
  ok_count = [1, 2, 2, 3].count(2) == 2 and xs.count() == 3
  ok_count_block = xs.count |x|: x > 1
  ok_tally = ["a", "b", "a"].tally() == {"a": 2, "b": 1}
  parts = xs.partition |x|: x > 1
  ok_partition = parts == [[2, 3], [1]]
  ok_zip = [1, 2].zip([3, 4], [5, 6]) == [[1, 3, 5], [2, 4, 6]] and [1, 2, 3].zip([4]) == [[1, 4]]
  ok_ewi = [10, 20].each_with_index() == [[10, 0], [20, 1]]
  acc = 0
  [5, 6].each_with_index |v, i|:
    acc = acc + v * (i + 1)
  ok_ewi_block = acc == 17
  ok_lazy = [1, 2, 3].lazy().sum() == 6 and (1..4).sum() == 10
  if ok_join and ok_sum and ok_product and ok_count and ok_count_block == 2 and ok_tally and ok_partition and ok_zip and ok_ewi and ok_ewi_block and ok_lazy:
    42
  else:
    0
