package conformance.run

def probe():
  s = Set{1, 2, 3, 4, 5}
  s.select!: _1 > 2
  s.delete_if!: _1 == 4
  e = Set{3, 5}
  ok = s.count() == 2 and s.difference(e).count() == 0
  s.clear!()
  if ok and s.count() == 0:
    42
  else:
    0
