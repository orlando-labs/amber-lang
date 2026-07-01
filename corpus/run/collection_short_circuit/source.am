package conformance.run

# Chain-first short-circuit combinators (take/drop/find_index/drop_while), on
# both eager lists and lazy sequences. take/find_index bound an infinite lazy
# source by short-circuiting; early-exit-with-value stays `find`, never `break`.
def probe():
  xs = [5, 3, 8, 1, 9, 2]
  taken = xs.take(2)
  dropped = xs.drop(4)
  idx = xs.find_index |x|: x == 8
  dw = [1, 2, 3, 1].drop_while |x|: x < 3
  lazy_take = (1..).lazy().take(3).to_array()
  lazy_idx = (1..).lazy().find_index |x|: x == 4
  "#{taken}:#{dropped}:#{idx}:#{dw}:#{lazy_take}:#{lazy_idx}"
