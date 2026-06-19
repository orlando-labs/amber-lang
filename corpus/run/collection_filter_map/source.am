package conformance.run

def probe():
  xs = [0, 1, 2, 3]
  arr = xs.filter_map |x|:
    if x % 2 == 0:
      x * 10
    else:
      null
  truthy_zero = [0, false, null].filter_map |x|: x
  mapped = {a: 1, b: 2, c: 3}.filter_map |k, v|:
    if v > 1:
      v * 10
    else:
      false
  mut = [0, 1, 2, 3]
  same = mut.filter_map! |x|:
    if x % 2 == 0:
      x * 10
    else:
      false
  same.push!(99)
  set = Set{1, 2, 3, 4}
  same_set = set.filter_map! |x|:
    if x % 2 == 0:
      x * 10
    else:
      null
  same_set.add!(60)
  set_ok = set.count() == 3 and set.contains?(20) and set.contains?(40)
  set_ok = set_ok and set.contains?(60)
  lazy = xs.lazy().filter_map |x|:
    if x > 1:
      x + 1
    else:
      null
  "#{arr}:#{truthy_zero}:#{mapped}:#{mut}:#{set_ok}:#{lazy.first(2)}"
