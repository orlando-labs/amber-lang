package conformance.run

# Array one-pass extremum finders (min / max / minmax, optional arity-1 key
# block) and the Math prelude namespace (sqrt/pow/floor/ceil/abs/max/hypot/...).
def probe():
  xs = [5, 3, 8, 1, 9, 2]
  by_neg = xs.min |x|: 0 - x
  s = Math.sqrt(144)
  p = Math.pow(2, 10)
  f = Math.floor(3.9)
  c = Math.ceil(3.1)
  a = Math.abs(0 - 7)
  m = Math.max(10, 4)
  h = Math.hypot(3, 4)
  "#{xs.min()}:#{xs.max()}:#{xs.minmax()}:#{by_neg}:#{s}:#{p}:#{f}:#{c}:#{a}:#{m}:#{h}"
