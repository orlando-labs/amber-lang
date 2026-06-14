package conformance.run

def probe():
  xs = [1, 2, 3, 4, 5, 6]
  g = xs.group: _1 % 3
  if g[0].count() == 2 and g[1].count() == 2 and g[2].count() == 2:
    42
  else:
    0
