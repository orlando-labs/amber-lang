package conformance.run

def each(xs, &blk):
  i = 0
  n = xs.length
  while i < n:
    blk(xs[i])
    i = i + 1
  xs

def my_map(xs, &blk):
  out = []
  each(xs) |x|:
    out.push!(blk(x))
  out

def maybe(xs, &blk):
  if blk == null:
    "noblock"
  else:
    my_map(xs, &blk)[0]

def probe():
  doubled = my_map([1, 2, 3]) |x|: x * 2
  total = 0
  each(doubled) |x|:
    total = total + x
  "#{doubled[0]}:#{doubled[1]}:#{doubled[2]}:sum=#{total}:#{maybe([5])}"
