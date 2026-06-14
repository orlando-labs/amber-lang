package conformance.run

def probe():
  xs = [1, 2, 2, 3, 4, 5]
  xs.delete!(2)
  xs.delete_if!: _1 > 3
  xs.append!(9)
  xs.keep_if!: _1 < 9
  xs.delete_at!(0)
  xs
